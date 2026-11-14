#pragma once
// DLSS in place of the game's TAA: game constant uploads, failure recovery, and the full resolution replays of the passes after TAA.

#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <smmintrin.h>
#include <windows.h>
#include <algorithm>
#include <emmintrin.h>
#include <immintrin.h>
#include <vector>
#include <atomic>
#include <cmath>
#include <limits>
#include <d3d11shader.h>
#include <array>
#include <span>
#include "../../External/WDK/includes/d3d11TokenizedProgramFormat.hpp"
#include "game_passes.h"

namespace Battlefront2Jitter
{
   // D3D11 WRITE_DISCARD mappings may be write-combined. Ordinary memcpy
   // reads from those mappings dominated the CPU profile. Read each entire
   // cache line with streaming loads; retain the complete upload because the
   // temporal constant-buffer offset is selected only after Unmap.
   inline void CopyMappedUpload(void* destination, const void* source, size_t size)
   {
      static const bool streaming_loads = []
      {
         int registers[4]{};
         __cpuid(registers, 1);
         return (registers[2] & (1 << 19)) != 0; // SSE4.1
      }();
      if (!streaming_loads || size < 64)
      {
         std::memcpy(destination, source, size);
         return;
      }
      auto* dst = static_cast<uint8_t*>(destination);
      const auto* src = static_cast<const uint8_t*>(source);
      // Order the producer's WC stores and invalidate old streaming buffers.
      _mm_mfence();
      const size_t prefix = (64 - (reinterpret_cast<uintptr_t>(src) & 63)) & 63;
      std::memcpy(dst, src, prefix);
      dst += prefix;
      src += prefix;
      size -= prefix;
      while (size >= 64)
      {
         const auto* line = reinterpret_cast<const __m128i*>(src);
         const __m128i a = _mm_stream_load_si128(line + 0);
         const __m128i b = _mm_stream_load_si128(line + 1);
         const __m128i c = _mm_stream_load_si128(line + 2);
         const __m128i d = _mm_stream_load_si128(line + 3);
         _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 0), a);
         _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 16), b);
         _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 32), c);
         _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + 48), d);
         src += 64;
         dst += 64;
         size -= 64;
      }
      std::memcpy(dst, src, size);
      _mm_mfence();
   }

   // Small changed runs dominate partially updated constants. Fixed-width
   // copies keep those runs inline and reuse each load for both destinations.
   // Every store stays inside the changed run, including unaligned/tail bytes.
   __forceinline void CopyChangedUploadRun(uint8_t* destination, const uint8_t* current,
      uint8_t* previous, size_t length)
   {
      while (length >= 8)
      {
         uint64_t value;
         std::memcpy(&value, current, 8);
         std::memcpy(destination, &value, 8);
         std::memcpy(previous, &value, 8);
         destination += 8;
         current += 8;
         previous += 8;
         length -= 8;
      }
      if (length >= 4)
      {
         uint32_t value;
         std::memcpy(&value, current, 4);
         std::memcpy(destination, &value, 4);
         std::memcpy(previous, &value, 4);
         destination += 4;
         current += 4;
         previous += 4;
         length -= 4;
      }
      if (length >= 2)
      {
         uint16_t value;
         std::memcpy(&value, current, 2);
         std::memcpy(destination, &value, 2);
         std::memcpy(previous, &value, 2);
         destination += 2;
         current += 2;
         previous += 2;
         length -= 2;
      }
      if (length) *destination = *previous = *current;
   }
   // Both inputs are cacheable CPU memory. The destination may be a write-
   // combined GPU mapping: never read it or store any unchanged neighbour.
   // Use the SIMD comparison mask to locate differing runs without comparing
   // the same partially changed vector again, byte by byte.
   inline void CopyChangedUploadBytesSSE2(void* mapped, const uint8_t* current,
      uint8_t* previous, size_t size)
   {
      auto* destination = static_cast<uint8_t*>(mapped);
      size_t i = 0;
      for (; i + 16 <= size; i += 16)
      {
         const auto value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(current + i));
         const auto prior = _mm_loadu_si128(reinterpret_cast<const __m128i*>(previous + i));
         unsigned long changed = static_cast<unsigned long>(
                                    ~_mm_movemask_epi8(_mm_cmpeq_epi8(value, prior))) &
                                 0xffffUL;
         if (changed == 0xffffUL)
         {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destination + i), value);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(previous + i), value);
            continue;
         }
         while (changed != 0)
         {
            unsigned long first, length;
            _BitScanForward(&first, changed);
            // Bits above the 16-byte mask are zero, so the complement always
            // has a set bit, including a run ending at the vector boundary.
            _BitScanForward(&length, ~(changed >> first));
            CopyChangedUploadRun(destination + i + first, current + i + first, previous + i + first, length);
            changed &= ~(((1UL << length) - 1) << first);
         }
      }
      while (i < size)
      {
         if (current[i] == previous[i])
         {
            ++i;
            continue;
         }
         const size_t first = i++;
         while (i < size && current[i] != previous[i])
            ++i;
         CopyChangedUploadRun(destination + first, current + first, previous + first, i - first);
      }
   }

   inline bool UploadAVX2Available()
   {
      static const bool available = []
      {
         int info[4]{};
         __cpuid(info, 0);
         if (info[0] < 7) return false;
         __cpuid(info, 1);
         constexpr int required = (1 << 27) | (1 << 28); // OSXSAVE and AVX
         if ((info[2] & required) != required || (_xgetbv(0) & 6) != 6) return false;
         __cpuidex(info, 7, 0);
         return (info[1] & (1 << 5)) != 0;
      }();
      return available;
   }

   // Keep AVX2 isolated from the fallback. The OS/CPU guard runs before entering
   // this function, without changing the project's baseline instruction set.
   __declspec(noinline) inline void CopyChangedUploadBytesAVX2(void* mapped,
      const uint8_t* current, uint8_t* previous, size_t size)
   {
      auto* destination = static_cast<uint8_t*>(mapped);
      size_t i = 0;
      for (; i + 32 <= size; i += 32)
      {
         const auto value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(current + i));
         const auto prior = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(previous + i));
         unsigned long changed = static_cast<uint32_t>(~_mm256_movemask_epi8(_mm256_cmpeq_epi8(value, prior)));
         if (changed == 0xffffffffUL)
         {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + i), value);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(previous + i), value);
            continue;
         }
         while (changed != 0)
         {
            unsigned long first, length;
            _BitScanForward(&first, changed);
            // A 32-byte run is handled above. Every partial mask has either
            // a zero inside its first run or a high zero after shifting it.
            _BitScanForward(&length, ~(changed >> first));
            CopyChangedUploadRun(destination + i + first, current + i + first, previous + i + first, length);
            changed &= ~(((1UL << length) - 1) << first);
         }
      }
      _mm256_zeroupper();
      CopyChangedUploadBytesSSE2(destination + i, current + i, previous + i, size - i);
   }

   inline void CopyChangedUploadBytes(void* mapped, const uint8_t* current,
      uint8_t* previous, size_t size)
   {
      if (size >= 32 && UploadAVX2Available())
         CopyChangedUploadBytesAVX2(mapped, current, previous, size);
      else
         CopyChangedUploadBytesSSE2(mapped, current, previous, size);
   }

   // Keep CPU producers in cacheable memory. WRITE_NO_OVERWRITE may update a
   // tiny range in a large arena thousands of times, so neither reading the
   // whole WC mapping nor copying the whole arena back is acceptable.
   class UploadShadow
   {
      uint8_t* data_ = nullptr;
      size_t size_ = 0;
      size_t page_size_ = 4096;
      std::vector<void*> dirty_;
      bool coherent_ = false;
      bool watch_clean_ = false;
      bool use_write_watch_ = false;

   public:
      UploadShadow() = default;
      UploadShadow(const UploadShadow&) = delete;
      UploadShadow& operator=(const UploadShadow&) = delete;
      ~UploadShadow()
      {
         if (data_) VirtualFree(data_, 0, MEM_RELEASE);
      }

      void Invalidate()
      {
         coherent_ = false;
         watch_clean_ = false;
      }
      void* Begin(size_t size, bool discard, const std::vector<uint8_t>& previous, bool previous_full)
      {
         if (size_ != size)
         {
            if (data_) VirtualFree(data_, 0, MEM_RELEASE);
            data_ = nullptr;
            size_ = size;
            coherent_ = false;
            watch_clean_ = false;
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            page_size_ = info.dwPageSize;
            // A full cached comparison of a single page costs less than two
            // kernel write-watch calls on thousands of small constant uploads.
            // Large arenas still use dirty-page tracking.
            use_write_watch_ = size > page_size_;
            dirty_.resize((size + page_size_ - 1) / page_size_);
            data_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, size,
               MEM_RESERVE | MEM_COMMIT | (use_write_watch_ ? MEM_WRITE_WATCH : 0), PAGE_READWRITE));
         }
         if (!data_) return nullptr;
         if (!coherent_ && !discard)
         {
            if (!previous_full || previous.size() != size) return nullptr;
            std::memcpy(data_, previous.data(), size);
            watch_clean_ = false;
         }
         // End(NO_OVERWRITE) already resets write-watch atomically. Reset only
         // after allocation, priming, DISCARD, or a tracking failure.
         if (use_write_watch_ && !watch_clean_ && ResetWriteWatch(data_, size_) != 0)
         {
            coherent_ = false;
            return nullptr;
         }
         watch_clean_ = false; // The producer is about to write.
         coherent_ = true;
         return data_;
      }

      void End(void* mapped, bool discard, std::vector<uint8_t>& previous)
      {
         previous.resize(size_);
         if (discard)
         {
            // Establish one coherent baseline for the new GPU allocation.
            // Later NO_OVERWRITE uploads may write the same value as an old
            // allocation, which a byte comparison would otherwise skip.
            std::memcpy(mapped, data_, size_);
            std::memcpy(previous.data(), data_, size_);
            return;
         }
         ULONG_PTR count = dirty_.size();
         DWORD granularity = 0;
         const bool tracked = use_write_watch_ && GetWriteWatch(WRITE_WATCH_FLAG_RESET, data_, size_,
                                                     dirty_.data(), &count, &granularity) == 0;
         watch_clean_ = tracked;
         // On tracking failure a cached-memory comparison remains correct.
         // Never fall back to reading the write-combined GPU mapping.
         if (!tracked) count = dirty_.size();
         for (size_t page = 0; page < count; ++page)
         {
            const size_t start = tracked ? static_cast<uint8_t*>(dirty_[page]) - data_ : page * page_size_;
            const size_t end = (std::min)(size_, start + page_size_);
            CopyChangedUploadBytes(static_cast<uint8_t*>(mapped) + start,
               data_ + start, previous.data() + start, end - start);
         }
      }
   };
} // namespace Battlefront2Jitter

// Decides at each present whether DLSS may run, from what that frame showed.
// After a failure it falls back to native rendering and retries later. It owns
// neither the user's DLSS setting nor any renderer memory.
namespace Battlefront2DLSSRecovery
{
   enum class Phase
   {
      Disabled,
      Learning,
      Active,
      NativeFallback
   };
   struct Input
   {
      uint64_t now_ms = 0; // Monotonic milliseconds, e.g. GetTickCount64().
      bool requested = false;
      // A rendered native scene candidate, not simply Present/loading progress.
      bool scene_candidate = false;
      // Supported native temporal producer linked to the final scene output.
      bool native_route_ready = false;
      bool evaluated = false;
      bool consumed = false; // In the same present as evaluated.
      // A real display/layout change. Not our own scale write, the engine's
      // resource ring rotation or a failed route observation.
      uint64_t layout_generation = 0;
      bool native_scale_restored = false;
      bool unsupported_packer = false; // A main route was seen, but with unsupported effects.
      // A native temporal dispatch counts as a scene even before the final
      // scene's route is known, so a missing route is not taken for a menu.
      bool temporal_observed = false;
   };
   struct Decision
   {
      Phase phase = Phase::Disabled;
      bool override_scale = false;
      bool attempt_started = false;
      bool returned_to_native = false;
      bool waiting_for_scene = false;
   };
   class State
   {
      static constexpr uint64_t learning_min_ms = 2000;
      static constexpr uint64_t active_failure_min_ms = 3000;
      static constexpr uint64_t hard_failure_ms = 8000;
      static constexpr uint32_t scene_grace_presents = 180;
      // The wait before an automatic retry doubles after every attempt that
      // fails without a sustained healthy run in between, up to the maximum:
      // a scene DLSS cannot serve is probed ever more rarely, but never given up on.
      static constexpr uint64_t retry_cooldown_ms = 5000;
      static constexpr uint64_t retry_cooldown_max_ms = 300000;
      static constexpr uint64_t retry_stable_route_ms = 1000;
      static constexpr uint32_t retry_stable_route_presents = 30;
      static constexpr uint64_t healthy_reset_ms = 10000;
      static constexpr uint32_t healthy_reset_presents = 180;

      Phase phase_ = Phase::Disabled;
      bool requested_ = false;
      std::atomic_bool explicit_retry_{false};
      bool clock_valid_ = false, failed_frame_seen_ = false, route_stable_ = false, healthy_ = false;
      bool scene_observed_ = false;
      uint64_t scene_clock_ = 0;
      uint64_t clock_ = 0, layout_ = 0, failure_start_ = 0, fallback_start_ = 0, stable_start_ = 0, healthy_start_ = 0;
      uint32_t failed_scene_presents_ = 0, retries_used_ = 0, stable_presents_ = 0, healthy_presents_ = 0;

      static void Increment(uint32_t& count)
      {
         if (count != (std::numeric_limits<uint32_t>::max)()) ++count;
      }
      void Begin()
      {
         phase_ = Phase::Learning;
         failure_start_ = scene_clock_;
         failed_frame_seen_ = true;
         failed_scene_presents_ = 0;
         route_stable_ = healthy_ = false;
         stable_presents_ = healthy_presents_ = 0;
      }
      uint64_t RetryCooldown() const
      {
         uint64_t cooldown = retry_cooldown_ms;
         for (uint32_t i = 0; i < retries_used_ && cooldown < retry_cooldown_max_ms; ++i)
            cooldown *= 2;
         return cooldown < retry_cooldown_max_ms ? cooldown : retry_cooldown_max_ms;
      }
      Decision Result(bool started = false, bool native = false) const
      {
         const bool allowed = AllowsEvaluation();
         return {phase_, allowed, started, native, allowed && !scene_observed_};
      }

   public:
      // For mode/preset changes and the user's retry; resets the retry backoff.
      // Renderer-internal resets must not call it.
      void RequestRetry()
      {
         explicit_retry_.store(true, std::memory_order_release);
      }
      bool AllowsEvaluation() const
      {
         return phase_ == Phase::Learning || phase_ == Phase::Active;
      }

