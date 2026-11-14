#pragma once
// Two fixes for the game itself, separate from HDR and DLSS: the engine's 200 FPS cap is removed and the death camera
// updates at the frame rate. Like the other engine patches, they check the code they patch first and do nothing on any
// other game build.

#include "engine_patches.h"
#include <Windows.h>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>

namespace Battlefront2FrameLimit
{
   // Removes the engine's 5 ms (200 FPS) cap of its "Interpolating" frame pacer by changing four bytes inside one
   // aligned 8-byte word of the deadline update:
   //   48 8B 8B 60 70 00 00  mov rcx,[rbx+7060h]
   //   31 C9 EB 03 70 00 00  xor ecx,ecx; jmp +3
   // The jump lands on the next instruction, mov rdx,[rbx+7068h]. With RCX = 0 the minimum frame interval is not
   // added to the deadline. Simulation and QPC are unchanged, and nothing divides by this interval.
   // The RVAs refer to the unpacked code, not to the file's encrypted .tls bytes.
   constexpr size_t word_rva = 0x7FFE5E8;
   constexpr LONG64 original_word = 0x0070608B8B482024;
   constexpr LONG64 bypass_word = 0x007003EBC9312024;
   // Image bases are 64 KB aligned, so the word is aligned for InterlockedCompareExchange64.
   static_assert(word_rva % alignof(LONG64) == 0);

   enum class Result
   {
      pending,
      applied,
      already_applied,
      unsupported,
      conflict,
      protection_failed
   };

   inline Result ReplaceWord(volatile LONG64* address, LONG64 expected, LONG64 replacement, bool* written = nullptr)
   {
      if (written) *written = false;
      DWORD previous = 0;
      if (!VirtualProtect(const_cast<LONG64*>(address), sizeof(LONG64), PAGE_EXECUTE_READWRITE, &previous))
         return Result::protection_failed;
      const LONG64 found = InterlockedCompareExchange64(address, replacement, expected);
      if (written) *written = found == expected;
      DWORD unused = 0;
      const BOOL restored = VirtualProtect(const_cast<LONG64*>(address), sizeof(LONG64), previous, &unused);
      if (found == expected)
         FlushInstructionCache(GetCurrentProcess(), const_cast<LONG64*>(address), sizeof(LONG64));
      if (!restored)
         return Result::protection_failed;
      return found == expected ? Result::applied : Result::conflict;
   }

   struct Patch
   {
      volatile LONG64* owned = nullptr;

      Result Apply(void* module)
      {
         auto* base = static_cast<unsigned char*>(module);
         // A malformed or unreadable image reports unsupported instead of faulting the render thread.
         __try
         {
            if (!base || !Battlefront2Executable::Supported(base))
               return Result::unsupported;
            // The pacer's name and interval, its constructor, and the whole deadline update around the word.
            // Until the code is unpacked these do not match yet.
            if (std::memcmp(base + 0x3162CD8, "Interpolating", 14) ||
                *reinterpret_cast<const uint64_t*>(base + 0x2EF7668) != 0x3F747AE147AE147B || // 0.005
                std::memcmp(base + 0x17607BA, "\xf2\x0f\x59\x05\xa6\x6e\x79\x01", 8) ||
                Battlefront2Executable::Fnv1a(base + 0x7FFE580, 21) != 0xBC4C585E26618210ULL ||
                std::memcmp(base + 0x7FFE5E1, "\xf2\x0f\x11\x36\x0f\x28\x74\x24\x20", 9) ||
                Battlefront2Executable::Fnv1a(base + word_rva + 8, 45) != 0x8FC6DD5799D2A73BULL)
               return Result::pending;

            auto* target = reinterpret_cast<volatile LONG64*>(base + word_rva);
            const LONG64 found = *target;
            if (found == bypass_word)
               return Result::already_applied; // Someone else's patch: not ours to restore.
            if (found != original_word)
               return Result::conflict;
            bool written = false;
            const Result result = ReplaceWord(target, original_word, bypass_word, &written);
            // Ours to restore even if resetting the page protection failed.
            if (written)
               owned = target;
            return result;
         }
         __except (EXCEPTION_EXECUTE_HANDLER)
         {
            return Result::unsupported;
         }
      }

      void Restore()
      {
         __try
         {
            // Compare-exchange, so a patch another mod installed since is left alone.
            if (owned)
               ReplaceWord(owned, bypass_word, original_word);
         }
         __except (EXCEPTION_EXECUTE_HANDLER)
         {
         }
         owned = nullptr;
      }
   };
} // namespace Battlefront2FrameLimit

// Finds the local player's active death camera in the engine's object graph, on the supported build only.
// Read-only: no engine calls, no allocations, no pointers kept from earlier calls. Every value read is recorded
// in the Token, so Recheck can later confirm that none of them changed.
namespace Battlefront2CameraDeathBinding
{
   constexpr std::uintptr_t address_limit = 0x0000800000000000ULL;
   constexpr unsigned anchor_capacity = 88, vector_capacity = 6;

