#pragma once
// Patches of the game executable and of its settings in memory. All of them check the exact game build first and do nothing on any other.

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bit>
#include <d3d11.h>
#include <array>
#include <atomic>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Battlefront2Executable
{
   // The game executable (September 2021 build) that every native patch here was verified against. `base` is the
   // running executable's image, whose headers are always mapped.
   inline bool Supported(const unsigned char* base)
   {
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew > 0x1000)
         return false;
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
      return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
             nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
             nt->FileHeader.TimeDateStamp == 0x614AF6B8 && nt->OptionalHeader.SizeOfImage == 0xE45C000;
   }

   // 64-bit FNV-1a. Checks of longer stretches of game code compare a length and this hash, so the mod does not
   // carry a copy of the game's code.
   inline std::uint64_t Fnv1a(const unsigned char* data, std::size_t size)
   {
      std::uint64_t hash = 0xCBF29CE484222325ULL;
      for (std::size_t i = 0; i < size; ++i)
         hash = (hash ^ data[i]) * 0x100000001B3ULL;
      return hash;
   }

   // Whether the running executable is that build.
   inline bool Running()
   {
      static const bool supported = Supported(reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr)));
      return supported;
   }
} // namespace Battlefront2Executable

// Frostbite's live GameRenderSettings clone and its resolution scale. Offsets checked in the September 2021
// executable (timestamp 0x614AF6B8); nothing is written on another build or to an unrecognized settings object.
namespace Battlefront2RenderScale
{
   struct Snapshot
   {
      uintptr_t settings = 0;
      float scale = 0.f;
   };