      Decision Tick(const Input& input)
      {
         const uint64_t now = clock_valid_ && input.now_ms < clock_ ? clock_ : input.now_ms; // Never step back in time.
         const bool scene_observed = input.scene_candidate || input.temporal_observed || input.native_route_ready || input.evaluated || input.consumed || input.unsupported_packer;
         // Menus and loading screens can present for minutes without a scene.
         // The deadlines only count time that follows a scene present, so idle
         // gaps neither expire nor reset an accumulating failure.
         if (clock_valid_ && scene_observed_) scene_clock_ += now - clock_;
         scene_observed_ = scene_observed;
         clock_ = now;
         clock_valid_ = true;
         const bool layout_changed = requested_ && layout_ != input.layout_generation;
         layout_ = input.layout_generation;
         if (!input.requested)
         {
            const bool restoring = AllowsEvaluation();
            requested_ = false;
            explicit_retry_.store(false, std::memory_order_release);
            phase_ = Phase::Disabled;
            failed_frame_seen_ = route_stable_ = healthy_ = false;
            retries_used_ = 0;
            return Result(false, restoring);
         }
         const bool retry_requested = explicit_retry_.exchange(false, std::memory_order_acq_rel);
         if (!requested_ || retry_requested)
         {
            requested_ = true;
            retries_used_ = 0;
            Begin();
            // An evaluation from an earlier frame does not validate a new mode.
            return Result(true);
         }
         if (layout_changed)
         {
            route_stable_ = healthy_ = false;
            // A resize restarts an active run. It does not extend Learning's
            // deadline, and a fallback is rearmed only by a stable supported
            // route or the user, not by a resize feedback loop.
            if (phase_ == Phase::Active)
            {
               Begin();
               return Result(true);
            }
         }
         if (phase_ == Phase::NativeFallback)
         {
            const bool eligible = input.native_scale_restored && input.scene_candidate && input.native_route_ready;
            if (!eligible)
            {
               route_stable_ = false;
               stable_presents_ = 0;
            }
            else
            {
               if (!route_stable_)
               {
                  route_stable_ = true;
                  stable_start_ = scene_clock_;
                  stable_presents_ = 0;
               }
               Increment(stable_presents_);
            }
            if (eligible && route_stable_ && now - fallback_start_ >= RetryCooldown() && scene_clock_ - stable_start_ >= retry_stable_route_ms && stable_presents_ >= retry_stable_route_presents)
            {
               Increment(retries_used_);
               Begin();
               return Result(true);
            }
            return Result();
         }
         if (!scene_observed) return Result();
         if (input.evaluated && input.consumed && !layout_changed)
         {
            phase_ = Phase::Active;
            failed_frame_seen_ = false;
            failed_scene_presents_ = 0;
            if (!healthy_)
            {
               healthy_ = true;
               healthy_start_ = scene_clock_;
               healthy_presents_ = 0;
            }
            Increment(healthy_presents_);
            // Only a sustained healthy run resets the backoff, not one success.
            if (scene_clock_ - healthy_start_ >= healthy_reset_ms && healthy_presents_ >= healthy_reset_presents)
               retries_used_ = 0;
            return Result();
         }
         healthy_ = false;
         if (!failed_frame_seen_)
         {
            failed_frame_seen_ = true;
            failure_start_ = scene_clock_;
            failed_scene_presents_ = 0;
         }
         if (input.scene_candidate) Increment(failed_scene_presents_);
         const uint64_t elapsed = scene_clock_ - failure_start_;
         const uint64_t minimum = phase_ == Phase::Learning ? learning_min_ms : active_failure_min_ms;
         if (elapsed >= hard_failure_ms || (elapsed >= minimum && failed_scene_presents_ >= scene_grace_presents))
         {
            phase_ = Phase::NativeFallback;
            fallback_start_ = now;
            route_stable_ = false;
            return Result(false, true);
         }
         return Result();
      }
   };

   // Owns the game's render scale setting only after its own write succeeded,
   // and restores the original value (native supersampling included) on
   // fallback or Off. If the settings object was replaced or the value changed
   // since, it belongs to the game or the user again and is left alone.
   // No pointers are dereferenced here.
   class ScaleLease
   {
   public:
      struct Plan
      {
         bool write = false, restore = false;
         uint64_t settings = 0;
         float target = 0.f, original = 0.f;
      };

   private:
      bool owned_ = false;
      uint64_t settings_ = 0;
      float original_ = 1.f, applied_ = 1.f;
      static bool Valid(float value)
      {
         return std::isfinite(value) && value >= .25f && value <= 2.f;
      }

   public:
      bool Owned() const
      {
         return owned_;
      }
      float Original() const
      {
         return original_;
      }
      Plan Prepare(uint64_t settings, float observed, bool override_scale, float target)
      {
         if (!settings || !Valid(observed)) return {};
         if (owned_ && (settings != settings_ || observed != applied_)) owned_ = false;
         if (!override_scale)
         {
            if (!owned_) return {};
            return {true, true, settings, original_, original_};
         }
         if (!Valid(target) || target == observed) return {};
         return {true, false, settings, target, owned_ ? original_ : observed};
      }
      // Call right after attempting the plan's write. A failed write neither
      // takes nor gives up ownership.
      void Complete(const Plan& plan, bool written)
      {
         if (!plan.write || !written) return;
         if (plan.restore)
         {
            owned_ = false;
            return;
         }
         owned_ = true;
         settings_ = plan.settings;
         original_ = plan.original;
         applied_ = plan.target;
      }
   };
} // namespace Battlefront2DLSSRecovery

// Recognises a scene packing pass that has no recipe yet (its pixel shader reads the scene and the 3D color grading
// LUT), so DLSS stays off for it.
namespace Battlefront2ScenePackerAudit
{
   // True for an unsupported scene packing pixel shader. Each hash is inspected once.
   inline bool Observe(uint32_t hash)
   {
      static thread_local std::unordered_map<uint32_t, bool> inspected;
      if (auto it = inspected.find(hash); it != inspected.end()) return it->second;
      if (inspected.size() >= 256) return false;
      inspected.emplace(hash, false);
      std::vector<uint8_t> bytes;
      {
         const std::lock_guard<std::recursive_mutex> lock(s_mutex_dumping);
         const auto entry = shader_cache.find(hash);
         if (entry == shader_cache.end() || !entry->second->data || !entry->second->size) return false;
         const auto* source = static_cast<const uint8_t*>(entry->second->data);
         bytes.assign(source, source + entry->second->size);
      }
      com_ptr<ID3D11ShaderReflection> reflection;
      if (!Shader::d3d_reflect || FAILED(Shader::d3d_reflect(bytes.data(), bytes.size(), IID_PPV_ARGS(&reflection)))) return false;
      D3D11_SHADER_INPUT_BIND_DESC scene{}, grade{};
      const bool found = SUCCEEDED(reflection->GetResourceBindingDescByName("mainTexture2", &scene)) || SUCCEEDED(reflection->GetResourceBindingDescByName("mainTexture", &scene));
      if (!found || FAILED(reflection->GetResourceBindingDescByName("colorGradingTexture", &grade))) return false;
      if (scene.Type != D3D_SIT_TEXTURE || scene.BindPoint != 0 || scene.BindCount != 1 || scene.Dimension != D3D_SRV_DIMENSION_TEXTURE2D) return false;
      if (grade.Type != D3D_SIT_TEXTURE || grade.BindPoint != 1 || grade.BindCount != 1 || grade.Dimension != D3D_SRV_DIMENSION_TEXTURE3D) return false;
      inspected[hash] = true;
      return true;
   }
} // namespace Battlefront2ScenePackerAudit

namespace Battlefront2ScenePacker
{
   // The game's scene packing passes. One recipe drives shader compilation,
   // selection and input checks. Bits match Includes/SceneFeatures.hlsl.
   enum Feature : uint32_t
   {
      Distortion = 1,
      Grain = 2,
      Dof = 4,
      Chromatic = 8,
      Lens = 16
   };
   enum class Path : uint32_t
   {
      Psycho = 1,
      NativePost = 2,
      DLSS = 3
   };
   struct Recipe
   {
      uint64_t hash;
      uint32_t features;
      constexpr bool Has(Feature feature) const
      {
         return (features & feature) != 0;
      }
      // t0-t1, then distortion (if any), bloom, grain (if any) and DOF (if any).
      constexpr uint32_t RequiredInputs() const
      {
         return 3 + uint32_t(Has(Distortion)) + uint32_t(Has(Grain)) + uint32_t(Has(Dof));
      }
      constexpr uint32_t ShaderKey(Path path) const
      {
         return 0xBF210000u | (uint32_t(path) << 8) | features;
      }
   };
   inline constexpr std::array<Recipe, 11> Recipes{{
      {0x71CBBDFD, 0},
      {0x5C57BEF0, Grain},
      {0x848FD892, Distortion},
      {0x7DE1009D, Dof},
      {0xC1AD9344, Distortion | Dof},
      {0xC3825BD6, Chromatic | Distortion},
      {0xB26EE036, Lens | Distortion | Grain | Dof},
      {0x1A6248F2, Lens | Grain | Dof},
      {0x8F3DCDB2, Chromatic},
      {0xAACFCEC0, Lens | Chromatic | Grain},
      {0x9490D850, Lens | Chromatic | Distortion | Grain},
   }};
   constexpr const Recipe* Find(uint64_t hash)
   {
      for (const auto& recipe : Recipes)
         if (recipe.hash == hash) return &recipe;
      return nullptr;
   }
   constexpr bool Supported(uint64_t hash)
   {
      return Find(hash) != nullptr;
   }
   constexpr bool Distorted(uint64_t hash)
   {
      const auto* recipe = Find(hash);
      return recipe && recipe->Has(Distortion);
   }
   constexpr bool Ironsights(uint64_t hash)
   {
      const auto* recipe = Find(hash);
      return recipe && recipe->Has(Dof);
   }
} // namespace Battlefront2ScenePacker