   inline bool Range(std::uintptr_t p, std::size_t n) noexcept
   {
      return p >= 0x10000 && n && p < address_limit && n <= address_limit - p;
   }
   inline bool Object(std::uintptr_t p, std::size_t n = 8) noexcept
   {
      return !(p & 7) && Range(p, n);
   }
   // Copies game memory; false on a bad range or a fault. Whole 8-byte words are read at once so pointers do not tear.
   inline bool Raw(std::uintptr_t p, void* out, std::size_t n) noexcept
   {
      if (!Range(p, n)) return false;
      __try
      {
         auto* destination = static_cast<unsigned char*>(out);
         std::size_t at = 0;
         for (; at + 8 <= n; at += 8)
         {
            const auto word = *reinterpret_cast<const volatile std::uint64_t*>(p + at);
            std::memcpy(destination + at, &word, 8);
         }
         for (; at < n; ++at)
            destination[at] = *reinterpret_cast<const volatile unsigned char*>(p + at);
         return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }
   inline std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept
   {
      return (hash ^ value) * 1099511628211ULL;
   }

   // An engine array as it was read: begin, element count, stride and a hash of the contents. With player_ids the
   // elements are player pointers, and each player's ID is hashed too.
   struct VectorStamp
   {
      std::uintptr_t begin = 0;
      std::uint64_t hash = 0;
      std::uint16_t count = 0;
      std::uint8_t stride = 0, player_ids = 0;
   };
   inline bool VectorHash(const VectorStamp& v, std::uint64_t& hash) noexcept
   {
      if (!Object(v.begin, std::size_t(v.count) * v.stride)) return false;
      std::uint64_t result = 1469598103934665603ULL;
      for (unsigned i = 0; i < v.count; ++i)
      {
         std::uint64_t row[5]{}; // Strides are 8 to 40 bytes.
         if (!Raw(v.begin + std::size_t(i) * v.stride, row, v.stride)) return false;
         for (unsigned j = 0; j < v.stride / 8u; ++j)
            result = Mix(result, row[j]);
         if (v.player_ids)
         {
            std::uint32_t id = ~std::uint32_t(0);
            if (row[0] && (!Object(row[0], 0x230) || !Raw(row[0] + 0x38, &id, 4))) return false;
            result = Mix(result, id);
         }
      }
      hash = result;
      return true;
   }

   // What identifies one bound death camera.
   struct Graph
   {
      std::uintptr_t manager = 0, camera = 0, group = 0, entity = 0, node = 0, normal_actor = 0, ws_actor = 0;
      std::uint32_t cut_generation = 0;
      bool selection = false;
      bool operator==(const Graph&) const = default;
   };
   struct Token
   {
      struct Anchor
      {
         std::uintptr_t address = 0;
         std::uint64_t value = 0;
      };
      std::uintptr_t base = 0;
      Graph graph;
      std::uint8_t anchor_count = 0, vector_count = 0;
      Anchor anchors[anchor_capacity]{};
      std::uint8_t widths[anchor_capacity]{};
      VectorStamp vectors[vector_capacity]{};

      // Reads and records a value. A later read of the same address must see the same value.
      template <class T>
      bool Get(std::uintptr_t address, T& value) noexcept
      {
         static_assert(sizeof(T) <= 8);
         std::uint64_t bits = 0;
         if (!Raw(address, &bits, sizeof(T))) return false;
         std::memcpy(&value, &bits, sizeof(T));
         for (unsigned i = 0; i < anchor_count; ++i)
            if (anchors[i].address == address && widths[i] == sizeof(T)) return anchors[i].value == bits;
         if (anchor_count == anchor_capacity) return false;
         anchors[anchor_count] = {address, bits};
         widths[anchor_count++] = sizeof(T);
         return true;
      }
      bool Equal(std::uintptr_t address, std::uintptr_t expected) noexcept
      {
         std::uintptr_t value = 0;
         return Get(address, value) && value == expected;
      }
      // Records the array behind an engine vector header (begin, end).
      bool Vector(std::uintptr_t header, unsigned stride, unsigned cap, VectorStamp& v, bool player_ids = false) noexcept
      {
         std::uintptr_t begin = 0, end = 0;
         if (!Get(header, begin) || !Get(header + 8, end) || !Object(begin) || end <= begin ||
             (end - begin) % stride || (end - begin) / stride > cap) return false;
         v = {begin, 0, std::uint16_t((end - begin) / stride), std::uint8_t(stride), std::uint8_t(player_ids)};
         if (!VectorHash(v, v.hash)) return false;
         for (unsigned i = 0; i < vector_count; ++i)
            if (vectors[i].begin == begin && vectors[i].count == v.count && vectors[i].stride == v.stride)
               return vectors[i].hash == v.hash && vectors[i].player_ids == v.player_ids;
         if (vector_count == vector_capacity) return false;
         vectors[vector_count++] = v;
         return true;
      }
      // Records an array whose length comes from another one.
      bool BodyVector(std::uintptr_t begin, unsigned count, unsigned stride, VectorStamp& v) noexcept
      {
         if (vector_count == vector_capacity) return false;
         v = {begin, 0, std::uint16_t(count), std::uint8_t(stride), 0};
         if (!VectorHash(v, v.hash)) return false;
         vectors[vector_count++] = v;
         return true;
      }
      bool Asset(std::uintptr_t asset, std::uintptr_t type_rva, std::uintptr_t vtable_rva) noexcept
      {
         return Object(asset, 16) && Equal(asset, base + vtable_rva) && Equal(asset + 8, base + type_rva);
      }
   };
   static_assert(sizeof(Token) <= 2048, "The token lives on the hook's stack");

   // Whether everything Resolve read is unchanged.
   inline bool Recheck(const Token& token) noexcept
   {
      for (unsigned i = 0; i < token.anchor_count; ++i)
      {
         std::uint64_t value = 0;
         if (!Raw(token.anchors[i].address, &value, token.widths[i]) || value != token.anchors[i].value) return false;
      }
      for (unsigned i = 0; i < token.vector_count; ++i)
      {
         std::uint64_t hash = 0;
         if (!VectorHash(token.vectors[i], hash) || hash != token.vectors[i].hash) return false;
      }
      return true;
   }

   struct Key
   {
      std::uintptr_t data = 0;
      std::uint32_t field = 0;
   };
   // The engine's lower_bound order: pointers unsigned, field IDs signed.
   inline bool KeyLess(Key a, Key b) noexcept
   {
      return a.data < b.data || (a.data == b.data && std::int32_t(a.field) < std::int32_t(b.field));
   }
   // The one row of a sorted table whose leading key is `wanted`.
   inline bool FindRow(const VectorStamp& table, unsigned stride, Key wanted, std::uint64_t (&found)[5], unsigned& index) noexcept
   {
      unsigned matches = 0;
      Key prior{};
      for (unsigned i = 0; i < table.count; ++i)
      {
         std::uint64_t row[5]{};
         if (!Raw(table.begin + std::size_t(i) * stride, row, stride)) return false;
         const Key current{row[0], std::uint32_t(row[1])};
         if (i && KeyLess(current, prior)) return false;
         prior = current;
         if (current.data == wanted.data && current.field == wanted.field)
         {
            ++matches;
            index = i;
            std::memcpy(found, row, sizeof(row));
         }
      }
      return matches == 1;
   }
   struct Connection
   {
      std::uintptr_t address = 0, value = 0, type = 0;
      std::uint32_t flags = 0;
   };
   // The connection into `target` on a bus: the targets table (40-byte rows) gives the source key, the sources table
   // (24-byte rows) its slot.
   inline bool Canonical(Token& t, const VectorStamp& targets, const VectorStamp& sources,
      std::uintptr_t slots, Key target, Key* source, Connection* connection) noexcept
   {
      std::uint64_t row[5]{};
      unsigned index = 0;
      if (!FindRow(targets, 40, target, row, index)) return false;
      const Key key{row[2], std::uint32_t(row[3])};
      std::uintptr_t tagged = 0;
      if (!Object(key.data, 16) || !FindRow(sources, 24, key, row, index) || !t.Get(slots + std::size_t(index) * 8, tagged)) return false;
      Connection c{};
      c.address = tagged & ~std::uintptr_t(3);
      if (!Object(c.address, 0x50) || !t.Get(c.address + 0x38, c.value) || !t.Get(c.address + 0x40, c.type) ||
          !t.Get(c.address + 0x48, c.flags) || !Object(c.value)) return false;
      *source = key;
      *connection = c;
      return true;
   }
   // The bus's actor for `asset` (parallel sorted arrays): it must have vtable `vt`, whose first entry is `getter`,
   // and point back at the bus and the asset.
   inline bool Peer(Token& t, std::uintptr_t bus, const VectorStamp& assets,
      const VectorStamp& peers, std::uintptr_t asset, std::uintptr_t vt, std::uintptr_t getter, std::uintptr_t* actor) noexcept
   {
      std::uintptr_t prior = 0, result = 0;
      unsigned matches = 0;
      for (unsigned i = 0; i < assets.count; ++i)
      {
         std::uintptr_t data = 0, peer = 0;
         if (!Raw(assets.begin + std::size_t(i) * 8, &data, 8) || data < prior) return false;
         prior = data;
         if (data == asset)
         {
            if (!Raw(peers.begin + std::size_t(i) * 8, &peer, 8)) return false;
            ++matches;
            result = peer;
         }
      }
      if (matches != 1 || !Object(result, 0x240) || !t.Equal(result, t.base + vt) ||
          !t.Equal(result + 0x20, bus) || !t.Equal(result + 0x28, asset) || !t.Equal(t.base + vt, t.base + getter)) return false;
      *actor = result;
      return true;
   }

   // Resolves the death camera that `entity` publishes to `camera`. Fails unless it is the local player's active one.
   inline bool Resolve(std::uintptr_t base, void* entity_argument, void* camera_argument, Token* output) noexcept
   {
      *output = Token{};
      Token& t = *output;
      Graph& g = t.graph;
      t.base = base;
      const auto entity = reinterpret_cast<std::uintptr_t>(entity_argument), camera = reinterpret_cast<std::uintptr_t>(camera_argument);
      if (!Object(base) || !Object(entity, 0x30) || !Object(camera, 0xE8) || camera < 0x100A8) return false;
      // The local player (ID 0), whose camera manager's active camera must be this one.
      std::uintptr_t context = 0, players = 0, local = 0;
      VectorStamp members{}, group_members{}, targets{}, sources{}, assets{}, peers{};
      if (!t.Get(base + 0x3EE7858, context) || !Object(context, 0x60) || !t.Get(context + 0x58, players) ||
          !Object(players, 0x550) || !t.Vector(players + 0x540, 8, 128, members, true)) return false;
      for (unsigned i = 0; i < members.count; ++i)
      {
         std::uintptr_t player = 0;
         std::uint32_t id = ~std::uint32_t(0);
         if (!Raw(members.begin + std::size_t(i) * 8, &player, 8)) return false;
         if (!player) continue;
         if (!Object(player, 0x230) || !Raw(player + 0x38, &id, 4)) return false;
         if (id == 0)
         {
            if (local) return false;
            local = player;
         }
      }
      if (!local || !t.Get(local + 0x228, g.manager) || !Object(g.manager, 0x1F48) ||
          !t.Equal(g.manager, base + 0x2F3CA88) || !t.Equal(g.manager + 0xF8, camera) ||
          !t.Get(g.manager + 0x1F44, g.cut_generation) || !t.Equal(camera, base + 0x2F3CDF8)) return false;
      // The camera's group, whose last member is this entity, and the camera's node.
      g.camera = camera;
      g.group = camera - 0xA8;
      g.entity = entity;
      if (!t.Equal(g.group, base + 0x2F3D390) || !t.Vector(g.group + 0x28, 16, 256, group_members) ||
          !t.Equal(group_members.begin + std::size_t(group_members.count) * 16 - 8, entity) ||
          !t.Equal(entity, base + 0x2F3CEC0) ||
          !t.Get(camera + 0xE0, g.node) || !Object(g.node, 0xB0) || !t.Equal(g.node, base + 0x306EE80)) return false;
      // The entity's property bus; bit 27 of the flags defers to the parent bus.
      std::uintptr_t data = 0, bus = 0;
      if (!t.Get(entity + 0x28, data) || !Object(data, 16) || !t.Get(entity + 0x20, bus) || !Object(bus, 0x80)) return false;
      for (unsigned depth = 0;; ++depth)
      {
         std::uint32_t flags = 0;
         std::uintptr_t parent = 0;
         if (depth == 16 || !t.Get(bus + 0x3C, flags)) return false;
         if (!(flags & (1u << 27))) break;
         if (!t.Get(bus + 0x10, parent) || !Object(parent, 0x80) || parent == bus) return false;
         bus = parent;
      }
      // The selector connected to the entity data, then the selector's normal, world-space and selection inputs.
      std::uintptr_t table = 0, slots = 0;
      if (!t.Get(bus + 0x68, table) || !Object(table, 0x110) || !t.Get(bus + 0x50, slots) || !Object(slots) ||
          !t.Vector(table, 40, 256, targets) || !t.Vector(table + 0x90, 24, 256, sources)) return false;
      Key selector{};
      Connection selector_connection{};
      if (!Canonical(t, targets, sources, slots, {data, 0x87525469}, &selector, &selector_connection) ||
          !t.Asset(selector.data, 0x4577C10, 0x3064F20) || selector.field != 0x0B87DF4B ||
          selector_connection.type != base + 0x3AF4FA0 || !(selector_connection.flags & 0x40)) return false;
      Key normal_key{}, ws_key{}, selection_key{};
      Connection normal{}, ws{}, selection{};
      if (!Canonical(t, targets, sources, slots, {selector.data, 0x0B87D373}, &normal_key, &normal) ||
          !Canonical(t, targets, sources, slots, {selector.data, 0x0B87D370}, &ws_key, &ws) ||
          !Canonical(t, targets, sources, slots, {selector.data, 0x11D5B185}, &selection_key, &selection) ||
          !t.Asset(normal_key.data, 0x46EEB50, 0x32D2458) || normal_key.field != 0x846F73F0 ||
          !t.Asset(ws_key.data, 0x46EEA50, 0x32D24A0) || ws_key.field != 0x49C06914 ||
          selection_key.data != ws_key.data || selection_key.field != 0xCA91E79B ||
          normal.type != base + 0x3AF4FA0 || ws.type != base + 0x3AF4FA0 ||
          selection.type != base + 0x43F4FE8 || !(selection.flags & 0x40)) return false;
      std::uint8_t choice = 0;
      if (!t.Get(selection.value, choice) || choice > 1) return false;
      g.selection = choice != 0;
      // The bus's two actors must be fed by those connections.
      std::uint32_t bus_flags = 0;
      std::uintptr_t peer_slots = 0;
      if (!t.Get(bus + 0x3C, bus_flags) || !t.Get(bus + 0x40, peer_slots) || !Object(peer_slots) ||
          !t.Vector(table + ((bus_flags & (1u << 28)) ? 0xF8 : 0xD0), 8, 256, assets) ||
          !t.BodyVector(peer_slots, assets.count, 8, peers) ||
          !Peer(t, bus, assets, peers, normal_key.data, 0x31FB838, 0x1A5F7E0, &g.normal_actor) ||
          !Peer(t, bus, assets, peers, ws_key.data, 0x31FB950, 0x1A5F890, &g.ws_actor) ||
          !t.Equal(g.normal_actor + 0xC8, normal.address) || !t.Equal(g.ws_actor + 0xE8, ws.address) ||
          !t.Equal(g.ws_actor + 0xF8, selection.address)) return false;
      return Recheck(t);
   }

   // The engine methods this walk mirrors, checked once at installation. Code longer than 16 bytes is compared by
   // length and hash.
   inline bool SupportedCode(const unsigned char* code) noexcept
   {
      const auto base = reinterpret_cast<std::uintptr_t>(code);
      __try
      {
         return !std::memcmp(code + 0x53156E0, "\x48\x8b\x41\x08\xc3", 5) &&
                !std::memcmp(code + 0x185CD0, "\xe9\x0b\xfa\x18\x05", 5) &&
                Battlefront2Executable::Fnv1a(code + 0xA7FD20, 35) == 0x5A27ACC12887FDFEULL &&
                !std::memcmp(code + 0x890E850, "\x48\x8d\x05\x99\xc1\xd3\xfb\xc3", 8) &&
                !std::memcmp(code + 0x890EF90, "\x48\x8d\x05\x89\xb9\xd3\xfb\xc3", 8) &&
                Battlefront2Executable::Fnv1a(code + 0x71DF800, 110) == 0x291DA1268AA1F271ULL &&
                Battlefront2Executable::Fnv1a(code + 0x543DA10, 292) == 0xC10F20B7FCA176DDULL &&
                Battlefront2Executable::Fnv1a(code + 0x53D9150, 85) == 0xE6A468DE846BAA85ULL &&
                Battlefront2Executable::Fnv1a(code + 0x71E1410, 332) == 0xAF0F7E354C0390B5ULL &&
                // The selector, normal and world-space vtables still start with the getter that returns the type
                // at +8 (0x185CD0, a jump to the first method above), which Token::Asset relies on.
                *reinterpret_cast<const std::uintptr_t*>(code + 0x3064F20) == base + 0x185CD0 &&
                *reinterpret_cast<const std::uintptr_t*>(code + 0x32D2458) == base + 0x185CD0 &&
                *reinterpret_cast<const std::uintptr_t*>(code + 0x32D24A0) == base + 0x185CD0;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }
} // namespace Battlefront2CameraDeathBinding

// Death camera update-rate fix. While the engine publishes the local player's death camera (found by
// Battlefront2CameraDeathBinding above), the fraction argument of that one SIM publication is changed from 0 to 1. Both hooked
// callbacks still call the original exactly once; nothing else is written and no camera state is kept.
namespace Battlefront2CameraNativeDeathRate
{
   namespace Binding = Battlefront2CameraDeathBinding;
   // The camera publication callback (vtable slot, native target), the SIM service callback, and the return address
   // of the service call that publishes the camera.
   constexpr std::uintptr_t slot_rva = 0x2F3CFA8, callback_rva = 0xA83BA0;
   constexpr std::uintptr_t service_slot_rva = 0x3162818, service_callback_rva = 0x176CC80;
   constexpr std::uintptr_t publication_return_rva = 0x74DFFC0;
   using Callback = void(__fastcall*)(void*, void*);
   using ServiceCallback = void(__fastcall*)(void*, std::uint32_t, float, bool);
   inline std::atomic<Callback> original{nullptr};
   inline std::atomic<ServiceCallback> original_service{nullptr};
   inline std::atomic<std::uintptr_t> game_base{0};
   // "requested" is the user's choice, "enabled" whether the fix runs. A failed installation clears only "enabled",
   // so a later successful retry turns it back on.
   inline std::atomic_bool enabled{false}, requested{false};
   // Changes on every switch, so a scope prepared before a switch is not used after it.
   inline std::atomic<std::uint64_t> epoch{1};

   inline void SetActive(bool value) noexcept
   {
      if (value)
      {
         epoch.fetch_add(1, std::memory_order_acq_rel);
         enabled.store(true, std::memory_order_release);
      }
      else if (enabled.exchange(false, std::memory_order_acq_rel))
         epoch.fetch_add(1, std::memory_order_acq_rel);
   }
   inline void SetEnabled(bool value) noexcept
   {
      requested.store(value, std::memory_order_release);
      SetActive(value);
   }
   inline void ReapplyRequested() noexcept
   {
      if (!requested.load(std::memory_order_acquire) || enabled.load(std::memory_order_acquire)) return;
      SetActive(true);
      // An Off that raced with this wins.
      if (!requested.load(std::memory_order_acquire)) SetActive(false);
   }
   template <class T>
   inline T Read(std::uintptr_t address, std::uintptr_t offset = 0) noexcept
   {
      return *reinterpret_cast<const volatile T*>(address + offset);
   }
   inline bool Pointer(std::uintptr_t value) noexcept
   {
      return value >= 0x10000 && value < Binding::address_limit;
   }

   // The first SIM generation after a camera is bound (on death or a camera cut) stays native; later ones are
   // repaired. Only one camera is bound at a time. This is lifecycle state, not camera history.
   struct CutGate
   {
      Binding::Graph graph{};
      std::uint64_t epoch = 0;
      std::uint32_t entry_generation = 0;
      bool Allow(const Binding::Graph& g, std::uint32_t generation, std::uint64_t e) noexcept
      {
         if (graph != g || epoch != e)
         {
            graph = g;
            epoch = e;
            entry_generation = generation;
            return false;
         }
         return generation != entry_generation;
      }
   };
   inline CutGate cut_gate;
   // Guards cut_gate. A caller that finds it taken skips its update instead of waiting.
   inline std::atomic_flag writer = ATOMIC_FLAG_INIT;

   // A death camera resolved for the duration of one publication callback.
   struct Scope
   {
      Binding::Token token{};
      std::uintptr_t service = 0;
      std::uint32_t handle = 0;
      std::uint64_t epoch = 0;
      bool valid = false;
   };
   inline thread_local Scope* current_scope = nullptr;

   inline void ForwardScoped(Scope* scope, void* entity, void* camera)
   {
      Scope* previous = current_scope;
      current_scope = scope;
      // /EHsc runs no destructors for SEH exceptions; __finally restores the scope for either kind and catches neither.
      __try
      {
         original.load(std::memory_order_acquire)(entity, camera);
      }
      __finally
      {
         current_scope = previous;
      }
   }
   inline void PrepareScope(void* entity, void* camera, std::uint64_t expected_epoch, Scope* scope) noexcept
   {
      __try
      {
         const auto base = game_base.load(std::memory_order_acquire);
         if (!Binding::Resolve(base, entity, camera, &scope->token)) return;
         const auto node = scope->token.graph.node;
         scope->epoch = expected_epoch;
         scope->service = Read<std::uintptr_t>(base + 0x4078830);
         if (!Pointer(scope->service) || Read<std::uintptr_t>(scope->service) != base + 0x31627F0 ||
             Read<std::int32_t>(node, 0xA8) != -1) return;
         scope->handle = Read<std::uint32_t>(node, 0x80);
         if (scope->handle < 2 || scope->handle >= 0x20002) return;
         scope->valid = enabled.load(std::memory_order_acquire) && epoch.load(std::memory_order_acquire) == expected_epoch &&
                        Binding::Recheck(scope->token);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
      }
   }
   // The node's current SIM generation, found through its handle the way the service at 0x8005990 does. Fails unless
   // the head is current and its fraction is 0 or 1. Faults go to the caller's handler.
   inline bool SimGeneration(std::uintptr_t base, std::uintptr_t node, std::uint32_t& generation) noexcept
   {
      if (Read<std::uintptr_t>(node) != base + 0x306EE80 || Read<std::int32_t>(node, 0xA8) != -1) return false;
      const auto handle = Read<std::uint32_t>(node, 0x80);
      if (handle < 2 || handle >= 0x20002) return false;
      const std::uint32_t index = handle - 2, shifted = index + 16;
      const auto power = static_cast<unsigned>(std::bit_width(shifted) - 1); // Bucket n holds 16 << n records.
      const auto service = Read<std::uintptr_t>(base + 0x4078830);
      if (!Pointer(service) || Read<std::uintptr_t>(service) != base + 0x31627F0) return false;
      const auto bucket = Read<std::uintptr_t>(service, 0x108 + 8 * static_cast<std::uintptr_t>(power - 4));
      if (!Pointer(bucket)) return false;
      const auto record = bucket + 96 * (shifted - (std::uint32_t(1) << power));
      if (!Pointer(record) || Read<std::uintptr_t>(record) != base + 0x3162440) return false;
      const auto realm_index = Read<std::uint8_t>(record, 0x59);
      if (realm_index > 3) return false;
      const auto realm = Read<std::uintptr_t>(service, 0xD8 + 8 * realm_index);
      if (!Pointer(realm)) return false;
      const auto sim = Read<std::uintptr_t>(realm, 0x400);
      if (!Pointer(sim)) return false;
      const auto page = Read<std::uintptr_t>(sim, 8 * (index >> 10));
      if (!Pointer(page)) return false;
      const auto head = Read<std::uintptr_t>(page, 8 * (index & 0x3FF));
      if (!Pointer(head)) return false;
      generation = Read<std::uint32_t>(sim, 0x418);
      if (Read<std::uint32_t>(head, 0x80) != generation || Read<std::uintptr_t>(head, 0x70) != 0) return false;
      const auto fraction = Read<std::uint32_t>(head, 0x78);
      return fraction == 0 || fraction == 0x3F800000;
   }
   // Whether this service call is the bound camera's publication (fraction 0, from the publication site) and may be
   // repaired. Any change since PrepareScope, or a SIM head that is not current, resets the cut gate.
   inline bool ShouldRepair(const Scope& scope, void* service, std::uint32_t handle, float fraction, bool overwrite,
      std::uintptr_t caller) noexcept
   {
      const auto base = scope.token.base, node = scope.token.graph.node;
      __try
      {
         if (caller != base + publication_return_rva || std::bit_cast<std::uint32_t>(fraction) != 0 || !overwrite ||
             reinterpret_cast<std::uintptr_t>(service) != scope.service || handle != scope.handle ||
             !enabled.load(std::memory_order_acquire) || epoch.load(std::memory_order_acquire) != scope.epoch) return false;
         std::uint32_t generation = 0;
         if (!Binding::Recheck(scope.token) || Read<std::uintptr_t>(base + 0x4078830) != scope.service ||
             Read<std::uintptr_t>(scope.service) != base + 0x31627F0 || Read<std::uint32_t>(node, 0x80) != handle ||
             !SimGeneration(base, node, generation))
         {
            cut_gate = CutGate{};
            return false;
         }
         return cut_gate.Allow(scope.token.graph, generation, scope.epoch);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         cut_gate = CutGate{};
         return false;
      }
   }
   // For a callback without a bound camera. The callback is shared by unrelated cameras, so the gate is reset only
   // when it is the bound camera itself or its manager no longer points at it.
   inline void InvalidateDeparted(void* entity, void* camera) noexcept
   {
      const auto base = game_base.load(std::memory_order_acquire);
      __try
      {
         const auto& bound = cut_gate.graph;
         if (bound.manager && (reinterpret_cast<std::uintptr_t>(entity) == bound.entity ||
                                 reinterpret_cast<std::uintptr_t>(camera) == bound.camera ||
                                 Read<std::uintptr_t>(bound.manager) != base + 0x2F3CA88 ||
                                 Read<std::uintptr_t>(bound.manager, 0xF8) != bound.camera))
            cut_gate = CutGate{};
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         cut_gate = CutGate{};
      }
   }
   inline __declspec(noinline) void __fastcall ServiceBridge(void* service, std::uint32_t handle, float fraction, bool overwrite)
   {
      const auto function = original_service.load(std::memory_order_acquire);
      bool repair = false;
      if (const auto* scope = current_scope; scope && !writer.test_and_set(std::memory_order_acquire))
      {
         repair = ShouldRepair(*scope, service, handle, fraction, overwrite, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
         writer.clear(std::memory_order_release);
      }
      // The original runs once, outside any SEH handler, so the engine's own lookup and locking stay where they were.
      function(service, handle, repair ? 1.f : fraction, overwrite);
   }
   inline void __fastcall Bridge(void* entity, void* camera)
   {
      Scope scope;
      if (enabled.load(std::memory_order_acquire)) PrepareScope(entity, camera, epoch.load(std::memory_order_acquire), &scope);
      if (!scope.valid && !writer.test_and_set(std::memory_order_acquire))
      {
         InvalidateDeparted(entity, camera);
         writer.clear(std::memory_order_release);
      }
      // A nested callback from another publisher gets no scope, which hides the outer death camera's.
      ForwardScoped(scope.valid ? &scope : nullptr, entity, camera);
   }

   enum class Result
   {
      pending,
      applied,
      unsupported,
      conflict,
      protection_failed,
      pin_failed
   };
   inline const char* ResultName(Result r) noexcept
   {
      constexpr const char* names[] = {"pending", "applied", "unsupported", "conflict", "protection_failed", "pin_failed"};
      return names[static_cast<int>(r)];
   }
   // The hooked callbacks and their thunks, the SIM service, and the publication call site. Code longer than 16 bytes
   // is compared by length and hash.
   inline bool SupportedCode(const unsigned char* base) noexcept
   {
      return !std::memcmp(base + 0xA83BA0, "\xe9\x1b\x5f\xb4\x05", 5) &&
             Battlefront2Executable::Fnv1a(base + 0x65C9AC0, 301) == 0x840B4168E5D391F6ULL &&
             Battlefront2Executable::Fnv1a(base + 0x1771B40, 23) == 0xA1B3416746E6C547ULL &&
             !std::memcmp(base + 0x176CC80, "\xe9\x0b\x0e\x89\x06", 5) &&
             !std::memcmp(base + 0x7FFDA90, "\x48\x83\xe9\x80\xe9\xb7\x9a\x77\xf9", 9) &&
             !std::memcmp(base + 0x1777550, "\xe9\x3b\xe4\x88\x06", 5) &&
             Battlefront2Executable::Fnv1a(base + 0x8005990, 576) == 0xABE4694594F96DF6ULL &&
             !std::memcmp(base + 0x1181070, "\xe9\x9b\xea\x35\x06", 5) &&
             Battlefront2Executable::Fnv1a(base + 0x74DFB10, 95) == 0x29136F81FFCA222CULL &&
             !std::memcmp(base + 0x11810D0, "\xe9\xbb\xec\x35\x06", 5) &&
             Battlefront2Executable::Fnv1a(base + 0x74DFF8D, 51) == 0xDDFFB1FF2CDD6086ULL;
   }
   inline Result Replace(volatile LONG64* slot, LONG64 expected, LONG64 replacement, bool* written) noexcept
   {
      *written = false;
      DWORD previous = 0;
      if (!VirtualProtect(const_cast<LONG64*>(slot), sizeof(LONG64), PAGE_READWRITE, &previous)) return Result::protection_failed;
      *written = InterlockedCompareExchange64(slot, replacement, expected) == expected;
      DWORD ignored = 0;
      if (!VirtualProtect(const_cast<LONG64*>(slot), sizeof(LONG64), previous, &ignored)) return Result::protection_failed;
      return *written ? Result::applied : Result::conflict;
   }
   struct Patch
   {
      volatile LONG64* owned = nullptr;
      volatile LONG64* owned_service = nullptr;
      LONG64 native = 0, service_native = 0;

      Result Install(void* module) noexcept
      {
         auto* base = static_cast<unsigned char*>(module);
         __try
         {
            if (!base) return Result::unsupported;
            if ((owned || owned_service) && game_base.load(std::memory_order_acquire) != reinterpret_cast<std::uintptr_t>(base)) return Result::conflict;
            if (!Battlefront2Executable::Supported(base)) return Result::unsupported;
            if (!SupportedCode(base) || !Binding::SupportedCode(base)) return Result::pending;
            auto* slot = reinterpret_cast<volatile LONG64*>(base + slot_rva);
            auto* service_slot = reinterpret_cast<volatile LONG64*>(base + service_slot_rva);
            native = reinterpret_cast<LONG64>(base + callback_rva);
            service_native = reinterpret_cast<LONG64>(base + service_callback_rva);
            const auto bridge = reinterpret_cast<LONG64>(&Bridge), service_bridge = reinterpret_cast<LONG64>(&ServiceBridge);
            if (*slot != (owned ? bridge : native) || *service_slot != (owned_service ? service_bridge : service_native))
            {
               SetActive(false);
               return Result::conflict;
            }
            if (!owned || !owned_service)
            {
               // The game calls into these hooks, so the add-on must never be unloaded while they are installed.
               HMODULE pinned = nullptr;
               if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                      reinterpret_cast<LPCWSTR>(&Bridge), &pinned)) return Result::pin_failed;
               original.store(reinterpret_cast<Callback>(native), std::memory_order_release);
               original_service.store(reinterpret_cast<ServiceCallback>(service_native), std::memory_order_release);
               game_base.store(reinterpret_cast<std::uintptr_t>(base), std::memory_order_release);
               // The service hook goes in first, so the camera hook never runs without it.
               auto result = Hook(owned_service, service_slot, service_native, service_bridge);
               if (result == Result::applied) result = Hook(owned, slot, native, bridge);
               if (result != Result::applied)
               {
                  SetActive(false);
                  return result;
               }
            }
            ReapplyRequested();
            return Result::applied;
         }
         __except (EXCEPTION_EXECUTE_HANDLER)
         {
            return Result::unsupported;
         }
      }
      // False if a bridge could not be removed. It stays in place but inactive (it only forwards), and ownership is
      // kept for a retry. The originals stay valid for engine threads that already fetched a bridge.
      bool Restore() noexcept
      {
         SetActive(false);
         return RestoreSlot(owned, reinterpret_cast<LONG64>(&Bridge), native) & RestoreSlot(owned_service, reinterpret_cast<LONG64>(&ServiceBridge), service_native);
      }

   private:
      // Ownership is recorded as soon as the slot holds our bridge, even if resetting the protection failed.
      static Result Hook(volatile LONG64*& owner, volatile LONG64* slot, LONG64 native_value, LONG64 bridge) noexcept
      {
         if (owner) return Result::applied;
         bool written = false;
         const auto result = Replace(slot, native_value, bridge, &written);
         if (written) owner = slot;
         return result;
      }
      static bool RestoreSlot(volatile LONG64*& slot, LONG64 bridge, LONG64 original_value) noexcept
      {
         if (!slot) return true;
         __try
         {
            bool written = false;
            Replace(slot, bridge, original_value, &written);
            // Drop ownership once restored, or when another hook replaced ours. A failed protection change leaves our
            // bridge in place: keep it.
            if (written || *slot != bridge)
            {
               slot = nullptr;
               return written;
            }
         }
         __except (EXCEPTION_EXECUTE_HANDLER)
         {
         }
         return false;
      }
   };
} // namespace Battlefront2CameraNativeDeathRate

namespace Battlefront2EngineFixes
{
   // Both fixes are retried at present (the game's code is unpacked by then) until they apply or time out; after that
   // Update() is a flag check. Restore() removes them when the add-on unloads.
   class Fixes
   {
      Battlefront2FrameLimit::Patch frame_limit;
      std::atomic_bool frame_limit_finished{false};
      std::mutex frame_limit_mutex;
      ULONGLONG frame_limit_deadline = 0;
      ULONGLONG frame_limit_next_attempt = 0;
      Battlefront2CameraNativeDeathRate::Patch death_camera;
      std::atomic_bool death_camera_finished{false};
      std::mutex death_camera_mutex;
      ULONGLONG death_camera_deadline = 0, death_camera_next_attempt = 0;