   inline bool Read(Snapshot& result)
   {
      if (!Battlefront2Executable::Running()) return false;
      __try
      {
         const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
         const uintptr_t renderer = *reinterpret_cast<const uintptr_t*>(base + 0x3FFBE10);
         if (renderer == 0) return false;
         const uintptr_t settings = *reinterpret_cast<const uintptr_t*>(renderer + 0x518);
         if (settings == 0 || *reinterpret_cast<const uintptr_t*>(settings) != base + 0x30F37B0) return false;
         const float scale = *reinterpret_cast<const float*>(settings + 0x28);
         if (!std::isfinite(scale) || scale < 0.25f || scale > 2.f) return false;
         result = {settings, scale};
         return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }

   // Writes only if the object and its scale are still the ones `expected` read.
   inline bool Write(const Snapshot& expected, float scale)
   {
      if (!std::isfinite(scale) || scale < 0.25f || scale > 2.f) return false;
      Snapshot current;
      if (!Read(current) || current.settings != expected.settings || current.scale != expected.scale) return false;
      const auto old_bits = std::bit_cast<LONG>(current.scale), new_bits = std::bit_cast<LONG>(scale);
      __try
      {
         return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(current.settings + 0x28), new_bits, old_bits) == old_bits;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }
} // namespace Battlefront2RenderScale

// Reads the temporal pass's constants straight from the engine. On the supported build the DX11 direct and indirect
// dispatch helpers commit CS constant buffer 0 from context + 0x50060 just before Dispatch, so this is only called
// inside the identified temporal dispatch. Other executables, contexts and layouts keep using the upload mirror.
namespace Battlefront2NativeTemporal
{
   inline bool SupportedImage(uintptr_t base)
   {
      __try
      {
         // The commit helper, its direct and indirect dispatch callers, and the thunk.
         const auto* code = reinterpret_cast<const unsigned char*>(base);
         return Battlefront2Executable::Supported(code) && Battlefront2Executable::Fnv1a(code + 0x7D88B80, 34) == 0x4B1BA415B7320498ULL &&
                std::memcmp(code + 0x161B4B8, "\xe8\xd3\x29\x00\x00\x48\x8b\x8b\xc8\x00\x0a\x00", 12) == 0 &&
                std::memcmp(code + 0x161B508, "\xe8\x83\x29\x00\x00\x48\x8b\x8b\xc8\x00\x0a\x00", 12) == 0 &&
                std::memcmp(code + 0x161DE90, "\xe9\xeb\xac\x76\x06", 5) == 0;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }
   inline bool ReadAt(uintptr_t base, ID3D11DeviceContext* d3d_context, UINT width, UINT height, UINT constants, float* result)
   {
      if (!d3d_context || constants < 15 || d3d_context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
      __try
      {
         const auto device = *reinterpret_cast<const uintptr_t*>(base + 0x3FFBA28);
         if (!device) return false;
         const auto context = *reinterpret_cast<const uintptr_t*>(device + 0xBC8);
         if (!context || *reinterpret_cast<const uint32_t*>(context + 0x40) != 304) return false;
         // The engine's immediate context must be the one drawing.
         if (*reinterpret_cast<IUnknown* const*>(context + 0xA00C8) != d3d_context) return false;
         // The first two values are the render size, as integers.
         const auto* source = reinterpret_cast<const float*>(context + 0x50060);
         const UINT actual_width = std::bit_cast<UINT>(source[0]), actual_height = std::bit_cast<UINT>(source[1]);
         if (!actual_width || actual_width > 32768 || !actual_height || actual_height > 32768 || (width && actual_width != width) || (height && actual_height != height)) return false;
         std::memcpy(result, source, 60 * sizeof(float));
         return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }
   inline bool Read(ID3D11DeviceContext* context, UINT width, UINT height, UINT constants,
      std::array<float, 60>& result)
   {
      static const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
      static const bool supported = SupportedImage(base);
      return supported && ReadAt(base, context, width, height, constants, result.data());
   }
} // namespace Battlefront2NativeTemporal

// Frostbite reflection, used to confirm an engine settings field before the SSR resolution or depth dither switches
// below toggle it.

namespace Battlefront2NativeSettings
{
   // Runs on every present, so it reads directly under a fault guard rather than through ReadProcessMemory.
   inline bool ReadBytes(uintptr_t address, void* target, size_t size)
   {
      if (address < 0x10000 || address >= 0x0000800000000000ull || size > 1024 * 1024 || address + size < address) return false;
      __try
      {
         std::memcpy(target, reinterpret_cast<const void*>(address), size);
         return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }
   template <class T>
   inline bool Read(uintptr_t address, T& value)
   {
      return ReadBytes(address, &value, sizeof(value));
   }
   // Swaps one byte atomically, so a setting never tears. Page protection is never changed; the fault guard covers
   // the engine freeing the object right after the caller's identity check.
   inline bool Exchange(uintptr_t address, uint8_t expected, uint8_t value)
   {
      MEMORY_BASIC_INFORMATION page{};
      if (!VirtualQuery(reinterpret_cast<void*>(address), &page, sizeof(page)) || page.State != MEM_COMMIT || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(page.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) return false;
      __try
      {
         return uint8_t(_InterlockedCompareExchange8(reinterpret_cast<volatile char*>(address),
                   char(value), char(expected))) == expected;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
         return false;
      }
   }

   inline bool Name(uintptr_t address, std::string& result)
   {
      char text[128] = {};
      if (!ReadBytes(address, text, sizeof(text))) return false;
      for (size_t i = 0; i < sizeof(text); ++i)
      {
         if (!text[i])
         {
            if (!i) return false;
            result.assign(text, i);
            return true;
         }
         if (static_cast<unsigned char>(text[i]) < 32 || static_cast<unsigned char>(text[i]) > 126) return false;
      }
      return false;
   }
   inline bool TypeName(uintptr_t type, std::string& result)
   {
      uintptr_t info = 0, name = 0;
      return Read(type, info) && Read(info, name) && Name(name, result);
   }
   // Walks the reflection data of `object`, of type `type_name`, through at most 16 classes, and checks every field:
   // within its class, readable, Booleans 0 or 1, floats finite. True only if all of that holds, the object's type did
   // not change meanwhile, and one field is the Boolean `field` at `offset` in an object of `object_size` bytes.
   inline bool HasBoolean(uintptr_t object, const char* type_name, const char* field, uint16_t offset, uint16_t object_size)
   {
      uintptr_t original_type = 0;
      std::string actual;
      if (!Read(object + 8, original_type) || !TypeName(original_type, actual) || actual != type_name) return false;
      bool found = false;
      uintptr_t type = original_type;
      uint16_t size = 0;
      std::vector<uintptr_t> visited;
      while (type)
      {
         if (std::find(visited.begin(), visited.end(), type) != visited.end() || visited.size() >= 16) return false;
         visited.push_back(type);
         uintptr_t info = 0, table = 0, parent = 0;
         uint16_t flags = 0, class_size = 0, count = 0;
         std::string owner;
         if (!Read(type, info) || !TypeName(type, owner) || !Read(info + 8, flags) || flags != 0x65 || !Read(info + 10, class_size) || !Read(info + 34, count) || count > 4096 || !Read(info + 48, table) || !Read(type + 56, parent)) return false;
         if (!size) size = class_size;
         if (!class_size || class_size > size) return false;
         for (uint16_t i = 0; i < count; ++i)
         {
            const uintptr_t entry = table + size_t(i) * 24;
            uintptr_t name_address = 0, field_type = 0, field_info = 0;
            uint16_t field_offset = 0, field_flags = 0;
            std::string name, field_type_name;
            if (!Read(entry, name_address) || !Read(entry + 10, field_offset) || !Read(entry + 16, field_type) || !Name(name_address, name) || !TypeName(field_type, field_type_name) || !Read(field_type, field_info) || !Read(field_info + 8, field_flags)) return false;
            const bool boolean = field_type_name == "Boolean", float32 = field_type_name == "Float32";
            // Flags 0xc115 mark an enum, stored in 4 bytes. Other types are not read.
            const size_t width = boolean ? 1 : (float32 || field_type_name == "Int32" || field_type_name == "Uint32" || field_flags == 0xc115 ? 4 : 0);
            if (field_offset >= class_size || field_offset + width > class_size) return false;
            uint32_t bits = 0;
            if (width && !ReadBytes(object + field_offset, &bits, width)) return false;
            if ((boolean && bits > 1) || (float32 && !std::isfinite(std::bit_cast<float>(bits)))) return false;
            found = found || (boolean && name == field && field_offset == offset && size == object_size);
         }
         if (parent == type && owner == "DataContainer" && !count) break;
         type = parent;
      }
      uintptr_t final_type = 0;
      return Read(object + 8, final_type) && final_type == original_type && found;
   }

   inline bool SupportedBuild(uintptr_t& base)
   {
      base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
      return Battlefront2Executable::Running();
   }
   // The renderer's current world view (renderer -> view owner -> view).
   inline bool WorldView(uintptr_t base, uintptr_t& view)
   {
      uintptr_t renderer = 0, owner = 0, vt = 0;
      return Read(base + 0x3FFBE10, renderer) && Read(renderer + 0x7C0, owner) && Read(owner, vt) && vt == base + 0x2F79BE8 && Read(owner + 0x20, view) && Read(view, vt) && vt == base + 0x3014528;
   }
   // Whether `object` has this vtable and type info.
   inline bool IsA(uintptr_t object, uintptr_t vtable, uintptr_t type)
   {
      uintptr_t vt = 0, actual = 0;
      return Read(object, vt) && vt == vtable && Read(object + 8, actual) && actual == type;
   }
} // namespace Battlefront2NativeSettings

namespace Battlefront2SSR
{
   // The current world view's post-process asset (EA build 0x614AF6B8). Frostbite copies it into its two live
   // post-process states and selects its full-resolution raytrace/resolve/history shader variants itself.
   inline bool PostProcessAsset(uintptr_t base, uintptr_t& asset)
   {
      uintptr_t view = 0;
      return Battlefront2NativeSettings::WorldView(base, view) && Battlefront2NativeSettings::Read(view + 0x918, asset) &&
             Battlefront2NativeSettings::IsA(asset, base + 0x31074D8, base + 0x45B8140);
   }

   // Turns one Boolean of the post-process asset on while enabled and puts the engine's value back when disabled.
   // Each world has its own asset, so the original value is kept per asset.
   class NativeResolution
   {
      struct Record
      {
         uintptr_t asset;
         uint8_t original;
         bool owned = false;
      };
      std::vector<Record> records_;
      uintptr_t verified_ = 0;
      const char* field_name_;
      uint16_t field_offset_;

   public:
      explicit NativeResolution(const char* field_name = "ScreenSpaceRaytraceFullresEnable", uint16_t field_offset = 0x24D)
          : field_name_(field_name), field_offset_(field_offset)
      {
      }
      void Update(bool enabled)
      {
         using Battlefront2NativeSettings::Read, Battlefront2NativeSettings::Exchange;
         uintptr_t base = 0, asset = 0;
         if (!Battlefront2NativeSettings::SupportedBuild(base) || !PostProcessAsset(base, asset)) return;
         if (asset != verified_)
         {
            if (!Battlefront2NativeSettings::HasBoolean(asset, "GlobalPostProcessSettings", field_name_, field_offset_, 0x270)) return;
            verified_ = asset;
         }
         uint8_t current = 0;
         if (!Read(asset + field_offset_, current) || current > 1) return;
         auto found = std::find_if(records_.begin(), records_.end(), [asset](const Record& r)
            { return r.asset == asset; });
         if (found == records_.end())
         {
            records_.push_back({asset, current});
            found = records_.end() - 1;
         }
         // Only the asset the renderer holds right now is written; an old world's asset is never touched again.
         uintptr_t identity = 0;
         if (!PostProcessAsset(base, identity) || identity != asset) return;
         if (enabled)
         {
            if (!current && !Exchange(asset + field_offset_, 0, 1)) return;
            found->owned = current == 0 || found->owned;
         }
         else if (found->owned)
         {
            if (current == 1 && !Exchange(asset + field_offset_, 1, found->original)) return;
            found->owned = false;
         }
      }
   };
} // namespace Battlefront2SSR

namespace Battlefront2DepthDither
{
   inline std::atomic_bool enabled{true};
   // EA build 0x614AF6B8: selects Frostbite's conservative minimum-depth reduction instead of its alternating
   // min/max checkerboard, in the renderer's own WorldRenderSettings asset (not a copied preset).
   inline bool Asset(uintptr_t base, uintptr_t& asset)
   {
      using Battlefront2NativeSettings::Read;
      uintptr_t view = 0, global = 0;
      return Battlefront2NativeSettings::WorldView(base, view) && Read(view + 0x900, asset) && Read(base + 0x3D7B068, global) && global == asset &&
             Battlefront2NativeSettings::IsA(asset, base + 0x3012148, base + 0x4550C50);
   }
   class State
   {
      uintptr_t asset_ = 0;
      uint8_t original_ = 0;
      bool owned_ = false;

   public:
      void Update(bool disable)
      {
         using Battlefront2NativeSettings::Read, Battlefront2NativeSettings::Exchange;
         uintptr_t base = 0, asset = 0;
         if (!Battlefront2NativeSettings::SupportedBuild(base) || !Asset(base, asset)) return;
         if (asset != asset_)
         {
            uint8_t original = 0;
            if (!Battlefront2NativeSettings::HasBoolean(asset, "WorldRenderSettings", "HalfResDepthMinMaxDitherEnable", 0x1E7, 0x5D0) ||
                !Read(asset + 0x1E7, original) || original > 1) return;
            // A new world: the old asset is never dereferenced again.
            asset_ = asset;
            original_ = original;
            owned_ = false;
         }
         uint8_t current = 0;
         uintptr_t identity = 0;
         if (!Read(asset + 0x1E7, current) || current > 1 || !Asset(base, identity) || identity != asset) return;
         if (disable)
         {
            if (current && !Exchange(asset + 0x1E7, 1, 0)) return;
            owned_ = owned_ || current == 1;
         }
         else if (owned_)
         {
            if (!current && !Exchange(asset + 0x1E7, 0, original_)) return;
            owned_ = false;
         }
      }
   };
} // namespace Battlefront2DepthDither