namespace Battlefront2PostShader
{
   // Patches a copy of a game pixel shader for the private full-resolution
   // replay. cb13: native/output pixel scale, inverse native extent. s15:
   // linear clamp.
   struct Operand
   {
      size_t begin = 0, end = 0;
      uint32_t type = 0, index = ~0u;
   };
   inline bool ReadOperand(std::span<const uint32_t> code, size_t& cursor, Operand& out, unsigned nesting = 0)
   {
      if (cursor >= code.size() || nesting > 8) return false;
      out.begin = cursor;
      const uint32_t token = code[cursor++];
      out.type = DECODE_D3D10_SB_OPERAND_TYPE(token);
      uint32_t extended = token;
      while (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(extended))
      {
         if (cursor >= code.size()) return false;
         extended = code[cursor++];
      }
      if (out.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || out.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64)
      {
         const auto components = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token);
         if (components != D3D10_SB_OPERAND_1_COMPONENT && components != D3D10_SB_OPERAND_4_COMPONENT) return false;
         cursor += (components == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 1) * (out.type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64 ? 2 : 1);
      }
      else
      {
         const unsigned dims = DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token);
         for (unsigned d = 0; d < dims; ++d)
         {
            const auto representation = DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(d, token);
            if (representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE)
            {
               if (cursor >= code.size()) return false;
               if (d == 0 && representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32) out.index = code[cursor];
               ++cursor;
            }
            else if (representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE64 || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE64_PLUS_RELATIVE)
               cursor += 2;
            else if (representation != D3D10_SB_OPERAND_INDEX_RELATIVE)
               return false;
            if (representation == D3D10_SB_OPERAND_INDEX_RELATIVE || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE || representation == D3D10_SB_OPERAND_INDEX_IMMEDIATE64_PLUS_RELATIVE)
            {
               Operand relative;
               if (!ReadOperand(code, cursor, relative, nesting + 1)) return false;
            }
         }
      }
      out.end = cursor;
      return cursor <= code.size();
   }
   inline uint32_t Reg(uint32_t type, bool destination = false, uint32_t swizzle = 0xe4)
   {
      return 2u | (destination ? 0xf0u : (1u << 2) | (swizzle << 4)) | (type << 12) | (1u << 20);
   }
   inline std::vector<uint32_t> CB(unsigned vector, uint32_t swizzle = 0xe4)
   {
      return {2u | (1u << 2) | (swizzle << 4) | (D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER << 12) | (2u << 20), 13, vector};
   }
   inline void Emit(std::vector<uint32_t>& out, uint32_t opcode, std::initializer_list<std::vector<uint32_t>> operands)
   {
      size_t count = 1;
      for (const auto& op : operands)
         count += op.size();
      out.push_back(opcode | (uint32_t(count) << 24));
      for (const auto& op : operands)
         out.insert(out.end(), op.begin(), op.end());
   }
   // The replay does not support dynamic shader linkage.
   inline bool DynamicLinkage(uint32_t opcode)
   {
      return opcode == D3D11_SB_OPCODE_DCL_INTERFACE || opcode == D3D11_SB_OPCODE_DCL_FUNCTION_BODY || opcode == D3D11_SB_OPCODE_DCL_FUNCTION_TABLE || opcode == D3D11_SB_OPCODE_INTERFACE_CALL;
   }
   // Length of an already validated declaration.
   inline size_t DeclarationLength(const std::vector<uint32_t>& code, size_t at)
   {
      return DECODE_D3D10_SB_OPCODE_TYPE(code[at]) == D3D10_SB_OPCODE_CUSTOMDATA ? code[at + 1] : DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(code[at]);
   }
   // A position scaled by literals of at most 1/16 tiles a small texture (noise) over the screen.
   inline bool TilingLiteral(std::span<const uint32_t> instruction, const Operand& destination, const Operand& literal)
   {
      if (literal.type != D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || literal.end - literal.begin != 5 || DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(instruction[destination.begin]) != D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) return false;
      const uint32_t mask = DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(instruction[destination.begin]);
      for (uint32_t c = 0; c < 4; ++c)
      {
         float value;
         std::memcpy(&value, &instruction[literal.begin + 1 + c], 4);
         if ((mask & (0x10u << c)) && !(std::fabs(value) > 0.f && std::fabs(value) <= 1.f / 16.f)) return false;
      }
      return mask != 0;
   }
   // Returns the rewritten SHEX/SHDR program, two-word header included, for a draw replayed at output resolution.
   // Position.xy stays in render pixels, so sizes in the shader keep their meaning; what the shader reads there
   // follows the output pixel instead of repeating one value per render pixel:
   // - integer loads of the mirrored scene, of the rebuilt DOF gathers (t12, t14) with `dof_gather` and of the
   //   `upsampled` inputs (t0-t15, bound at output size) become filtered samples;
   // - a DOF composite (the scene at t0) rounds the position to its texel's centre to read the blur amount: it
   //   keeps the pixel's own position, for the blur amount bound at output size;
   // - a motion blur composite (the scene at t1) tiles its noise over the position: that keeps output pixels.
   inline std::vector<uint32_t> Rewrite(std::span<const uint32_t> program, int scene_slot, bool dof_gather = false, uint32_t upsampled = 0)
   {
      if (program.size() < 3 || program[1] != program.size() || (program[0] >> 16) != 0 || (program[0] & 0xff) != 0x50) return {};
      uint32_t position = ~0u, position_mask = 0, temps = ~0u;
      size_t body = 2;
      while (body < program.size())
      {
         const auto opcode = DECODE_D3D10_SB_OPCODE_TYPE(program[body]);
         if (DynamicLinkage(opcode)) return {};
         size_t length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(program[body]);
         if (opcode == D3D10_SB_OPCODE_CUSTOMDATA)
         {
            if (body + 1 >= program.size()) return {};
            length = program[body + 1];
         }
         if (!length || length > program.size() - body) return {};
         const bool declaration = (opcode >= D3D10_SB_OPCODE_DCL_RESOURCE && opcode <= D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS) || opcode == D3D10_SB_OPCODE_CUSTOMDATA || opcode == D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED || opcode == D3D11_SB_OPCODE_DCL_RESOURCE_RAW;
         if (!declaration) break;
         if (opcode == D3D10_SB_OPCODE_DCL_TEMPS)
         {
            if (length != 2) return {};
            temps = program[body + 1];
         }
         if (opcode == D3D10_SB_OPCODE_DCL_INPUT_PS_SIV && length == 4 && program[body + 3] == D3D10_SB_NAME_POSITION)
         {
            position = program[body + 2];
            position_mask = program[body + 1] & 0xf0u;
         }
         if ((opcode == D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER || opcode == D3D10_SB_OPCODE_DCL_SAMPLER) && length >= 3 && program[body + 2] == (opcode == D3D10_SB_OPCODE_DCL_SAMPLER ? 15u : 13u)) return {};
         body += length;
      }
      const bool has_temps = temps != ~0u;
      if (!has_temps) temps = 0;
      // temps: the position in render pixels; +1: its fraction; +2: scratch; +3: the position minus half a pixel.
      const uint32_t extra_temps = 4;
      if (temps > 4096 - extra_temps) return {};
      std::vector<uint32_t> out(program.begin(), program.begin() + body);
      // The foreground gather (t14) is a native-size structured buffer read
      // only at the current pixel. Its private replacement is a full-size
      // texture, declared by copying the float4 t12 declaration.
      std::vector<uint32_t> gather_declaration;
      if (dof_gather)
         for (size_t p = 2; p < body;)
         {
            const auto opcode = DECODE_D3D10_SB_OPCODE_TYPE(out[p]);
            const size_t length = DeclarationLength(out, p);
            if (opcode == D3D10_SB_OPCODE_DCL_RESOURCE && length == 4 && out[p + 2] == 12)
               gather_declaration.assign(out.begin() + p, out.begin() + p + length);
            p += length;
         }
      for (size_t p = 2; p < body;)
      {
         const auto opcode = DECODE_D3D10_SB_OPCODE_TYPE(out[p]);
         if (opcode == D3D10_SB_OPCODE_DCL_TEMPS) out[p + 1] = temps + extra_temps;
         if (dof_gather && opcode == D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED && out[p + 2] == 14)
         {
            if (gather_declaration.size() != 4 || DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(out[p]) != 4 || out[p + 3] != 16) return {};
            std::copy(gather_declaration.begin(), gather_declaration.end(), out.begin() + p);
            out[p + 2] = 14;
         }
         p += DeclarationLength(out, p);
      }
      if (dof_gather && gather_declaration.empty()) return {};
      if (!has_temps) out.insert(out.end(), {D3D10_SB_OPCODE_DCL_TEMPS | (2u << 24), temps + extra_temps});
      Emit(out, D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {CB(2)});
      if (scene_slot >= 0 || upsampled) Emit(out, D3D10_SB_OPCODE_DCL_SAMPLER, {{Reg(D3D10_SB_OPERAND_TYPE_SAMPLER) & ~0xfffu, 15}});
      if (position != ~0u)
      {
         Emit(out, D3D10_SB_OPCODE_MOV, {{Reg(D3D10_SB_OPERAND_TYPE_TEMP, true), temps}, {0x4002, 0, 0, 0, 0}});
         uint32_t swizzle = 0, first = 0;
         while (first < 4 && !(position_mask & (0x10u << first)))
            ++first;
         if (first == 4) return {};
         for (uint32_t c = 0; c < 4; ++c)
            swizzle |= ((position_mask & (0x10u << c)) ? c : first) << (c * 2);
         Emit(out, D3D10_SB_OPCODE_MUL, {{(Reg(D3D10_SB_OPERAND_TYPE_TEMP, true) & ~0xf0u) | position_mask, temps}, {Reg(D3D10_SB_OPERAND_TYPE_INPUT, false, swizzle), position}, CB(0)});
         Emit(out, D3D10_SB_OPCODE_FRC, {{Reg(D3D10_SB_OPERAND_TYPE_TEMP, true), temps + 1}, {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps}});
         Emit(out, D3D10_SB_OPCODE_ADD, {{Reg(D3D10_SB_OPERAND_TYPE_TEMP, true), temps + 3}, {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps}, {0x4002, 0xbf000000, 0xbf000000, 0, 0}});
      }
      for (size_t p = body; p < program.size();)
      {
         const auto opcode = DECODE_D3D10_SB_OPCODE_TYPE(program[p]);
         if (DynamicLinkage(opcode)) return {};
         const size_t length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(program[p]);
         if (!length || length > program.size() - p || opcode == D3D10_SB_OPCODE_CUSTOMDATA) return {};
         auto instruction = std::vector<uint32_t>(program.begin() + p, program.begin() + p + length);
         size_t cursor = 1;
         while (cursor < length && DECODE_IS_D3D10_SB_OPCODE_EXTENDED(instruction[cursor - 1]))
            ++cursor;
         std::vector<Operand> operands;
         while (cursor < length)
         {
            Operand op;
            if (!ReadOperand(instruction, cursor, op)) return {};
            operands.push_back(op);
         }
         const auto is_position = [&](const Operand& op)
         { return position != ~0u && op.type == D3D10_SB_OPERAND_TYPE_INPUT && op.index == position; };
         uint32_t replacement = temps;
         bool output_pixels = false;
         if (scene_slot == 0 && (opcode == D3D10_SB_OPCODE_ROUND_Z || opcode == D3D10_SB_OPCODE_ROUND_NI || opcode == D3D10_SB_OPCODE_ROUND_NE || opcode == D3D10_SB_OPCODE_ROUND_PI) && operands.size() == 2 && is_position(operands[1]))
         {
            // The composite adds the half pixel back.
            instruction[0] = (instruction[0] & ~D3D10_SB_OPCODE_TYPE_MASK) | D3D10_SB_OPCODE_MOV;
            replacement = temps + 3;
         }
         else if (scene_slot == 1 && ((opcode == D3D10_SB_OPCODE_MUL && operands.size() == 3) || (opcode == D3D10_SB_OPCODE_MAD && operands.size() == 4)))
            output_pixels = (is_position(operands[1]) && TilingLiteral(instruction, operands[0], operands[2])) || (is_position(operands[2]) && TilingLiteral(instruction, operands[0], operands[1]));
         if (!output_pixels)
            for (const auto& op : operands)
               if (is_position(op))
               {
                  instruction[op.begin] = (instruction[op.begin] & ~D3D10_SB_OPERAND_TYPE_MASK) | (D3D10_SB_OPERAND_TYPE_TEMP << 12);
                  instruction[op.end - 1] = replacement;
               }
         const auto slice = [&](const Operand& op)
         { return std::vector<uint32_t>(instruction.begin() + op.begin, instruction.begin() + op.end); };
         if (opcode == D3D10_SB_OPCODE_LD && operands.size() == 3 && operands[2].type == D3D10_SB_OPERAND_TYPE_RESOURCE && (operands[2].index == uint32_t(scene_slot) || (dof_gather && operands[2].index == 12) || (operands[2].index < 16 && (upsampled >> operands[2].index & 1))))
         {
            if (position == ~0u) return {};
            Emit(out, D3D10_SB_OPCODE_ITOF, {{Reg(D3D10_SB_OPERAND_TYPE_TEMP, true), temps + 2}, slice(operands[1])});
            Emit(out, D3D10_SB_OPCODE_ADD, {{Reg(D3D10_SB_OPERAND_TYPE_TEMP, true), temps + 2}, {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps + 2}, {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps + 1}});
            Emit(out, D3D10_SB_OPCODE_MUL, {{Reg(D3D10_SB_OPERAND_TYPE_TEMP, true), temps + 2}, {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps + 2}, CB(1)});
            Emit(out, D3D10_SB_OPCODE_SAMPLE_L, {slice(operands[0]), {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps + 2}, slice(operands[2]), {Reg(D3D10_SB_OPERAND_TYPE_SAMPLER) & ~0xfffu, 15}, {D3D10_SB_OPERAND_TYPE_IMMEDIATE32 << 12 | 1u, 0}});
         }
         else if (dof_gather && opcode == D3D11_SB_OPCODE_LD_STRUCTURED && operands.size() == 4 && operands[3].type == D3D10_SB_OPERAND_TYPE_RESOURCE && operands[3].index == 14)
         {
            if (position == ~0u || operands[2].type != D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || instruction[operands[2].begin + 1] != 0 || operands[3].end - operands[3].begin != 2) return {};
            Emit(out, D3D10_SB_OPCODE_MUL, {{Reg(D3D10_SB_OPERAND_TYPE_TEMP, true), temps + 2}, {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps}, CB(1)});
            Emit(out, D3D10_SB_OPCODE_SAMPLE_L, {slice(operands[0]), {Reg(D3D10_SB_OPERAND_TYPE_TEMP), temps + 2}, slice(operands[3]), {Reg(D3D10_SB_OPERAND_TYPE_SAMPLER) & ~0xfffu, 15}, {D3D10_SB_OPERAND_TYPE_IMMEDIATE32 << 12 | 1u, 0}});
         }
         else
            out.insert(out.end(), instruction.begin(), instruction.end());
         p += length;
      }
      out[1] = uint32_t(out.size());
      return out;
   }
   // Rebuilds the container; the caller recomputes the DXBC checksum.
   inline std::vector<uint8_t> Patch(std::span<const uint8_t> blob, int scene_slot, bool dof_gather = false, uint32_t upsampled = 0)
   {
      if (blob.size() < 32 || std::memcmp(blob.data(), "DXBC", 4)) return {};
      const auto word = [&](size_t offset)
      {uint32_t v; std::memcpy(&v, blob.data() + offset, 4); return v; };
      const uint32_t count = word(28);
      if (word(24) != blob.size() || count > (blob.size() - 32) / 4) return {};
      std::vector<uint8_t> out(blob.begin(), blob.begin() + 32 + size_t(count) * 4);
      bool rewritten = false;
      for (uint32_t i = 0; i < count; ++i)
      {
         const size_t offset = word(32 + i * 4);
         if (offset > blob.size() - 8) return {};
         const size_t size = word(offset + 4);
         if (size > blob.size() - offset - 8) return {};
         const uint32_t new_offset = uint32_t(out.size());
         std::memcpy(out.data() + 32 + i * 4, &new_offset, 4);
         if (!std::memcmp(blob.data() + offset, "SHEX", 4) || !std::memcmp(blob.data() + offset, "SHDR", 4))
         {
            if (rewritten || size % 4) return {};
            std::vector<uint32_t> words(size / 4);
            std::memcpy(words.data(), blob.data() + offset + 8, size);
            auto program = Rewrite(words, scene_slot, dof_gather, upsampled);
            if (program.empty()) return {};
            out.insert(out.end(), blob.begin() + offset, blob.begin() + offset + 8);
            const uint32_t bytes = uint32_t(program.size() * 4);
            std::memcpy(out.data() + new_offset + 4, &bytes, 4);
            const auto* data = reinterpret_cast<const uint8_t*>(program.data());
            out.insert(out.end(), data, data + bytes);
            rewritten = true;
         }
         else
            out.insert(out.end(), blob.begin() + offset, blob.begin() + offset + 8 + size);
      }
      if (!rewritten) return {};
      const uint32_t size = uint32_t(out.size());
      std::memcpy(out.data() + 24, &size, 4);
      return out;
   }
} // namespace Battlefront2PostShader