      void UpdateDeathCamera()
      {
         if (death_camera_finished.load(std::memory_order_acquire)) return;
         const std::lock_guard lock(death_camera_mutex);
         if (death_camera_finished.load(std::memory_order_relaxed)) return;
         const auto now = GetTickCount64();
         if (now < death_camera_next_attempt) return;
         if (!death_camera_deadline) death_camera_deadline = now + 30000;
         death_camera_next_attempt = now + 500;
         using Battlefront2CameraNativeDeathRate::Result;
         const auto result = death_camera.Install(GetModuleHandleW(nullptr));
         if ((result == Result::pending || result == Result::protection_failed) && now < death_camera_deadline) return;
         const auto message = std::string("[Battlefront II Luma] Death camera update-rate fix: ") +
                              Battlefront2CameraNativeDeathRate::ResultName(result) + ".";
         reshade::log::message(result == Result::applied       ? reshade::log::level::info
                               : result == Result::unsupported ? reshade::log::level::warning
                                                               : reshade::log::level::error,
            message.c_str());
         death_camera_finished.store(true, std::memory_order_release);
      }

      void UpdateFrameLimit()
      {
         if (frame_limit_finished.load(std::memory_order_acquire)) return;
         const std::lock_guard lock(frame_limit_mutex);
         if (frame_limit_finished.load(std::memory_order_relaxed)) return;
         const ULONGLONG now = GetTickCount64();
         if (now < frame_limit_next_attempt) return;
         if (!frame_limit_deadline) frame_limit_deadline = now + 30000;
         frame_limit_next_attempt = now + 500;
         using Battlefront2FrameLimit::Result;
         const Result result = frame_limit.Apply(GetModuleHandleW(nullptr));
         if (result == Result::pending && now < frame_limit_deadline) return;
         if (result == Result::applied || result == Result::already_applied)
            reshade::log::message(reshade::log::level::info,
               "[Battlefront II Luma] Engine limiter: native Interpolating 5 ms (200 FPS) cap bypass active at RVA 0x7FFE5EA. Simulation/QPC unchanged.");
         else if (result == Result::unsupported)
            reshade::log::message(reshade::log::level::warning,
               "[Battlefront II Luma] Engine limiter: not applied (unsupported game build, or its code was changed by another patch).");
         else
            reshade::log::message(reshade::log::level::error,
               "[Battlefront II Luma] Engine limiter: not applied (the code could not be patched).");
         frame_limit_finished.store(true, std::memory_order_release);
      }

   public:
      Fixes()
      {
         Battlefront2CameraNativeDeathRate::SetEnabled(true);
      }
      void Update()
      {
         UpdateFrameLimit();
         UpdateDeathCamera();
      }
      void Restore()
      {
         if (!death_camera.Restore())
            reshade::log::message(reshade::log::level::error,
               "[Battlefront II Luma] Death camera hook could not be removed; it stays installed but inactive.");
         frame_limit.Restore();
      }
   };
} // namespace Battlefront2EngineFixes