namespace Battlefront2DOF
{
   struct Kernel
   {
      uint32_t hash;
      bool vertical, foreground;
   };
   // The game's depth-of-field gather shaders that Luma_SWBF2_DOFGather reproduces.
   inline constexpr std::array<Kernel, 32> kernels = {{{0x0DE3630F, 0, 0}, {0x19124EC3, 0, 0}, {0x3550F1E1, 0, 0}, {0x3C433A8F, 0, 0},
      {0x54D53155, 0, 0}, {0x648559DC, 0, 0}, {0x99140888, 0, 0}, {0xA2D116A9, 0, 0},
      {0x054E7557, 0, 1}, {0x2A1D3CD9, 0, 1}, {0x327F60CD, 0, 1}, {0x49E61E36, 0, 1},
      {0x7E9B0433, 0, 1}, {0x87838E27, 0, 1}, {0xC5312A09, 0, 1}, {0xE526D1CB, 0, 1},
      {0x0A2480F2, 1, 0}, {0x1348BFF4, 1, 0}, {0x503AD9EA, 1, 0}, {0x6851AF5A, 1, 0},
      {0x920875F6, 1, 0}, {0x9B181230, 1, 0}, {0xBDF6A6EC, 1, 0}, {0xF155C656, 1, 0},
      {0x026279BC, 1, 1}, {0x02CC414E, 1, 1}, {0x0D548408, 1, 1}, {0x1FCD9937, 1, 1},
      {0x27B07032, 1, 1}, {0x547C9F64, 1, 1}, {0x9DCD9AE9, 1, 1}, {0xE1F2EDCB, 1, 1}}};
   constexpr const Kernel* FindKernel(uint64_t hash)
   {
      for (const auto& kernel : kernels)
         if (kernel.hash == hash) return &kernel;
      return nullptr;
   }
   constexpr uint32_t ShaderKey(bool vertical, bool foreground)
   {
      return static_cast<uint32_t>(foreground ? (vertical ? CompileTimeStringHash("Battlefront II DOF Near V CS") : CompileTimeStringHash("Battlefront II DOF Near H CS"))
                                              : (vertical ? CompileTimeStringHash("Battlefront II DOF V CS") : CompileTimeStringHash("Battlefront II DOF H CS")));
   }
   // 0: native bokeh layers only; 1: gather; 3: gather plus foreground gather.
   constexpr unsigned CompositeInputs(uint64_t hash)
   {
      switch (hash)
      {
      case 0x00670380:
      case 0x2F2E3DFE:
      case 0x431CDB02:
      case 0x9267D341:
      case 0x9549CC61:
      case 0x96D4B425:
      case 0x9D7D83E4:
      case 0xA25D2014:
      case 0xD5EAC2C5:
      case 0xD9433742:
      case 0xEB58CEBD:
      case 0xF9823524:
         return 3;
      case 0x113D1C87:
      case 0x1775EB92:
      case 0x24DFBCB4:
      case 0x357B259D:
      case 0x4831636F:
      case 0x6F2D7A75:
      case 0x7CE3D155:
      case 0x98990B1A:
      case 0xB49FACA5:
      case 0xCE5A1BFE:
      case 0xEA66CA99:
      case 0xEFE8A8FE:
         return 1;
      default:
         return 0;
      }
   }
   struct Image
   {
      com_ptr<ID3D11Texture2D> texture;
      com_ptr<ID3D11ShaderResourceView> srv;
      com_ptr<ID3D11UnorderedAccessView> uav;
      bool Ensure(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT)
      {
         D3D11_TEXTURE2D_DESC d{};
         if (texture) texture->GetDesc(&d);
         if (texture && srv && uav && d.Width == width && d.Height == height && d.Format == format) return true;
         *this = {};
         d = {};
         d.Width = width;
         d.Height = height;
         d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
         d.Format = format;
         d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
         return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &texture)) && SUCCEEDED(device->CreateShaderResourceView(texture.get(), nullptr, &srv)) && SUCCEEDED(device->CreateUnorderedAccessView(texture.get(), nullptr, &uav));
      }
   };
   struct State
   {
      std::array<Image, 2> color, near_color;
      com_ptr<ID3D11SamplerState> border;
      com_ptr<ID3D11Resource> scene, coc, horizontal, vertical, near_horizontal, near_vertical;
      uint64_t horizontal_frame = ~uint64_t(0), vertical_frame = ~uint64_t(0);
      bool has_foreground = false;
      void Begin()
      {
         horizontal_frame = vertical_frame = ~uint64_t(0);
         scene = coc = horizontal = vertical = near_horizontal = near_vertical = nullptr;
      }
      void InvalidateWrite(ID3D11Resource* resource)
      {
         if (resource == horizontal.get() || resource == near_horizontal.get() || resource == coc.get())
            horizontal_frame = vertical_frame = ~uint64_t(0);
         if (resource == vertical.get() || resource == near_vertical.get()) vertical_frame = ~uint64_t(0);
      }
      static bool TextureView(ID3D11ShaderResourceView* view, UINT width, UINT height, DXGI_FORMAT format)
      {
         if (!view) return false;
         D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
         view->GetDesc(&sv);
         com_ptr<ID3D11Resource> resource;
         view->GetResource(&resource);
         com_ptr<ID3D11Texture2D> texture;
         if (FAILED(resource->QueryInterface(&texture))) return false;
         D3D11_TEXTURE2D_DESC d{};
         texture->GetDesc(&d);
         return d.Width == width && d.Height == height && d.SampleDesc.Count == 1 && d.ArraySize == 1 && sv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && sv.Texture2D.MostDetailedMip == 0 && sv.Texture2D.MipLevels == 1 && sv.Format == format;
      }
      // Runs at the native gather dispatch, with its ranged cb0, its blur
      // amount and the output pixels' shares of the render samples
      // (Luma_SWBF2_PostWeights.hlsl, with their constants), and restores the
      // compute state; the native dispatch still runs after.
      // Returns the failure reason, or null on success.
      const char* Run(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data,
         const Kernel& kernel, ID3D11ShaderResourceView* input, ID3D11ShaderResourceView* coc_view,
         ID3D11ShaderResourceView* shares, ID3D11Buffer* footprint, UINT width, UINT height)
      {
         const UINT axis = kernel.vertical ? 1 : 0;
         auto found = data.native_compute_shaders.find(ShaderKey(kernel.vertical, kernel.foreground));
         if (found == data.native_compute_shaders.end() || !found->second) return "dof-shader-unavailable";
         if (!color[axis].Ensure(device, width, height) || (kernel.foreground && !near_color[axis].Ensure(device, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT))) return "dof-allocation";
         if (!border)
         {
            D3D11_SAMPLER_DESC d{};
            d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
            d.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&d, &border))) return "dof-sampler";
         }
         com_ptr<ID3D11Buffer> cb;
         com_ptr<ID3D11DeviceContext1> context1;
         UINT first = 0, count = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
         if (SUCCEEDED(context->QueryInterface(&context1)))
            context1->CSGetConstantBuffers1(0, 1, &cb, &first, &count);
         else
            context->CSGetConstantBuffers(0, 1, &cb);
         D3D11_BUFFER_DESC bd{};
         if (cb) cb->GetDesc(&bd);
         if (!cb || count < 4 || uint64_t(first) * 16 + 64 > bd.ByteWidth) return "dof-constant-range";
         DrawStateStack<DrawStateStackType::Compute> saved;
         saved.Cache(context, data.uav_max_count);
         ID3D11ShaderResourceView* srvs[] = {input, coc_view, kernel.foreground && kernel.vertical ? near_color[0].srv.get() : nullptr, shares};
         ID3D11SamplerState* samplers[] = {border.get()};
         ID3D11UnorderedAccessView* uavs[] = {color[axis].uav.get(), kernel.foreground ? near_color[axis].uav.get() : nullptr};
         context->CSSetShaderResources(0, 4, srvs);
         context->CSSetConstantBuffers(1, 1, &footprint);
         context->CSSetSamplers(0, 1, samplers);
         context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
         context->CSSetShader(found->second.get(), nullptr, 0);
         context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
         // Unbind our UAVs before the state restore rebinds SRVs (a private one
         // could be among them). Null counts keep the original UAV counters.
         ID3D11UnorderedAccessView* empty[2]{};
         context->CSSetUnorderedAccessViews(0, 2, empty, nullptr);
         saved.Restore(context);
         return nullptr;
      }
   };
} // namespace Battlefront2DOF

namespace Battlefront2PostDLSS
{
   inline bool WritesStencil(const D3D11_DEPTH_STENCIL_DESC& state)
   {
      return state.StencilEnable && state.StencilWriteMask != 0 && (state.FrontFace.StencilPassOp != D3D11_STENCIL_OP_KEEP || state.BackFace.StencilPassOp != D3D11_STENCIL_OP_KEEP || state.FrontFace.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP || state.BackFace.StencilDepthFailOp != D3D11_STENCIL_OP_KEEP || state.FrontFace.StencilFailOp != D3D11_STENCIL_OP_KEEP || state.BackFace.StencilFailOp != D3D11_STENCIL_OP_KEEP);
   }
   inline BYTE StencilReadMask(const D3D11_DEPTH_STENCIL_DESC& state)
   {
      const auto reads = [](D3D11_COMPARISON_FUNC function)
      {
         return function != D3D11_COMPARISON_ALWAYS && function != D3D11_COMPARISON_NEVER;
      };
      // ALWAYS/NEVER tests do not read the private stencil copy. What they write
      // lands in the native stencil, and Replay invalidates the written bits.
      return state.StencilEnable && (reads(state.FrontFace.StencilFunc) || reads(state.BackFace.StencilFunc))
                ? state.StencilReadMask
                : 0;
   }
   // A null depth-stencil state binding means D3D11's defaults.
   inline D3D11_DEPTH_STENCIL_DESC DepthStencilDesc(ID3D11DepthStencilState* state)
   {
      CD3D11_DEPTH_STENCIL_DESC desc(D3D11_DEFAULT);
      if (state) state->GetDesc(&desc);
      return desc;
   }
   inline bool IsUAV(D3D_SHADER_INPUT_TYPE type)
   {
      return type == D3D_SIT_UAV_RWTYPED || type == D3D_SIT_UAV_RWSTRUCTURED || type == D3D_SIT_UAV_RWBYTEADDRESS || type == D3D_SIT_UAV_APPEND_STRUCTURED || type == D3D_SIT_UAV_CONSUME_STRUCTURED || type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
   }
   // The FullGraphics state stack does not cover the geometry and tessellation
   // shaders, which the fullscreen copy passes unbind.
   struct SavedGraphicsState
   {
      DrawStateStack<DrawStateStackType::FullGraphics> stack;
      com_ptr<ID3D11GeometryShader> gs;
      com_ptr<ID3D11HullShader> hs;
      com_ptr<ID3D11DomainShader> ds;
      SavedGraphicsState(ID3D11DeviceContext* context, UINT uav_count)
      {
         stack.Cache(context, uav_count);
         context->GSGetShader(&gs, nullptr, nullptr);
         context->HSGetShader(&hs, nullptr, nullptr);
         context->DSGetShader(&ds, nullptr, nullptr);
      }
      void Restore(ID3D11DeviceContext* context)
      {
         stack.Restore(context);
         context->GSSetShader(gs.get(), nullptr, 0);
         context->HSSetShader(hs.get(), nullptr, 0);
         context->DSSetShader(ds.get(), nullptr, 0);
      }
   };
   struct Image
   {
      com_ptr<ID3D11Texture2D> color;
      com_ptr<ID3D11RenderTargetView> rtv;
      com_ptr<ID3D11ShaderResourceView> srv;
      com_ptr<ID3D11ShaderResourceView> mips;
      com_ptr<ID3D11Resource> native;
      uint64_t frame = ~uint64_t(0);
      bool borrowed = false;
   };
   struct Depth
   {
      com_ptr<ID3D11Resource> native;
      com_ptr<ID3D11Texture2D> texture;
      com_ptr<ID3D11DepthStencilView> dsv;
      com_ptr<ID3D11ShaderResourceView> depth, stencil;
      uint64_t frame = ~uint64_t(0);
      uint64_t stencil_frame = ~uint64_t(0);
      BYTE stencil_valid_mask = 0;
   };
   struct State
   {
      uint64_t frame = ~uint64_t(0);
      UINT width = 0, height = 0;
      UINT native_width = 0, native_height = 0;
      bool valid = false;
      bool bypass_dof = false;
      bool bypass_motion_blur = false;
      Battlefront2DOF::State dof;
      com_ptr<ID3D11ShaderResourceView> output_srv, output_mips;
      std::vector<Image> images;
      std::vector<Depth> depths;
      std::unordered_map<uint64_t, com_ptr<ID3D11PixelShader>> shaders;
      std::unordered_map<uint64_t, uint64_t> compute_writes;
      com_ptr<ID3D11Buffer> constants, keep_constants;
      com_ptr<ID3D11BlendState> keep_blend;
      com_ptr<ID3D11SamplerState> sampler;
      com_ptr<ID3D11Buffer> no_ironsights, zero_vector;
      com_ptr<ID3D11DepthStencilState> depth_copy;
      std::array<com_ptr<ID3D11DepthStencilState>, 8> stencil_copy;
      std::array<com_ptr<ID3D11DepthStencilState>, 256> stencil_clear;
      // After DLSS the render-size inputs of the replayed passes are jittered samples under a settled picture.
      // `sample_offset` is where this frame's samples sit (from the render pixel centres, in render pixels, +y
      // down); `shares` holds each output pixel's shares of the four samples around it
      // (Luma_SWBF2_PostWeights.hlsl) and `upsampled` the inputs read through them this frame
      // (Luma_SWBF2_PostUpsample.hlsl).
      float sample_offset[2]{};
      Battlefront2DOF::Image shares;
      com_ptr<ID3D11Resource> shares_input;
      com_ptr<ID3D11ShaderResourceView> shares_input_srv;
      com_ptr<ID3D11Buffer> footprint;
      uint64_t shares_frame = ~uint64_t(0);
      struct Upsampled
      {
         com_ptr<ID3D11Resource> source;
         Battlefront2DOF::Image image;
         uint64_t frame = ~uint64_t(0);
         uint32_t blend_weight = 0;
      };
      std::vector<Upsampled> upsampled;
      uint32_t failure_logs = 0;
      Image* Find(ID3D11Resource* native)
      {
         for (auto& image : images)
            if (image.frame == frame && image.native == native) return &image;
         return nullptr;
      }
      // Always false, so callers can return it.
      bool Fail(const char* reason)
      {
         valid = false;
         if (failure_logs++ < 12)
         {
            std::ostringstream log;
            log << "[Battlefront II Luma] Post-DLSS replay unavailable: " << reason << " frame=" << frame
                << "; native presentation retained.";
            reshade::log::message(reshade::log::level::warning, log.str().c_str());
         }
         return false;
      }
      void Reset()
      {
         frame = ~uint64_t(0);
         valid = false;
         images.clear();
         depths.clear();
         dof = {};
         shares = {};
         shares_input = nullptr;
         shares_input_srv = nullptr;
         shares_frame = ~uint64_t(0);
         upsampled.clear();
         output_srv = output_mips = nullptr;
         width = height = 0;
      }
      bool Begin(ID3D11Device* device, ID3D11Resource* native, ID3D11Texture2D* output,
         ID3D11ShaderResourceView* srv, uint64_t current_frame, float offset_x, float offset_y)
      {
         sample_offset[0] = offset_x;
         sample_offset[1] = offset_y;
         D3D11_TEXTURE2D_DESC d{};
         output->GetDesc(&d);
         if (width != d.Width || height != d.Height) Reset();
         frame = current_frame;
         width = d.Width;
         height = d.Height;
         valid = false;
         dof.Begin();
         output_srv = output_mips = nullptr;
         com_ptr<ID3D11Texture2D> native_texture;
         if (FAILED(native->QueryInterface(&native_texture))) return false;
         D3D11_TEXTURE2D_DESC nd{};
         native_texture->GetDesc(&nd);
         if (nd.Format != DXGI_FORMAT_R11G11B10_FLOAT || nd.SampleDesc.Count != 1 || nd.ArraySize != 1) return false;
         if (native_width != nd.Width || native_height != nd.Height) upsampled.clear(); // inputs of another render size
         native_width = nd.Width;
         native_height = nd.Height;
         if (images.empty()) images.resize(1);
         auto& root = images[0];
         if (root.color != output)
         {
            root = {};
            root.color = output;
         }
         root.srv = srv;
         root.native = native;
         root.frame = frame;
         root.borrowed = true;
         D3D11_RENDER_TARGET_VIEW_DESC view{};
         view.Format = d.Format;
         view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
         if (!root.rtv && FAILED(device->CreateRenderTargetView(output, &view, &root.rtv))) return false;
         if (!root.mips && FAILED(device->CreateShaderResourceView(output, nullptr, &root.mips))) return false;
         valid = true;
         return true;
      }
      ID3D11ShaderResourceView* Shares() const
      {
         return valid && shares_frame == frame ? shares.srv.get() : nullptr;
      }
      // The constants of the passes that read through the shares, with the pass's own flag.
      ID3D11Buffer* Footprint(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t flag)
      {
         if (!footprint)
         {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = 32;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&d, nullptr, &footprint))) return nullptr;
         }
         const struct
         {
            float scale[2], offset[2];
            int32_t render_max[2];
            uint32_t flag, padding;
         } values{{float(native_width) / width, float(native_height) / height}, {sample_offset[0], sample_offset[1]}, {int32_t(native_width) - 1, int32_t(native_height) - 1}, flag, 0};
         context->UpdateSubresource(footprint.get(), 0, nullptr, &values, 0, 0);
         return footprint.get();
      }
      // One of the two compute passes: inputs at t0-t1, constants at b0, output at u0.
      bool Dispatch(ID3D11DeviceContext* context, DeviceData& data, uint32_t shader_key, ID3D11ShaderResourceView* first, ID3D11ShaderResourceView* second, ID3D11Buffer* cb, ID3D11UnorderedAccessView* output)
      {
         const auto shader = data.native_compute_shaders.find(shader_key);
         if (shader == data.native_compute_shaders.end() || !shader->second || !first || !second || !cb || !output) return false;
         DrawStateStack<DrawStateStackType::Compute> saved;
         saved.Cache(context, data.uav_max_count);
         ID3D11ShaderResourceView* inputs[] = {first, second};
         context->CSSetShaderResources(0, 2, inputs);
         context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
         context->CSSetConstantBuffers(0, 1, &cb);
         context->CSSetShader(shader->second.get(), nullptr, 0);
         context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
         output = nullptr;
         context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
         saved.Restore(context);
         return true;
      }
      // This frame's shares, from the DLSS input colour and output (the root image), right after Begin: the game
      // may reuse the input later in the frame.
      void BuildShares(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data, ID3D11Texture2D* input)
      {
         shares_frame = ~uint64_t(0);
         if (!valid || !input) return;
         D3D11_TEXTURE2D_DESC d{};
         input->GetDesc(&d);
         if (d.Width != native_width || d.Height != native_height || d.SampleDesc.Count != 1 || d.ArraySize != 1) return;
         if (shares_input != input)
         {
            shares_input = nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = d.Format;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            shares_input_srv = nullptr;
            if (FAILED(device->CreateShaderResourceView(input, &sv, &shares_input_srv))) return;
            shares_input = input;
         }
         if (!shares.Ensure(device, width, height, DXGI_FORMAT_R8G8B8A8_UNORM)) return;
         if (Dispatch(context, data, CompileTimeStringHash("Battlefront II Post Weights CS"), images[0].srv.get(), shares_input_srv.get(), Footprint(device, context, 0), shares.uav.get()))
            shares_frame = frame;
      }
      // A render-size depth, velocity or blur amount input read per output pixel through the shares, once a frame (until
      // it is written again). `blend_weight` for a DOF composite's blur amount: 1 when it blends by the far blur only, 2
      // by either (Luma_SWBF2_PostUpsample.hlsl). Null if it cannot be made.
      ID3D11ShaderResourceView* Upsample(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data, ID3D11ShaderResourceView* view, uint32_t blend_weight = 0)
      {
         if (!view || !Shares()) return nullptr;
         D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
         view->GetDesc(&vd);
         const DXGI_FORMAT format = vd.Format == DXGI_FORMAT_R16_FLOAT || vd.Format == DXGI_FORMAT_R16G16_FLOAT ? vd.Format : vd.Format == DXGI_FORMAT_R32_FLOAT || vd.Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS || vd.Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT
                                                                                                                                                                                                                                                                        : DXGI_FORMAT_UNKNOWN;
         com_ptr<ID3D11Resource> source;
         view->GetResource(&source);
         com_ptr<ID3D11Texture2D> texture;
         D3D11_TEXTURE2D_DESC d{};
         if (format == DXGI_FORMAT_UNKNOWN || vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || vd.Texture2D.MostDetailedMip != 0 || FAILED(source->QueryInterface(&texture))) return nullptr;
         texture->GetDesc(&d);
         if (d.Width != native_width || d.Height != native_height || d.SampleDesc.Count != 1 || d.ArraySize != 1) return nullptr;
         Upsampled* entry = nullptr;
         for (auto& candidate : upsampled)
            if (candidate.source == source && candidate.blend_weight == blend_weight) entry = &candidate;
         if (!entry)
         {
            // Full: drop the inputs not read this frame (a level load leaves old ones behind), never one a draw holds.
            if (upsampled.size() >= 8) std::erase_if(upsampled, [&](const Upsampled& other)
               { return other.frame != frame; });
            if (upsampled.size() >= 8) return nullptr;
            entry = &upsampled.emplace_back();
            entry->source = source;
            entry->blend_weight = blend_weight;
         }
         if (entry->frame == frame) return entry->image.srv.get();
         if (!entry->image.Ensure(device, width, height, format) || !Dispatch(context, data, CompileTimeStringHash("Battlefront II Post Upsample CS"), view, Shares(), Footprint(device, context, blend_weight), entry->image.uav.get())) return nullptr;
         entry->frame = frame;
         return entry->image.srv.get();
      }
      // The first pool entry not used this frame (entry 0 is the DLSS output), or null when 16 are in use.
      Image* FreeSlot()
      {
         size_t index = 1;
         while (index < images.size() && images[index].frame == frame)
            ++index;
         if (index >= 16) return nullptr;
         if (index == images.size()) images.resize(index + 1);
         return &images[index];
      }
      Image* Target(ID3D11Device* device, ID3D11Resource* native)
      {
         if (auto* existing = Find(native)) return existing;
         auto* slot = FreeSlot();
         if (!slot) return nullptr;
         auto& image = *slot;
         if (image.borrowed) image = {};
         if (!image.color)
         {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width;
            d.Height = height;
            d.MipLevels = 0;
            d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R11G11B10_FLOAT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            D3D11_RENDER_TARGET_VIEW_DESC rv{};
            rv.Format = d.Format;
            rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = d.Format;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &image.color)) || FAILED(device->CreateRenderTargetView(image.color.get(), &rv, &image.rtv)) || FAILED(device->CreateShaderResourceView(image.color.get(), &sv, &image.srv)) || FAILED(device->CreateShaderResourceView(image.color.get(), nullptr, &image.mips)))
            {
               image = {};
               return nullptr;
            }
         }
         image.native = native;
         image.frame = frame;
         return &image;
      }
      bool Alias(ID3D11Resource* source, ID3D11Resource* target)
      {
         auto* original = Find(source);
         if (!original) return false;
         Image alias = *original; // Copy first: FreeSlot can grow the pool.
         alias.native = target;
         alias.borrowed = true;
         auto* slot = Find(target);
         if (!slot) slot = FreeSlot();
         if (!slot) return false;
         *slot = std::move(alias);
         return true;
      }
      // Private native pack only: cb0[3].xy are the ironsights blend/color
      // scales in 7DE1009D and C1AD9344; zw are unused there.
      bool BindNoIronsights(ID3D11Device* device, ID3D11DeviceContext* context)
      {
         Battlefront2RenderState::PixelConstantBufferBinding original;
         if (!original.Capture(context) || !original.context1) return false;
         D3D11_BUFFER_DESC d{};
         original.buffer->GetDesc(&d);
         const UINT offset = original.first_constant * 16;
         if (offset >= d.ByteWidth) return false;
         const UINT bytes = (std::min)(d.ByteWidth - offset, original.num_constants * 16);
         if (bytes < 64 || bytes > 65536 || bytes % 16) return false;
         D3D11_BUFFER_DESC current{};
         if (no_ironsights) no_ironsights->GetDesc(&current);
         if (!no_ironsights || current.ByteWidth != bytes)
         {
            no_ironsights = nullptr;
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = bytes;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&desc, nullptr, &no_ironsights))) return false;
         }
         if (!zero_vector)
         {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = 16;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            const float zero[4]{};
            D3D11_SUBRESOURCE_DATA initial{zero, 0, 0};
            if (FAILED(device->CreateBuffer(&desc, &initial, &zero_vector))) return false;
         }
         const D3D11_BOX box{offset, 0, 0, offset + bytes, 1, 1};
         context->CopySubresourceRegion(no_ironsights.get(), 0, 0, 0, 0, original.buffer.Get(), 0, &box);
         context->CopySubresourceRegion(no_ironsights.get(), 0, 48, 0, 0, zero_vector.get(), 0, nullptr);
         ID3D11Buffer* cb = no_ironsights.get();
         context->PSSetConstantBuffers(0, 1, &cb);
         return true;
      }
      bool Prepare(ID3D11Device* device)
      {
         if (!constants)
         {
            D3D11_BUFFER_DESC d{};
            d.ByteWidth = 32;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&d, nullptr, &constants))) return false;
         }
         if (!sampler)
         {
            D3D11_SAMPLER_DESC d{};
            d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            d.MaxLOD = D3D11_FLOAT32_MAX;
            d.MaxAnisotropy = 1;
            d.ComparisonFunc = D3D11_COMPARISON_NEVER;
            if (FAILED(device->CreateSamplerState(&d, &sampler))) return false;
         }
         return true;
      }
      void InvalidateDepth(ID3D11Resource* native, bool depth_written, bool stencil_written)
      {
         InvalidateDepthBits(native, depth_written, stencil_written ? 255 : 0);
      }
      void InvalidateDepthBits(ID3D11Resource* native, bool depth_written, BYTE stencil_written_mask)
      {
         for (auto& copy : depths)
            if (copy.native == native)
            {
               if (depth_written) copy.frame = ~uint64_t(0);
               copy.stencil_valid_mask &= BYTE(~stencil_written_mask);
               if (!copy.stencil_valid_mask) copy.stencil_frame = ~uint64_t(0);
            }
         // The same depth read through the shares.
         if (depth_written)
            for (auto& entry : upsampled)
               if (entry.source == native) entry.frame = ~uint64_t(0);
      }
      void InvalidateAllDepths()
      {
         for (auto& copy : depths)
         {
            copy.frame = copy.stencil_frame = ~uint64_t(0);
            copy.stencil_valid_mask = 0;
         }
         // The inputs read through the shares as well: the draw may have written any of them.
         for (auto& entry : upsampled)
            entry.frame = ~uint64_t(0);
      }
      // A private, patched copy of the game's pixel shader (from the framework's
      // original bytecode cache). Rejected once per key; the result is cached.
      ID3D11PixelShader* Shader(ID3D11Device* device, uint64_t hash, int scene_slot, bool rebuild_dof = false, uint16_t upsampled_inputs = 0)
      {
         const uint64_t key = hash | (uint64_t(scene_slot + 1) << 32) | (uint64_t(rebuild_dof) << 40) | (uint64_t(upsampled_inputs) << 41);
         if (auto found = shaders.find(key); found != shaders.end()) return found->second.get();
         const auto reject = [&](const char* reason) -> ID3D11PixelShader*
         {
            std::ostringstream log;
            log << "[Battlefront II Luma] Private post shader rejected: ps=0x" << std::hex << hash
                << std::dec << " scene_slot=" << scene_slot << " reason=" << reason;
            reshade::log::message(reshade::log::level::warning, log.str().c_str());
            shaders.emplace(key, nullptr);
            return nullptr;
         };
         std::vector<uint8_t> bytes;
         {
            const std::lock_guard<std::recursive_mutex> lock(s_mutex_dumping);
            const auto found = shader_cache.find(uint32_t(hash));
            if (found == shader_cache.end()) return nullptr;
            const auto* data = static_cast<const uint8_t*>(found->second->data);
            bytes.assign(data, data + found->second->size);
         }
         com_ptr<ID3D11ShaderReflection> reflection;
         if (!::Shader::d3d_reflect || FAILED(::Shader::d3d_reflect(bytes.data(), bytes.size(), IID_PPV_ARGS(&reflection)))) return nullptr;
         D3D11_SHADER_DESC desc{};
         reflection->GetDesc(&desc);
         // Stripped Frostbite blobs have no interface metadata, so
         // GetNumInterfaceSlots can return garbage for them; Patch rejects
         // dynamic linkage in the bytecode instead.
         for (UINT i = 0; i < desc.BoundResources; ++i)
         {
            D3D11_SHADER_INPUT_BIND_DESC resource{};
            reflection->GetResourceBindingDesc(i, &resource);
            const auto overlaps = [&](UINT first, UINT end)
            {
               return resource.BindPoint < end && resource.BindPoint + resource.BindCount > first;
            };
            const bool srv = resource.Type == D3D_SIT_TEXTURE || resource.Type == D3D_SIT_STRUCTURED || resource.Type == D3D_SIT_BYTEADDRESS;
            // b13 and s15 are the replay's own.
            if ((resource.Type == D3D_SIT_CBUFFER && overlaps(13, 14)) || (resource.Type == D3D_SIT_SAMPLER && overlaps(15, 16)) || IsUAV(resource.Type)) return reject("reserved-binding-or-uav");
         }
         // Frostbite material variants can declare SV_Target outputs beyond the
         // bound RTV0; both draws discard them. Replay checks the bound targets,
         // so the signature is not rejected here.
         auto patched = Battlefront2PostShader::Patch(bytes, scene_slot, rebuild_dof, upsampled_inputs);
         if (patched.empty()) return reject("bytecode-transform");
         auto digest = ::Shader::CalcDXBCHash(patched.data(), patched.size());
         std::memcpy(patched.data() + 4, digest.data.data(), 16);
         com_ptr<ID3D11PixelShader> ps;
         if (FAILED(device->CreatePixelShader(patched.data(), patched.size(), nullptr, &ps))) return reject("driver-validation");
         return shaders.emplace(key, ps).first->second.get();
      }
      ID3D11DepthStencilView* CopyDepth(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data,
         ID3D11DepthStencilView* source, bool stencil_enabled, BYTE required_stencil_mask = 255)
      {
         com_ptr<ID3D11Resource> native;
         source->GetResource(&native);
         com_ptr<ID3D11Texture2D> texture;
         if (FAILED(native->QueryInterface(&texture))) return nullptr;
         D3D11_TEXTURE2D_DESC d{};
         texture->GetDesc(&d);
         D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
         source->GetDesc(&vd);
         if (vd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0 || d.SampleDesc.Count != 1 || d.ArraySize != 1) return nullptr;
         auto vs = data.native_vertex_shaders[CompileTimeStringHash("Battlefront II DLSS Scene Conversion VS")];
         auto ps = data.native_pixel_shaders[CompileTimeStringHash("Battlefront II Post Depth PS")];
         auto stencil_ps = data.native_pixel_shaders[CompileTimeStringHash("Battlefront II Post Stencil PS")];
         if (!vs || !ps || (stencil_enabled && !stencil_ps)) return nullptr;
         Depth* copy = nullptr;
         for (auto& depth : depths)
            if (depth.native == native)
            {
               copy = &depth;
               break;
            }
         if (!copy)
         {
            // Add the entry only once all its views exist: a partial entry would
            // be found again and fail on every later draw.
            Depth created;
            created.native = native;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            sv.Format = vd.Format == DXGI_FORMAT_D32_FLOAT ? DXGI_FORMAT_R32_FLOAT : vd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                                                                                  : vd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT     ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                                                                                                                   : DXGI_FORMAT_UNKNOWN;
            if (sv.Format == DXGI_FORMAT_UNKNOWN || FAILED(device->CreateShaderResourceView(native.get(), &sv, &created.depth))) return nullptr;
            if (vd.Format != DXGI_FORMAT_D32_FLOAT)
            {
               sv.Format = vd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT : DXGI_FORMAT_X24_TYPELESS_G8_UINT;
               if (FAILED(device->CreateShaderResourceView(native.get(), &sv, &created.stencil))) return nullptr;
            }
            D3D11_TEXTURE2D_DESC private_desc = d;
            private_desc.Width = width;
            private_desc.Height = height;
            private_desc.MipLevels = 1;
            private_desc.Usage = D3D11_USAGE_DEFAULT;
            private_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            private_desc.CPUAccessFlags = private_desc.MiscFlags = 0;
            D3D11_DEPTH_STENCIL_VIEW_DESC private_view = vd;
            private_view.Flags = 0;
            if (FAILED(device->CreateTexture2D(&private_desc, nullptr, &created.texture)) || FAILED(device->CreateDepthStencilView(created.texture.get(), &private_view, &created.dsv))) return nullptr;
            if (depths.size() >= 8) depths.clear();
            depths.push_back(std::move(created));
            copy = &depths.back();
         }
         if (stencil_enabled && !copy->stencil) return nullptr;
         const bool refresh_depth = copy->frame != frame;
         if (copy->stencil_frame != frame) copy->stencil_valid_mask = 0;
         const BYTE refresh_stencil_mask = stencil_enabled ? BYTE(required_stencil_mask & ~copy->stencil_valid_mask) : 0;
         if (!refresh_depth && !refresh_stencil_mask) return copy->dsv.get();
         if (!depth_copy)
         {
            D3D11_DEPTH_STENCIL_DESC ds{};
            ds.DepthEnable = true;
            ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
            if (FAILED(device->CreateDepthStencilState(&ds, &depth_copy))) return nullptr;
            ds.DepthEnable = false;
            ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            ds.StencilEnable = true;
            ds.StencilReadMask = 255;
            ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
            for (UINT bit = 0; bit < 8; ++bit)
            {
               ds.StencilWriteMask = BYTE(1u << bit);
               if (FAILED(device->CreateDepthStencilState(&ds, &stencil_copy[bit])))
               {
                  // All eight bit states or none: the masked clear copies its desc from bit 0.
                  depth_copy = nullptr;
                  stencil_copy = {};
                  return nullptr;
               }
            }
         }
         if (refresh_stencil_mask && refresh_stencil_mask != 255 && !stencil_clear[refresh_stencil_mask])
         {
            D3D11_DEPTH_STENCIL_DESC ds{};
            stencil_copy[0]->GetDesc(&ds);
            ds.StencilWriteMask = refresh_stencil_mask;
            if (FAILED(device->CreateDepthStencilState(&ds, &stencil_clear[refresh_stencil_mask]))) return nullptr;
         }
         // Copied right before the replay, so it includes every native
         // depth/stencil write so far and matches the game's current tests.
         SavedGraphicsState saved(context, data.uav_max_count);
         context->OMSetRenderTargets(0, nullptr, copy->dsv.get());
         // Depth and stencil have separate native writers, so a depth-only
         // refresh keeps the copied stencil. The depth pass writes every pixel;
         // only a full stencil rebuild needs the zero clear.
         if (refresh_stencil_mask == 255) context->ClearDepthStencilView(copy->dsv.get(), D3D11_CLEAR_STENCIL, 0.f, 0);
         // After DLSS a render-size depth is read at the samples' offset through the output pixels' shares of them;
         // otherwise the sample under the pixel.
         const bool render_size = d.Width == native_width && d.Height == native_height;
         ID3D11ShaderResourceView* inputs[] = {copy->depth.get(), copy->stencil.get(), render_size ? Shares() : nullptr};
         context->PSSetShaderResources(0, 3, inputs);
         float constants_data[8] = {float(d.Width) / width, float(d.Height) / height, render_size ? sample_offset[0] : 0.f, render_size ? sample_offset[1] : 0.f};
         const uint32_t guided = inputs[2] != nullptr;
         std::memcpy(constants_data + 5, &guided, 4);
         context->UpdateSubresource(constants.get(), 0, nullptr, constants_data, 0, 0);
         ID3D11Buffer* cb = constants.get();
         context->PSSetConstantBuffers(13, 1, &cb);
         context->IASetInputLayout(nullptr);
         context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
         context->RSSetState(nullptr);
         context->RSSetScissorRects(0, nullptr);
         D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
         context->RSSetViewports(1, &vp);
         context->GSSetShader(nullptr, nullptr, 0);
         context->HSSetShader(nullptr, nullptr, 0);
         context->DSSetShader(nullptr, nullptr, 0);
         context->VSSetShader(vs.get(), nullptr, 0);
         if (refresh_depth)
         {
            context->PSSetShader(ps.get(), nullptr, 0);
            context->OMSetDepthStencilState(depth_copy.get(), 0);
            context->Draw(4, 0);
         }
         if (refresh_stencil_mask && refresh_stencil_mask != 255)
         {
            // Clear only the dirty bits (ClearDepthStencilView would clear all
            // eight): write reference 0 through the dirty write mask.
            context->PSSetShader(ps.get(), nullptr, 0);
            context->OMSetDepthStencilState(stencil_clear[refresh_stencil_mask].get(), 0);
            context->Draw(4, 0);
         }
         for (UINT bit = 0; bit < 8; ++bit)
         {
            const UINT mask = 1u << bit;
            if (!(refresh_stencil_mask & mask)) continue;
            std::memcpy(constants_data + 4, &mask, 4);
            context->UpdateSubresource(constants.get(), 0, nullptr, constants_data, 0, 0);
            context->PSSetShader(stencil_ps.get(), nullptr, 0);
            context->OMSetDepthStencilState(stencil_copy[bit].get(), 255);
            context->Draw(4, 0);
         }
         saved.Restore(context);
         if (refresh_depth) copy->frame = frame;
         if (refresh_stencil_mask)
         {
            copy->stencil_frame = frame;
            copy->stencil_valid_mask |= refresh_stencil_mask;
         }
         return copy->dsv.get();
      }
      // Rebuilds the DOF gathers of this frame's private scene at full
      // resolution. The native dispatch still runs with its own resources.
      void ReplayDOFGather(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data, uint64_t hash)
      {
         if (!valid || bypass_dof) return;
         const auto* kernel = Battlefront2DOF::FindKernel(hash);
         if (!kernel) return;
         com_ptr<ID3D11ShaderResourceView> source, coc_view;
         context->CSGetShaderResources(kernel->vertical ? 11 : 0, 1, &source);
         context->CSGetShaderResources(9, 1, &coc_view);
         com_ptr<ID3D11Resource> source_resource, coc_resource;
         if (source) source->GetResource(&source_resource);
         if (coc_view) coc_view->GetResource(&coc_resource);
         auto* image = kernel->vertical ? nullptr : Find(source_resource.get());
         if (kernel->vertical ? (dof.horizontal_frame != frame || source_resource != dof.horizontal) : !image) return;
         dof.vertical_frame = ~uint64_t(0);
         ID3D11Buffer* footprint_constants = Footprint(device, context, 0);
         if (!Shares() || !footprint_constants || !Battlefront2DOF::State::TextureView(coc_view.get(), native_width, native_height, DXGI_FORMAT_R16_FLOAT))
         {
            Fail("dof-coc");
            return;
         }
         com_ptr<ID3D11Predicate> predicate;
         BOOL predicate_value;
         context->GetPredication(&predicate, &predicate_value);
         if (predicate)
         {
            Fail("dof-predication");
            return;
         }
         const auto output = [&](UINT slot, bool structured) -> com_ptr<ID3D11Resource>
         {
            com_ptr<ID3D11UnorderedAccessView> view;
            context->CSGetUnorderedAccessViews(slot, 1, &view);
            if (!view) return nullptr;
            D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
            view->GetDesc(&vd);
            com_ptr<ID3D11Resource> resource;
            view->GetResource(&resource);
            if (structured)
            {
               com_ptr<ID3D11Buffer> buffer;
               if (FAILED(resource->QueryInterface(&buffer))) return nullptr;
               D3D11_BUFFER_DESC d{};
               buffer->GetDesc(&d);
               if (vd.ViewDimension != D3D11_UAV_DIMENSION_BUFFER || vd.Buffer.FirstElement || vd.Buffer.Flags || vd.Buffer.NumElements != uint64_t(native_width) * native_height || d.StructureByteStride != 16) return nullptr;
            }
            else
            {
               com_ptr<ID3D11Texture2D> texture;
               if (FAILED(resource->QueryInterface(&texture))) return nullptr;
               D3D11_TEXTURE2D_DESC d{};
               texture->GetDesc(&d);
               if (vd.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice || vd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || d.Width != native_width || d.Height != native_height || d.ArraySize != 1 || d.SampleDesc.Count != 1) return nullptr;
            }
            return resource;
         };
         auto target = output(kernel->vertical ? 1 : 0, false);
         auto near_target = kernel->foreground ? output(kernel->vertical ? 3 : 2, true) : nullptr;
         if (!target || (kernel->foreground && !near_target))
         {
            Fail("dof-gather-output");
            return;
         }
         if (kernel->vertical)
         {
            com_ptr<ID3D11ShaderResourceView> near_source;
            com_ptr<ID3D11Resource> near_resource;
            if (kernel->foreground)
            {
               context->CSGetShaderResources(13, 1, &near_source);
               if (near_source) near_source->GetResource(&near_resource);
            }
            if (coc_resource != dof.coc || kernel->foreground != dof.has_foreground || (kernel->foreground && near_resource != dof.near_horizontal))
            {
               Fail("dof-gather-lineage");
               return;
            }
         }
         if (const char* error = dof.Run(device, context, data, *kernel, kernel->vertical ? dof.color[0].srv.get() : image->srv.get(),
                coc_view.get(), Shares(), footprint_constants, width, height))
         {
            Fail(error);
            return;
         }
         if (kernel->vertical)
         {
            dof.vertical = target;
            dof.near_vertical = near_target;
            dof.vertical_frame = frame;
         }
         else
         {
            dof.scene = source_resource;
            dof.coc = coc_resource;
            dof.horizontal = target;
            dof.near_horizontal = near_target;
            dof.horizontal_frame = frame;
            dof.has_foreground = kernel->foreground;
         }
      }
      // After a depth-of-field composite at output resolution: DLSS's own pixels (the composite's input) come back next
      // to render samples in focus, at the composite's in-focus scale (Luma_SWBF2_DOFKeep.hlsl). `target` is the
      // composite's private output; `foreground` its rebuilt foreground gather, if any.
      void KeepInFocus(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data, ID3D11ShaderResourceView* sharp,
         ID3D11ShaderResourceView* coc, ID3D11ShaderResourceView* foreground, ID3D11RenderTargetView* target)
      {
         auto vs = data.native_vertex_shaders[CompileTimeStringHash("Battlefront II DLSS Scene Conversion VS")];
         auto ps = data.native_pixel_shaders[CompileTimeStringHash("Battlefront II DOF Keep PS")];
         if (!vs || !ps || !sharp || !coc || !target) return;
         if (!keep_blend)
         {
            D3D11_BLEND_DESC bd{};
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
            rt.DestBlendAlpha = D3D11_BLEND_ONE;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
            if (FAILED(device->CreateBlendState(&bd, &keep_blend))) return;
         }
         if (!keep_constants)
         {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 32;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &keep_constants))) return;
         }
         const struct
         {
            float scale[2], offset[2];
            int32_t render_max[2], padding[2];
         } values{{float(native_width) / width, float(native_height) / height}, {sample_offset[0], sample_offset[1]}, {int32_t(native_width) - 1, int32_t(native_height) - 1}, {}};
         context->UpdateSubresource(keep_constants.get(), 0, nullptr, &values, 0, 0);
         SavedGraphicsState saved(context, data.uav_max_count);
         // b0 stays the composite's own constants, for its in-focus scale.
         ID3D11ShaderResourceView* inputs[] = {sharp, coc, foreground};
         context->PSSetShaderResources(0, 3, inputs);
         ID3D11Buffer* cb = keep_constants.get();
         context->PSSetConstantBuffers(13, 1, &cb);
         context->OMSetRenderTargets(1, &target, nullptr);
         context->OMSetBlendState(keep_blend.get(), nullptr, D3D11_DEFAULT_SAMPLE_MASK);
         context->OMSetDepthStencilState(nullptr, 0);
         context->IASetInputLayout(nullptr);
         context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
         context->RSSetState(nullptr);
         context->RSSetScissorRects(0, nullptr);
         const D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
         context->RSSetViewports(1, &vp);
         context->GSSetShader(nullptr, nullptr, 0);
         context->HSSetShader(nullptr, nullptr, 0);
         context->DSSetShader(nullptr, nullptr, 0);
         context->VSSetShader(vs.get(), nullptr, 0);
         context->PSSetShader(ps.get(), nullptr, 0);
         context->Draw(4, 0);
         saved.Restore(context);
      }
      // Mirrors a draw into the private scene (or a known compositor reading
      // it), then runs the native draw once with its own state. Wide-radius
      // native bokeh layers stay blurred, at their engine sizes.
      bool Replay(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data,
         uint64_t hash, int scene_slot, std::function<void()>& draw)
      {
         if (!valid) return false;
         // Every later draw of the frame (HUD included) gets here: check the
         // first color target before querying the full output-merger state.
         com_ptr<ID3D11RenderTargetView> first_target;
         context->OMGetRenderTargets(1, &first_target, nullptr);
         if (!first_target) return false;
         com_ptr<ID3D11Resource> target;
         first_target->GetResource(&target);
         Image* destination = Find(target.get());
         com_ptr<ID3D11ShaderResourceView> source;
         com_ptr<ID3D11Resource> source_resource;
         if (scene_slot >= 0)
         {
            context->PSGetShaderResources(UINT(scene_slot), 1, &source);
            if (source) source->GetResource(&source_resource);
         }
         const bool composite = scene_slot >= 0 && Find(source_resource.get());
         if (!destination && !composite) return false;
         com_ptr<ID3D11RenderTargetView> targets[8];
         com_ptr<ID3D11DepthStencilView> dsv;
         context->OMGetRenderTargets(8, &targets[0], &dsv);
         com_ptr<ID3D11Predicate> predicate;
         BOOL predicate_value = FALSE;
         context->GetPredication(&predicate, &predicate_value);
         if (predicate) return Fail("predicated-scene-draw");
         com_ptr<ID3D11Buffer> stream_output[4];
         context->SOGetTargets(4, &stream_output[0]);
         for (const auto& buffer : stream_output)
            if (buffer) return Fail("stream-output-side-effects");
         for (UINT i = 1; i < 8; ++i)
            if (targets[i]) return Fail("multiple-color-targets");
         com_ptr<ID3D11Texture2D> native_target;
         if (FAILED(target->QueryInterface(&native_target))) return Fail("target-type");
         D3D11_TEXTURE2D_DESC td{};
         native_target->GetDesc(&td);
         D3D11_RENDER_TARGET_VIEW_DESC tv{};
         targets[0]->GetDesc(&tv);
         if (td.Width != native_width || td.Height != native_height || td.Format != DXGI_FORMAT_R11G11B10_FLOAT || tv.Format != DXGI_FORMAT_R11G11B10_FLOAT)
            return destination ? Fail("scene-extent-or-format") : false;
         D3D11_VIEWPORT vp{};
         UINT count = 1;
         context->RSGetViewports(&count, &vp);
         if (count != 1 || vp.TopLeftX != 0 || vp.TopLeftY != 0 || vp.Width != float(td.Width) || vp.Height != float(td.Height) || td.SampleDesc.Count != 1 || tv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || tv.Texture2D.MipSlice != 0)
            return Fail("viewport-or-view");
         // DOF or full-scene motion blur (scene at t1) turned off: the native
         // draw still runs, so its resources stay valid, and the private target
         // aliases the sharp input. The depth copies do not see this draw.
         if (composite && ((bypass_dof && scene_slot == 0) || (bypass_motion_blur && scene_slot == 1 && Battlefront2SceneLineage::IsMotionBlurComposite(hash))))
         {
            if (!Alias(source_resource.get(), target.get())) return Fail(scene_slot == 0 ? "dof-bypass-alias" : "motion-blur-bypass-alias");
            draw();
            InvalidateAllDepths();
            return true;
         }
         const unsigned dof_inputs = composite && scene_slot == 0 ? Battlefront2DOF::CompositeInputs(hash) : 0;
         com_ptr<ID3D11ShaderResourceView> original_gather, original_near_gather;
         if (dof_inputs)
         {
            context->PSGetShaderResources(12, 1, &original_gather);
            if (dof_inputs & 2) context->PSGetShaderResources(14, 1, &original_near_gather);
            com_ptr<ID3D11Resource> gather_resource, near_resource, coc_resource;
            if (original_gather) original_gather->GetResource(&gather_resource);
            if (original_near_gather) original_near_gather->GetResource(&near_resource);
            com_ptr<ID3D11ShaderResourceView> coc_view;
            context->PSGetShaderResources(9, 1, &coc_view);
            if (coc_view) coc_view->GetResource(&coc_resource);
            // The engine may gather before motion blur and composite after it,
            // so the gathered scene only has to be one of this frame's images.
            if (dof.vertical_frame != frame || !Find(dof.scene.get()) || dof.vertical != gather_resource || dof.coc != coc_resource || ((dof_inputs & 2) && (!dof.has_foreground || dof.near_vertical != near_resource)))
               return Fail("dof-gather-not-rebuilt");
         }
         // The draw's render-size inputs that are bound at output size for the replay, read through the output pixels'
         // shares of the render samples: a DOF composite's blur amount at t9 (the scene at t0), as the weight it
         // blends by, and other draws' depth and velocity.
         com_ptr<ID3D11ShaderResourceView> original_inputs[16];
         ID3D11ShaderResourceView* upsampled_inputs[16]{};
         uint16_t upsampled_mask = 0;
         if (composite && scene_slot == 0)
         {
            context->PSGetShaderResources(9, 1, &original_inputs[9]);
            upsampled_inputs[9] = Upsample(device, context, data, original_inputs[9].get(), Battlefront2SceneLineage::BlendsFarBlurOnly(hash) ? 1 : 2);
            if (!upsampled_inputs[9]) return Fail("dof-coc");
         }
         else if (Shares())
         {
            context->PSGetShaderResources(0, 16, &original_inputs[0]);
            for (UINT slot = 0; slot < 16; ++slot)
               if (!(composite && int(slot) == scene_slot) && (upsampled_inputs[slot] = Upsample(device, context, data, original_inputs[slot].get())) != nullptr) upsampled_mask |= uint16_t(1u << slot);
         }
         auto* ps = Shader(device, hash, composite ? scene_slot : -1, dof_inputs != 0, upsampled_mask);
         if (!ps || !Prepare(device)) return Fail("shader-or-resources");
         if (!destination) destination = Target(device, target.get());
         if (!destination) return Fail("scene-target-allocation");
         com_ptr<ID3D11DepthStencilState> depth_state;
         UINT reference = 0;
         context->OMGetDepthStencilState(&depth_state, &reference);
         const D3D11_DEPTH_STENCIL_DESC depth_desc = DepthStencilDesc(depth_state.get());
         ID3D11DepthStencilView* mirror_depth = nullptr;
         if (dsv && (depth_desc.DepthEnable || depth_desc.StencilEnable))
         {
            const BYTE stencil_mask = StencilReadMask(depth_desc);
            mirror_depth = CopyDepth(device, context, data, dsv.get(), stencil_mask != 0, stencil_mask);
            if (!mirror_depth) return Fail("depth-stencil-copy");
         }
         // Save only what the replay changes: a full state stack would query
         // 128 SRVs per draw, and post-temporal geometry can be dozens of draws.
         com_ptr<ID3D11PixelShader> original_ps;
         context->PSGetShader(&original_ps, nullptr, nullptr);
         com_ptr<ID3D11SamplerState> original_sampler;
         context->PSGetSamplers(15, 1, &original_sampler);
         Battlefront2RenderState::PixelConstantBufferBinding original_constants;
         original_constants.Capture(context, 13);
         com_ptr<ID3D11UnorderedAccessView> original_uavs[D3D11_1_UAV_SLOT_COUNT];
         context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, data.uav_max_count - 1, &original_uavs[1]);
         D3D11_RECT original_rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
         UINT rect_count = _countof(original_rects);
         context->RSGetScissorRects(&rect_count, original_rects);
         const D3D11_VIEWPORT original_viewport = vp;
         // The replay reads the private scene, the inputs made for the output pixels and the rebuilt gathers.
         const auto bind_inputs = [&](bool replay)
         {
            for (UINT slot = 0; slot < 16; ++slot)
               if (upsampled_inputs[slot])
               {
                  ID3D11ShaderResourceView* input = replay ? upsampled_inputs[slot] : original_inputs[slot].get();
                  context->PSSetShaderResources(slot, 1, &input);
               }
            if (composite)
            {
               ID3D11ShaderResourceView* input = replay ? Find(source_resource.get())->srv.get() : source.get();
               context->PSSetShaderResources(UINT(scene_slot), 1, &input);
            }
            if (dof_inputs)
            {
               ID3D11ShaderResourceView* gather = replay ? dof.color[1].srv.get() : original_gather.get();
               context->PSSetShaderResources(12, 1, &gather);
               if (dof_inputs & 2)
               {
                  gather = replay ? dof.near_color[1].srv.get() : original_near_gather.get();
                  context->PSSetShaderResources(14, 1, &gather);
               }
            }
         };
         ID3D11RenderTargetView* rtv = destination->rtv.get();
         context->OMSetRenderTargets(1, &rtv, mirror_depth);
         bind_inputs(true);
         const float values[8] = {float(td.Width) / width, float(td.Height) / height, 1.f, 1.f, 1.f / td.Width, 1.f / td.Height, 1.f, 1.f};
         context->UpdateSubresource(constants.get(), 0, nullptr, values, 0, 0);
         ID3D11Buffer* cb = constants.get();
         context->PSSetConstantBuffers(13, 1, &cb);
         ID3D11SamplerState* linear = sampler.get();
         context->PSSetSamplers(15, 1, &linear);
         vp.Width = float(width);
         vp.Height = float(height);
         // The game draws its geometry after the temporal pass (fur, bolts, sabers) with this frame's jittered camera: in
         // the DLSS picture it would jump by up to half a render pixel every frame. Moving the viewport by the samples'
         // offset puts it where DLSS put the rest of the picture. Fullscreen passes (no depth test) have no jitter.
         if (!composite && depth_desc.DepthEnable)
         {
            vp.TopLeftX = sample_offset[0] * width / td.Width;
            vp.TopLeftY = sample_offset[1] * height / td.Height;
         }
         context->RSSetViewports(1, &vp);
         D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
         for (UINT i = 0; i < rect_count; ++i)
         {
            rects[i].left = LONG(std::floor(double(original_rects[i].left) * width / td.Width));
            rects[i].top = LONG(std::floor(double(original_rects[i].top) * height / td.Height));
            rects[i].right = LONG(std::ceil(double(original_rects[i].right) * width / td.Width));
            rects[i].bottom = LONG(std::ceil(double(original_rects[i].bottom) * height / td.Height));
         }
         context->RSSetScissorRects(rect_count, rects);
         context->PSSetShader(ps, nullptr, 0);
         draw();
         if (composite && scene_slot == 0)
            KeepInFocus(device, context, data, Find(source_resource.get())->srv.get(), original_inputs[9].get(), (dof_inputs & 2) ? dof.near_color[1].srv.get() : nullptr, destination->rtv.get());
         ID3D11RenderTargetView* original_target = targets[0].get();
         ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
         for (UINT i = 1; i < data.uav_max_count; ++i)
            uavs[i] = original_uavs[i].get();
         context->OMSetRenderTargetsAndUnorderedAccessViews(1, &original_target, dsv.get(), 1, data.uav_max_count - 1, &uavs[1], nullptr);
         bind_inputs(false);
         original_constants.Restore(context);
         ID3D11SamplerState* native_sampler = original_sampler.get();
         context->PSSetSamplers(15, 1, &native_sampler);
         context->RSSetViewports(1, &original_viewport);
         context->RSSetScissorRects(rect_count, original_rects);
         context->PSSetShader(original_ps.get(), nullptr, 0);
         draw();
         const bool depth_written = depth_desc.DepthEnable && depth_desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
         const bool stencil_written = WritesStencil(depth_desc);
         if (dsv && (depth_written || stencil_written))
         {
            com_ptr<ID3D11Resource> native;
            dsv->GetResource(&native);
            InvalidateDepthBits(native.get(), depth_written, stencil_written ? depth_desc.StencilWriteMask : 0);
         }
         return true;
      }
      void ObserveCompute(ID3D11DeviceContext* context, uint64_t hash)
      {
         if (!valid) return;
         auto found = compute_writes.find(hash);
         if (found == compute_writes.end())
         {
            uint64_t mask = 0;
            const std::lock_guard<std::recursive_mutex> lock(s_mutex_dumping);
            auto cached = shader_cache.find(uint32_t(hash));
            if (cached == shader_cache.end())
            {
               Fail("unknown-compute-writes");
               return;
            }
            com_ptr<ID3D11ShaderReflection> reflection;
            if (!::Shader::d3d_reflect || FAILED(::Shader::d3d_reflect(cached->second->data, cached->second->size, IID_PPV_ARGS(&reflection))))
            {
               Fail("compute-reflection");
               return;
            }
            D3D11_SHADER_DESC desc{};
            reflection->GetDesc(&desc);
            for (UINT i = 0; i < desc.BoundResources; ++i)
            {
               D3D11_SHADER_INPUT_BIND_DESC resource{};
               reflection->GetResourceBindingDesc(i, &resource);
               if (IsUAV(resource.Type))
                  for (UINT slot = resource.BindPoint; slot < resource.BindPoint + resource.BindCount && slot < 64; ++slot)
                     mask |= uint64_t(1) << slot;
            }
            found = compute_writes.emplace(hash, mask).first;
         }
         for (UINT slot = 0; slot < 64; ++slot)
            if (found->second & (uint64_t(1) << slot))
            {
               com_ptr<ID3D11UnorderedAccessView> view;
               context->CSGetUnorderedAccessViews(slot, 1, &view);
               if (!view) continue;
               D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
               view->GetDesc(&d);
               com_ptr<ID3D11Resource> resource;
               view->GetResource(&resource);
               dof.InvalidateWrite(resource.get());
               for (auto& entry : upsampled)
                  if (entry.source == resource) entry.frame = ~uint64_t(0);
               if (d.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D || d.Texture2D.MipSlice != 0) continue;
               if (Find(resource.get()))
               {
                  Fail("unmirrored-compute-scene-write");
                  return;
               }
            }
      }
      bool Finish(ID3D11Resource* native_scene)
      {
         if (!valid) return false;
         auto* image = Find(native_scene);
         if (!image) return Fail("untracked-pack-input");
         output_srv = image->srv;
         output_mips = image->mips;
         return true;
      }
   };
   // Separate depth/stencil validity lasts only across draws this state
   // mirrors. Any other graphics draw ends it, including depth-only native
   // passes with no pixel shader.
   struct NativeDrawGuard
   {
      State* state = nullptr;
      uint64_t frame = ~uint64_t(0);
      bool mirrored = false;
      ~NativeDrawGuard()
      {
         if (state && !mirrored && state->frame == frame) state->InvalidateAllDepths();
      }
   };
} // namespace Battlefront2PostDLSS

namespace Battlefront2FullTransparency
{
   // Draws the game's half-resolution transparency again at full resolution
   // into private images, which replace it at its composite. The native draws
   // still run, so a failed image keeps the native composite.
   struct State
   {
      struct Image
      {
         com_ptr<ID3D11Resource> native;
         com_ptr<ID3D11Texture2D> texture;
         com_ptr<ID3D11RenderTargetView> rtv;
         com_ptr<ID3D11ShaderResourceView> srv;
         uint64_t frame = ~uint64_t(0);
         bool valid = false;
         UINT draws = 0;
      };
      Battlefront2PostDLSS::State shader_cache;
      std::vector<Image> images;
      com_ptr<ID3D11Resource> half_depth;
      com_ptr<ID3D11ShaderResourceView> full_depth;
      com_ptr<ID3D11SamplerState> point;
      com_ptr<ID3D11Texture2D> linear_depth;
      com_ptr<ID3D11RenderTargetView> linear_depth_rtv;
      com_ptr<ID3D11ShaderResourceView> linear_depth_srv;
      com_ptr<ID3D11Buffer> linear_depth_constants;
      struct DepthState
      {
         com_ptr<ID3D11DepthStencilState> native, replacement;
         UINT flags;
         bool stencil;
      };
      std::vector<DepthState> depth_states;
      uint64_t frame = ~uint64_t(0);
      UINT width = 0, height = 0;
      bool ready = false, composited = false;
      uint64_t current_ps = 0;
      std::unordered_set<uint64_t> failure_keys;
      // Always false, so callers can return it. Logs each shader/reason pair once.
      bool Fail(Image& image, const char* reason)
      {
         image.valid = false;
         if (failure_keys.size() < 64 && failure_keys.insert((current_ps << 32) ^ std::hash<std::string_view>{}(reason)).second)
         {
            std::ostringstream message;
            message << "[Battlefront II Luma] Full transparency fallback: " << reason << " ps=0x" << std::hex << current_ps << std::dec << " frame=" << frame;
            reshade::log::message(reshade::log::level::warning, message.str().c_str());
         }
         return false;
      }
      void EndPass()
      {
         ready = composited = false;
         half_depth = nullptr;
         full_depth = nullptr;
      }
      Image* Find(ID3D11Resource* native)
      {
         for (auto& image : images)
            if (image.frame == frame && image.native == native) return &image;
         return nullptr;
      }

      bool Begin(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data, uint64_t current_frame)
      {
         EndPass();
         frame = current_frame;
         com_ptr<ID3D11RenderTargetView> target;
         context->OMGetRenderTargets(1, &target, nullptr);
         if (!target) return false;
         com_ptr<ID3D11Resource> resource;
         com_ptr<ID3D11Texture2D> half, full;
         target->GetResource(&resource);
         if (FAILED(resource->QueryInterface(&half))) return false;
         context->PSGetShaderResources(0, 1, &full_depth);
         if (!full_depth) return false;
         com_ptr<ID3D11Resource> full_resource;
         full_depth->GetResource(&full_resource);
         if (FAILED(full_resource->QueryInterface(&full))) return false;
         D3D11_TEXTURE2D_DESC hd{}, fd{};
         half->GetDesc(&hd);
         full->GetDesc(&fd);
         D3D11_RENDER_TARGET_VIEW_DESC rv{};
         target->GetDesc(&rv);
         if (rv.Format != DXGI_FORMAT_R32_FLOAT || fd.Width != hd.Width * 2 || fd.Height != hd.Height * 2 || hd.SampleDesc.Count != 1 || fd.SampleDesc.Count != 1 || fd.ArraySize != 1) return false;
         if (width != fd.Width || height != fd.Height)
         {
            images.clear();
            shader_cache.depths.clear();
            linear_depth = nullptr;
            linear_depth_rtv = nullptr;
            linear_depth_srv = nullptr;
         }
         width = fd.Width;
         height = fd.Height;
         // Native 199CF6D5 writes raw depth to SV_Depth, but linear distance
         // to its R32 color target: cb0[2].x / (raw - cb0[2].y).
         auto vs = data.native_vertex_shaders[CompileTimeStringHash("Battlefront II DLSS Scene Conversion VS")];
         auto ps = data.native_pixel_shaders[CompileTimeStringHash("Battlefront II Transparency Linear Depth PS")];
         Battlefront2RenderState::PixelConstantBufferBinding binding;
         if (!vs || !ps || !binding.Capture(context)) return false;
         D3D11_BUFFER_DESC bd{};
         binding.buffer->GetDesc(&bd);
         const UINT offset = binding.first_constant * 16 + 32;
         if (binding.num_constants < 3 || offset > bd.ByteWidth || bd.ByteWidth - offset < 16) return false;
         if (!linear_depth_constants)
         {
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 16;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&cb, nullptr, &linear_depth_constants))) return false;
         }
         if (!linear_depth)
         {
            D3D11_TEXTURE2D_DESC ld{};
            ld.Width = width;
            ld.Height = height;
            ld.MipLevels = ld.ArraySize = ld.SampleDesc.Count = 1;
            ld.Format = DXGI_FORMAT_R32_FLOAT;
            ld.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&ld, nullptr, &linear_depth)) || FAILED(device->CreateRenderTargetView(linear_depth.get(), nullptr, &linear_depth_rtv)) || FAILED(device->CreateShaderResourceView(linear_depth.get(), nullptr, &linear_depth_srv))) return false;
         }
         com_ptr<ID3D11ShaderResourceView> raw = full_depth;
         full_depth = linear_depth_srv;
         D3D11_BOX box{offset, 0, 0, offset + 16, 1, 1};
         context->CopySubresourceRegion(linear_depth_constants.get(), 0, 0, 0, 0, binding.buffer.Get(), 0, &box);
         Battlefront2PostDLSS::SavedGraphicsState saved(context, data.uav_max_count);
         ID3D11RenderTargetView* rtv = linear_depth_rtv.get();
         context->OMSetRenderTargets(1, &rtv, nullptr);
         context->OMSetBlendState(nullptr, nullptr, ~0u);
         context->OMSetDepthStencilState(nullptr, 0);
         context->IASetInputLayout(nullptr);
         context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
         context->RSSetState(nullptr);
         D3D11_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
         context->RSSetViewports(1, &vp);
         context->GSSetShader(nullptr, nullptr, 0);
         context->HSSetShader(nullptr, nullptr, 0);
         context->DSSetShader(nullptr, nullptr, 0);
         context->VSSetShader(vs.get(), nullptr, 0);
         context->PSSetShader(ps.get(), nullptr, 0);
         ID3D11ShaderResourceView* input = raw.get();
         context->PSSetShaderResources(0, 1, &input);
         ID3D11Buffer* cb = linear_depth_constants.get();
         context->PSSetConstantBuffers(13, 1, &cb);
         context->Draw(4, 0);
         saved.Restore(context);
         shader_cache.frame = frame;
         shader_cache.width = width;
         shader_cache.height = height;
         shader_cache.InvalidateAllDepths();
         half_depth = resource;
         ready = true;
         return true;
      }

      Image* Target(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* native)
      {
         if (auto* existing = Find(native)) return existing;
         size_t index = 0;
         while (index < images.size() && images[index].frame == frame)
            ++index;
         if (index >= 8) return nullptr;
         if (index == images.size()) images.emplace_back();
         auto& image = images[index];
         if (!image.texture)
         {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width;
            d.Height = height;
            d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&d, nullptr, &image.texture)) || FAILED(device->CreateRenderTargetView(image.texture.get(), nullptr, &image.rtv)) || FAILED(device->CreateShaderResourceView(image.texture.get(), nullptr, &image.srv)))
            {
               image = {};
               return nullptr;
            }
         }
         image.native = native;
         image.frame = frame;
         image.valid = true;
         image.draws = 0;
         const float clear[4]{};
         context->ClearRenderTargetView(image.rtv.get(), clear);
         return &image;
      }

      template <class DrawFn>
      bool Composite(ID3D11DeviceContext* context, DeviceData& data, DrawFn& draw)
      {
         com_ptr<ID3D11ShaderResourceView> source;
         context->PSGetShaderResources(0, 1, &source);
         if (!source) return false;
         com_ptr<ID3D11Resource> native;
         source->GetResource(&native);
         Image* image = Find(native.get());
         auto ps = data.native_pixel_shaders[CompileTimeStringHash("Battlefront II Full Transparency PS")];
         if (!image || !image->valid || !image->draws || !ps) return false;
         com_ptr<ID3D11RenderTargetView> target;
         context->OMGetRenderTargets(1, &target, nullptr);
         com_ptr<ID3D11Resource> target_resource;
         com_ptr<ID3D11Texture2D> texture;
         if (!target) return false;
         target->GetResource(&target_resource);
         if (FAILED(target_resource->QueryInterface(&texture))) return false;
         D3D11_TEXTURE2D_DESC d{};
         texture->GetDesc(&d);
         D3D11_VIEWPORT vp{};
         UINT count = 1;
         context->RSGetViewports(&count, &vp);
         if (d.Width != width || d.Height != height || count != 1 || vp.TopLeftX != 0 || vp.TopLeftY != 0 || vp.Width != float(width) || vp.Height != float(height)) return false;
         com_ptr<ID3D11PixelShader> original;
         context->PSGetShader(&original, nullptr, nullptr);
         ID3D11ShaderResourceView* srv = image->srv.get();
         context->PSSetShaderResources(0, 1, &srv);
         context->PSSetShader(ps.get(), nullptr, 0);
         draw();
         srv = source.get();
         context->PSSetShaderResources(0, 1, &srv);
         context->PSSetShader(original.get(), nullptr, 0);
         composited = true;
         ready = false; // End this view's half-resolution material batch.
         return true;
      }

      // The draw callback is a template parameter so a capturing lambda needs
      // no std::function (and no heap allocation) on every draw.
      template <class DrawFn>
      bool Draw(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data, uint64_t current_frame,
         uint64_t vs, uint64_t ps_hash, DrawFn& draw)
      {
         current_ps = ps_hash;
         if (current_frame != frame) EndPass();
         if (vs == 0xB5ED337B && ps_hash == 0x199CF6D5)
         {
            Begin(device, context, data, current_frame);
            return false;
         }
         if (!ready) return false;
         if (vs == 0x8574172D && ps_hash == 0xF92ACE47) return Composite(context, data, draw);
         // Check the first color target before querying the full output state.
         com_ptr<ID3D11RenderTargetView> first_target;
         context->OMGetRenderTargets(1, &first_target, nullptr);
         if (!first_target) return false;
         com_ptr<ID3D11Resource> native;
         com_ptr<ID3D11Texture2D> texture;
         first_target->GetResource(&native);
         if (FAILED(native->QueryInterface(&texture))) return false;
         D3D11_TEXTURE2D_DESC d{};
         texture->GetDesc(&d);
         D3D11_RENDER_TARGET_VIEW_DESC rv{};
         first_target->GetDesc(&rv);
         if (d.Width * 2 != width || d.Height * 2 != height || rv.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
         com_ptr<ID3D11RenderTargetView> targets[8];
         com_ptr<ID3D11DepthStencilView> dsv;
         context->OMGetRenderTargets(8, &targets[0], &dsv);
         auto* image = Target(device, context, native.get());
         if (!image || !image->valid) return false;
         if (d.SampleDesc.Count != 1 || d.ArraySize != 1) return Fail(*image, "target-shape");
         ID3D11DepthStencilView* private_dsv = nullptr;
         com_ptr<ID3D11DepthStencilState> native_ds;
         ID3D11DepthStencilState* private_ds = nullptr;
         UINT stencil_reference = 0;
         if (dsv)
         {
            context->OMGetDepthStencilState(&native_ds, &stencil_reference);
            D3D11_DEPTH_STENCIL_DESC dd = Battlefront2PostDLSS::DepthStencilDesc(native_ds.get());
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            dsv->GetDesc(&vd);
            const bool native_stencil = vd.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || vd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            if (dd.DepthEnable || (dd.StencilEnable && native_stencil))
            {
               // Seed the tests from the native transparency DSV, not the opaque
               // scene's, and copy its stencil even before its first use.
               if (!shader_cache.Prepare(device)) return Fail(*image, "depth-copy-resources");
               private_dsv = shader_cache.CopyDepth(device, context, data, dsv.get(), native_stencil);
               if (!private_dsv) return Fail(*image, "transparency-depth-copy");
               // The writable copy has no read-only flags, so the state applies
               // them; a depth-only native view must not gain a stencil test.
               if (vd.Flags || (dd.StencilEnable && !native_stencil))
               {
                  for (auto& entry : depth_states)
                     if (entry.native == native_ds && entry.flags == vd.Flags && entry.stencil == native_stencil)
                     {
                        private_ds = entry.replacement.get();
                        break;
                     }
                  if (!private_ds)
                  {
                     if (vd.Flags & D3D11_DSV_READ_ONLY_DEPTH) dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
                     if (vd.Flags & D3D11_DSV_READ_ONLY_STENCIL) dd.StencilWriteMask = 0;
                     if (!native_stencil) dd.StencilEnable = false;
                     com_ptr<ID3D11DepthStencilState> replacement;
                     if (FAILED(device->CreateDepthStencilState(&dd, &replacement))) return Fail(*image, "private-depth-state");
                     if (depth_states.size() >= 64) depth_states.clear();
                     depth_states.push_back({native_ds, replacement, vd.Flags, native_stencil});
                     private_ds = replacement.get();
                  }
               }
               else
                  private_ds = native_ds.get();
            }
         }
         for (UINT i = 1; i < 8; ++i)
            if (targets[i]) return Fail(*image, "unsupported-draw-state");
         com_ptr<ID3D11Predicate> predicate;
         BOOL predicate_value;
         context->GetPredication(&predicate, &predicate_value);
         com_ptr<ID3D11Buffer> so[4];
         context->SOGetTargets(4, &so[0]);
         if (predicate) return Fail(*image, "predication");
         for (auto& buffer : so)
            if (buffer) return Fail(*image, "stream-output");
         D3D11_VIEWPORT vp{};
         UINT vp_count = 1;
         context->RSGetViewports(&vp_count, &vp);
         if (vp_count != 1 || vp.TopLeftX != 0 || vp.TopLeftY != 0 || vp.Width != float(d.Width) || vp.Height != float(d.Height)) return Fail(*image, "unsupported-draw-state");
         com_ptr<ID3D11ShaderResourceView> depth;
         context->PSGetShaderResources(2, 1, &depth);
         com_ptr<ID3D11Resource> depth_resource;
         if (depth) depth->GetResource(&depth_resource);
         const bool uses_depth = depth_resource == half_depth;
         auto* ps = shader_cache.Shader(device, ps_hash, uses_depth ? 2 : -1);
         if (!ps || !shader_cache.Prepare(device)) return Fail(*image, "shader-or-resources");
         if (!point)
         {
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            sampler.MaxAnisotropy = 1;
            sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
            if (FAILED(device->CreateSamplerState(&sampler, &point))) return Fail(*image, "unsupported-draw-state");
         }
         com_ptr<ID3D11PixelShader> original_ps;
         context->PSGetShader(&original_ps, nullptr, nullptr);
         com_ptr<ID3D11SamplerState> original_sampler;
         context->PSGetSamplers(15, 1, &original_sampler);
         Battlefront2RenderState::PixelConstantBufferBinding original_constants;
         original_constants.Capture(context, 13);
         com_ptr<ID3D11UnorderedAccessView> original_uavs[D3D11_1_UAV_SLOT_COUNT];
         context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, data.uav_max_count - 1, &original_uavs[1]);
         for (UINT i = 1; i < data.uav_max_count; ++i)
            if (original_uavs[i]) return Fail(*image, "output-uav");
         D3D11_RECT rects[16]{};
         UINT rect_count = 16;
         context->RSGetScissorRects(&rect_count, rects);
         D3D11_RECT scaled[16]{};
         for (UINT i = 0; i < rect_count; ++i)
            scaled[i] = {rects[i].left * 2, rects[i].top * 2, rects[i].right * 2, rects[i].bottom * 2};
         const D3D11_VIEWPORT original_vp = vp;
         vp.Width = float(width);
         vp.Height = float(height);
         ID3D11RenderTargetView* rtv = image->rtv.get();
         context->OMSetRenderTargets(1, &rtv, private_dsv);
         if (private_dsv) context->OMSetDepthStencilState(private_ds, stencil_reference);
         if (uses_depth)
         {
            ID3D11ShaderResourceView* srv = full_depth.get();
            context->PSSetShaderResources(2, 1, &srv);
         }
         const float values[8] = {.5f, .5f, 1.f, 1.f, 1.f / d.Width, 1.f / d.Height, 1.f, 1.f};
         context->UpdateSubresource(shader_cache.constants.get(), 0, nullptr, values, 0, 0);
         ID3D11Buffer* cb = shader_cache.constants.get();
         context->PSSetConstantBuffers(13, 1, &cb);
         ID3D11SamplerState* sampler = point.get();
         context->PSSetSamplers(15, 1, &sampler);
         context->RSSetViewports(1, &vp);
         context->RSSetScissorRects(rect_count, scaled);
         context->PSSetShader(ps, nullptr, 0);
         draw();
         // Restore the native state and run the native draw once, for the
         // native composite should this image fail later.
         rtv = targets[0].get();
         context->OMSetRenderTargets(1, &rtv, dsv.get());
         if (private_dsv) context->OMSetDepthStencilState(native_ds.get(), stencil_reference);
         if (uses_depth)
         {
            ID3D11ShaderResourceView* srv = depth.get();
            context->PSSetShaderResources(2, 1, &srv);
         }
         original_constants.Restore(context);
         sampler = original_sampler.get();
         context->PSSetSamplers(15, 1, &sampler);
         context->RSSetViewports(1, &original_vp);
         context->RSSetScissorRects(rect_count, rects);
         context->PSSetShader(original_ps.get(), nullptr, 0);
         draw();
         ++image->draws;
         return true;
      }
   };
} // namespace Battlefront2FullTransparency
