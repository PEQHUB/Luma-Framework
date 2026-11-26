#define GAME_STAR_WARS_BATTLEFRONT_II 1

// DLSS (the project's UseLumaNGX property) replaces the game's TAA High pass.
#define ENABLE_BATTLEFRONT2_DLSS_BACKEND ENABLE_NGX
#define ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK ENABLE_NGX
// The full-resolution replays rebuild game shaders from their original bytecode, which the framework's shader
// cache only keeps with shader dumping compiled in. Dumping itself stays off (auto_dump in DllMain).
#define ALLOW_SHADERS_DUMPING 1
// The LUT snapshot needs the group count of the game's LUT generation dispatch.
#define ENABLE_DRAW_DISPATCH_DATA_CACHE 1
// DLSS and the replays run the game's own draw or dispatch from the draw callback.
#define ENABLE_POST_DRAW_DISPATCH_CALLBACK 1
#define GEOMETRY_SHADER_SUPPORT 0
#define CHECK_GRAPHICS_API_COMPATIBILITY 1
// Saving and restoring state can hand back our replacement shader; map it to the original hashes.
#define ENABLE_GAME_PIPELINE_STATE_READBACK 1
// The game must receive WM_KILLFOCUS/WM_ACTIVATE during Alt+Tab and fullscreen changes.
#define DISABLE_FOCUS_LOSS_SUPPRESSION 1
#define DISABLE_AUTO_DEBUGGER 1
// OnCreateFlipSwapchain selects the flip model, the FP16 output and tearing itself.
#define DISABLE_SWAPCHAIN_FLIP_MODEL 1

#include <algorithm>
#include "game_passes.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "..\..\Core\core.hpp"
#include "engine_patches.h"
#include "engine_fixes.h"
#include "dlss.h"
#include "samplers.h"
#include "tone_mapping.h"
#include "ssr.h"
#include "xegtao.h"
#include "swapchain.h"
#include "ui.h"

// Luma's shared XeGTAO passes (Luma_SWBF2_XeGTAO.hlsl). Indexed by VB-GTAO, then by quality (Battlefront2XeGTAO::quality).
inline constexpr std::array<uint32_t, 2> bf2_xegtao_prefilter_keys = {
   CompileTimeStringHash("Battlefront II XeGTAO Prefilter Depths CS"),
   CompileTimeStringHash("Battlefront II VB-GTAO Prefilter Depths CS")};
inline constexpr std::array<std::array<uint32_t, 2>, 2> bf2_xegtao_main_keys = {{
   {CompileTimeStringHash("Battlefront II XeGTAO Main Pass CS (Medium)"), CompileTimeStringHash("Battlefront II XeGTAO Main Pass CS (High)")},
   {CompileTimeStringHash("Battlefront II VB-GTAO Main Pass CS (Medium)"), CompileTimeStringHash("Battlefront II VB-GTAO Main Pass CS (High)")},
}};
inline constexpr uint32_t bf2_xegtao_denoise_key = CompileTimeStringHash("Battlefront II XeGTAO Denoise CS");
// CORTAO (Luma_SWBF2_CORTAO.hlsl): prepass, main pass per sample count (Battlefront2XeGTAO::cortao_samples), filter.
inline constexpr uint32_t bf2_cortao_prepass_key = CompileTimeStringHash("Battlefront II CORTAO Prepass CS");
inline constexpr std::array<uint32_t, 7> bf2_cortao_main_keys = {
   CompileTimeStringHash("Battlefront II CORTAO Main Pass CS (4)"), CompileTimeStringHash("Battlefront II CORTAO Main Pass CS (6)"),
   CompileTimeStringHash("Battlefront II CORTAO Main Pass CS (8)"), CompileTimeStringHash("Battlefront II CORTAO Main Pass CS (12)"),
   CompileTimeStringHash("Battlefront II CORTAO Main Pass CS (16)"), CompileTimeStringHash("Battlefront II CORTAO Main Pass CS (24)"),
   CompileTimeStringHash("Battlefront II CORTAO Main Pass CS (32)")};
inline constexpr uint32_t bf2_cortao_filter_key = CompileTimeStringHash("Battlefront II CORTAO Filter CS");
// The eye glare passes (Luma_SWBF2_Glare.hlsl, BF2_GLARE_PASS 0 to 3).
inline constexpr std::array<uint32_t, 4> bf2_glare_keys = {
   CompileTimeStringHash("Battlefront II Glare Excess CS"),
   CompileTimeStringHash("Battlefront II Glare Down CS"),
   CompileTimeStringHash("Battlefront II Glare Veil CS"),
   CompileTimeStringHash("Battlefront II Glare Cutoff CS")};

inline constexpr std::array<uint32_t, 12> bf2_video_shader_keys = {
   CompileTimeStringHash("Battlefront II SDR Video PS 0"), CompileTimeStringHash("Battlefront II SDR Video PS 1"),
   CompileTimeStringHash("Battlefront II SDR Video PS 2"), CompileTimeStringHash("Battlefront II SDR Video PS 3"),
   CompileTimeStringHash("Battlefront II SDR Video PS 4"), CompileTimeStringHash("Battlefront II SDR Video PS 5"),
   CompileTimeStringHash("Battlefront II SDR Video PS 6"), CompileTimeStringHash("Battlefront II SDR Video PS 7"),
   CompileTimeStringHash("Battlefront II SDR Video PS 8"), CompileTimeStringHash("Battlefront II SDR Video PS 9"),
   CompileTimeStringHash("Battlefront II SDR Video PS 10"), CompileTimeStringHash("Battlefront II SDR Video PS 11")};

struct Battlefront2GameDeviceData final : public GameDeviceData
{
   ID3D11Device* reflex_device = nullptr; // Not owned; the Reflex backend keeps its own reference.
   Battlefront2SSR::PrivateConstants ssr_constants;
   Battlefront2SSR::FilteredUpsampling ssr_upsampling;
   Battlefront2SSR::SpatialFilter ssr_spatial_filter;
   Battlefront2XeGTAO::State xegtao;
   Battlefront2Psycho::State psycho;
   Battlefront2Glare::State glare;
   Battlefront2Sparks::Layer sparks;
   Battlefront2ACES2::State aces2;
   Battlefront2NativePresentation::State native_presentation;
   Battlefront2Video::State video_presentation;
   Battlefront2NativeLUT::State native_lut;
   Battlefront2FullTransparency::State full_transparency;
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
   int dlss_control_mode = static_cast<int>(Battlefront2DLSSMode::Auto);
   int dlss_quality_mode = 2; // NGX order: Performance, Balanced, Quality, Ultra Performance, Ultra Quality, DLAA.
   bool dlss_preset_auto = true;
   bool native_render_scale_owned = false;
   float native_render_scale_original = 1.f;
   float native_render_scale_observed = 0.f;
   Battlefront2DLSSRecovery::State dlss_recovery;
   Battlefront2DLSSRecovery::Decision dlss_recovery_decision{};
   Battlefront2DLSSRecovery::ScaleLease dlss_scale_lease;
   uint32_t dlss_binding_rejection_logs = 0;
   int dlss_recovery_mode = -1;
   uint32_t dlss_recovery_preset = ~uint32_t(0);
   UINT dlss_last_effective_input_width = 0;
   UINT dlss_last_effective_input_height = 0;
   bool dlss_has_drawn_once = false;
   bool dlss_failure_logged = false;
   bool dlss_setup_logged = false;
   bool dlss_last_evaluation_succeeded = false;
   Battlefront2PostDLSS::State dlss_post;
   Battlefront2ShieldMipBias::State shield_mip_bias;
   // Inputs of this frame's main temporal pass, evaluated by NGX right after the game's own TAA.
   struct TemporalDLSSFrame
   {
      size_t temporal_producer = ~size_t(0);
      bool reset = true;
      uint64_t frame = ~uint64_t(0);
      uint64_t generation = 0;
      uint64_t swapchain_generation = 0;
      float jitter_x = 0.f, jitter_y = 0.f;
      SR::SettingsData settings;
      com_ptr<ID3D11Texture2D> color, depth, motion, native_output;
      // Keeps the scene a scene pair was registered for alive until present, so its raw pointer cannot be reused.
      com_ptr<ID3D11Texture2D> late_scene;
   } dlss_temporal_inputs;
   const char* dlss_temporal_inputs_failure_reason = "none";
   uint64_t dlss_temporal_inputs_failure_log_frame = ~uint64_t(0);
   uint64_t dlss_last_pack_frame = ~uint64_t(0), dlss_last_pack_shader = 0;
   const void* dlss_last_pack_source = nullptr;
   const void* dlss_last_pack_root = nullptr;
   SR::Type dlss_observed_sr_type = SR::Type::None;
   uint64_t dlss_last_temporal_dispatch_frame = ~uint64_t(0);
   // Presented scenes in a row without a TAA High pass (TAA Low or AA off): DLSS has nothing to replace.
   uint32_t dlss_scene_frames_without_taa = 0;
   // Last frame rendered above the display size beyond Frostbite's 4-pixel alignment (Resolution Scale above
   // 100%, which Luma cannot override).
   uint64_t dlss_supersampled_frame = ~uint64_t(0);
   uint64_t dlss_last_evaluated_frame = ~uint64_t(0);
   uint64_t dlss_evaluation_token = 0;
   uint64_t dlss_output_token = 0;
   uint64_t dlss_display_handoff_token = 0;
   // Temporal outputs proven to be the main view: a scene packed from them reached the final composite on the
   // back buffer. The COM references keep a raw pointer from being recycled for another texture.
   static constexpr uint32_t kDLSSConfirmedMainResourceCount = 16;
   std::unordered_map<const void*, com_ptr<ID3D11Resource>> dlss_main_temporal_output_resources;
   std::vector<const void*> dlss_main_temporal_output_resource_order;
   // A full-screen overwrite of this frame: a DOF or motion blur composite, or a scene packer (temporal output
   // to packed scene).
   struct DLSSSceneEdge
   {
      com_ptr<ID3D11Resource> temporal_resource;
      com_ptr<ID3D11Resource> packed_resource;
      uint64_t frame = ~uint64_t(0);
      uint64_t generation = 0;
      bool packer_supported = true;
   };
   Battlefront2SceneLineage::PackSupport dlss_pack_support;
   std::vector<DLSSSceneEdge> dlss_frame_pack_edges;
   std::vector<DLSSSceneEdge> dlss_frame_scene_edges;
   struct FrameTemporalOutput
   {
      com_ptr<ID3D11Resource> resource;
      size_t scene_edge_count = 0;
   };
   std::vector<FrameTemporalOutput> dlss_frame_temporal_outputs;
   uint64_t dlss_frame_edges_frame = ~uint64_t(0);
   uint64_t dlss_main_view_frame = ~uint64_t(0);
   const void* dlss_main_view_resource = nullptr;
   bool dlss_main_view_ambiguous = false;
   uint64_t dlss_main_view_generation = ~uint64_t(0);
   uint64_t dlss_main_view_swapchain_generation = ~uint64_t(0);
   UINT dlss_main_temporal_output_width = 0;
   UINT dlss_main_temporal_output_height = 0;
   UINT dlss_main_temporal_output_format = DXGI_FORMAT_UNKNOWN;
   uint64_t dlss_generation = 0; // Bumped whenever DLSS history restarts; anything from an older one is stale.
   UINT dlss_last_input_width = 0;
   UINT dlss_last_input_height = 0;
   UINT dlss_last_output_width = 0;
   UINT dlss_last_output_height = 0;
   UINT dlss_last_target_width = 0;
   UINT dlss_last_target_height = 0;
   // Size of the swapchain-sized packed scene. It differs from the DLSS target only when Frostbite pads a
   // native-scale render to a multiple of 4 (2052 for a 2049 output): DLAA then reconstructs the whole padded
   // render and the display conversion takes a centered 1:1 crop.
   UINT dlss_last_display_width = 0;
   UINT dlss_last_display_height = 0;
   UINT dlss_display_crop_x = 0;
   UINT dlss_display_crop_y = 0;
   uint64_t dlss_logged_feature_key = 0; // Extents of the last logged feature.
   // The samplers the game bound on the immediate context (stage, slot), before OnPushSamplers
   // replaced them with mip-biased variants.
   com_ptr<ID3D11SamplerState> native_mip_samplers[6][D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
   Battlefront2MipBias::Variants mip_variants;
   UINT dlss_last_swapchain_width = 0;
   UINT dlss_last_swapchain_height = 0;
   // Back buffer size measured at each present; output_resolution can lag behind a resize. It sets the DLSS
   // target size, so evaluation, conversion and the final composite agree on it.
   UINT dlss_present_swapchain_width = 0;
   UINT dlss_present_swapchain_height = 0;
   uint64_t dlss_swapchain_generation = 0;
   // Set from the UI; the render thread applies it at its next draw or dispatch.
   std::atomic<bool> dlss_pending_reset = false;
   UINT dlss_last_source_format = DXGI_FORMAT_UNKNOWN;
   UINT dlss_last_output_format = DXGI_FORMAT_UNKNOWN;
   bool dlss_display_handoff_seen = false;
   uint64_t dlss_display_handoff_frame = ~uint64_t(0);
   com_ptr<ID3D11Texture2D> dlss_display_color;
   com_ptr<ID3D11RenderTargetView> dlss_display_rtv;
   com_ptr<ID3D11ShaderResourceView> dlss_display_srv;
   // The game's final display shader reads the scene size from the first 16 bytes of PS cb0. With the
   // full-resolution scene bound at t0 they must describe it; the rest of cb0 (UI, accessibility, display
   // mapping) is copied from the game's buffer.
   com_ptr<ID3D11Buffer> dlss_final_composite_constants;
   com_ptr<ID3D11Buffer> dlss_final_composite_dimensions;
   UINT dlss_final_composite_constants_size = 0;
   UINT dlss_final_composite_width = 0;
   UINT dlss_final_composite_height = 0;
   com_ptr<ID3D11ShaderResourceView> dlss_output_srv;
   com_ptr<ID3D11UnorderedAccessView> dlss_output_uav;
   uint64_t dlss_output_mips_token = 0;
   uint64_t dlss_output_generation = 0;
   uint64_t dlss_output_consumed_frame = ~uint64_t(0);
   uint32_t dlss_final_rejection_logs = 0;
   uint32_t dlss_final_constants_logs = 0;
   uint32_t dlss_scene_conversion_fail_logs = 0;
   uint32_t dlss_non_immediate_skips = 0;
   uint32_t dlss_main_view_logs = 0;
   // The frame's jitter in render pixels as submitted to NGX (see ReadNativeJitter).
   float dlss_jitter_x = 0.f;
   float dlss_jitter_y = 0.f;
   uint64_t dlss_jitter_frame = ~uint64_t(0);
   uint32_t dlss_jitter_logs = 0;
   bool dlss_jitter_frame_seen = false;
   bool dlss_evaluated_since_reset = false;
   // Scenes that evaluated DLSS this frame. The game can render a second, smaller view in the same frame, so
   // the handoff matches against all of them instead of the last evaluation. Each pair links the temporal
   // texture NGX evaluated to the packed texture its converted scene went into.
   static constexpr uint32_t kDLSSScenePairCount = 6;
   struct DLSSScenePair
   {
      const void* temporal = nullptr;
      const void* packed = nullptr; // Set once the scene is converted.
      uint64_t frame = 0;
      uint64_t generation = 0;
   };
   DLSSScenePair dlss_scene_pairs[kDLSSScenePairCount] = {};
   uint32_t dlss_scene_pair_count = 0;
   // Every conversion rewrites the private display texture; the final composite only takes it for the scene
   // whose conversion it currently holds.
   const void* dlss_display_srv_owner_scene = nullptr;
#endif
};

#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
// Scene pairs are keyed by raw COM pointers and cleared at every present.
static inline void DLSSScenePairClear(Battlefront2GameDeviceData& data)
{
   data.dlss_scene_pair_count = 0;
   data.dlss_display_srv_owner_scene = nullptr;
}

static inline void DLSSMainOutputsClear(Battlefront2GameDeviceData& data)
{
   data.dlss_pack_support = {};
   data.dlss_main_temporal_output_resources.clear();
   data.dlss_main_temporal_output_resource_order.clear();
   data.dlss_main_view_generation = ~uint64_t(0);
   data.dlss_main_view_swapchain_generation = ~uint64_t(0);
   data.dlss_main_temporal_output_width = 0;
   data.dlss_main_temporal_output_height = 0;
   data.dlss_main_temporal_output_format = DXGI_FORMAT_UNKNOWN;
}

static inline void DLSSMainViewClear(Battlefront2GameDeviceData& data)
{
   DLSSMainOutputsClear(data);
   data.dlss_frame_pack_edges.clear();
   data.dlss_frame_edges_frame = ~uint64_t(0);
   data.dlss_main_view_frame = ~uint64_t(0);
   data.dlss_main_view_resource = nullptr;
   data.dlss_main_view_ambiguous = false;
}

static inline void DLSSScenePairPush(
   Battlefront2GameDeviceData& data,
   const void* temporal,
   uint64_t frame,
   uint64_t generation)
{
   if (temporal == nullptr || data.dlss_scene_pair_count >= Battlefront2GameDeviceData::kDLSSScenePairCount)
      return;
   for (uint32_t i = 0; i < data.dlss_scene_pair_count; ++i)
   {
      if (data.dlss_scene_pairs[i].temporal == temporal && data.dlss_scene_pairs[i].frame == frame)
      {
         data.dlss_scene_pairs[i].generation = generation;
         return;
      }
   }
   data.dlss_scene_pairs[data.dlss_scene_pair_count++] = {temporal, nullptr, frame, generation};
}

static inline bool DLSSScenePairContains(
   const Battlefront2GameDeviceData& data,
   const void* temporal,
   uint64_t frame,
   uint64_t generation)
{
   if (temporal == nullptr)
      return false;
   for (uint32_t i = 0; i < data.dlss_scene_pair_count; ++i)
   {
      if (data.dlss_scene_pairs[i].temporal == temporal && data.dlss_scene_pairs[i].frame == frame && data.dlss_scene_pairs[i].generation == generation)
         return true;
   }
   return false;
}

static inline void DLSSScenePairMarkConverted(
   Battlefront2GameDeviceData& data,
   const void* temporal,
   const void* packed,
   uint64_t frame)
{
   for (uint32_t i = 0; i < data.dlss_scene_pair_count; ++i)
   {
      if (data.dlss_scene_pairs[i].temporal == temporal && data.dlss_scene_pairs[i].frame == frame)
      {
         data.dlss_scene_pairs[i].packed = packed;
         return;
      }
   }
}

// Whether a packed scene holds a conversion of this frame's DLSS output.
static inline bool DLSSScenePairContainsPackedConverted(
   const Battlefront2GameDeviceData& data,
   const void* packed,
   uint64_t frame,
   uint64_t generation)
{
   if (packed == nullptr)
      return false;
   for (uint32_t i = 0; i < data.dlss_scene_pair_count; ++i)
   {
      if (data.dlss_scene_pairs[i].packed == packed && data.dlss_scene_pairs[i].frame == frame && data.dlss_scene_pairs[i].generation == generation)
         return true;
   }
   return false;
}

namespace Battlefront2Jitter
{
   // CPU copy of the temporal constant buffer, taken from the game's own uploads (Map/Unmap and
   // UpdateSubresource events), so reading the jitter never maps a GPU buffer. The events fire on the game's
   // threads; this state is guarded by s_mirror_mutex and never touches NGX or DLSS resources.
   struct MirrorEntry
   {
      // The bound offset is only known after the unmap and Frostbite moves it between frames, so keep the
      // whole latest upload; the reader copies out the 240 bytes it needs.
      std::vector<uint8_t> bytes;
      UploadShadow shadow;
      uint64_t offset = 0;
      uint64_t tick = 0, consumed_tick = 0;
   };
   struct PendingMap
   {
      void* data;
      uint64_t offset, size;
      bool redirected = false, discard = false;
   };
   static std::mutex s_mirror_mutex;
   static std::unordered_map<uint64_t, MirrorEntry> s_mirror_entries;
   static std::unordered_map<uint64_t, PendingMap> s_pending_map_ptrs;
   static std::unordered_set<uint64_t> s_watched_handles;
   // The events fire for every upload of the game. This lock-free bit filter keeps unrelated buffers off the
   // mutex; a collision only costs the exact lookup.
   static std::array<std::atomic<uint64_t>, 256> s_watch_filter{};
   // Handles per filter bit (guarded by s_mirror_mutex), so removing one handle keeps a bit another live
   // handle shares.
   static std::array<std::array<size_t, 64>, 256> s_watch_references{};

   static uint64_t WatchHash(uint64_t handle)
   {
      uint64_t hash = handle >> 4;
      hash ^= hash >> 33;
      hash *= 0xff51afd7ed558ccdULL;
      hash ^= hash >> 33;
      return hash;
   }

   static bool MaybeWatched(uint64_t handle)
   {
      const uint64_t hash = WatchHash(handle);
      const uint64_t bit = 1ULL << ((hash >> 8) & 63);
      return (s_watch_filter[hash & 255].load(std::memory_order_acquire) & bit) != 0;
   }

   static void AddWatch(uint64_t handle)
   {
      std::lock_guard<std::mutex> lock(s_mirror_mutex);
      if (s_watched_handles.insert(handle).second)
      {
         const uint64_t hash = WatchHash(handle);
         const size_t bucket = hash & 255, bit = (hash >> 8) & 63;
         if (s_watch_references[bucket][bit]++ == 0)
            s_watch_filter[bucket].fetch_or(1ULL << bit, std::memory_order_release);
      }
   }

   static void RecordMappedPtr(reshade::api::device* device, uint64_t handle,
      void*& data, uint64_t offset, uint64_t size, reshade::api::map_access access)
   {
      if (data == nullptr || access == reshade::api::map_access::read_only || !MaybeWatched(handle))
         return;
      std::lock_guard<std::mutex> lock(s_mirror_mutex);
      if (!s_watched_handles.contains(handle))
         return;
      const auto desc = device->get_resource_desc({handle});
      if (desc.type != reshade::api::resource_type::buffer || offset >= desc.buffer.size)
         return;
      const uint64_t available = desc.buffer.size - offset;
      PendingMap mapped{data, offset, (std::min)(size, available)};
      auto& entry = s_mirror_entries[handle];
      if (offset == 0 && mapped.size == desc.buffer.size && (access == reshade::api::map_access::write_discard || access == reshade::api::map_access::write_only))
      {
         mapped.discard = access == reshade::api::map_access::write_discard;
         if (void* shadow = entry.shadow.Begin(static_cast<size_t>(mapped.size), mapped.discard,
                entry.bytes, entry.offset == 0 && entry.tick != 0))
         {
            data = shadow;
            mapped.redirected = true;
         }
      }
      if (!mapped.redirected) entry.shadow.Invalidate();
      s_pending_map_ptrs[handle] = mapped;
   }

   static void CaptureMapped(reshade::api::device* /*device*/, uint64_t handle)
   {
      if (!MaybeWatched(handle))
         return;
      std::lock_guard<std::mutex> lock(s_mirror_mutex);
      auto pending = s_pending_map_ptrs.find(handle);
      if (pending == s_pending_map_ptrs.end())
         return;
      const PendingMap mapped = pending->second;
      s_pending_map_ptrs.erase(pending);
      auto& entry = s_mirror_entries[handle];
      entry.offset = mapped.offset;
      entry.bytes.resize(static_cast<size_t>(mapped.size));
      if (mapped.redirected)
         entry.shadow.End(mapped.data, mapped.discard, entry.bytes);
      else if (mapped.size != 0)
         CopyMappedUpload(entry.bytes.data(), mapped.data, entry.bytes.size());
      ++entry.tick;
   }

   static void CaptureUpdate(reshade::api::device* device, uint64_t handle, uint64_t offset, const void* src_data, uint64_t size)
   {
      if (src_data == nullptr || size == 0 || !MaybeWatched(handle))
         return;
      std::lock_guard<std::mutex> lock(s_mirror_mutex);
      if (!s_watched_handles.contains(handle)) return;
      const auto desc = device->get_resource_desc({handle});
      if (desc.type != reshade::api::resource_type::buffer || offset >= desc.buffer.size) return;
      // ReShade uses UINT64_MAX for a whole-buffer UpdateSubresource.
      const size_t byte_count = static_cast<size_t>((std::min)(size, desc.buffer.size - offset));
      auto& entry = s_mirror_entries[handle];
      entry.shadow.Invalidate();
      entry.offset = offset;
      entry.bytes.resize(byte_count);
      std::memcpy(entry.bytes.data(), src_data, byte_count);
      ++entry.tick;
   }

   static void RemoveWatch(uint64_t handle)
   {
      if (!MaybeWatched(handle)) return;
      std::lock_guard<std::mutex> lock(s_mirror_mutex);
      s_pending_map_ptrs.erase(handle);
      s_mirror_entries.erase(handle);
      if (s_watched_handles.erase(handle) != 0)
      {
         const uint64_t hash = WatchHash(handle);
         const size_t bucket = hash & 255, bit = (hash >> 8) & 63;
         if (--s_watch_references[bucket][bit] == 0)
            s_watch_filter[bucket].fetch_and(~(1ULL << bit), std::memory_order_release);
      }
   }

   template <size_t FloatCount>
   static bool ReadFromMirror(uint64_t handle, size_t offset, std::array<float, FloatCount>& out_floats)
   {
      std::lock_guard<std::mutex> lock(s_mirror_mutex);
      auto found = s_mirror_entries.find(handle);
      if (found == s_mirror_entries.end())
         return false;
      auto& entry = found->second;
      constexpr size_t byte_count = FloatCount * sizeof(float);
      if (entry.tick == entry.consumed_tick || offset < entry.offset || offset - entry.offset > entry.bytes.size() || byte_count > entry.bytes.size() - static_cast<size_t>(offset - entry.offset))
         return false;
      std::memcpy(out_floats.data(), entry.bytes.data() + offset - entry.offset, byte_count);
      entry.consumed_tick = entry.tick;
      return true;
   }

   struct Sample final
   {
      const char* failure = "unknown";
      float jitter_x = 0.f;
      float jitter_y = 0.f;
      float native_matrix_delta_x = 0.f;
      float native_matrix_delta_y = 0.f;
      UINT first_constant = 0;
      UINT num_constants = 0;
   };

   // Decodes the game's jitter of this frame from the temporal pass's constants, in render pixels. The scene
   // is already rendered at this point, so the constants are left as they are. NGX gets the shift the jitter gives
   // the picture (+y down): registering consecutive captured frames matched the submitted value in every pair.
   bool ReadNativeJitter(
      ID3D11DeviceContext* native_device_context,
      UINT input_width,
      UINT input_height,
      Sample& sample)
   {
      if (native_device_context == nullptr || input_width == 0 || input_height == 0)
      {
         sample.failure = "missing-context-or-size";
         return false;
      }

      com_ptr<ID3D11Buffer> temporal_cb;
      native_device_context->CSGetConstantBuffers(0, 1, &temporal_cb);
      if (temporal_cb == nullptr)
      {
         sample.failure = "missing-cs-cb0";
         return false;
      }

      UINT first_constant = 0;
      UINT num_constants = 0;
      com_ptr<ID3D11DeviceContext1> native_device_context_1;
      if (SUCCEEDED(native_device_context->QueryInterface(&native_device_context_1)) && native_device_context_1 != nullptr)
      {
         com_ptr<ID3D11Buffer> ranged_cb;
         native_device_context_1->CSGetConstantBuffers1(0, 1, &ranged_cb, &first_constant, &num_constants);
         if (ranged_cb != nullptr)
            temporal_cb = ranged_cb;
      }
      sample.first_constant = first_constant;
      sample.num_constants = num_constants;

      // Read the game's CPU copy of the constants, or else the mirror of its last upload; never the GPU buffer
      // (a D3D11_MAP_READ would stall). A new buffer falls back to native TAA until its next upload.
      std::array<float, 60> temporal_cb_data{};
      const uint64_t temporal_cb_handle = reinterpret_cast<uint64_t>(temporal_cb.get());
      bool fresh = Battlefront2NativeTemporal::Read(native_device_context, input_width, input_height, num_constants ? num_constants : 4096, temporal_cb_data);
      if (!fresh)
      {
         AddWatch(temporal_cb_handle);
         fresh = ReadFromMirror(temporal_cb_handle, static_cast<size_t>(first_constant) * 16, temporal_cb_data);
      }
      if (!fresh)
      {
         sample.failure = "cb-mirror-awaiting-upload";
         return false;
      }
      // The bound range (first_constant is an offset into the buffer, not a register) holds two inverse
      // view-projections at c7..c10 and c11..c14. The shader names them g_invJitteredViewProjection and
      // g_invUnjitteredViewProjection, but the game fills them the other way round: with a still camera c7..c10
      // stays constant and c11..c14 moves by the jitter every frame (measured).
      if (num_constants != 0 && num_constants < 15)
      {
         sample.failure = "cb-range-clips-matrix-layout";
         return false;
      }
      sample.native_matrix_delta_x = temporal_cb_data[47] - temporal_cb_data[31];
      sample.native_matrix_delta_y = temporal_cb_data[51] - temporal_cb_data[35];

      // The difference of the two matrices' last columns (jittered minus unjittered) is minus the NDC jitter,
      // expressed in the camera's two screen basis columns. Solve for both together: m00 and m11 approach zero as
      // the camera rotates, so they cannot be used as divisors.
      double basis_x_sq = 0.0;
      double basis_y_sq = 0.0;
      double basis_xy = 0.0;
      double delta_x = 0.0;
      double delta_y = 0.0;
      for (size_t row = 0; row < 4; ++row)
      {
         const double basis_x = temporal_cb_data[44 + row * 4];
         const double basis_y = temporal_cb_data[45 + row * 4];
         const double delta = static_cast<double>(temporal_cb_data[47 + row * 4]) - temporal_cb_data[31 + row * 4];
         basis_x_sq += basis_x * basis_x;
         basis_y_sq += basis_y * basis_y;
         basis_xy += basis_x * basis_y;
         delta_x += basis_x * delta;
         delta_y += basis_y * delta;
      }
      const double determinant = basis_x_sq * basis_y_sq - basis_xy * basis_xy;
      if (!std::isfinite(determinant) || !std::isfinite(delta_x) || !std::isfinite(delta_y) || determinant <= 1.0e-12 * basis_x_sq * basis_y_sq)
      {
         sample.failure = "jitter-basis-invalid";
         return false;
      }

      const float native_ndc_x = static_cast<float>((delta_x * basis_y_sq - delta_y * basis_xy) / determinant);
      const float native_ndc_y = static_cast<float>((delta_y * basis_x_sq - delta_x * basis_xy) / determinant);
      const float native_direct_jitter_x = native_ndc_x * static_cast<float>(input_width) * 0.5f;
      const float native_direct_jitter_y = native_ndc_y * static_cast<float>(input_height) * 0.5f;
      // The picture shift in pixels: x keeps the jitter's sign (minus the decoded value), y flips it because NDC y
      // points up and pixel rows down.
      const float native_jitter_x = -native_direct_jitter_x;
      const float native_jitter_y = native_direct_jitter_y;
      // NGX needs each component within half a render pixel. Frostbite's 64-frame Halton(2,3) sequence starts
      // exactly on the (-0.5, -0.5) corner, and the float decode of that sample at 3840 wide lands just outside:
      // a 0.0001 px bound dropped it every few seconds in DLAA (a native frame and a history reset each time).
      // All other samples stay below 0.49, so 0.01 px absorbs the noise and still rejects larger offsets.
      constexpr float jitter_decode_tolerance = 0.01f;
      if (!std::isfinite(native_ndc_x) || !std::isfinite(native_ndc_y) || !std::isfinite(native_jitter_x) || !std::isfinite(native_jitter_y) || std::fabs(native_jitter_x) > 0.5f + jitter_decode_tolerance || std::fabs(native_jitter_y) > 0.5f + jitter_decode_tolerance)
      {
         sample.failure = "jitter-pixel-value-out-of-range";
         return false;
      }
      sample.jitter_x = (std::clamp)(native_jitter_x, -0.5f, 0.5f);
      sample.jitter_y = (std::clamp)(native_jitter_y, -0.5f, 0.5f);
      return true;
   }
} // namespace Battlefront2Jitter
#endif

class Battlefront2Game final : public Game
{
   std::atomic_bool full_resolution_transparency{true};
   Battlefront2SSR::NativeResolution ssr_resolution;
   Battlefront2SSR::NativeResolution ssr_velocity{"ScreenSpaceRaytraceUseVelocityVectorsForTemporal", 0x24B};
   Battlefront2DepthDither::State depth_dither;
   Battlefront2EngineFixes::Fixes engine_fixes;

public:
   ~Battlefront2Game() override
   {
      engine_fixes.Restore();
      depth_dither.Update(false);
      ssr_resolution.Update(false);
      ssr_velocity.Update(false);
   }

private:
   // The DLSS mode as loaded from the config; each device copies it when it is created.
   int dlss_control_mode = static_cast<int>(Battlefront2DLSSMode::Auto);
   int dlss_quality_mode = 2;
   bool dlss_preset_auto = true;

   static Battlefront2GameDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<Battlefront2GameDeviceData*>(device_data.game);
   }

   // Saved as NGX preset IDs, so a new menu entry cannot change a saved choice.
   inline static constexpr unsigned int dlss_render_preset_ids[] = {0, 5, 6, 10, 11, 12, 13};
   inline static constexpr const char* dlss_render_preset_names[] = {"Auto", "E", "F", "J", "K", "L", "M"};
   // NVIDIA's default preset for Ultra Performance (nvsdk_ngx_defs.h).
   inline static constexpr unsigned int dlss_ultra_performance_preset = 12;
   std::atomic_uint dlss_render_preset{0};
   std::atomic<bool> dlss_disable_dof{true};

   static size_t DLSSRenderPresetIndex(unsigned int preset)
   {
      for (size_t i = 0; i < std::size(dlss_render_preset_ids); ++i)
         if (dlss_render_preset_ids[i] == preset) return i;
      return 0;
   }

   void ApplyDLSSRenderPreset(DeviceData& device_data, unsigned int preset)
   {
      preset = dlss_render_preset_ids[DLSSRenderPresetIndex(preset)];
      if (dlss_render_preset.exchange(preset) == preset) return;
      GetGameDeviceData(device_data).dlss_pending_reset = true;
      reshade::set_config_value(nullptr, NAME, "BattlefrontIIDLSSRenderPreset", preset);
   }

   static const char* GetDLSSQualityModeName(int mode)
   {
      constexpr const char* names[] = {"Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
      return mode >= 0 && mode < int(std::size(names)) ? names[mode] : "Quality";
   }

   // Indexed by Battlefront2DLSSMode.
   static const char* GetDLSSControlModeName(int mode)
   {
      constexpr const char* names[] = {"Off", "Auto", "Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality", "DLAA"};
      return mode >= 0 && mode < int(std::size(names)) ? names[mode] : "Auto";
   }

   static void UpdateNativeRenderScale(DeviceData& device_data)
   {
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      auto& data = GetGameDeviceData(device_data);
      Battlefront2RenderScale::Snapshot snapshot;
      if (!Battlefront2RenderScale::Read(snapshot)) return;
      data.native_render_scale_observed = snapshot.scale;
      const bool enabled = device_data.sr_type == SR::Type::DLSS && data.dlss_control_mode != static_cast<int>(Battlefront2DLSSMode::Off) && data.dlss_recovery_decision.override_scale;
      float requested = snapshot.scale;
      if (enabled)
      {
         const UINT width = static_cast<UINT>(device_data.output_resolution.x);
         const UINT height = static_cast<UINT>(device_data.output_resolution.y);
         if (width == 0 || height == 0) return;
         const int mode = data.dlss_preset_auto ? 2 : data.dlss_quality_mode;
         // Frostbite accepts one uniform scale and renders align4(ceil(output * scale)) per axis, stretching its view
         // over that size.
         requested = Battlefront2UI::dlss_quality_scales[(std::clamp)(mode, 0, 5)];
         // At full scale a side that is not a multiple of 4 would render up to 3 pixels larger than the output, which
         // DLSS cannot downscale. Round such sides down instead (still inside NGX's DLAA range): DLSS then maps the
         // whole render onto the output, where a crop of the padded render would shift the edges by up to 1.5 px.
         if (requested >= 1.f)
            for (const UINT size : {width, height})
               if (size % 4 != 0) requested = (std::min)(requested, (static_cast<float>(size & ~3u) - 0.01f) / static_cast<float>(size));
         requested = (std::clamp)(requested, 0.25f, 1.f);
      }
      const auto plan = data.dlss_scale_lease.Prepare(snapshot.settings, snapshot.scale, enabled, requested);
      if (plan.write)
      {
         const bool written = Battlefront2RenderScale::Write(snapshot, plan.target);
         data.dlss_scale_lease.Complete(plan, written);
         if (!written) return;
         device_data.force_reset_sr = true;
      }
      data.native_render_scale_owned = data.dlss_scale_lease.Owned();
      data.native_render_scale_original = data.dlss_scale_lease.Original();
#endif
   }

   // Gives the game back its own render scale before the device data that remembers it is destroyed;
   // otherwise the next device would take the reduced scale for the game's own.
   static void ReleaseNativeRenderScale(Battlefront2GameDeviceData& data)
   {
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      if (!data.dlss_scale_lease.Owned()) return;
      Battlefront2RenderScale::Snapshot snapshot;
      const bool readable = Battlefront2RenderScale::Read(snapshot);
      const auto plan = readable ? data.dlss_scale_lease.Prepare(snapshot.settings, snapshot.scale, false, 0.f)
                                 : Battlefront2DLSSRecovery::ScaleLease::Plan{};
      const bool written = plan.write && plan.restore && Battlefront2RenderScale::Write(snapshot, plan.target);
      data.dlss_scale_lease.Complete(plan, written);
      if (!written)
      {
         std::ostringstream message;
         message << "[Battlefront II Luma] Frostbite render scale release (device destroyed): ";
         if (!readable)
            message << "settings unavailable; nothing written.";
         else
            message << "ownership no longer matched the live settings; native value " << snapshot.scale << " retained.";
         reshade::log::message(readable ? reshade::log::level::warning : reshade::log::level::info, message.str().c_str());
      }
#else
      (void)data;
#endif
   }

   void ApplyDLSSControlMode(int mode)
   {
      dlss_control_mode = (std::clamp)(mode,
         static_cast<int>(Battlefront2DLSSMode::Off),
         static_cast<int>(Battlefront2DLSSMode::DLAA));
      dlss_preset_auto = dlss_control_mode == static_cast<int>(Battlefront2DLSSMode::Auto);
      // The explicit modes follow the NGX quality order from Performance on; Off and Auto keep Quality.
      constexpr int performance = static_cast<int>(Battlefront2DLSSMode::Performance);
      dlss_quality_mode = dlss_control_mode >= performance ? dlss_control_mode - performance : 2;
   }

#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
   static void ResetDLSSState(DeviceData& device_data)
   {
      auto& game_device_data = GetGameDeviceData(device_data);
      ++game_device_data.dlss_generation;
      game_device_data.dlss_temporal_inputs = {};
      game_device_data.dlss_post.Reset();
      game_device_data.dlss_frame_temporal_outputs.clear();
      game_device_data.dlss_has_drawn_once = false;
      game_device_data.dlss_failure_logged = false;
      game_device_data.dlss_setup_logged = false;
      game_device_data.dlss_last_evaluation_succeeded = false;
      game_device_data.dlss_observed_sr_type = device_data.sr_type;
      game_device_data.dlss_last_evaluated_frame = ~uint64_t(0);
      game_device_data.dlss_last_temporal_dispatch_frame = ~uint64_t(0);
      game_device_data.dlss_evaluation_token = 0;
      game_device_data.dlss_output_token = 0;
      game_device_data.dlss_display_handoff_token = 0;
      DLSSScenePairClear(game_device_data);
      DLSSMainViewClear(game_device_data);
      game_device_data.dlss_last_input_width = 0;
      game_device_data.dlss_last_input_height = 0;
      game_device_data.dlss_last_output_width = 0;
      game_device_data.dlss_last_output_height = 0;
      game_device_data.dlss_last_target_width = 0;
      game_device_data.dlss_last_target_height = 0;
      game_device_data.dlss_last_display_width = 0;
      game_device_data.dlss_last_display_height = 0;
      game_device_data.dlss_display_crop_x = 0;
      game_device_data.dlss_display_crop_y = 0;
      game_device_data.dlss_last_source_format = DXGI_FORMAT_UNKNOWN;
      game_device_data.dlss_last_output_format = DXGI_FORMAT_UNKNOWN;
      game_device_data.dlss_display_handoff_seen = false;
      game_device_data.dlss_display_handoff_frame = ~uint64_t(0);
      game_device_data.dlss_display_color = nullptr;
      game_device_data.dlss_display_rtv = nullptr;
      game_device_data.dlss_display_srv = nullptr;
      game_device_data.dlss_final_composite_constants = nullptr;
      game_device_data.dlss_final_composite_dimensions = nullptr;
      game_device_data.dlss_final_composite_constants_size = 0;
      game_device_data.dlss_final_composite_width = 0;
      game_device_data.dlss_final_composite_height = 0;
      game_device_data.dlss_output_srv = nullptr;
      game_device_data.dlss_output_uav = nullptr;
      game_device_data.dlss_output_mips_token = 0;
      game_device_data.psycho.Reset();
      game_device_data.glare.Reset();
      game_device_data.sparks.Reset();
      game_device_data.aces2.Reset();
      game_device_data.native_presentation.Reset();
      game_device_data.video_presentation.Reset();
      game_device_data.dlss_output_generation = 0;
      game_device_data.dlss_output_consumed_frame = ~uint64_t(0);
      game_device_data.dlss_final_rejection_logs = 0;
      game_device_data.dlss_final_constants_logs = 0;
      game_device_data.dlss_scene_conversion_fail_logs = 0;
      game_device_data.dlss_non_immediate_skips = 0;
      game_device_data.dlss_jitter_x = 0.f;
      game_device_data.dlss_jitter_y = 0.f;
      game_device_data.dlss_jitter_frame = ~uint64_t(0);
      game_device_data.dlss_jitter_logs = 0;
      game_device_data.dlss_jitter_frame_seen = false;
      game_device_data.dlss_evaluated_since_reset = false;
      device_data.sr_output_color = nullptr;

      const UINT output_width = device_data.output_resolution.x > 1.f ? static_cast<UINT>(device_data.output_resolution.x) : 0;
      const UINT output_height = device_data.output_resolution.y > 1.f ? static_cast<UINT>(device_data.output_resolution.y) : 0;
      game_device_data.dlss_last_swapchain_width = output_width;
      game_device_data.dlss_last_swapchain_height = output_height;

      device_data.force_reset_sr = true;
      device_data.sr_suppressed = false;
      device_data.has_drawn_sr = false;
      device_data.taa_detected = false;
   }

   // format < 0 leaves the format out of the message.
   static void LogCreationFailure(const char* what, UINT width, UINT height, int format, HRESULT hr)
   {
      std::ostringstream failure;
      failure << "[Battlefront II Luma] Full-resolution " << what << " creation failed: size=" << width << 'x' << height;
      if (format >= 0)
         failure << " format=" << format;
      failure << " hr=0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr) << ".";
      reshade::log::message(reshade::log::level::warning, failure.str().c_str());
   }

   static bool EnsureFullResolutionDLSSOutput(
      ID3D11Device* native_device,
      DeviceData& device_data,
      Battlefront2GameDeviceData& game_device_data,
      const D3D11_TEXTURE2D_DESC& source_desc,
      UINT output_width,
      UINT output_height)
   {
      if (native_device == nullptr || output_width == 0 || output_height == 0)
         return false;

      if (device_data.sr_output_color != nullptr && game_device_data.dlss_output_srv != nullptr && game_device_data.dlss_output_uav != nullptr)
      {
         D3D11_TEXTURE2D_DESC existing_desc = {};
         device_data.sr_output_color->GetDesc(&existing_desc);
         if (existing_desc.Width == output_width && existing_desc.Height == output_height && existing_desc.Format == source_desc.Format && (existing_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 && (existing_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 && (existing_desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && (existing_desc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) != 0)
         {
            return true;
         }
      }

      device_data.sr_output_color = nullptr;
      game_device_data.dlss_output_srv = nullptr;
      game_device_data.dlss_output_uav = nullptr;
      game_device_data.dlss_output_mips_token = 0;

      UINT format_support = 0;
      if (FAILED(native_device->CheckFormatSupport(source_desc.Format, &format_support)) || (format_support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) == 0)
         return false;

      D3D11_TEXTURE2D_DESC output_desc = source_desc;
      output_desc.Width = output_width;
      output_desc.Height = output_height;
      output_desc.MipLevels = 0; // Complete pyramid; NGX writes only subresource 0.
      output_desc.ArraySize = 1;
      output_desc.SampleDesc.Count = 1;
      output_desc.SampleDesc.Quality = 0;
      output_desc.Usage = D3D11_USAGE_DEFAULT;
      output_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      output_desc.CPUAccessFlags = 0;
      output_desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

      const int format = static_cast<int>(output_desc.Format);
      HRESULT hr = native_device->CreateTexture2D(&output_desc, nullptr, &device_data.sr_output_color);
      if (FAILED(hr) || device_data.sr_output_color == nullptr)
      {
         LogCreationFailure("DLSS target", output_width, output_height, format, hr);
         return false;
      }

      D3D11_SHADER_RESOURCE_VIEW_DESC output_view = {};
      output_view.Format = output_desc.Format;
      output_view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      output_view.Texture2D.MipLevels = 1;
      hr = native_device->CreateShaderResourceView(device_data.sr_output_color.get(), &output_view, &game_device_data.dlss_output_srv);
      if (FAILED(hr) || game_device_data.dlss_output_srv == nullptr)
      {
         LogCreationFailure("DLSS SRV", output_width, output_height, format, hr);
         device_data.sr_output_color = nullptr;
         return false;
      }

      hr = native_device->CreateUnorderedAccessView(device_data.sr_output_color.get(), nullptr, &game_device_data.dlss_output_uav);
      if (FAILED(hr) || game_device_data.dlss_output_uav == nullptr)
      {
         LogCreationFailure("DLSS UAV", output_width, output_height, format, hr);
         device_data.sr_output_color = nullptr;
         game_device_data.dlss_output_srv = nullptr;
         return false;
      }
      return true;
   }

   static bool EnsureFullResolutionDLSSDisplayTarget(
      ID3D11Device* native_device,
      Battlefront2GameDeviceData& game_device_data,
      UINT output_width,
      UINT output_height)
   {
      if (native_device == nullptr || output_width == 0 || output_height == 0)
         return false;

      if (game_device_data.dlss_display_color != nullptr && game_device_data.dlss_display_rtv != nullptr && game_device_data.dlss_display_srv != nullptr)
      {
         D3D11_TEXTURE2D_DESC existing_desc = {};
         game_device_data.dlss_display_color->GetDesc(&existing_desc);
         if (existing_desc.Width == output_width && existing_desc.Height == output_height && existing_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
         {
            return true;
         }
      }

      game_device_data.dlss_display_color = nullptr;
      game_device_data.dlss_display_rtv = nullptr;
      game_device_data.dlss_display_srv = nullptr;

      D3D11_TEXTURE2D_DESC display_desc = {};
      display_desc.Width = output_width;
      display_desc.Height = output_height;
      display_desc.MipLevels = 1;
      display_desc.ArraySize = 1;
      display_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      display_desc.SampleDesc.Count = 1;
      display_desc.Usage = D3D11_USAGE_DEFAULT;
      display_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

      HRESULT hr = native_device->CreateTexture2D(&display_desc, nullptr, &game_device_data.dlss_display_color);
      if (FAILED(hr) || game_device_data.dlss_display_color == nullptr)
      {
         LogCreationFailure("packed display target", output_width, output_height, static_cast<int>(display_desc.Format), hr);
         return false;
      }

      hr = native_device->CreateRenderTargetView(game_device_data.dlss_display_color.get(), nullptr, &game_device_data.dlss_display_rtv);
      if (FAILED(hr) || game_device_data.dlss_display_rtv == nullptr)
      {
         LogCreationFailure("packed display RTV", output_width, output_height, -1, hr);
         game_device_data.dlss_display_color = nullptr;
         return false;
      }

      hr = native_device->CreateShaderResourceView(game_device_data.dlss_display_color.get(), nullptr, &game_device_data.dlss_display_srv);
      if (FAILED(hr) || game_device_data.dlss_display_srv == nullptr)
      {
         LogCreationFailure("packed display SRV", output_width, output_height, -1, hr);
         game_device_data.dlss_display_color = nullptr;
         game_device_data.dlss_display_rtv = nullptr;
         return false;
      }
      return true;
   }

   // The game's own TAA has already run for this frame; a failure keeps its result.
   static void FailTemporalDLSS(DeviceData& device_data, Battlefront2GameDeviceData& data, const char* reason)
   {
      data.dlss_output_token = 0;
      data.dlss_display_handoff_token = 0;
      data.dlss_display_handoff_seen = false;
      data.dlss_last_evaluation_succeeded = false;
      data.dlss_has_drawn_once = false;
      device_data.has_drawn_sr = false;
      device_data.force_reset_sr = true;
      const uint64_t frame = cb_luma_global_settings.FrameIndex;
      if (std::strcmp(data.dlss_temporal_inputs_failure_reason, reason) != 0 || data.dlss_temporal_inputs_failure_log_frame == ~uint64_t(0) || frame - data.dlss_temporal_inputs_failure_log_frame >= 120)
      {
         data.dlss_temporal_inputs_failure_reason = reason;
         data.dlss_temporal_inputs_failure_log_frame = frame;
         std::ostringstream log;
         log << "[Battlefront II Luma] Temporal DLSS failed: reason=" << reason
             << " frame=" << frame
             << " queued_output=" << data.dlss_temporal_inputs.native_output.get()
             << " pack_frame=" << data.dlss_last_pack_frame
             << " pack_ps=" << std::hex << data.dlss_last_pack_shader << std::dec
             << " pack_source=" << data.dlss_last_pack_source
             << " pack_root=" << data.dlss_last_pack_root
             << " scene_edges=" << data.dlss_frame_scene_edges.size()
             << "; native TAA retained for this evaluation.";
         reshade::log::message(reshade::log::level::warning, log.str().c_str());
      }
   }

   static void EvaluateTemporalDLSS(
      ID3D11Device* native_device, ID3D11DeviceContext* native_device_context,
      DeviceData& device_data, Battlefront2GameDeviceData& game_device_data)
   {
      const auto& pending = game_device_data.dlss_temporal_inputs;
      if (pending.frame != cb_luma_global_settings.FrameIndex || pending.generation != game_device_data.dlss_generation || pending.swapchain_generation != game_device_data.dlss_swapchain_generation || device_data.sr_type != SR::Type::DLSS)
      {
         FailTemporalDLSS(device_data, game_device_data, "stale-contract");
         return;
      }
      auto* sr_instance_data = device_data.GetSRInstanceData();
      auto sr_implementation = sr_implementations.find(device_data.sr_type);
      if (sr_instance_data == nullptr || sr_implementation == sr_implementations.end() || sr_implementation->second == nullptr)
      {
         FailTemporalDLSS(device_data, game_device_data, "missing-implementation");
         return;
      }
      const SR::SettingsData& settings_data = pending.settings;
      const UINT feature_render_width = settings_data.render_width;
      const UINT feature_render_height = settings_data.render_height;
      const UINT dlss_target_width = settings_data.output_width, dlss_target_height = settings_data.output_height;
      D3D11_TEXTURE2D_DESC output_desc = {};
      pending.native_output->GetDesc(&output_desc);
      const UINT output_width = output_desc.Width, output_height = output_desc.Height;

      // NGX changes both pipelines; save them, ranged constant buffers included.
      DrawStateStack<DrawStateStackType::FullGraphics> graphics_state_stack;
      DrawStateStack<DrawStateStackType::Compute> compute_state_stack;
      graphics_state_stack.Cache(native_device_context, device_data.uav_max_count);
      compute_state_stack.Cache(native_device_context, device_data.uav_max_count);
      if (!sr_implementation->second->UpdateSettings(sr_instance_data, native_device_context, settings_data))
      {
         compute_state_stack.Restore(native_device_context, device_data.uav_max_count);
         graphics_state_stack.Restore(native_device_context);
         FailTemporalDLSS(device_data, game_device_data, "ngx-settings");
         return;
      }
      {
         const uint64_t feature_key = (uint64_t(feature_render_width) << 32) ^ (uint64_t(feature_render_height) << 16) ^ (uint64_t(dlss_target_width) << 8) ^ dlss_target_height;
         if (game_device_data.dlss_logged_feature_key != feature_key)
         {
            game_device_data.dlss_logged_feature_key = feature_key;
            std::ostringstream feature;
            feature << "[Battlefront II Luma] NGX feature active:"
                    << " render=" << feature_render_width << 'x' << feature_render_height
                    << " output=" << dlss_target_width << 'x' << dlss_target_height
                    << " display=" << game_device_data.dlss_last_display_width << 'x' << game_device_data.dlss_last_display_height
                    << " crop=" << game_device_data.dlss_display_crop_x << ',' << game_device_data.dlss_display_crop_y
                    << " menu_mode=" << GetDLSSControlModeName(game_device_data.dlss_control_mode)
                    << " preset=" << dlss_render_preset_names[DLSSRenderPresetIndex(settings_data.render_preset)];
            // NGX loads the DLSS runtime when the feature is created (the game folder first).
            if (HMODULE runtime = GetModuleHandleW(L"nvngx_dlss.dll"))
               feature << " runtime=" << System::GetModulePath(runtime).string();
            feature << '.';
            reshade::log::message(reshade::log::level::info, feature.str().c_str());
         }
      }
      SR::SuperResolutionImpl::DrawData draw_data;
      // A skipped or failed evaluation since the last one breaks the history: reset it rather than pair a multi-frame
      // step with one frame of motion vectors.
      const uint64_t evaluation_frame = cb_luma_global_settings.FrameIndex;
      const bool previous_evaluation_is_adjacent_and_valid = evaluation_frame > 0 && game_device_data.dlss_last_evaluated_frame == evaluation_frame - 1 && game_device_data.dlss_last_evaluation_succeeded;
      const bool evaluation_gap_requires_reset = game_device_data.dlss_evaluated_since_reset && !previous_evaluation_is_adjacent_and_valid;
      game_device_data.dlss_evaluated_since_reset = true;
      draw_data.source_color = pending.color.get();
      draw_data.output_color = device_data.sr_output_color.get();
      draw_data.motion_vectors = pending.motion.get();
      draw_data.depth_buffer = pending.depth.get();
      draw_data.reset = device_data.force_reset_sr || pending.reset || evaluation_gap_requires_reset || game_device_data.dlss_last_effective_input_width != feature_render_width || game_device_data.dlss_last_effective_input_height != feature_render_height || game_device_data.dlss_last_output_width != output_width || game_device_data.dlss_last_output_height != output_height;
      draw_data.render_width = feature_render_width;
      draw_data.render_height = feature_render_height;
      draw_data.jitter_x = pending.jitter_x;
      draw_data.jitter_y = pending.jitter_y;
      draw_data.frame_index = cb_luma_global_settings.FrameIndex;

      game_device_data.dlss_last_evaluated_frame = cb_luma_global_settings.FrameIndex;
      const uint64_t evaluation_token = ++game_device_data.dlss_evaluation_token;
      const bool dlss_succeeded = sr_implementation->second->Draw(sr_instance_data, native_device_context, draw_data);
      compute_state_stack.Restore(native_device_context, device_data.uav_max_count);
      graphics_state_stack.Restore(native_device_context);

      if (dlss_succeeded)
      {
         device_data.has_drawn_sr = true;
         device_data.force_reset_sr = false;
         game_device_data.dlss_has_drawn_once = true;
         game_device_data.dlss_output_generation = game_device_data.dlss_generation;
         game_device_data.dlss_output_token = evaluation_token;
         game_device_data.dlss_last_evaluation_succeeded = true;
      }
      else
      {
         FailTemporalDLSS(device_data, game_device_data, "ngx-evaluation");
         if (!game_device_data.dlss_failure_logged)
         {
            game_device_data.dlss_failure_logged = true;
            reshade::log::message(reshade::log::level::warning,
               "[Battlefront II Luma] Temporal NGX failed; native TAA retained; history restarts after the missed evaluation.");
         }
      }
   }

   // The DLSS scene with mips, for the distortion packers. The mips are built from mip 0 once per evaluation,
   // without another full-resolution copy.
   static ID3D11ShaderResourceView* DLSSOutputMips(ID3D11DeviceContext* context, Battlefront2GameDeviceData& data)
   {
      if (data.dlss_output_mips_token != data.dlss_output_token)
      {
         context->GenerateMips(data.dlss_post.output_mips.get());
         data.dlss_output_mips_token = data.dlss_output_token;
      }
      return data.dlss_post.output_mips.get();
   }

   static bool DrawDLSSSceneConversion(
      ID3D11DeviceContext* native_device_context,
      DeviceData& device_data,
      Battlefront2GameDeviceData& game_device_data,
      ID3D11ShaderResourceView* dlss_scene,
      ID3D11RenderTargetView* packed_target,
      UINT output_width,
      UINT output_height,
      UINT crop_x,
      UINT crop_y,
      uint64_t pixel_shader_hash)
   {
      const auto* recipe = Battlefront2ScenePacker::Find(pixel_shader_hash);
      if (native_device_context == nullptr || dlss_scene == nullptr || packed_target == nullptr || output_width == 0 || output_height == 0 || !recipe)
         return false;

      auto scene_vs = device_data.native_vertex_shaders[CompileTimeStringHash("Battlefront II DLSS Scene Conversion VS")];
      if (scene_vs == nullptr)
         return false;

      com_ptr<ID3D11PixelShader> distortion_ps;
      if (Battlefront2ScenePacker::Distorted(pixel_shader_hash))
      {
         distortion_ps = device_data.native_pixel_shaders[recipe->ShaderKey(Battlefront2ScenePacker::Path::DLSS)];
         if (distortion_ps == nullptr || game_device_data.dlss_post.output_mips == nullptr || game_device_data.dlss_output_token == 0)
            return false;
      }

      DrawStateStack<DrawStateStackType::FullGraphics> state;
      state.Cache(native_device_context, device_data.uav_max_count);

      // DrawStateStack does not save the GS/HS/DS stages; the fullscreen draw unbinds them and restores them after.
      com_ptr<ID3D11GeometryShader> previous_gs;
      com_ptr<ID3D11HullShader> previous_hs;
      com_ptr<ID3D11DomainShader> previous_ds;
      native_device_context->GSGetShader(&previous_gs, nullptr, nullptr);
      native_device_context->HSGetShader(&previous_hs, nullptr, nullptr);
      native_device_context->DSGetShader(&previous_ds, nullptr, nullptr);

      // The packer keeps its own inputs (exposure, LUT, bloom, ...); only the scene at t0 is replaced.
      com_ptr<ID3D11ShaderResourceView> native_srvs[6];
      native_device_context->PSGetShaderResources(0, ARRAYSIZE(native_srvs), &native_srvs[0]);
      for (UINT slot = 0; slot < recipe->RequiredInputs(); ++slot)
      {
         if (native_srvs[slot]) continue;
         state.Restore(native_device_context);
         return false;
      }

      com_ptr<ID3D11PixelShader> native_ps;
      native_device_context->PSGetShader(&native_ps, nullptr, nullptr);
      if (native_ps == nullptr)
      {
         state.Restore(native_device_context);
         return false;
      }

      if (distortion_ps != nullptr)
      {
         dlss_scene = DLSSOutputMips(native_device_context, game_device_data);
         // The shader reads only the size of t7, to keep the game's blur radius in screen space without
         // changing cb0.
         ID3D11ShaderResourceView* native_scene = native_srvs[0].get();
         native_device_context->PSSetShaderResources(7, 1, &native_scene);
      }

      if (game_device_data.dlss_post.bypass_dof && Battlefront2ScenePacker::Ironsights(pixel_shader_hash))
      {
         com_ptr<ID3D11Device> device;
         native_device_context->GetDevice(&device);
         if (!game_device_data.dlss_post.BindNoIronsights(device.get(), native_device_context))
         {
            state.Restore(native_device_context);
            return false;
         }
      }
      ID3D11ShaderResourceView* scene_srvs[] = {
         dlss_scene,
         native_srvs[1].get(),
         native_srvs[2].get(),
         native_srvs[3].get(),
         native_srvs[4].get(),
         native_srvs[5].get(),
      };
      native_device_context->PSSetShaderResources(0, ARRAYSIZE(scene_srvs), scene_srvs);
      native_device_context->OMSetRenderTargets(1, &packed_target, nullptr);
      native_device_context->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
      native_device_context->OMSetDepthStencilState(nullptr, 0);
      native_device_context->IASetInputLayout(nullptr);
      native_device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      native_device_context->RSSetState(nullptr);
      native_device_context->RSSetScissorRects(0, nullptr);
      // output_* is the DLSS scene size. A 4-aligned native-scale scene is up to 3 pixels larger than the
      // swapchain-sized target: the offset viewport keeps 1:1 texel addressing (every packer samples through
      // TEXCOORD1) and the target clips the centered remainder.
      D3D11_VIEWPORT viewport = {};
      viewport.TopLeftX = -static_cast<float>(crop_x);
      viewport.TopLeftY = -static_cast<float>(crop_y);
      viewport.Width = static_cast<float>(output_width);
      viewport.Height = static_cast<float>(output_height);
      viewport.MinDepth = 0.f;
      viewport.MaxDepth = 1.f;
      native_device_context->RSSetViewports(1, &viewport);
      native_device_context->GSSetShader(nullptr, nullptr, 0);
      native_device_context->HSSetShader(nullptr, nullptr, 0);
      native_device_context->DSSetShader(nullptr, nullptr, 0);
      native_device_context->VSSetShader(scene_vs.get(), nullptr, 0);
      native_device_context->PSSetShader(distortion_ps != nullptr ? distortion_ps.get() : native_ps.get(), nullptr, 0);
      native_device_context->Draw(4, 0);
      state.Restore(native_device_context);
      native_device_context->GSSetShader(previous_gs.get(), nullptr, 0);
      native_device_context->HSSetShader(previous_hs.get(), nullptr, 0);
      native_device_context->DSSetShader(previous_ds.get(), nullptr, 0);
      return true;
   }

   struct Battlefront2FinalCompositeDimensions
   {
      float main_texture_size[2];
      float main_texture_inverse_size[2];
   };
   static_assert(sizeof(Battlefront2FinalCompositeDimensions) == 16);

   static bool BindFullResolutionFinalCompositeConstants(
      ID3D11Device* native_device,
      ID3D11DeviceContext* native_device_context,
      Battlefront2GameDeviceData& game_device_data,
      Battlefront2RenderState::PixelConstantBufferBinding& original_binding,
      UINT output_width,
      UINT output_height)
   {
      if (native_device == nullptr || native_device_context == nullptr || output_width == 0 || output_height == 0)
         return false;

      if (!original_binding.Capture(native_device_context))
         return false;
      ID3D11Buffer* original_constants = original_binding.buffer.Get();
      const UINT first_constant = original_binding.first_constant;
      const UINT num_constants = original_binding.num_constants;

      D3D11_BUFFER_DESC original_desc = {};
      original_constants->GetDesc(&original_desc);
      const UINT source_offset = first_constant * 16u;
      if (source_offset >= original_desc.ByteWidth)
         return false;

      const UINT available_size = original_desc.ByteWidth - source_offset;
      const UINT requested_size = num_constants == 0 || num_constants == D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT
                                     ? available_size
                                     : num_constants * 16u;
      const UINT constants_size = (std::min)(available_size, requested_size);
      // Both observed final shaders declare cb0[10] and read through byte 151.
      if (constants_size < 160u || constants_size > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u || (constants_size % 16u) != 0)
         return false;

      if (game_device_data.dlss_final_composite_constants == nullptr || game_device_data.dlss_final_composite_constants_size != constants_size)
      {
         game_device_data.dlss_final_composite_constants = nullptr;
         D3D11_BUFFER_DESC replacement_desc = {};
         replacement_desc.ByteWidth = constants_size;
         replacement_desc.Usage = D3D11_USAGE_DEFAULT;
         replacement_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         const HRESULT hr = native_device->CreateBuffer(
            &replacement_desc,
            nullptr,
            &game_device_data.dlss_final_composite_constants);
         if (FAILED(hr) || game_device_data.dlss_final_composite_constants == nullptr)
            return false;
         game_device_data.dlss_final_composite_constants_size = constants_size;
      }

      if (game_device_data.dlss_final_composite_dimensions == nullptr || game_device_data.dlss_final_composite_width != output_width || game_device_data.dlss_final_composite_height != output_height)
      {
         game_device_data.dlss_final_composite_dimensions = nullptr;
         const Battlefront2FinalCompositeDimensions dimensions = {
            {static_cast<float>(output_width), static_cast<float>(output_height)},
            { 1.f / static_cast<float>(output_width),
               1.f / static_cast<float>(output_height) }};
         D3D11_BUFFER_DESC dimensions_desc = {};
         dimensions_desc.ByteWidth = sizeof(dimensions);
         dimensions_desc.Usage = D3D11_USAGE_DEFAULT;
         D3D11_SUBRESOURCE_DATA dimensions_data = {};
         dimensions_data.pSysMem = &dimensions;
         const HRESULT hr = native_device->CreateBuffer(
            &dimensions_desc,
            &dimensions_data,
            &game_device_data.dlss_final_composite_dimensions);
         if (FAILED(hr) || game_device_data.dlss_final_composite_dimensions == nullptr)
            return false;
         game_device_data.dlss_final_composite_width = output_width;
         game_device_data.dlss_final_composite_height = output_height;
      }

      // Copy the game's cb0 on the GPU (no readback stall), then overwrite only g_mainTexSize/g_mainTexInvSize.
      const D3D11_BOX source_box = {source_offset, 0, 0, source_offset + constants_size, 1, 1};
      native_device_context->CopySubresourceRegion(
         game_device_data.dlss_final_composite_constants.get(),
         0,
         0,
         0,
         0,
         original_constants,
         0,
         &source_box);
      const D3D11_BOX dimensions_box = {0, 0, 0, sizeof(Battlefront2FinalCompositeDimensions), 1, 1};
      native_device_context->CopySubresourceRegion(
         game_device_data.dlss_final_composite_constants.get(),
         0,
         0,
         0,
         0,
         game_device_data.dlss_final_composite_dimensions.get(),
         0,
         &dimensions_box);

      ID3D11Buffer* replacement_constants = game_device_data.dlss_final_composite_constants.get();
      native_device_context->PSSetConstantBuffers(0, 1, &replacement_constants);
      return true;
   }

   // Final display shader for the selected tone mapper and resample filter.
   static uint32_t PsychoDisplayShaderKey(const Battlefront2Psycho::Settings& settings, bool linear_filter)
   {
      switch (Battlefront2Psycho::MapperOf(settings))
      {
      case Battlefront2Psycho::Mapper::Native:
         return linear_filter ? CompileTimeStringHash("Battlefront II Native Post Linear Display PS")
                              : CompileTimeStringHash("Battlefront II Native Post Display PS");
      case Battlefront2Psycho::Mapper::GT7:
         return linear_filter ? CompileTimeStringHash("Battlefront II GT7 Linear Display PS")
                              : CompileTimeStringHash("Battlefront II GT7 Display PS");
      case Battlefront2Psycho::Mapper::ACES2:
         return linear_filter ? CompileTimeStringHash("Battlefront II ACES2 Linear Display PS")
                              : CompileTimeStringHash("Battlefront II ACES2 Display PS");
      default: // PsychoV
         return linear_filter ? CompileTimeStringHash("Battlefront II Psycho Linear Display PS")
                              : CompileTimeStringHash("Battlefront II Psycho Display PS");
      }
   }

   static bool DrawPsychoScene(
      ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data,
      Battlefront2GameDeviceData& game_data, uint64_t ps_hash, std::function<void()>& original_draw)
   {
      auto& tone = game_data.psycho;
      const uint64_t frame = cb_luma_global_settings.FrameIndex;
      const auto& frame_settings = tone.Frame(frame);
      const bool psycho_enabled = frame_settings.values[Battlefront2Psycho::Enabled] != 0.f;
      if (!psycho_enabled && !Battlefront2Psycho::PostControlsActive(frame_settings) && !Battlefront2Compare::Pending()) return false;
      auto vs = data.native_vertex_shaders[CompileTimeStringHash("Battlefront II DLSS Scene Conversion VS")];
      const auto* recipe = Battlefront2ScenePacker::Find(ps_hash);
      if (!recipe) return false;
      const uint32_t scene_shader = recipe->ShaderKey(psycho_enabled
                                                         ? Battlefront2ScenePacker::Path::Psycho
                                                         : Battlefront2ScenePacker::Path::NativePost);
      auto ps = data.native_pixel_shaders[scene_shader];
      const uint64_t display_shader = PsychoDisplayShaderKey(frame_settings, false);
      if (!vs || !ps || !data.native_pixel_shaders[display_shader]) return false;
      com_ptr<ID3D11ShaderResourceView> source;
      com_ptr<ID3D11RenderTargetView> target;
      context->PSGetShaderResources(0, 1, &source);
      context->OMGetRenderTargets(1, &target, nullptr);
      if (!source || !target) return false;
      // The native grade needs the game's LUT; Luma's mappers can do without it when the grade is bypassed.
      com_ptr<ID3D11ShaderResourceView> grade;
      com_ptr<ID3D11SamplerState> grade_sampler;
      context->PSGetShaderResources(1, 1, &grade);
      context->PSGetSamplers(1, 1, &grade_sampler);
      D3D11_SHADER_RESOURCE_VIEW_DESC gv{};
      if (grade) grade->GetDesc(&gv);
      com_ptr<ID3D11Resource> grade_resource;
      com_ptr<ID3D11Texture3D> grade_texture;
      if (grade) grade->GetResource(&grade_resource);
      if (grade_resource) grade_resource->QueryInterface(&grade_texture);
      D3D11_TEXTURE3D_DESC gd{};
      if (grade_texture) grade_texture->GetDesc(&gd);
      const bool native_grade_valid = grade && grade_sampler && grade_texture && gv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D && gv.Texture3D.MostDetailedMip == 0 && gv.Format == DXGI_FORMAT_R10G10B10A2_UNORM && gd.Width == 33 && gd.Height == 33 && gd.Depth == 33;
      if ((!psycho_enabled || Battlefront2Psycho::LUTNative(frame_settings)) && !native_grade_valid) return false;
      // The game's LUT split into the layers chosen in Advanced, for a scene path that uses Luma's grade.
      const auto separated_grade_for = [&](bool luma_grade) -> ID3D11ShaderResourceView*
      {
         if (!luma_grade || Battlefront2Psycho::LUTBypassed(frame_settings) || Battlefront2Psycho::LUTNative(frame_settings)) return nullptr;
         const auto kernel = data.native_compute_shaders.find(CompileTimeStringHash("Battlefront II Native LUT Split CS"));
         return game_data.native_lut.Find(device, context, native_grade_valid ? grade.get() : nullptr,
            kernel != data.native_compute_shaders.end() ? kernel->second.get() : nullptr, frame_settings);
      };
      ID3D11ShaderResourceView* separated_grade = separated_grade_for(psycho_enabled);
      com_ptr<ID3D11Resource> source_resource, target_resource;
      source->GetResource(&source_resource);
      target->GetResource(&target_resource);
      com_ptr<ID3D11Texture2D> source_texture, target_texture;
      if (FAILED(source_resource->QueryInterface(&source_texture)) || FAILED(target_resource->QueryInterface(&target_texture))) return false;
      D3D11_TEXTURE2D_DESC sd{}, td{};
      source_texture->GetDesc(&sd);
      target_texture->GetDesc(&td);
      D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
      source->GetDesc(&sv);
      D3D11_RENDER_TARGET_VIEW_DESC tv{};
      target->GetDesc(&tv);
      UINT viewport_count = 1;
      D3D11_VIEWPORT vp{};
      context->RSGetViewports(&viewport_count, &vp);
      if (sd.Format != DXGI_FORMAT_R11G11B10_FLOAT || sv.Format != sd.Format || sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || sv.Texture2D.MostDetailedMip != 0 || tv.Format != DXGI_FORMAT_R10G10B10A2_UNORM || tv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || tv.Texture2D.MipSlice != 0 || sd.SampleDesc.Count != 1 || td.SampleDesc.Count != 1 || sd.Width != td.Width || sd.Height != td.Height || viewport_count != 1 || vp.Width != float(sd.Width) || vp.Height != float(sd.Height) || vp.TopLeftX != 0 || vp.TopLeftY != 0) return false;
      const bool dlss = data.sr_type == SR::Type::DLSS && data.has_drawn_sr && game_data.dlss_output_generation == game_data.dlss_generation && game_data.dlss_output_token && game_data.dlss_output_srv && DLSSScenePairContains(game_data, source_resource.get(), frame, game_data.dlss_generation);
      UINT width = dlss ? game_data.dlss_last_target_width : sd.Width, height = dlss ? game_data.dlss_last_target_height : sd.Height;
      // The private scene has the display's size; a padded native-scale DLSS scene goes in with a centered crop.
      const UINT scene_width = dlss ? game_data.dlss_last_display_width : width, scene_height = dlss ? game_data.dlss_last_display_height : height;
      const UINT crop_x = dlss ? game_data.dlss_display_crop_x : 0, crop_y = dlss ? game_data.dlss_display_crop_y : 0;
      if (!scene_width || !scene_height || scene_width + crop_x > width || scene_height + crop_y > height) return false;
      ID3D11ShaderResourceView* input = dlss ? game_data.dlss_post.output_srv.get() : source.get();
      if (dlss && Battlefront2ScenePacker::Distorted(ps_hash))
      {
         if (!game_data.dlss_post.output_mips) return false;
         input = DLSSOutputMips(context, game_data);
      }
      if (!Battlefront2Psycho::Prepare(device, context, tone, scene_width, scene_height, frame, dlss && game_data.dlss_post.bypass_dof)) return false;
      original_draw(); // The game's own packed scene still feeds bloom and the UI glass.
      tone.native_packed = nullptr;
      tone.scene_frame = ~uint64_t(0);
      ID3D11ShaderResourceView* light_layer = dlss ? game_data.dlss_post.LightLayer() : nullptr;
      // The eye glare is scene light: its veil is drawn from the scene the grade takes in and added by the scene shader.
      ID3D11ShaderResourceView* glare = nullptr;
      if (psycho_enabled)
      {
         const bool native_lut = Battlefront2Psycho::LUTNative(frame_settings);
         const auto glare_kind = native_lut ? Battlefront2Glare::Grade::Native : separated_grade ? Battlefront2Glare::Grade::Separated
                                                                                                 : Battlefront2Glare::Grade::None;
         com_ptr<ID3D11Buffer> scene_constants;
         UINT first_constant = 0, constant_count = 4096;
         com_ptr<ID3D11DeviceContext1> context1;
         context->QueryInterface(&context1);
         if (context1)
            context1->PSGetConstantBuffers1(0, 1, &scene_constants, &first_constant, &constant_count);
         else
            context->PSGetConstantBuffers(0, 1, &scene_constants);
         const auto glare_pass = [&](size_t pass)
         {
            const auto kernel = data.native_compute_shaders.find(bf2_glare_keys[pass]);
            return kernel != data.native_compute_shaders.end() ? kernel->second.get() : nullptr;
         };
         glare = Battlefront2Glare::Draw(device, context, game_data.glare, tone, {glare_pass(0), glare_pass(1), glare_pass(2), glare_pass(3)}, input,
            native_lut ? grade.get() : separated_grade, glare_kind, scene_constants.get(), first_constant, constant_count, crop_x, crop_y,
            game_data.sparks.Srv(frame), data.sampler_state_linear.get(), data.uav_max_count);
      }
      Battlefront2Psycho::DrawScene(context, tone, vs.get(), ps.get(), input, data.uav_max_count, separated_grade,
         width, height, crop_x, crop_y, nullptr, light_layer, glare, data.sampler_state_linear.get());
      // For the tone mapper comparison, also draw the other scene path: Native uses the game's whole grade, the
      // other mappers Luma's.
      tone.compare_frame = ~uint64_t(0);
      if (Battlefront2Compare::Pending() && Battlefront2Psycho::AllocateCompareScene(device, tone))
      {
         auto other = data.native_pixel_shaders[recipe->ShaderKey(psycho_enabled
                                                                     ? Battlefront2ScenePacker::Path::NativePost
                                                                     : Battlefront2ScenePacker::Path::Psycho)];
         ID3D11ShaderResourceView* other_grade = separated_grade_for(!psycho_enabled);
         if (other && (!psycho_enabled || native_grade_valid))
         {
            Battlefront2Psycho::DrawScene(context, tone, vs.get(), other.get(), input, data.uav_max_count, other_grade,
               width, height, crop_x, crop_y, tone.compare_rtv.get(), light_layer);
            tone.compare_frame = frame;
         }
      }
      tone.native_packed = target_resource;
      tone.scene_frame = frame;
      tone.used_dlss = dlss;
      if (dlss)
      {
         DLSSScenePairMarkConverted(game_data, source_resource.get(), target_resource.get(), frame);
         game_data.dlss_display_handoff_frame = frame;
         game_data.dlss_display_handoff_seen = true;
         game_data.dlss_display_handoff_token = game_data.dlss_output_token;
      }
      return true;
   }

   static bool DrawNativePresentation(
      ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data,
      Battlefront2GameDeviceData& game_data, std::function<void()>& original_draw, uint64_t hash)
   {
      const auto key = Battlefront2ShaderVariants::LinearDisplay(hash)
                          ? CompileTimeStringHash("Battlefront II Native Presentation Linear PS")
                       : Battlefront2ShaderVariants::CubicDisplay(hash)
                          ? CompileTimeStringHash("Battlefront II Native Presentation Cubic PS")
                          : CompileTimeStringHash("Battlefront II Native Presentation Point PS");
      auto shader = data.native_pixel_shaders[key];
      auto& tone = game_data.psycho;
      auto& presentation = game_data.native_presentation;
      const auto result = Battlefront2NativePresentation::Present(device, context, presentation, shader.get(), hash, tone.Frame(cb_luma_global_settings.FrameIndex), tone.display_hdr, tone.linear_output, [&](ID3D11Resource* resource)
         {
            const std::shared_lock lock(data.mutex);
            return data.back_buffers.contains(reinterpret_cast<uint64_t>(resource)); }, original_draw, tone.windows_sdr_white);
      if (result != Battlefront2NativePresentation::Result::Drawn)
      {
         if (result != Battlefront2NativePresentation::Result::NotBackBuffer && presentation.failure_logs < 4)
         {
            ++presentation.failure_logs;
            std::ostringstream log;
            log << "[Battlefront II Luma] Native presentation skipped: reason=" << Battlefront2NativePresentation::Name(result)
                << " shader=" << std::hex << hash << " (original draw retained).";
            reshade::log::message(reshade::log::level::warning, log.str().c_str());
         }
         return false;
      }
      return true;
   }

   // Runs this frame's final pass once per tone mapper into a target like the back buffer, reads it back and
   // hands the images to the comparison writer. The pass keeps its own bindings (UI, fade, accessibility);
   // only the tone mapper, its scene and its constants change.
   static void RenderToneMapperComparison(ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data,
      Battlefront2GameDeviceData& game_data, std::function<void()>& original_draw, bool linear_filter,
      const D3D11_TEXTURE2D_DESC& output, uint64_t frame)
   {
      namespace P = Battlefront2Psycho;
      auto& tone = game_data.psycho;
      // HDR PNGs while HDR is shown, else 8-bit sRGB PNGs drawn through the shaders' 8-bit path.
      const bool hdr_target = output.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && tone.hdr_output;
      D3D11_TEXTURE2D_DESC d{};
      d.Width = output.Width;
      d.Height = output.Height;
      d.MipLevels = d.ArraySize = 1;
      d.Format = hdr_target ? output.Format : DXGI_FORMAT_R8G8B8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_RENDER_TARGET;
      D3D11_TEXTURE2D_DESC sd = d;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = sizeof(P::Settings);
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      com_ptr<ID3D11Texture2D> target, staging;
      com_ptr<ID3D11RenderTargetView> rtv;
      com_ptr<ID3D11Buffer> constants;
      if (FAILED(device->CreateTexture2D(&d, nullptr, &target)) || FAILED(device->CreateRenderTargetView(target.get(), nullptr, &rtv)) || FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || FAILED(device->CreateBuffer(&bd, nullptr, &constants)))
      {
         P::ReleaseCompareScene(tone);
         Battlefront2Compare::status = Battlefront2Compare::Status::Failed;
         return;
      }
      com_ptr<ID3D11RenderTargetView> previous_rtv;
      com_ptr<ID3D11DepthStencilView> previous_dsv;
      context->OMGetRenderTargets(1, &previous_rtv, &previous_dsv);
      Battlefront2RenderState::PixelConstantBufferBinding previous_aces2;
      previous_aces2.Capture(context, Battlefront2ACES2::constant_buffer_slot);
      const P::Mapper current = P::MapperOf(tone.frame_settings);
      std::vector<Battlefront2Compare::Image> images;
      std::ostringstream notes;
      int index = 0;
      for (const auto mapper : P::mappers)
      {
         ++index;
         const char* name = P::MapperName(mapper);
         auto values = tone.uploaded;
         values.values[P::Enabled] = float(int(mapper));
         values.values[P::OutputHDR] = P::HDROutput(tone.hdr_output, mapper != P::Mapper::Native) ? 1.f : 0.f;
         if (!hdr_target) values.values[P::OutputLinear] = 0.f;
         // Native shows the natively graded scene, the others Luma's.
         ID3D11ShaderResourceView* scene = ((mapper == P::Mapper::Native) == (current == P::Mapper::Native))
                                              ? tone.srv.get()
                                              : tone.compare_srv.get();
         const auto shader = data.native_pixel_shaders[PsychoDisplayShaderKey(values, linear_filter)];
         ID3D11Buffer* aces2 = nullptr;
         if (mapper == P::Mapper::ACES2)
         {
            auto aces2_settings = tone.frame_settings;
            aces2_settings.values[P::Enabled] = P::MapperACES2;
            aces2 = Battlefront2ACES2::Prepare(device, context, game_data.aces2, aces2_settings, values.values[P::OutputHDR] != 0.f);
         }
         if (!scene || !shader || (mapper == P::Mapper::ACES2 && !aces2))
         {
            notes << name << "=unavailable\n";
            continue;
         }
         context->UpdateSubresource(constants.get(), 0, nullptr, &values, 0, 0);
         ID3D11Buffer* cb = constants.get();
         context->PSSetConstantBuffers(11, 1, &cb);
         if (aces2) context->PSSetConstantBuffers(Battlefront2ACES2::constant_buffer_slot, 1, &aces2);
         context->PSSetShaderResources(0, 1, &scene);
         context->PSSetShader(shader.get(), nullptr, 0);
         const float black[4]{0.f, 0.f, 0.f, 1.f};
         context->ClearRenderTargetView(rtv.get(), black);
         ID3D11RenderTargetView* r = rtv.get();
         context->OMSetRenderTargets(1, &r, nullptr);
         original_draw();
         context->CopyResource(staging.get(), target.get());
         D3D11_MAPPED_SUBRESOURCE mapped{};
         if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
         {
            notes << name << "=readback_failed\n";
            continue;
         }
         Battlefront2Compare::Image image;
         image.name = std::to_wstring(index) + L" " + std::wstring(name, name + std::strlen(name));
         image.width = d.Width;
         image.height = d.Height;
         image.hdr = hdr_target;
         const size_t row = size_t(d.Width) * (hdr_target ? 8u : 4u);
         image.pixels.resize(row * d.Height);
         for (UINT y = 0; y < d.Height; ++y)
            std::memcpy(image.pixels.data() + row * y, static_cast<const uint8_t*>(mapped.pData) + size_t(mapped.RowPitch) * y, row);
         context->Unmap(staging.get(), 0);
         images.push_back(std::move(image));
         notes << name << "=saved\n";
      }
      ID3D11RenderTargetView* restore = previous_rtv.get();
      context->OMSetRenderTargets(1, &restore, previous_dsv.get());
      previous_aces2.Restore(context);
      P::ReleaseCompareScene(tone);
      const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm local{};
      localtime_s(&local, &t);
      char stamp[32]{};
      std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &local);
      std::ostringstream header;
      header << "Luma for Battlefront II tone mapper comparison\n"
             << "Every image is the same game frame (" << frame << "), rendered once per tone mapper with the same scene, UI and settings.\n"
             << "selected_tone_mapper=" << P::MapperName(current) << "\n"
             << "format=" << (hdr_target ? "HDR PNG, 16-bit, BT.2020 primaries, PQ (SMPTE ST 2084) absolute luminance, BT.2020 PQ ICC profile and cLLi" : "8-bit sRGB PNG") << "\n"
             << "windows_hdr=" << tone.hdr_output << "\nwindows_sdr_content_brightness_nits=" << tone.windows_sdr_white
             << "\ngame_hdr_display=" << tone.native_hdr_display << "\ndlss=" << tone.used_dlss << "\nsize=" << d.Width << 'x' << d.Height << "\n";
      header << "\n[images]\n"
             << notes.str() << "\n[settings]\n";
      for (const auto& c : P::controls)
         header << c.key << '=' << tone.frame_settings.values[c.field] << '\n';
      if (images.empty())
      {
         Battlefront2Compare::status = Battlefront2Compare::Status::Failed;
         return;
      }
      const std::string relative = std::string("Luma\\Comparisons\\") + stamp;
      const auto folder = System::GetModulePath().parent_path() / L"Luma" / L"Comparisons" / std::filesystem::path(stamp);
      Battlefront2Compare::Save(folder, relative, std::move(images), header.str());
   }

   static bool DrawPsychoDisplay(
      ID3D11Device* device, ID3D11DeviceContext* context, DeviceData& data,
      Battlefront2GameDeviceData& game_data, std::function<void()>& original_draw, bool linear_filter, bool native_hdr)
   {
      auto& tone = game_data.psycho;
      const uint64_t frame = cb_luma_global_settings.FrameIndex;
      if (tone.scene_frame != frame || !tone.native_packed || !tone.srv || !tone.constants) return false;
      auto shader = data.native_pixel_shaders[PsychoDisplayShaderKey(tone.Frame(frame), linear_filter)];
      if (!shader) return false;
      com_ptr<ID3D11ShaderResourceView> source;
      com_ptr<ID3D11RenderTargetView> target;
      context->PSGetShaderResources(0, 1, &source);
      context->OMGetRenderTargets(1, &target, nullptr);
      if (!source || !target) return false;
      com_ptr<ID3D11Resource> source_resource, target_resource;
      source->GetResource(&source_resource);
      target->GetResource(&target_resource);
      if (source_resource != tone.native_packed) return false;
      {
         const std::shared_lock lock(data.mutex);
         if (!data.back_buffers.contains(reinterpret_cast<uint64_t>(target_resource.get()))) return false;
      }
      com_ptr<ID3D11Texture2D> target_texture;
      if (FAILED(target_resource->QueryInterface(&target_texture))) return false;
      D3D11_TEXTURE2D_DESC td{};
      target_texture->GetDesc(&td);
      if (td.Format != DXGI_FORMAT_R8G8B8A8_UNORM && td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
      if ((td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) != tone.linear_output) return false;
      // The game's HDR final pass carries absolute luminance; Native shows its SDR pass at the Windows SDR
      // content brightness.
      if (tone.native_hdr_display != native_hdr)
      {
         tone.native_hdr_display = native_hdr;
         tone.uploaded.values[Battlefront2Psycho::NativeHDRDisplay] = native_hdr ? 1.f : 0.f;
         context->UpdateSubresource(tone.constants.get(), 0, nullptr, &tone.uploaded, 0, 0);
      }
      // ACES 2.0 reads its tables (built for the current peak, scene white and primaries) from b10. They are
      // prepared before anything is bound, so a failure leaves the frame to the game's own draw.
      const auto& mapper_settings = tone.Frame(frame);
      const bool aces2 = Battlefront2Psycho::MapperOf(mapper_settings) == Battlefront2Psycho::Mapper::ACES2;
      ID3D11Buffer* aces2_constants = aces2 ? Battlefront2ACES2::Prepare(device, context, game_data.aces2, mapper_settings,
                                                 tone.uploaded.values[Battlefront2Psycho::OutputHDR] != 0.f)
                                            : nullptr;
      if (aces2 && !aces2_constants) return false;
      Battlefront2RenderState::PixelBindings saved(context);
      Battlefront2RenderState::PixelConstantBufferBinding previous_aces2;
      if (aces2) previous_aces2.Capture(context, Battlefront2ACES2::constant_buffer_slot);
      Battlefront2RenderState::PixelConstantBufferBinding previous_constants;
      if (!BindFullResolutionFinalCompositeConstants(device, context, game_data, previous_constants, tone.width, tone.height))
      {
         saved.Restore(context);
         return false;
      }
      ID3D11ShaderResourceView* replacement = tone.srv.get();
      context->PSSetShaderResources(0, 1, &replacement);
      ID3D11Buffer* cb = tone.constants.get();
      context->PSSetConstantBuffers(11, 1, &cb);
      if (aces2) context->PSSetConstantBuffers(Battlefront2ACES2::constant_buffer_slot, 1, &aces2_constants);
      context->PSSetShader(shader.get(), nullptr, 0);
      original_draw();
      if (Battlefront2Compare::Pending() && tone.compare_frame == frame && td.SampleDesc.Count == 1)
         RenderToneMapperComparison(device, context, data, game_data, original_draw, linear_filter, td, frame);
      if (aces2) previous_aces2.Restore(context);
      saved.Restore(context);
      if (tone.used_dlss)
      {
         game_data.dlss_output_consumed_frame = frame;
         game_data.dlss_display_handoff_seen = false;
         game_data.dlss_display_handoff_token = 0;
      }
      return true;
   }

public:
   // Capture the game's uploads of the temporal constants for Battlefront2Jitter. They run on the game's
   // threads and only touch the mirror.
   static void OnJitterMapBufferRegion(reshade::api::device* device, reshade::api::resource resource, uint64_t offset, uint64_t size, reshade::api::map_access access, void** data)
   {
      if (data == nullptr)
         return;
      Battlefront2Jitter::RecordMappedPtr(device, resource.handle, *data, offset, size, access);
   }

   static void OnJitterDestroyResource(reshade::api::device*, reshade::api::resource resource)
   {
      Battlefront2Jitter::RemoveWatch(resource.handle);
   }

   static void OnJitterUnmapBufferRegion(reshade::api::device* device, reshade::api::resource resource)
   {
      Battlefront2Jitter::CaptureMapped(device, resource.handle);
   }

   static bool OnJitterUpdateBufferRegion(reshade::api::device* device, const void* src_data, reshade::api::resource dest, uint64_t dest_offset, uint64_t size)
   {
      Battlefront2Jitter::CaptureUpdate(device, dest.handle, dest_offset, src_data, size);
      return false; // The game's update still runs.
   }

   static bool OnCreateFlipSwapchain(reshade::api::device_api api, reshade::api::swapchain_desc& desc, void* /*hwnd*/)
   {
      if (api != reshade::api::device_api::d3d11) return false;
      static const bool tearing_supported = []
      {
         com_ptr<IDXGIFactory5> factory;
         BOOL supported = FALSE;
         const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
            GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "CreateDXGIFactory1"));
         return create_factory && SUCCEEDED(create_factory(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))) && supported != FALSE;
      }();
      return Battlefront2Presentation::PrepareFlip(desc, tearing_supported);
   }

   static bool OnCreateHDRBackbufferView(reshade::api::device* device, reshade::api::resource resource,
      reshade::api::resource_usage usage, reshade::api::resource_view_desc& desc)
   {
      if (device->get_api() != reshade::api::device_api::d3d11 || (usage != reshade::api::resource_usage::render_target && usage != reshade::api::resource_usage::shader_resource)) return false;
      auto* data = device->get_private_data<DeviceData>();
      if (!data) return false;
      {
         const std::shared_lock lock(data->mutex);
         if (!data->back_buffers.contains(resource.handle)) return false;
      }
      const auto storage = device->get_resource_desc(resource);
      if (storage.type != reshade::api::resource_type::texture_2d) return false;
      return Battlefront2Presentation::PrepareBackbufferView(desc, static_cast<DXGI_FORMAT>(storage.texture.format));
   }

   static Battlefront2GameDeviceData* NativeDepthData(reshade::api::command_list* cmd_list)
   {
      if (!cmd_list || cmd_list->get_device()->get_api() != reshade::api::device_api::d3d11 || reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native())->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return nullptr;
      auto* data = cmd_list->get_device()->get_private_data<DeviceData>();
      return data && data->game ? &GetGameDeviceData(*data) : nullptr;
   }
   static void InvalidateNativeDepth(Battlefront2GameDeviceData& data, uint64_t resource, bool depth, bool stencil)
   {
      auto* native = reinterpret_cast<ID3D11Resource*>(resource);
      data.dlss_post.InvalidateDepth(native, depth, stencil);
      data.full_transparency.shader_cache.InvalidateDepth(native, depth, stencil);
   }
   static bool OnNativeDepthClear(reshade::api::command_list* cmd_list, reshade::api::resource_view view,
      const float* depth, const uint8_t* stencil, uint32_t, const reshade::api::rect*)
   {
      if (auto* data = NativeDepthData(cmd_list))
         if (view.handle)
            InvalidateNativeDepth(*data, cmd_list->get_device()->get_resource_from_view(view).handle, depth != nullptr, stencil != nullptr);
      return false;
   }
   static bool OnNativeDepthCopy(reshade::api::command_list* cmd_list, reshade::api::resource, reshade::api::resource destination)
   {
      if (auto* data = NativeDepthData(cmd_list))
      {
         InvalidateNativeDepth(*data, destination.handle, true, true);
         data->native_lut.Invalidate(reinterpret_cast<ID3D11Resource*>(destination.handle));
      }
      return false;
   }
   static bool OnNativeDepthCopyRegion(reshade::api::command_list* cmd_list, reshade::api::resource, uint32_t,
      const reshade::api::subresource_box*, reshade::api::resource destination, uint32_t destination_subresource,
      const reshade::api::subresource_box*, reshade::api::filter_mode)
   {
      if (destination_subresource == 0)
         if (auto* data = NativeDepthData(cmd_list))
         {
            InvalidateNativeDepth(*data, destination.handle, true, true);
            data->native_lut.Invalidate(reinterpret_cast<ID3D11Resource*>(destination.handle));
         }
      return false;
   }
   template <class Value>
   static bool OnNativeLUTClear(reshade::api::command_list* cmd_list, reshade::api::resource_view view,
      const Value*, uint32_t, const reshade::api::rect*)
   {
      if (auto* data = NativeDepthData(cmd_list))
         if (view.handle && !data->native_lut.entries.empty())
            data->native_lut.Invalidate(reinterpret_cast<ID3D11Resource*>(cmd_list->get_device()->get_resource_from_view(view).handle));
      return false;
   }
   static void OnNativeDepthCommandList(reshade::api::command_queue* queue, reshade::api::command_list*)
   {
      if (queue->get_device()->get_api() != reshade::api::device_api::d3d11) return;
      if (auto* data = queue->get_device()->get_private_data<DeviceData>())
         if (data->game)
         {
            auto& native = GetGameDeviceData(*data);
            native.dlss_post.InvalidateAllDepths();
            native.full_transparency.shader_cache.InvalidateAllDepths();
         }
   }

#if !DEVELOPMENT
   // Player builds show only this project's settings panel in the Luma tab: the framework's panel has its own display
   // and HDR settings, which this mod replaces. Development builds keep the framework's panel, which also draws this one.
   static void OnBattlefront2Overlay(reshade::api::effect_runtime* runtime)
   {
      SKIP_UNSUPPORTED_DEVICE_API(runtime->get_device()->get_api());
      DeviceData& device_data = *runtime->get_device()->get_private_data<DeviceData>();
      device_data.cb_luma_global_settings_dirty = true;
      const std::unique_lock lock_reshade(s_mutex_reshade);
      game->DrawImGuiSettings(device_data);
   }
#endif

   // Binds the game's samplers with DLSS's mip bias (Battlefront2MipBias::Adjust) and records the samplers the game
   // binds on the immediate context, so a bias change can rebind them. ReShade passes the game's own bind array, so the
   // variants go into a copy of the update.
   static void OnPushSamplers(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
      reshade::api::pipeline_layout layout, uint32_t layout_param, const reshade::api::descriptor_table_update& update)
   {
      if (update.type != reshade::api::descriptor_type::sampler || update.count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) return;
      const auto* cmd_list_data = cmd_list->get_private_data<CommandListData>();
      auto* device_data = cmd_list->get_device()->get_private_data<DeviceData>();
      if (cmd_list_data == nullptr || device_data == nullptr || device_data->game == nullptr) return;
      auto& game_device_data = GetGameDeviceData(*device_data);
      const auto* samplers = static_cast<const reshade::api::sampler*>(update.descriptors);
      if (cmd_list_data->is_primary)
      {
         auto& native = game_device_data.native_mip_samplers;
         constexpr reshade::api::shader_stage sampler_stages[] = {
            reshade::api::shader_stage::vertex, reshade::api::shader_stage::hull,
            reshade::api::shader_stage::domain, reshade::api::shader_stage::geometry,
            reshade::api::shader_stage::pixel, reshade::api::shader_stage::compute};
         for (unsigned stage = 0; stage < 6; ++stage)
         {
            if ((stages & sampler_stages[stage]) == 0) continue;
            for (uint32_t i = 0; i < update.count && update.binding + i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
            {
               auto* original = reinterpret_cast<ID3D11SamplerState*>(samplers[i].handle);
               if (native[stage][update.binding + i].get() != original)
                  native[stage][update.binding + i] = original;
            }
         }
      }
      float bias = 0.f;
      {
         const std::shared_lock lock(s_mutex_samplers);
         bias = device_data->texture_mip_lod_bias_offset;
      }
      if (bias == 0.f) return;
      auto* device = reinterpret_cast<ID3D11Device*>(cmd_list->get_device()->get_native());
      std::array<reshade::api::sampler, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> replaced;
      bool any_replaced = false;
      for (uint32_t i = 0; i < update.count; ++i)
      {
         replaced[i] = samplers[i];
         if (auto* variant = game_device_data.mip_variants.Get(device, reinterpret_cast<ID3D11SamplerState*>(samplers[i].handle), bias))
         {
            replaced[i].handle = reinterpret_cast<uint64_t>(variant);
            any_replaced = true;
         }
      }
      if (!any_replaced) return;
      auto biased = update;
      biased.descriptors = replaced.data();
      cmd_list->push_descriptors(stages, layout, layout_param, biased);
   }

   // Registered after the framework's own events, so these callbacks run after them.
   void RegisterEvents()
   {
      Battlefront2Overlay::Register();
#if !DEVELOPMENT
      reshade::unregister_overlay(NAME, OnRegisterMainOverlay);
      reshade::register_overlay(NAME, OnBattlefront2Overlay);
#endif
      reshade::register_event<reshade::addon_event::push_descriptors>(OnPushSamplers);
      reshade::register_event<reshade::addon_event::create_swapchain>(OnCreateFlipSwapchain);
      reshade::register_event<reshade::addon_event::create_resource_view>(OnCreateHDRBackbufferView);
      reshade::register_event<reshade::addon_event::map_buffer_region>(OnJitterMapBufferRegion);
      reshade::register_event<reshade::addon_event::unmap_buffer_region>(OnJitterUnmapBufferRegion);
      reshade::register_event<reshade::addon_event::update_buffer_region>(OnJitterUpdateBufferRegion);
      reshade::register_event<reshade::addon_event::destroy_resource>(OnJitterDestroyResource);
      reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(OnNativeDepthClear);
      reshade::register_event<reshade::addon_event::execute_command_list>(OnNativeDepthCommandList);
      reshade::register_event<reshade::addon_event::copy_resource>(OnNativeDepthCopy);
      reshade::register_event<reshade::addon_event::copy_texture_region>(OnNativeDepthCopyRegion);
      reshade::register_event<reshade::addon_event::clear_unordered_access_view_uint>(OnNativeLUTClear<uint32_t>);
      reshade::register_event<reshade::addon_event::clear_unordered_access_view_float>(OnNativeLUTClear<float>);
   }

   void UnregisterEvents()
   {
      Battlefront2Overlay::Unregister();
#if !DEVELOPMENT
      reshade::unregister_overlay(NAME, OnBattlefront2Overlay);
#endif
      reshade::unregister_event<reshade::addon_event::push_descriptors>(OnPushSamplers);
      Battlefront2Reflex::Shutdown();
      reshade::unregister_event<reshade::addon_event::create_swapchain>(OnCreateFlipSwapchain);
      reshade::unregister_event<reshade::addon_event::create_resource_view>(OnCreateHDRBackbufferView);
      reshade::unregister_event<reshade::addon_event::map_buffer_region>(OnJitterMapBufferRegion);
      reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(OnJitterUnmapBufferRegion);
      reshade::unregister_event<reshade::addon_event::update_buffer_region>(OnJitterUpdateBufferRegion);
      reshade::unregister_event<reshade::addon_event::destroy_resource>(OnJitterDestroyResource);
      reshade::unregister_event<reshade::addon_event::clear_depth_stencil_view>(OnNativeDepthClear);
      reshade::unregister_event<reshade::addon_event::execute_command_list>(OnNativeDepthCommandList);
      reshade::unregister_event<reshade::addon_event::copy_resource>(OnNativeDepthCopy);
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(OnNativeDepthCopyRegion);
      reshade::unregister_event<reshade::addon_event::clear_unordered_access_view_uint>(OnNativeLUTClear<uint32_t>);
      reshade::unregister_event<reshade::addon_event::clear_unordered_access_view_float>(OnNativeLUTClear<float>);
   }

private:
#endif

   void OnInit(bool /*async*/) override
   {
      // The framework's own post-process and display passes are not used: the game's final composite stays in
      // place and this mod replaces it.
      GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('1');
      GetShaderDefineData(EARLY_DISPLAY_ENCODING_HASH).SetDefaultValue('0');
      GetShaderDefineData(VANILLA_ENCODING_TYPE_HASH).SetDefaultValue('0');
      GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('0');
      GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('0');

      force_disable_display_composition = true;
      Battlefront2Compare::root = System::GetModulePath().parent_path() / L"Luma" / L"Comparisons";
      // The game renders HDR only in exclusive fullscreen; when it leaves fullscreen it switches its swapchain
      // to 8-bit SDR whatever its HDR setting says. Fullscreen requests therefore get a borderless window while
      // ReShade reports the state the game asked for, so the game's Fullscreen mode keeps its HDR.
      // BattlefrontIIExclusiveFullscreen=1 restores real exclusive fullscreen.
      {
         bool exclusive = false;
         reshade::get_config_value(nullptr, NAME, "BattlefrontIIExclusiveFullscreen", exclusive);
         prevent_fullscreen_state = !exclusive;
      }
      force_borderless = false;

      using Stage = reshade::api::pipeline_subobject_type;
      const auto add_shader = [](uint32_t key, const char* file, Stage type, const char* function, std::vector<SimplerShaderDefine> defines = {})
      {
         native_shaders_definitions.emplace(key, ShaderDefinition{file, type, nullptr, function, std::move(defines)});
      };

      // XeGTAO and VB-GTAO (VISIBILITY_BITMASK), each at the shared Medium and High presets (XE_GTAO_QUALITY 1 and 2).
      constexpr std::array<const char*, 2> xegtao_bitmask = {"0", "1"};
      constexpr std::array<const char*, 2> xegtao_quality = {"1", "2"};
      for (size_t method = 0; method < 2; ++method)
      {
         add_shader(bf2_xegtao_prefilter_keys[method], "Luma_SWBF2_XeGTAO", Stage::compute_shader, "prefilter_depths16x16_cs", {{"VISIBILITY_BITMASK", xegtao_bitmask[method]}});
         for (size_t quality = 0; quality < 2; ++quality)
            add_shader(bf2_xegtao_main_keys[method][quality], "Luma_SWBF2_XeGTAO", Stage::compute_shader, "main_pass_cs", {{"VISIBILITY_BITMASK", xegtao_bitmask[method]}, {"XE_GTAO_QUALITY", xegtao_quality[quality]}});
      }
      add_shader(bf2_xegtao_denoise_key, "Luma_SWBF2_XeGTAO", Stage::compute_shader, "denoise_pass_cs", {{"XE_GTAO_FINAL_APPLY", "1"}});
      constexpr std::array<const char*, 7> cortao_sample_count = {"4", "6", "8", "12", "16", "24", "32"};
      static_assert(cortao_sample_count.size() == Battlefront2XeGTAO::cortao_samples.size());
      add_shader(bf2_cortao_prepass_key, "Luma_SWBF2_CORTAO", Stage::compute_shader, "main", {{"CORTAO_PASS", "0"}});
      for (size_t level = 0; level < cortao_sample_count.size(); ++level)
         add_shader(bf2_cortao_main_keys[level], "Luma_SWBF2_CORTAO", Stage::compute_shader, "main", {{"CORTAO_PASS", "1"}, {"CORTAO_SAMPLE_COUNT", cortao_sample_count[level]}});
      add_shader(bf2_cortao_filter_key, "Luma_SWBF2_CORTAO", Stage::compute_shader, "main", {{"CORTAO_PASS", "2"}});
      constexpr std::array<const char*, 4> glare_pass = {"0", "1", "2", "3"};
      for (size_t pass = 0; pass < glare_pass.size(); ++pass)
         add_shader(bf2_glare_keys[pass], "Luma_SWBF2_Glare", Stage::compute_shader, "main", {{"BF2_GLARE_PASS", glare_pass[pass]}});

      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12;
      luma_ui_cbuffer_index = -1;
      add_shader(CompileTimeStringHash("Battlefront II SSR Spatial Filter CS"), "Luma_SWBF2_SSRFilter", Stage::compute_shader, "main");
      add_shader(CompileTimeStringHash("Battlefront II SSR Constants CS"), "Luma_SWBF2_SSRConstants", Stage::compute_shader, "main", {{"BF2_SSR_CONSTANT_COUNT", "73"}});
      add_shader(CompileTimeStringHash("Battlefront II SSR Trace Constants CS"), "Luma_SWBF2_SSRConstants", Stage::compute_shader, "main", {{"BF2_SSR_CONSTANT_COUNT", "51"}});
      add_shader(CompileTimeStringHash("Battlefront II SSR Temporal Constants CS"), "Luma_SWBF2_SSRConstants", Stage::compute_shader, "main", {{"BF2_SSR_CONSTANT_COUNT", "55"}});

#if ENABLE_BATTLEFRONT2_DLSS_BACKEND
      // The core falls back to None when NGX or the driver does not support DLSS.
      sr_user_type = SR::UserType::DLSS;
      sr_game_tooltip = "Battlefront II DLSS uses this single mode control for activation and NGX quality. Off restores the game's native path.";
#endif
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      add_shader(CompileTimeStringHash("Battlefront II DLSS Scene Conversion VS"), "Luma_SWBF2_SceneConversion", Stage::vertex_shader, nullptr, {{"VERTEXSHADER", "1"}});
      for (const auto& recipe : Battlefront2ScenePacker::Recipes)
      {
         const std::string features = std::to_string(recipe.features);
         for (const auto path : {Battlefront2ScenePacker::Path::Psycho, Battlefront2ScenePacker::Path::NativePost})
            add_shader(recipe.ShaderKey(path), "Luma_SWBF2_PsychoScene", Stage::pixel_shader, nullptr,
               {{"BF2_PACK_FEATURES", features}, {"BF2_NATIVE_POST", path == Battlefront2ScenePacker::Path::NativePost ? "1" : "0"}});
         if (recipe.Has(Battlefront2ScenePacker::Distortion))
            add_shader(recipe.ShaderKey(Battlefront2ScenePacker::Path::DLSS), "Luma_SWBF2_DistortionPack", Stage::pixel_shader, nullptr, {{ "BF2_PACK_FEATURES",
               features }});
      }
      add_shader(CompileTimeStringHash("Battlefront II Transparency Linear Depth PS"), "Luma_SWBF2_TransparencyDepth", Stage::pixel_shader, nullptr);
      add_shader(CompileTimeStringHash("Battlefront II Full Transparency PS"), "Luma_SWBF2_FullTransparency", Stage::pixel_shader, nullptr);
      add_shader(CompileTimeStringHash("Battlefront II Post Depth PS"), "Luma_SWBF2_PostDepth", Stage::pixel_shader, nullptr, {{"BF2_COPY_STENCIL", "0"}});
      add_shader(CompileTimeStringHash("Battlefront II Post Stencil PS"), "Luma_SWBF2_PostDepth", Stage::pixel_shader, nullptr, {{"BF2_COPY_STENCIL", "1"}});
      add_shader(CompileTimeStringHash("Battlefront II Post Weights CS"), "Luma_SWBF2_PostWeights", Stage::compute_shader, nullptr);
      add_shader(CompileTimeStringHash("Battlefront II Post Upsample CS"), "Luma_SWBF2_PostUpsample", Stage::compute_shader, nullptr);
      add_shader(CompileTimeStringHash("Battlefront II DOF Keep PS"), "Luma_SWBF2_DOFKeep", Stage::pixel_shader, nullptr);
      for (bool foreground : {false, true})
         for (bool vertical : {false, true})
            add_shader(Battlefront2DOF::ShaderKey(vertical, foreground), "Luma_SWBF2_DOFGather", Stage::compute_shader, nullptr,
               {{"BF2_DOF_VERTICAL", vertical ? "1" : "0"}, {"BF2_DOF_NEAR", foreground ? "1" : "0"}});
      add_shader(CompileTimeStringHash("Battlefront II Native LUT Split CS"), "Luma_SWBF2_NativeLUT", Stage::compute_shader, nullptr);
      const auto add_display = [&](uint32_t key, std::vector<SimplerShaderDefine> defines)
      {
         add_shader(key, "Luma_SWBF2_PsychoDisplay", Stage::pixel_shader, nullptr, std::move(defines));
      };
      add_display(CompileTimeStringHash("Battlefront II Psycho Display PS"), {});
      add_display(CompileTimeStringHash("Battlefront II Native Post Display PS"), {{"BF2_NATIVE_MAPPER", "1"}});
      add_display(CompileTimeStringHash("Battlefront II Psycho Linear Display PS"), {{"BF2_FINAL_LINEAR", "1"}});
      add_display(CompileTimeStringHash("Battlefront II Native Post Linear Display PS"), {{"BF2_NATIVE_MAPPER", "1"}, {"BF2_FINAL_LINEAR", "1"}});
      add_display(CompileTimeStringHash("Battlefront II GT7 Display PS"), {{"BF2_GT7", "1"}});
      add_display(CompileTimeStringHash("Battlefront II GT7 Linear Display PS"), {{"BF2_GT7", "1"}, {"BF2_FINAL_LINEAR", "1"}});
      add_display(CompileTimeStringHash("Battlefront II ACES2 Display PS"), {{"BF2_ACES2", "1"}});
      add_display(CompileTimeStringHash("Battlefront II ACES2 Linear Display PS"), {{"BF2_ACES2", "1"}, {"BF2_FINAL_LINEAR", "1"}});
      add_display(CompileTimeStringHash("Battlefront II Native Presentation Point PS"), {{"BF2_PRESENTATION_ONLY", "1"}, {"BF2_FINAL_POINT", "1"}});
      add_display(CompileTimeStringHash("Battlefront II Native Presentation Cubic PS"), {{"BF2_PRESENTATION_ONLY", "1"}});
      add_display(CompileTimeStringHash("Battlefront II Native Presentation Linear PS"), {{"BF2_PRESENTATION_ONLY", "1"}, {"BF2_FINAL_LINEAR", "1"}});
      for (unsigned i = 0; i < bf2_video_shader_keys.size(); ++i)
         add_shader(bf2_video_shader_keys[i], "Luma_SWBF2_Video", Stage::pixel_shader, nullptr, {{ "BF2_VIDEO_VARIANT",
            std::to_string(i) }});
#endif
   }

   void LoadConfigs() override
   {
      Battlefront2SSR::Load();
      Battlefront2XeGTAO::Load();
      Battlefront2Psycho::Load();
      reshade::get_config_value(nullptr, NAME, "BattlefrontIIComparisonScreenshots", Battlefront2Compare::menu);
      bool stored_full_res_transparency = true;
      reshade::get_config_value(nullptr, NAME, "BattlefrontIIFullResolutionTransparency", stored_full_res_transparency);
      full_resolution_transparency = stored_full_res_transparency;
      bool stable_depth = true;
      reshade::get_config_value(nullptr, NAME, "BattlefrontIIStableDepthWithDLSS", stable_depth);
      Battlefront2DepthDither::enabled = stable_depth;
#if ENABLE_SR && ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      int stored_mode = static_cast<int>(Battlefront2DLSSMode::Performance);
      if (!reshade::get_config_value(nullptr, NAME, "BattlefrontIIDLSSMode", stored_mode))
      {
         // Profiles from before the DLSS mode setting saved the framework's selector. A fresh installation
         // uses Performance with the Auto render preset, without changing old Off.
         int legacy_type = static_cast<int>(sr_user_type);
         if (reshade::get_config_value(nullptr, NAME, "SRUserType", legacy_type))
            stored_mode = legacy_type == static_cast<int>(SR::UserType::None)
                             ? static_cast<int>(Battlefront2DLSSMode::Off)
                             : static_cast<int>(Battlefront2DLSSMode::Auto);
      }
      ApplyDLSSControlMode(stored_mode);
      unsigned int stored_render_preset = 0; // Auto: NVIDIA's default preset for the mode.
      reshade::get_config_value(nullptr, NAME, "BattlefrontIIDLSSRenderPreset", stored_render_preset);
      dlss_render_preset = dlss_render_preset_ids[DLSSRenderPresetIndex(stored_render_preset)];
      bool stored_disable_dof = true;
      reshade::get_config_value(nullptr, NAME, "BattlefrontIIDLSSDisableDOFTest", stored_disable_dof);
      dlss_disable_dof = stored_disable_dof;
      sr_user_type = dlss_control_mode == static_cast<int>(Battlefront2DLSSMode::Off)
                        ? SR::UserType::None
                        : SR::UserType::DLSS;
#endif
   }

#if ENABLE_SR && ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
   // The same steps as the framework's Super Resolution selector.
   static void ApplySuperResolutionSelection(DeviceData& device_data, SR::Type sr_type)
   {
      if (device_data.sr_type == sr_type) return;
      if (auto* sr_instance_data = device_data.GetSRInstanceData())
         sr_implementations[device_data.sr_type]->ReleaseResources(sr_instance_data);
      device_data.sr_type = sr_type;
      device_data.sr_suppressed = false;
      if (sr_type == SR::Type::None)
      {
         device_data.sr_output_color = nullptr;
         device_data.sr_exposure = nullptr;
         device_data.sr_render_resolution_scale = 1.f;
         device_data.sr_scene_exposure = 1.f;
         device_data.sr_scene_pre_exposure = 1.f;
         game->CleanExtraSRResources(device_data);
      }
   }

   void DrawImGuiSuperResolutionSettings(DeviceData& device_data, SR::UserType& user_type, SR::Type& sr_type)
   {
      auto& game_device_data = GetGameDeviceData(device_data);
      const auto dlss_instance = device_data.sr_implementations_instances.find(SR::Type::DLSS);
      const bool dlss_available = dlss_instance != device_data.sr_implementations_instances.end() && dlss_instance->second != nullptr && dlss_instance->second->is_supported;
      int selected_mode = (std::clamp)(dlss_control_mode,
         static_cast<int>(Battlefront2DLSSMode::Off),
         static_cast<int>(Battlefront2DLSSMode::DLAA));
      Battlefront2UI::DrawDLSSQuality(selected_mode, dlss_available,
         game_device_data.native_render_scale_owned ? game_device_data.native_render_scale_original
                                                    : game_device_data.native_render_scale_observed);

      if (selected_mode != dlss_control_mode)
      {
         ApplyDLSSControlMode(selected_mode);
         game_device_data.dlss_pending_reset = true;
         reshade::set_config_value(nullptr, NAME, "BattlefrontIIDLSSMode", dlss_control_mode);
      }

      game_device_data.dlss_control_mode = dlss_control_mode;
      game_device_data.dlss_quality_mode = dlss_quality_mode;
      game_device_data.dlss_preset_auto = dlss_preset_auto;

      Battlefront2UI::Row("DLSS render preset");
      const auto preset = dlss_render_preset.load();
      if (Battlefront2UI::BeginCombo("##preset", dlss_render_preset_names[DLSSRenderPresetIndex(preset)]))
      {
         for (size_t i = 0; i < std::size(dlss_render_preset_ids); ++i)
         {
            const bool selected = preset == dlss_render_preset_ids[i];
            if (Battlefront2UI::Selectable(dlss_render_preset_names[i], selected))
               ApplyDLSSRenderPreset(device_data, dlss_render_preset_ids[i]);
            if (selected) ImGui::SetItemDefaultFocus();
         }
         ImGui::EndCombo();
      }
      Battlefront2UI::Hint("Changes how DLSS reconstructs detail. Auto uses NVIDIA's default.");

      const bool dlss_requested = dlss_control_mode != static_cast<int>(Battlefront2DLSSMode::Off);
      user_type = dlss_requested && dlss_available ? SR::UserType::DLSS : SR::UserType::None;
      sr_type = user_type == SR::UserType::DLSS ? SR::Type::DLSS : SR::Type::None;

      if (dlss_requested && !dlss_available)
         Battlefront2UI::TextColored(ImVec4(1.f, 0.48f, 0.2f, 1.f), "DLSS unavailable");
      // TextColored/TextDisabled take a format string: text with a '%' goes through "%s".
      else if (dlss_requested && game_device_data.dlss_scene_frames_without_taa > 120)
         Battlefront2UI::TextColored(ImVec4(1.f, 0.68f, 0.25f, 1.f), "%s",
            "Native rendering: DLSS requires Anti-aliasing set to TAA High (Video settings)");
      else if (dlss_requested && game_device_data.dlss_supersampled_frame != ~uint64_t(0) && cb_luma_global_settings.FrameIndex - game_device_data.dlss_supersampled_frame < 120)
         Battlefront2UI::TextColored(ImVec4(1.f, 0.68f, 0.25f, 1.f), "%s",
            "Native rendering: set the game's Resolution Scale to 100% or lower (Video settings)");
      else if (dlss_requested && game_device_data.dlss_recovery_decision.phase == Battlefront2DLSSRecovery::Phase::NativeFallback)
      {
         Battlefront2UI::TextColored(ImVec4(1.f, 0.68f, 0.25f, 1.f), game_device_data.dlss_scale_lease.Owned()
                                                                        ? "Restoring native resolution"
                                                                        : "Native rendering (DLSS unavailable for this scene)");
         if (Battlefront2UI::SmallButton("Retry DLSS")) game_device_data.dlss_recovery.RequestRetry();
      }
      else if (dlss_requested && game_device_data.dlss_recovery_decision.waiting_for_scene)
         Battlefront2UI::TextDisabled("DLSS ready; waiting for a rendered scene.");
      if (dlss_requested && dlss_available && !Battlefront2Executable::Running())
         Battlefront2UI::TextDisabled("%s",
            "On this game version, the game's Resolution Scale sets the DLSS mode (100% = DLAA).");
   }
#endif

   // Windows HDR and its SDR content brightness (used by Native on an HDR display) can change while the game runs, and
   // the game keeps its swapchain, so the output follows the display every frame (each query takes about 3 microseconds).
   static void UpdateDisplay(Battlefront2Psycho::State& tone, HWND window, IDXGISwapChain3* swapchain)
   {
      bool supported = false, enabled = false;
      const bool display_hdr = Display::IsHDRSupportedAndEnabled(window, supported, enabled, swapchain) && supported && enabled;
      const bool changed = display_hdr != tone.display_hdr || !tone.display_checked;
      tone.display_hdr = display_hdr;
      tone.display_checked = true;
      tone.hdr_output = tone.linear_output && display_hdr;
      float nits = 0.f;
      if (display_hdr && Display::GetSDRWhiteLevel(window, nits) && std::isfinite(nits) && nits > 0.f)
         tone.windows_sdr_white = (std::clamp)(nits, 80.f, 480.f);
      if (changed)
      {
         std::ostringstream message;
         message << "[Battlefront II Luma] Windows HDR " << (display_hdr ? "on" : "off") << ", output " << (tone.linear_output ? "FP16" : "8-bit") << '.';
         reshade::log::message(reshade::log::level::info, message.str().c_str());
      }
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      auto& data = GetGameDeviceData(device_data);
      Battlefront2UI::DrawPanel(
         data.psycho,
         [&]
         {
#if ENABLE_SR && ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
            SR::Type selected = device_data.sr_type;
            const auto previous = sr_user_type;
            DrawImGuiSuperResolutionSettings(device_data, sr_user_type, selected);
            if (sr_user_type != previous) reshade::set_config_value(nullptr, NAME, "SRUserType", int(sr_user_type));
            ApplySuperResolutionSelection(device_data, selected);
#endif
            Battlefront2Reflex::DrawControl();
            Battlefront2XeGTAO::DrawControl();
         },
         [&]
         {
            if (Battlefront2UI::BeginRows("focus"))
            {
               Battlefront2UI::Row("Depth of field");
               bool dof = !dlss_disable_dof.load();
               if (Battlefront2UI::Checkbox("##dof", &dof))
               {
                  dlss_disable_dof = !dof;
                  reshade::set_config_value(nullptr, NAME, "BattlefrontIIDLSSDisableDOFTest", !dof);
               }
               ImGui::EndTable();
            }
         },
         [&]
         {
            // These rows follow Battlefront2Psycho's Advanced page. Stable depth and the reflection and AO tuning
            // values are config-only.
            if (Battlefront2UI::BeginRows("rendering"))
            {
               Battlefront2UI::Row("Full-resolution transparency");
               bool transparency = full_resolution_transparency.load();
               if (Battlefront2UI::Checkbox("##transparency", &transparency))
               {
                  full_resolution_transparency = transparency;
                  reshade::set_config_value(nullptr, NAME, "BattlefrontIIFullResolutionTransparency", transparency);
               }
               Battlefront2UI::Hint("Sharper particles and shields at a performance cost.");
               ImGui::EndTable();
            }
            Battlefront2SSR::DrawMenu();
         },
         [&]
         { PrintImGuiAbout(); });
   }

   void OnCreateDevice(ID3D11Device* /*native_device*/, DeviceData& device_data) override
   {
      device_data.game = new Battlefront2GameDeviceData;
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      auto& game_device_data = *static_cast<Battlefront2GameDeviceData*>(device_data.game);
      game_device_data.dlss_control_mode = dlss_control_mode;
      game_device_data.dlss_quality_mode = dlss_quality_mode;
      game_device_data.dlss_preset_auto = dlss_preset_auto;
#endif
   }

   void OnDestroyDeviceData(DeviceData& device_data) override
   {
      // GameDeviceData has no virtual destructor, so the payload is deleted through its own type.
      auto* data = static_cast<Battlefront2GameDeviceData*>(device_data.game);
      if (data == nullptr) return;
      ReleaseNativeRenderScale(*data);
      if (data->reflex_device) Battlefront2Reflex::Shutdown(data->reflex_device);
      delete data;
      device_data.game = nullptr;
   }

   void OnInitDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      // The framework sets one queued frame; keep the game's three so the CPU and GPU overlap. Latency is the
      // driver's low-latency mode's job (swapchain.h).
      if (com_ptr<IDXGIDevice1> dxgi_device; SUCCEEDED(native_device->QueryInterface(&dxgi_device)))
         dxgi_device->SetMaximumFrameLatency(3);
#if ENABLE_BATTLEFRONT2_DLSS_BACKEND
      const auto dlss_instance = device_data.sr_implementations_instances.find(SR::Type::DLSS);
      const bool initialized = dlss_instance != device_data.sr_implementations_instances.end() && dlss_instance->second != nullptr;
      const bool supported = initialized && dlss_instance->second->is_supported;

      std::ostringstream status;
      status << "[Battlefront II Luma] DLSS: initialized=" << (initialized ? 1 : 0)
             << " supported=" << (supported ? 1 : 0)
             << " selected=" << (device_data.sr_type == SR::Type::DLSS ? 1 : 0) << '.';
      reshade::log::message(supported ? reshade::log::level::info : reshade::log::level::warning, status.str().c_str());
#endif
   }

   void OnInitSwapchain(reshade::api::swapchain* swapchain) override
   {
      if (swapchain == nullptr || swapchain->get_device() == nullptr)
         return;
      auto* device = swapchain->get_device();
      if (device->get_api() != reshade::api::device_api::d3d11)
         return;
      DeviceData* device_data = device->get_private_data<DeviceData>();
      // Low latency is set on the device that presents the game: other add-ons and overlays create D3D11 devices of
      // their own, which never present.
      if (device_data != nullptr)
      {
         auto* native_device = reinterpret_cast<ID3D11Device*>(device->get_native());
         GetGameDeviceData(*device_data).reflex_device = native_device;
         Battlefront2Reflex::Initialize(native_device);
      }

#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      auto* native_swapchain = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
      DXGI_SWAP_CHAIN_DESC swapchain_desc = {};
      if (native_swapchain != nullptr && SUCCEEDED(native_swapchain->GetDesc(&swapchain_desc)))
      {
         com_ptr<IDXGISwapChain3> native_swapchain3;
         native_swapchain->QueryInterface(&native_swapchain3);
         // The swapchain object survives ResizeBuffers: apply the color space and ReShade's marker every time.
         if (const auto space = Battlefront2Presentation::ColorSpaceForBuffer(swapchain_desc.BufferDesc.Format))
         {
            const HRESULT applied = Battlefront2Presentation::ApplyColorSpace(native_swapchain3.get(), *space);
            if (FAILED(applied))
            {
               std::ostringstream transport;
               transport << "[Battlefront II Luma] Output transport: "
                         << (*space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 ? "FP16/scRGB" : "SDR/sRGB")
                         << " color space and ReShade state failed (HRESULT=0x" << std::hex << static_cast<unsigned long>(applied) << ").";
               reshade::log::message(reshade::log::level::error, transport.str().c_str());
            }
         }
         if (device_data != nullptr)
         {
            auto& psycho = GetGameDeviceData(*device_data).psycho;
            psycho.linear_output = swapchain_desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            UpdateDisplay(psycho, swapchain_desc.OutputWindow, native_swapchain3.get());
         }
      }

      if (device_data != nullptr)
      {
         // A new generation makes every DLSS output of the old swapchain stale; OnPresent measures the new size.
         ++GetGameDeviceData(*device_data).dlss_swapchain_generation;
         ResetDLSSState(*device_data);
      }
#endif
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(
      ID3D11Device* native_device,
      ID3D11DeviceContext* native_device_context,
      CommandListData& cmd_list_data,
      DeviceData& device_data,
      reshade::api::shader_stage stages,
      const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes,
      bool is_custom_pass,
      bool& updated_cbuffers,
      std::function<void()>* original_draw_dispatch_func) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);
      const bool is_dispatch = stages == reshade::api::shader_stage::all_compute;
      const bool is_immediate_context = native_device_context != nullptr && native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
      // Spark sprites drawn before the temporal pass also go into a layer the eye glare ignores.
      if (!is_custom_pass && is_immediate_context && !is_dispatch && original_draw_dispatch_func && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel) && game_device_data.dlss_post.frame != cb_luma_global_settings.FrameIndex && Battlefront2Sparks::Matches(uint32_t(*original_shader_hashes.pixel_shaders.begin())))
      {
         const auto& tone_settings = game_device_data.psycho.Frame(cb_luma_global_settings.FrameIndex);
         if (tone_settings.values[Battlefront2Psycho::GlareStrength] > 0.f && tone_settings.values[Battlefront2Psycho::Enabled] != 0.f && Battlefront2Psycho::MapperOf(tone_settings) != Battlefront2Psycho::Mapper::Native && game_device_data.sparks.Draw(native_device, native_device_context, cb_luma_global_settings.FrameIndex, *original_draw_dispatch_func))
            return DrawOrDispatchOverrideType::Replaced;
      }
      if (!is_custom_pass && is_dispatch && !is_immediate_context && original_shader_hashes.HasAny(reshade::api::shader_stage::compute) && Battlefront2NativeLUT::SourceCount(*original_shader_hashes.compute_shaders.begin()) >= 0)
         game_device_data.native_lut.MarkDeferredUnsupported();
      Battlefront2PostDLSS::NativeDrawGuard native_depth_guard{
         !is_custom_pass && !is_dispatch && is_immediate_context ? &game_device_data.dlss_post : nullptr,
         cb_luma_global_settings.FrameIndex};
      // The game's LUT generation, AO and reflection dispatches.
      if (!is_custom_pass && is_dispatch && is_immediate_context && original_draw_dispatch_func != nullptr && original_shader_hashes.HasAny(reshade::api::shader_stage::compute))
      {
         const uint32_t cs_hash = static_cast<uint32_t>(*original_shader_hashes.compute_shaders.begin());
         // Snapshot the LUT generators; their dispatch always runs unchanged.
         const auto& groups = last_draw_dispatch_data.dispatch_count;
         game_device_data.native_lut.Observe(native_device, native_device_context, cs_hash,
            last_draw_dispatch_data.indirect ? 0 : groups.x,
            last_draw_dispatch_data.indirect ? 0 : groups.y,
            last_draw_dispatch_data.indirect ? 0 : groups.z);
         if (Battlefront2ShaderVariants::AODepth(cs_hash))
            game_device_data.xegtao.RecordFullDepth(native_device, native_device_context, cb_luma_global_settings.FrameIndex);
         // Runs the game's dispatch once, through XeGTAO for the raw AO pass when that is enabled.
         const auto dispatch = [&]()
         {
            const auto ao_settings = Battlefront2ShaderVariants::RawAO(cs_hash)
                                        ? Battlefront2XeGTAO::Snapshot()
                                        : Battlefront2XeGTAO::Settings{};
            if (Battlefront2ShaderVariants::RawAO(cs_hash) && ao_settings.enabled)
            {
               const auto find_kernel = [&](uint32_t key) -> ID3D11ComputeShader*
               {
                  const auto shader = device_data.native_compute_shaders.find(key);
                  return shader != device_data.native_compute_shaders.end() ? shader->second.get() : nullptr;
               };
               const size_t method = ao_settings.visibility_bitmask ? 1 : 0;
               const size_t quality = (std::min)(ao_settings.quality, 1u);
               const size_t cortao_level = (std::min)(size_t(ao_settings.cortao_level), bf2_cortao_main_keys.size() - 1);
               const Battlefront2XeGTAO::State::Kernels kernels = ao_settings.cortao
                                                                     ? Battlefront2XeGTAO::State::Kernels{true, find_kernel(bf2_cortao_prepass_key), find_kernel(bf2_cortao_main_keys[cortao_level]), find_kernel(bf2_cortao_filter_key)}
                                                                     : Battlefront2XeGTAO::State::Kernels{false, find_kernel(bf2_xegtao_prefilter_keys[method]), find_kernel(bf2_xegtao_main_keys[method][quality]), find_kernel(bf2_xegtao_denoise_key)};
               // XeGTAO or CORTAO rewrites the whole R8 AO target; the game's two blur passes still run after it.
               game_device_data.xegtao.Dispatch(native_device_context, cb_luma_global_settings.FrameIndex, kernels, [&]
                  { (*original_draw_dispatch_func)(); });
            }
            else
               (*original_draw_dispatch_func)();
         };
         if (Battlefront2SSR::Pass(cs_hash) != 0)
         {
            const auto patch_key = Battlefront2SSR::Pass(cs_hash) == 1
                                      ? CompileTimeStringHash("Battlefront II SSR Trace Constants CS")
                                   : Battlefront2SSR::Pass(cs_hash) == 3
                                      ? CompileTimeStringHash("Battlefront II SSR Temporal Constants CS")
                                      : CompileTimeStringHash("Battlefront II SSR Constants CS");
            const auto patch_shader = device_data.native_compute_shaders.find(patch_key);
            Battlefront2SSR::Settings ssr_settings = Battlefront2SSR::Snapshot();
            // When it succeeds, Replay has run the dispatch once with Luma's reflection constants.
            if (patch_shader != device_data.native_compute_shaders.end() && game_device_data.ssr_constants.Replay(native_device_context, cs_hash, ssr_settings, patch_shader->second.get(), dispatch))
               return DrawOrDispatchOverrideType::Replaced;
         }
         if (Battlefront2ShaderVariants::RawAO(cs_hash) && Battlefront2XeGTAO::enabled.load())
         {
            dispatch();
            return DrawOrDispatchOverrideType::Replaced;
         }
      }
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      const uint64_t compute_shader_hash = original_shader_hashes.HasAny(reshade::api::shader_stage::compute)
                                              ? static_cast<uint64_t>(*original_shader_hashes.compute_shaders.begin())
                                              : 0;
      const bool is_battlefront2_temporal_pass = is_dispatch && Battlefront2ShaderVariants::Temporal(compute_shader_hash);
      const auto dispatch_native_temporal = [&]()
      {
         if (!is_battlefront2_temporal_pass || original_draw_dispatch_func == nullptr)
            return false;
         (*original_draw_dispatch_func)();
         return true;
      };
      // The DLSS state below belongs to the immediate context's thread; draws recorded on deferred contexts run on
      // other threads and must not reset or clear it.
      if (is_immediate_context && game_device_data.dlss_observed_sr_type != device_data.sr_type)
         ResetDLSSState(device_data);
      // Resets requested by the UI are applied here, on the render thread. The plain load keeps the
      // read-modify-write off the common path.
      if (is_immediate_context && game_device_data.dlss_pending_reset.load(std::memory_order_relaxed) && game_device_data.dlss_pending_reset.exchange(false))
      {
         game_device_data.dlss_has_drawn_once = false;
         device_data.force_reset_sr = true;
      }
      const bool integration_active = device_data.sr_type == SR::Type::DLSS;

      const bool has_vs_ps = !is_dispatch && original_shader_hashes.HasAny(reshade::api::shader_stage::vertex) && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel);
      const uint64_t vs_hash = has_vs_ps ? *original_shader_hashes.vertex_shaders.begin() : 0;
      const uint64_t ps_hash = has_vs_ps ? *original_shader_hashes.pixel_shaders.begin() : 0;
      const bool scene_packer_vertex = has_vs_ps && vs_hash == 0x11437AAE;
      const bool is_battlefront2_linear_composite = scene_packer_vertex && Battlefront2ScenePacker::Supported(ps_hash);
      const bool is_battlefront2_final_display_composite = has_vs_ps && vs_hash == 0xFA088C75 && Battlefront2ShaderVariants::Display(ps_hash);

      if (!is_dispatch && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel))
      {
         const uint64_t hash = *original_shader_hashes.pixel_shaders.begin();
         const int variant = Battlefront2Video::Find(hash);
         if (variant >= 0 && !is_custom_pass && is_immediate_context && original_draw_dispatch_func)
         {
            const auto is_back_buffer = [&](ID3D11Resource* resource)
            {
               const std::shared_lock lock(device_data.mutex);
               return device_data.back_buffers.contains(reinterpret_cast<uint64_t>(resource));
            };
            const auto shader = device_data.native_pixel_shaders.find(bf2_video_shader_keys[variant]);
            const auto result = Battlefront2Video::Present(native_device, native_device_context, game_device_data.video_presentation,
               shader == device_data.native_pixel_shaders.end() ? nullptr : shader->second.get(), hash,
               game_device_data.psycho.Frame(cb_luma_global_settings.FrameIndex), game_device_data.psycho.display_hdr, game_device_data.psycho.linear_output,
               is_back_buffer, *original_draw_dispatch_func, game_device_data.psycho.windows_sdr_white);
            if (result == Battlefront2Video::Result::Drawn)
               return DrawOrDispatchOverrideType::Replaced;
         }
      }
      bool unknown_scene_packer = false;
      if (integration_active && is_immediate_context && scene_packer_vertex && !is_battlefront2_linear_composite)
         unknown_scene_packer = Battlefront2ScenePackerAudit::Observe(static_cast<uint32_t>(ps_hash));
      const bool observed_scene_packer = is_battlefront2_linear_composite || unknown_scene_packer;
      if (integration_active && is_immediate_context && !is_battlefront2_temporal_pass && is_dispatch && game_device_data.dlss_post.frame == cb_luma_global_settings.FrameIndex)
      {
         game_device_data.dlss_post.ObserveCompute(native_device_context, compute_shader_hash);
         if (!is_custom_pass && original_draw_dispatch_func != nullptr)
            game_device_data.dlss_post.ReplayDOFGather(native_device, native_device_context, device_data, compute_shader_hash);
      }

      if (is_immediate_context && game_device_data.dlss_frame_edges_frame != cb_luma_global_settings.FrameIndex)
      {
         game_device_data.dlss_frame_pack_edges.clear();
         game_device_data.dlss_frame_scene_edges.clear();
         game_device_data.dlss_frame_temporal_outputs.clear();
         game_device_data.dlss_frame_edges_frame = cb_luma_global_settings.FrameIndex;
      }
      if (is_immediate_context && game_device_data.dlss_main_view_frame != cb_luma_global_settings.FrameIndex)
      {
         game_device_data.dlss_main_view_frame = cb_luma_global_settings.FrameIndex;
         game_device_data.dlss_main_view_resource = nullptr;
         game_device_data.dlss_main_view_ambiguous = false;
      }

      // DOF (scene at t0) and full-scene motion blur (scene at t1, t0 is depth) sit between the temporal pass and
      // the scene packer. Their full-screen overwrites are recorded so the packer's input can be traced back to a
      // temporal output. Motion blur also runs at zero intensity.
      const int scene_composite_source_slot = has_vs_ps ? Battlefront2SceneLineage::CompositeSourceSlot(vs_hash, ps_hash) : -1;
      if (integration_active && is_immediate_context && scene_composite_source_slot >= 0)
      {
         com_ptr<ID3D11ShaderResourceView> source_view;
         com_ptr<ID3D11RenderTargetView> target_view;
         native_device_context->PSGetShaderResources(static_cast<UINT>(scene_composite_source_slot), 1, &source_view);
         native_device_context->OMGetRenderTargets(1, &target_view, nullptr);
         com_ptr<ID3D11Resource> source, target;
         com_ptr<ID3D11Texture2D> source_texture, target_texture;
         D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
         D3D11_RENDER_TARGET_VIEW_DESC tv{};
         D3D11_TEXTURE2D_DESC sd{}, td{};
         if (source_view && target_view)
         {
            source_view->GetResource(&source);
            source_view->GetDesc(&sv);
            target_view->GetResource(&target);
            target_view->GetDesc(&tv);
            if (SUCCEEDED(source->QueryInterface(&source_texture)) && SUCCEEDED(target->QueryInterface(&target_texture)))
            {
               source_texture->GetDesc(&sd);
               target_texture->GetDesc(&td);
               UINT viewport_count = 1;
               D3D11_VIEWPORT viewport{};
               native_device_context->RSGetViewports(&viewport_count, &viewport);
               com_ptr<ID3D11BlendState> blend;
               FLOAT blend_factor[4]{};
               UINT sample_mask = 0;
               native_device_context->OMGetBlendState(&blend, blend_factor, &sample_mask);
               D3D11_BLEND_DESC blend_desc{};
               if (blend) blend->GetDesc(&blend_desc);
               const bool overwrite = !blend || (!blend_desc.RenderTarget[0].BlendEnable && blend_desc.RenderTarget[0].RenderTargetWriteMask == D3D11_COLOR_WRITE_ENABLE_ALL);
               if (overwrite && viewport_count == 1 && viewport.TopLeftX == 0 && viewport.TopLeftY == 0 && viewport.Width == float(td.Width) && viewport.Height == float(td.Height) && source != target && sd.Width == td.Width && sd.Height == td.Height && sd.ArraySize == 1 && td.ArraySize == 1 && sd.SampleDesc.Count == 1 && td.SampleDesc.Count == 1 && sd.Format == DXGI_FORMAT_R11G11B10_FLOAT && td.Format == sd.Format && sv.Format == sd.Format && tv.Format == td.Format && sv.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && sv.Texture2D.MostDetailedMip == 0 && tv.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && tv.Texture2D.MipSlice == 0)
               {
                  game_device_data.dlss_frame_scene_edges.push_back({source, target,
                     cb_luma_global_settings.FrameIndex, game_device_data.dlss_generation});
               }
            }
         }
      }

      if (integration_active && observed_scene_packer && is_immediate_context)
      {
         // Records the edge from a temporal output to this packed scene. Whether that is the main view is only
         // known at the final composite below.
         com_ptr<ID3D11ShaderResourceView> native_scene_source;
         native_device_context->PSGetShaderResources(0, 1, &native_scene_source);
         com_ptr<ID3D11Resource> native_scene_resource;
         com_ptr<ID3D11Texture2D> native_scene_texture;
         D3D11_SHADER_RESOURCE_VIEW_DESC native_scene_view_desc = {};
         D3D11_TEXTURE2D_DESC native_scene_desc = {};
         if (native_scene_source != nullptr)
         {
            native_scene_source->GetDesc(&native_scene_view_desc);
            native_scene_source->GetResource(&native_scene_resource);
         }
         if (native_scene_resource != nullptr && SUCCEEDED(native_scene_resource->QueryInterface(&native_scene_texture)))
         {
            native_scene_texture->GetDesc(&native_scene_desc);
         }
         com_ptr<ID3D11RenderTargetView> packed_target_view;
         native_device_context->OMGetRenderTargets(1, &packed_target_view, nullptr);
         D3D11_RENDER_TARGET_VIEW_DESC packed_target_view_desc = {};
         com_ptr<ID3D11Resource> packed_target_resource;
         com_ptr<ID3D11Texture2D> packed_target_texture;
         D3D11_TEXTURE2D_DESC packed_target_desc = {};
         if (packed_target_view != nullptr)
         {
            packed_target_view->GetDesc(&packed_target_view_desc);
            packed_target_view->GetResource(&packed_target_resource);
         }
         if (packed_target_resource != nullptr && SUCCEEDED(packed_target_resource->QueryInterface(&packed_target_texture)))
         {
            packed_target_texture->GetDesc(&packed_target_desc);
         }
         UINT pack_viewport_count = 1;
         D3D11_VIEWPORT pack_viewport = {};
         native_device_context->RSGetViewports(&pack_viewport_count, &pack_viewport);
         const bool pack_valid = native_scene_source != nullptr && native_scene_resource != nullptr && native_scene_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && native_scene_view_desc.Texture2D.MostDetailedMip == 0 && native_scene_view_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && native_scene_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && native_scene_desc.SampleDesc.Count == 1 && native_scene_desc.ArraySize == 1 && native_scene_desc.Width != 0 && native_scene_desc.Height != 0 && packed_target_view != nullptr && packed_target_resource != nullptr && packed_target_view_desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && packed_target_view_desc.Texture2D.MipSlice == 0 && packed_target_view_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM
                                 // Frostbite allocates the packed scene as R10G10B10A2_TYPELESS and binds a UNORM view.
                                 && (packed_target_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || packed_target_desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) && packed_target_desc.SampleDesc.Count == 1 && packed_target_desc.ArraySize == 1 && packed_target_desc.Width == native_scene_desc.Width && packed_target_desc.Height == native_scene_desc.Height && pack_viewport_count == 1 && pack_viewport.TopLeftX == 0 && pack_viewport.TopLeftY == 0 && pack_viewport.Width == static_cast<float>(packed_target_desc.Width) && pack_viewport.Height == static_cast<float>(packed_target_desc.Height);
#if DEVELOPMENT
         if (!pack_valid && game_device_data.dlss_last_evaluated_frame == cb_luma_global_settings.FrameIndex)
         {
            static thread_local unsigned rejection_logs = 0;
            if (rejection_logs < 8)
            {
               ++rejection_logs;
               std::ostringstream log;
               log << "[Battlefront II Luma] Recognized packer contract rejected: frame=" << cb_luma_global_settings.FrameIndex
                   << " ps=0x" << std::hex << ps_hash << std::dec
                   << " source=" << native_scene_desc.Width << 'x' << native_scene_desc.Height
                   << ":format=" << native_scene_desc.Format << ":view=" << native_scene_view_desc.Format
                   << ":dimension=" << native_scene_view_desc.ViewDimension << ":mip=" << native_scene_view_desc.Texture2D.MostDetailedMip
                   << " target=" << packed_target_desc.Width << 'x' << packed_target_desc.Height
                   << ":format=" << packed_target_desc.Format << ":view=" << packed_target_view_desc.Format
                   << " viewport=" << pack_viewport_count << ':' << pack_viewport.Width << 'x' << pack_viewport.Height;
               reshade::log::message(reshade::log::level::warning, log.str().c_str());
            }
         }
#endif
         if (pack_valid)
         {
            size_t root_producer = ~size_t(0);
            const auto temporal_root = Battlefront2SceneLineage::Resolve(
               game_device_data.dlss_frame_scene_edges, native_scene_resource,
               cb_luma_global_settings.FrameIndex, game_device_data.dlss_generation,
               [&](const com_ptr<ID3D11Resource>& resource, size_t end, size_t earliest)
               {
                  for (size_t i = game_device_data.dlss_frame_temporal_outputs.size(); i != 0;)
                  {
                     const auto& output = game_device_data.dlss_frame_temporal_outputs[--i];
                     if (output.resource == resource && output.scene_edge_count >= earliest && output.scene_edge_count <= end)
                     {
                        root_producer = i;
                        return true;
                     }
                  }
                  return false;
               });
            game_device_data.dlss_last_pack_frame = cb_luma_global_settings.FrameIndex;
            game_device_data.dlss_last_pack_shader = ps_hash;
            game_device_data.dlss_last_pack_source = native_scene_resource.get();
            game_device_data.dlss_last_pack_root = temporal_root.get();
            if (temporal_root)
            {
               if (is_battlefront2_linear_composite && game_device_data.dlss_post.frame == cb_luma_global_settings.FrameIndex && game_device_data.dlss_temporal_inputs.native_output.get() == temporal_root.get() && game_device_data.dlss_temporal_inputs.temporal_producer == root_producer && game_device_data.dlss_output_token != 0)
               {
                  if (game_device_data.dlss_post.Finish(native_scene_resource.get()))
                  {
                     game_device_data.dlss_temporal_inputs.late_scene = native_scene_texture;
                     DLSSScenePairPush(game_device_data, native_scene_resource.get(), cb_luma_global_settings.FrameIndex,
                        game_device_data.dlss_generation);
                     game_device_data.dlss_output_mips_token = 0;
                  }
                  else
                  {
                     device_data.has_drawn_sr = false;
                     game_device_data.dlss_output_token = 0;
                  }
               }

               const auto existing_edge = std::find_if(
                  game_device_data.dlss_frame_pack_edges.begin(),
                  game_device_data.dlss_frame_pack_edges.end(),
                  [&](const Battlefront2GameDeviceData::DLSSSceneEdge& edge)
                  {
                     return edge.frame == cb_luma_global_settings.FrameIndex && edge.generation == game_device_data.dlss_generation && edge.temporal_resource.get() == temporal_root.get() && edge.packed_resource.get() == packed_target_resource.get();
                  });
               if (existing_edge == game_device_data.dlss_frame_pack_edges.end())
               {
                  Battlefront2GameDeviceData::DLSSSceneEdge edge;
                  edge.temporal_resource = temporal_root;
                  edge.packed_resource = packed_target_resource;
                  edge.frame = cb_luma_global_settings.FrameIndex;
                  edge.generation = game_device_data.dlss_generation;
                  edge.packer_supported = is_battlefront2_linear_composite;
                  game_device_data.dlss_frame_pack_edges.push_back(std::move(edge));
               }
               else if (!is_battlefront2_linear_composite)
                  existing_edge->packer_supported = false;
            }
         }
      }

      if (is_battlefront2_final_display_composite && is_immediate_context)
      {
         // Only the final draw tells the main scene from same-sized auxiliary views: a temporal output is the main
         // view when the scene at t0 traces back to exactly one of them and the draw targets the back buffer.
         com_ptr<ID3D11ShaderResourceView> final_scene_view;
         native_device_context->PSGetShaderResources(0, 1, &final_scene_view);
         D3D11_SHADER_RESOURCE_VIEW_DESC final_scene_view_desc = {};
         com_ptr<ID3D11Resource> final_scene_resource;
         com_ptr<ID3D11Texture2D> final_scene_texture;
         D3D11_TEXTURE2D_DESC final_scene_desc = {};
         if (final_scene_view != nullptr)
         {
            final_scene_view->GetDesc(&final_scene_view_desc);
            final_scene_view->GetResource(&final_scene_resource);
         }
         if (final_scene_resource != nullptr && SUCCEEDED(final_scene_resource->QueryInterface(&final_scene_texture)))
         {
            final_scene_texture->GetDesc(&final_scene_desc);
         }

         com_ptr<ID3D11RenderTargetView> final_target_view;
         native_device_context->OMGetRenderTargets(1, &final_target_view, nullptr);
         com_ptr<ID3D11Resource> final_target_resource;
         com_ptr<ID3D11Texture2D> final_target_texture;
         D3D11_TEXTURE2D_DESC final_target_desc = {};
         if (final_target_view != nullptr)
            final_target_view->GetResource(&final_target_resource);
         if (final_target_resource != nullptr && SUCCEEDED(final_target_resource->QueryInterface(&final_target_texture)))
         {
            final_target_texture->GetDesc(&final_target_desc);
         }
         bool final_targets_backbuffer = false;
         {
            const std::shared_lock lock(device_data.mutex);
            final_targets_backbuffer = final_target_resource != nullptr && device_data.back_buffers.contains(reinterpret_cast<uint64_t>(final_target_resource.get()));
         }
         const bool final_scene_view_valid = final_scene_view != nullptr && final_scene_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && final_scene_view_desc.Texture2D.MostDetailedMip == 0 && final_scene_view_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM
                                             // Typeless resource, UNORM view, as above.
                                             && (final_scene_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || final_scene_desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) && final_scene_desc.SampleDesc.Count == 1;
         const bool final_target_size_valid = game_device_data.dlss_present_swapchain_width == 0 || (final_target_desc.Width == game_device_data.dlss_present_swapchain_width && final_target_desc.Height == game_device_data.dlss_present_swapchain_height);

         if (final_targets_backbuffer && final_scene_view_valid && final_target_size_valid)
         {
            com_ptr<ID3D11Resource> confirmed_temporal_resource;
            bool ambiguous = false, all_supported = true;
            for (const Battlefront2GameDeviceData::DLSSSceneEdge& edge : game_device_data.dlss_frame_pack_edges)
            {
               if (edge.frame != cb_luma_global_settings.FrameIndex || edge.generation != game_device_data.dlss_generation || edge.packed_resource.get() != final_scene_resource.get())
                  continue;
               all_supported &= edge.packer_supported;
               if (confirmed_temporal_resource == nullptr)
                  confirmed_temporal_resource = edge.temporal_resource;
               else if (confirmed_temporal_resource.get() != edge.temporal_resource.get())
                  ambiguous = true;
            }

            if (ambiguous)
            {
               game_device_data.dlss_main_view_ambiguous = true;
               DLSSMainOutputsClear(game_device_data);
               if (game_device_data.dlss_main_view_logs < 8)
               {
                  ++game_device_data.dlss_main_view_logs;
                  reshade::log::message(reshade::log::level::warning,
                     "[Battlefront II Luma] Main-view lineage ambiguous: a backbuffer scene matched multiple temporal inputs; NGX eligibility cleared.");
               }
            }

            com_ptr<ID3D11Texture2D> confirmed_temporal_texture;
            D3D11_TEXTURE2D_DESC confirmed_temporal_desc = {};
            if (!ambiguous && !game_device_data.dlss_main_view_ambiguous && confirmed_temporal_resource != nullptr && SUCCEEDED(confirmed_temporal_resource->QueryInterface(&confirmed_temporal_texture)))
            {
               confirmed_temporal_texture->GetDesc(&confirmed_temporal_desc);
               if (confirmed_temporal_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && confirmed_temporal_desc.SampleDesc.Count == 1)
               {
                  if (game_device_data.dlss_main_view_resource != nullptr && game_device_data.dlss_main_view_resource != confirmed_temporal_resource.get())
                  {
                     game_device_data.dlss_main_view_ambiguous = true;
                     DLSSMainOutputsClear(game_device_data);
                     if (game_device_data.dlss_main_view_logs < 8)
                     {
                        ++game_device_data.dlss_main_view_logs;
                        reshade::log::message(reshade::log::level::warning,
                           "[Battlefront II Luma] Main-view lineage ambiguous: multiple temporal resources fed backbuffer composites this frame; NGX eligibility cleared.");
                     }
                  }
                  else if (!game_device_data.dlss_main_view_ambiguous)
                  {
                     game_device_data.dlss_main_view_resource = confirmed_temporal_resource.get();
                     const bool main_view_changed =
                        game_device_data.dlss_main_view_generation != game_device_data.dlss_generation || game_device_data.dlss_main_view_swapchain_generation != game_device_data.dlss_swapchain_generation || (game_device_data.dlss_main_temporal_output_width != 0 && (game_device_data.dlss_main_temporal_output_width != confirmed_temporal_desc.Width || game_device_data.dlss_main_temporal_output_height != confirmed_temporal_desc.Height || game_device_data.dlss_main_temporal_output_format != confirmed_temporal_desc.Format));
                     if (main_view_changed)
                        DLSSMainOutputsClear(game_device_data);

                     game_device_data.dlss_pack_support.Confirm(cb_luma_global_settings.FrameIndex,
                        game_device_data.dlss_generation, game_device_data.dlss_swapchain_generation, all_supported);
                     game_device_data.dlss_main_view_generation = game_device_data.dlss_generation;
                     game_device_data.dlss_main_view_swapchain_generation = game_device_data.dlss_swapchain_generation;
                     game_device_data.dlss_main_temporal_output_width = confirmed_temporal_desc.Width;
                     game_device_data.dlss_main_temporal_output_height = confirmed_temporal_desc.Height;
                     game_device_data.dlss_main_temporal_output_format = confirmed_temporal_desc.Format;
                     if (!game_device_data.dlss_main_temporal_output_resources.contains(confirmed_temporal_resource.get()))
                     {
                        if (game_device_data.dlss_main_temporal_output_resources.size() >= Battlefront2GameDeviceData::kDLSSConfirmedMainResourceCount)
                        {
                           const void* oldest_resource = game_device_data.dlss_main_temporal_output_resource_order.front();
                           game_device_data.dlss_main_temporal_output_resources.erase(oldest_resource);
                           game_device_data.dlss_main_temporal_output_resource_order.erase(
                              game_device_data.dlss_main_temporal_output_resource_order.begin());
                        }
                        game_device_data.dlss_main_temporal_output_resource_order.push_back(confirmed_temporal_resource.get());
                     }
                     game_device_data.dlss_main_temporal_output_resources[confirmed_temporal_resource.get()] = confirmed_temporal_resource;
                  }
               }
            }
         }
      }

      if (integration_active && is_immediate_context && !is_custom_pass && original_draw_dispatch_func && full_resolution_transparency.load() && !is_dispatch && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel))
      {
         const uint64_t ps = *original_shader_hashes.pixel_shaders.begin();
         const uint64_t vs = original_shader_hashes.HasAny(reshade::api::shader_stage::vertex)
                                ? *original_shader_hashes.vertex_shaders.begin()
                                : 0;
         auto& pass = game_device_data.full_transparency;
         // Shields keep their sampler bias in both draws. A plain lambda (not std::function) avoids an allocation
         // per draw.
         auto draw = [&]()
         {
            if (!Battlefront2ShieldMipBias::Matches(vs, ps) || !game_device_data.shield_mip_bias.Draw(native_device, native_device_context,
                                                                  game_device_data.native_mip_samplers[4][1].get(), *original_draw_dispatch_func))
               (*original_draw_dispatch_func)();
         };
         if (pass.Draw(native_device, native_device_context, device_data, cb_luma_global_settings.FrameIndex, vs, ps, draw))
            return DrawOrDispatchOverrideType::Replaced;
      }

      if (integration_active && is_immediate_context && !is_custom_pass && original_draw_dispatch_func != nullptr && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel) && Battlefront2SSR::Lighting(*original_shader_hashes.pixel_shaders.begin()) && Battlefront2SSR::Snapshot().spatial_filter)
      {
         const auto shader = device_data.native_compute_shaders.find(CompileTimeStringHash("Battlefront II SSR Spatial Filter CS"));
         auto& filter = game_device_data.ssr_spatial_filter;
         if (shader != device_data.native_compute_shaders.end() && filter.Replay(native_device, native_device_context, shader->second.get(), *original_draw_dispatch_func))
            return DrawOrDispatchOverrideType::Replaced;
      }

      if (integration_active && is_immediate_context && !is_custom_pass && original_draw_dispatch_func != nullptr && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel) && Battlefront2SSR::Lighting(*original_shader_hashes.pixel_shaders.begin()) && Battlefront2SSR::Snapshot().filtered_upsampling)
      {
         if (game_device_data.ssr_upsampling.Replay(native_device, native_device_context, *original_draw_dispatch_func))
            return DrawOrDispatchOverrideType::Replaced;
      }

      if (integration_active && is_immediate_context && !is_custom_pass && original_draw_dispatch_func != nullptr && original_shader_hashes.HasAny(reshade::api::shader_stage::vertex) && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel) && *original_shader_hashes.vertex_shaders.begin() == 0x62B9A472)
      {
         auto* native_sampler = game_device_data.native_mip_samplers[4][1].get();
         auto& shield = game_device_data.shield_mip_bias;
         const bool replaced = Battlefront2ShieldMipBias::Matches(*original_shader_hashes.vertex_shaders.begin(),
                                  *original_shader_hashes.pixel_shaders.begin()) &&
                               shield.Draw(native_device, native_device_context, native_sampler,
                                  *original_draw_dispatch_func);
         if (replaced) return DrawOrDispatchOverrideType::Replaced;
      }

      if (is_battlefront2_temporal_pass && !is_immediate_context)
      {
         if (game_device_data.dlss_non_immediate_skips < 8)
         {
            ++game_device_data.dlss_non_immediate_skips;
            std::ostringstream skip;
            skip << "[Battlefront II Luma] DLSS temporal evaluation skipped on non-immediate context: frame="
                 << cb_luma_global_settings.FrameIndex << ". Native temporal pass is retained.";
            reshade::log::message(reshade::log::level::warning, skip.str().c_str());
         }
         return DrawOrDispatchOverrideType::None;
      }

      if (integration_active && is_immediate_context && original_draw_dispatch_func != nullptr && !is_dispatch && !is_battlefront2_linear_composite && !is_battlefront2_final_display_composite && game_device_data.dlss_post.frame == cb_luma_global_settings.FrameIndex && original_shader_hashes.HasAny(reshade::api::shader_stage::pixel))
      {
         if (game_device_data.dlss_post.Replay(native_device, native_device_context, device_data,
                static_cast<uint64_t>(*original_shader_hashes.pixel_shaders.begin()), scene_composite_source_slot, *original_draw_dispatch_func))
         {
            native_depth_guard.mirrored = true;
            return DrawOrDispatchOverrideType::Replaced;
         }
      }

      if (is_immediate_context && original_draw_dispatch_func != nullptr)
      {
         if (is_battlefront2_linear_composite && DrawPsychoScene(native_device, native_device_context, device_data,
                                                    game_device_data, ps_hash, *original_draw_dispatch_func))
            return DrawOrDispatchOverrideType::Replaced;
         if (is_battlefront2_final_display_composite)
         {
            if (DrawPsychoDisplay(native_device, native_device_context, device_data, game_device_data,
                   *original_draw_dispatch_func, Battlefront2ShaderVariants::LinearDisplay(ps_hash), Battlefront2ShaderVariants::HDRDisplay(ps_hash)))
               return DrawOrDispatchOverrideType::Replaced;
            // Loading and UI-only frames have a final display draw too: they use their own inputs, never an old
            // private scene or DLSS output.
            if (DrawNativePresentation(native_device, native_device_context, device_data, game_device_data,
                   *original_draw_dispatch_func, ps_hash)) return DrawOrDispatchOverrideType::Replaced;
         }
      }

      // Without the private scene above, DLSS still goes through the game's own scene packer, which keeps its
      // color conversion, HDR mapping and UI composition: the packer reruns on the DLSS output into a private
      // full-resolution target that the final display pass then reads. Binding the linear R11G11B10 DLSS output
      // to the final pass directly would skip the conversion (blown-out colors, banding, a flickering background).
      if (is_battlefront2_linear_composite && device_data.sr_type == SR::Type::DLSS && device_data.has_drawn_sr && game_device_data.dlss_output_generation == game_device_data.dlss_generation && game_device_data.dlss_output_token != 0 && game_device_data.dlss_output_srv != nullptr && is_immediate_context && original_draw_dispatch_func != nullptr)
      {
         com_ptr<ID3D11ShaderResourceView> previous_source;
         native_device_context->PSGetShaderResources(0, 1, &previous_source);

         com_ptr<ID3D11Resource> previous_resource;
         if (previous_source != nullptr)
            previous_source->GetResource(&previous_resource);
         D3D11_TEXTURE2D_DESC previous_desc = {};
         com_ptr<ID3D11Texture2D> previous_texture;
         if (previous_resource != nullptr && SUCCEEDED(previous_resource->QueryInterface(&previous_texture)))
            previous_texture->GetDesc(&previous_desc);

         D3D11_RENDER_TARGET_VIEW_DESC target_view_desc = {};
         com_ptr<ID3D11RenderTargetView> target_view;
         native_device_context->OMGetRenderTargets(1, &target_view, nullptr);
         if (target_view != nullptr)
            target_view->GetDesc(&target_view_desc);
         D3D11_TEXTURE2D_DESC native_target_resource_desc = {};
         com_ptr<ID3D11Resource> native_target_resource;
         com_ptr<ID3D11Texture2D> native_target_texture;
         if (target_view != nullptr)
            target_view->GetResource(&native_target_resource);
         if (native_target_resource != nullptr && SUCCEEDED(native_target_resource->QueryInterface(&native_target_texture)))
         {
            native_target_texture->GetDesc(&native_target_resource_desc);
         }
         UINT native_viewport_count = 1;
         D3D11_VIEWPORT native_viewport = {};
         native_device_context->RSGetViewports(&native_viewport_count, &native_viewport);

         if (device_data.sr_output_color == nullptr)
            return DrawOrDispatchOverrideType::None;
         D3D11_TEXTURE2D_DESC dlss_desc = {};
         device_data.sr_output_color->GetDesc(&dlss_desc);

         const bool full_resolution_handoff = game_device_data.dlss_display_rtv != nullptr && game_device_data.dlss_display_srv != nullptr && device_data.native_vertex_shaders[CompileTimeStringHash("Battlefront II DLSS Scene Conversion VS")] != nullptr && dlss_desc.Width == game_device_data.dlss_last_target_width && dlss_desc.Height == game_device_data.dlss_last_target_height && dlss_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && DLSSScenePairContains(game_device_data, previous_resource.get(), cb_luma_global_settings.FrameIndex, game_device_data.dlss_generation) && previous_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && previous_desc.Width == native_target_resource_desc.Width && previous_desc.Height == native_target_resource_desc.Height && native_viewport.Width == static_cast<float>(previous_desc.Width) && native_viewport.Height == static_cast<float>(previous_desc.Height) && target_view_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
         if (!full_resolution_handoff)
            return DrawOrDispatchOverrideType::None;

         // A second matching packer in the same frame is a new attempt; it must not reuse the previous image.
         game_device_data.dlss_display_handoff_seen = false;
         game_device_data.dlss_display_handoff_token = 0;
         // The game's packer still runs into its own target: other passes read it, and skipping it leaves stale
         // alpha in the frame. The callback only issues the game's Draw, which changes no bindings.
         (*original_draw_dispatch_func)();
         const bool converted = DrawDLSSSceneConversion(
            native_device_context,
            device_data,
            game_device_data,
            game_device_data.dlss_post.output_srv.get(),
            game_device_data.dlss_display_rtv.get(),
            game_device_data.dlss_last_target_width,
            game_device_data.dlss_last_target_height,
            game_device_data.dlss_display_crop_x,
            game_device_data.dlss_display_crop_y,
            ps_hash);
         if (!converted)
         {
            if (game_device_data.dlss_scene_conversion_fail_logs < 8)
            {
               ++game_device_data.dlss_scene_conversion_fail_logs;
               reshade::log::message(reshade::log::level::warning, "[Battlefront II Luma] DLSS scene conversion rejected; native linear/final path retained and UI resources untouched.");
            }
            return DrawOrDispatchOverrideType::Replaced; // The game's draw already ran.
         }

         // Only a completed conversion links the packed target to this frame's evaluation.
         DLSSScenePairMarkConverted(game_device_data, previous_resource.get(), native_target_resource.get(), cb_luma_global_settings.FrameIndex);
         game_device_data.dlss_display_srv_owner_scene = native_target_resource.get();
         game_device_data.dlss_display_handoff_seen = true;
         game_device_data.dlss_display_handoff_frame = cb_luma_global_settings.FrameIndex;
         game_device_data.dlss_display_handoff_token = game_device_data.dlss_output_token;
         return DrawOrDispatchOverrideType::Replaced;
      }

      if (is_battlefront2_final_display_composite && device_data.sr_type == SR::Type::DLSS && game_device_data.dlss_display_handoff_seen && game_device_data.dlss_display_handoff_frame == cb_luma_global_settings.FrameIndex && game_device_data.dlss_display_handoff_token != 0 && game_device_data.dlss_display_handoff_token == game_device_data.dlss_output_token && game_device_data.dlss_display_srv != nullptr && is_immediate_context && original_draw_dispatch_func != nullptr)
      {
         com_ptr<ID3D11ShaderResourceView> previous_source;
         native_device_context->PSGetShaderResources(0, 1, &previous_source);
         D3D11_SHADER_RESOURCE_VIEW_DESC previous_source_view_desc = {};
         if (previous_source != nullptr)
            previous_source->GetDesc(&previous_source_view_desc);
         com_ptr<ID3D11Resource> target_resource;
         com_ptr<ID3D11RenderTargetView> target_view;
         native_device_context->OMGetRenderTargets(1, &target_view, nullptr);
         if (target_view != nullptr)
            target_view->GetResource(&target_resource);
         D3D11_TEXTURE2D_DESC target_desc = {};
         com_ptr<ID3D11Texture2D> target_texture;
         if (target_resource == nullptr || FAILED(target_resource->QueryInterface(&target_texture)))
            return DrawOrDispatchOverrideType::None;
         target_texture->GetDesc(&target_desc);
         bool target_is_swapchain_backbuffer = false;
         {
            const std::shared_lock lock(device_data.mutex);
            target_is_swapchain_backbuffer = device_data.back_buffers.contains(
               reinterpret_cast<uint64_t>(target_resource.get()));
         }

         com_ptr<ID3D11Resource> previous_scene_resource;
         com_ptr<ID3D11Texture2D> previous_scene_texture;
         D3D11_TEXTURE2D_DESC previous_scene_desc = {};
         if (previous_source != nullptr)
            previous_source->GetResource(&previous_scene_resource);
         if (previous_scene_resource == nullptr || FAILED(previous_scene_resource->QueryInterface(&previous_scene_texture)))
            return DrawOrDispatchOverrideType::None;
         previous_scene_texture->GetDesc(&previous_scene_desc);

         // Anything else falls back to the game's own image. An FP16 back buffer does not mean Windows HDR is on:
         // Windows also takes scRGB on an SDR display.
         const bool target_format_valid = target_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || target_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || target_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
         const bool scene_tuple_valid = previous_source_view_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && DLSSScenePairContainsPackedConverted(game_device_data, previous_scene_resource.get(), cb_luma_global_settings.FrameIndex, game_device_data.dlss_generation) && game_device_data.dlss_display_srv_owner_scene == previous_scene_resource.get() && previous_scene_desc.Width == game_device_data.dlss_last_input_width && previous_scene_desc.Height == game_device_data.dlss_last_input_height && previous_source_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && previous_source_view_desc.Texture2D.MostDetailedMip == 0;
         if (target_desc.Width != game_device_data.dlss_last_display_width || target_desc.Height != game_device_data.dlss_last_display_height || !target_is_swapchain_backbuffer || !target_format_valid || !scene_tuple_valid)
         {
            if (game_device_data.dlss_final_rejection_logs < 8)
            {
               ++game_device_data.dlss_final_rejection_logs;
               std::ostringstream reject_log;
               reject_log << "[Battlefront II Luma] final candidate detected but rejected: ";
               if (target_desc.Width != game_device_data.dlss_last_display_width || target_desc.Height != game_device_data.dlss_last_display_height)
                  reject_log << "wrong target dimensions";
               else if (!target_is_swapchain_backbuffer)
                  reject_log << "not swapchain backbuffer";
               else if (!target_format_valid)
                  reject_log << "wrong target format";
               else if (!scene_tuple_valid)
                  reject_log << "wrong scene identity";
               reject_log << " target=" << target_desc.Width << 'x' << target_desc.Height << ":fmt=" << target_desc.Format
                          << " scene=" << previous_scene_desc.Width << 'x' << previous_scene_desc.Height << ":fmt=" << previous_source_view_desc.Format
                          << " frame=" << cb_luma_global_settings.FrameIndex << " generation=" << game_device_data.dlss_generation
                          << " pair_converted=" << (DLSSScenePairContainsPackedConverted(game_device_data, previous_scene_resource.get(), cb_luma_global_settings.FrameIndex, game_device_data.dlss_generation) ? 1 : 0)
                          << " owner_match=" << (game_device_data.dlss_display_srv_owner_scene == previous_scene_resource.get() ? 1 : 0)
                          << " expected_target=" << game_device_data.dlss_last_display_width << 'x' << game_device_data.dlss_last_display_height << ".";
               reshade::log::message(reshade::log::level::warning, reject_log.str().c_str());
            }
            return DrawOrDispatchOverrideType::None;
         }

         // Only t0 and cb0 change (cb0 with its D3D11.1 range); the UI overlay (t1) and the auxiliary composite
         // (t12) stay as the game bound them.
         Battlefront2RenderState::PixelConstantBufferBinding previous_constants;
         ID3D11ShaderResourceView* replacement = game_device_data.dlss_display_srv.get();
         native_device_context->PSSetShaderResources(0, 1, &replacement);
         if (!BindFullResolutionFinalCompositeConstants(
                native_device,
                native_device_context,
                game_device_data,
                previous_constants,
                game_device_data.dlss_last_display_width,
                game_device_data.dlss_last_display_height))
         {
            ID3D11ShaderResourceView* original_scene = previous_source.get();
            native_device_context->PSSetShaderResources(0, 1, &original_scene);
            if (previous_constants.buffer != nullptr)
               previous_constants.Restore(native_device_context);
            game_device_data.dlss_display_handoff_seen = false;
            if (game_device_data.dlss_final_constants_logs < 4)
            {
               ++game_device_data.dlss_final_constants_logs;
               reshade::log::message(reshade::log::level::warning, "[Battlefront II Luma] DLSS final scene handoff rejected: unable to preserve and patch Frostbite final-composite constants.");
            }
            return DrawOrDispatchOverrideType::None;
         }
         (*original_draw_dispatch_func)();
         ID3D11ShaderResourceView* original_scene = previous_source.get();
         native_device_context->PSSetShaderResources(0, 1, &original_scene);
         previous_constants.Restore(native_device_context);
         game_device_data.dlss_output_consumed_frame = cb_luma_global_settings.FrameIndex;
         game_device_data.dlss_display_handoff_seen = false;
         game_device_data.dlss_display_handoff_token = 0;
         return DrawOrDispatchOverrideType::Replaced;
      }

      if (is_battlefront2_temporal_pass)
      {
         // A frame can have several temporal dispatches; keep a handoff this frame already produced.
         if (game_device_data.dlss_display_handoff_frame != cb_luma_global_settings.FrameIndex)
            game_device_data.dlss_display_handoff_seen = false;
         game_device_data.dlss_last_temporal_dispatch_frame = cb_luma_global_settings.FrameIndex;
         device_data.taa_detected = true;

         // NGX history advances once per frame. Another temporal dispatch in the same frame (an auxiliary view)
         // stays native.
         if (game_device_data.dlss_last_evaluated_frame == cb_luma_global_settings.FrameIndex || game_device_data.dlss_temporal_inputs.frame == cb_luma_global_settings.FrameIndex)
         {
            // It still writes its output: record it, and drop this frame's DLSS result if it overwrites the
            // main pass's output.
            com_ptr<ID3D11UnorderedAccessView> output_view;
            native_device_context->CSGetUnorderedAccessViews(2, 1, &output_view);
            if (output_view)
            {
               com_ptr<ID3D11Resource> output_resource;
               output_view->GetResource(&output_resource);
               game_device_data.dlss_frame_temporal_outputs.push_back({output_resource,
                  game_device_data.dlss_frame_scene_edges.size()});
               if (game_device_data.dlss_temporal_inputs.native_output.get() == output_resource.get())
               {
                  game_device_data.dlss_post.Fail("temporal-output-overwritten");
                  game_device_data.dlss_output_token = 0;
                  device_data.has_drawn_sr = false;
               }
            }
            return dispatch_native_temporal()
                      ? DrawOrDispatchOverrideType::Replaced
                      : DrawOrDispatchOverrideType::None;
         }

         // The main view's TAA: NGX takes this dispatch's color, depth, motion and jitter. The game's
         // post-processing follows DLSS.
         if (device_data.sr_type == SR::Type::DLSS && original_draw_dispatch_func != nullptr)
         {
            com_ptr<ID3D11ShaderResourceView> shader_resources[6];
            com_ptr<ID3D11UnorderedAccessView> unordered_access_views[6];
            native_device_context->CSGetShaderResources(0, ARRAYSIZE(shader_resources), &shader_resources[0]);
            native_device_context->CSGetUnorderedAccessViews(0, ARRAYSIZE(unordered_access_views), &unordered_access_views[0]);

            const bool scalar_temporal_variant = Battlefront2ShaderVariants::TemporalScalar(compute_shader_hash);
            const bool views_valid = shader_resources[0] && shader_resources[1] && shader_resources[3] && unordered_access_views[2] && unordered_access_views[3] && (!scalar_temporal_variant || (shader_resources[5] && unordered_access_views[4] && unordered_access_views[5]));
            if (views_valid)
            {
               com_ptr<ID3D11Resource> depth_buffer, motion_vectors, source_color, output_color, color_history;
               com_ptr<ID3D11Resource> scalar_source, scalar_output, scalar_history;
               shader_resources[0]->GetResource(&depth_buffer);
               shader_resources[1]->GetResource(&motion_vectors);
               shader_resources[3]->GetResource(&source_color);
               unordered_access_views[2]->GetResource(&output_color);
               unordered_access_views[3]->GetResource(&color_history);
               if (scalar_temporal_variant)
               {
                  shader_resources[5]->GetResource(&scalar_source);
                  unordered_access_views[4]->GetResource(&scalar_output);
                  unordered_access_views[5]->GetResource(&scalar_history);
               }

               com_ptr<ID3D11Texture2D> depth_texture, motion_texture, source_texture, output_texture, color_history_texture;
               com_ptr<ID3D11Texture2D> scalar_source_texture, scalar_output_texture, scalar_history_texture;
               const bool resources_valid = depth_buffer && motion_vectors && source_color && output_color && SUCCEEDED(depth_buffer->QueryInterface(&depth_texture)) && SUCCEEDED(motion_vectors->QueryInterface(&motion_texture)) && SUCCEEDED(source_color->QueryInterface(&source_texture)) && SUCCEEDED(output_color->QueryInterface(&output_texture)) && color_history && SUCCEEDED(color_history->QueryInterface(&color_history_texture)) && (!scalar_temporal_variant || (scalar_source && scalar_output && scalar_history && SUCCEEDED(scalar_source->QueryInterface(&scalar_source_texture)) && SUCCEEDED(scalar_output->QueryInterface(&scalar_output_texture)) && SUCCEEDED(scalar_history->QueryInterface(&scalar_history_texture))));
               if (resources_valid)
               {
                  D3D11_TEXTURE2D_DESC depth_desc{}, motion_desc{}, source_desc{}, output_desc{}, color_history_desc{};
                  D3D11_TEXTURE2D_DESC scalar_source_desc{}, scalar_output_desc{}, scalar_history_desc{};
                  depth_texture->GetDesc(&depth_desc);
                  motion_texture->GetDesc(&motion_desc);
                  source_texture->GetDesc(&source_desc);
                  output_texture->GetDesc(&output_desc);
                  color_history_texture->GetDesc(&color_history_desc);
                  if (scalar_temporal_variant)
                  {
                     scalar_source_texture->GetDesc(&scalar_source_desc);
                     scalar_output_texture->GetDesc(&scalar_output_desc);
                     scalar_history_texture->GetDesc(&scalar_history_desc);
                  }

                  D3D11_SHADER_RESOURCE_VIEW_DESC depth_view_desc{}, motion_view_desc{}, source_view_desc{}, scalar_source_view_desc{};
                  D3D11_UNORDERED_ACCESS_VIEW_DESC output_view_desc{}, color_history_view_desc{}, scalar_output_view_desc{}, scalar_history_view_desc{};
                  shader_resources[0]->GetDesc(&depth_view_desc);
                  shader_resources[1]->GetDesc(&motion_view_desc);
                  shader_resources[3]->GetDesc(&source_view_desc);
                  if (scalar_temporal_variant)
                     shader_resources[5]->GetDesc(&scalar_source_view_desc);
                  unordered_access_views[2]->GetDesc(&output_view_desc);
                  unordered_access_views[3]->GetDesc(&color_history_view_desc);
                  if (scalar_temporal_variant)
                  {
                     unordered_access_views[4]->GetDesc(&scalar_output_view_desc);
                     unordered_access_views[5]->GetDesc(&scalar_history_view_desc);
                  }
                  // The scene depth carries stencil (R32G8X24_TYPELESS, sampled through R32_FLOAT_X8X24); plain
                  // R32_TYPELESS/R32_FLOAT is accepted too.
                  const bool depth_resource_format_valid = depth_desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS || depth_desc.Format == DXGI_FORMAT_R32_TYPELESS;
                  const bool depth_view_format_valid = depth_view_desc.Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || depth_view_desc.Format == DXGI_FORMAT_R32_FLOAT;
                  const bool views_match = depth_resource_format_valid && depth_view_format_valid && depth_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && depth_view_desc.Texture2D.MostDetailedMip == 0 && motion_desc.Format == DXGI_FORMAT_R16G16_TYPELESS && motion_view_desc.Format == DXGI_FORMAT_R16G16_FLOAT && motion_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && motion_view_desc.Texture2D.MostDetailedMip == 0 && source_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && source_view_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && source_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && source_view_desc.Texture2D.MostDetailedMip == 0 && output_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && output_view_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && output_view_desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && output_view_desc.Texture2D.MipSlice == 0 && depth_desc.SampleDesc.Count == 1 && motion_desc.SampleDesc.Count == 1 && source_desc.SampleDesc.Count == 1 && output_desc.SampleDesc.Count == 1;
                  const bool history_matches = source_color != output_color && source_color != color_history && source_desc.Width == output_desc.Width && source_desc.Height == output_desc.Height && output_desc.Format == DXGI_FORMAT_R11G11B10_FLOAT && (output_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 && color_history_desc.Width == source_desc.Width && color_history_desc.Height == source_desc.Height && (color_history_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || color_history_desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) && (color_history_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 && color_history_desc.SampleDesc.Count == 1 && color_history_view_desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM && color_history_view_desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && color_history_view_desc.Texture2D.MipSlice == 0 && (scalar_temporal_variant ? (scalar_source_desc.Width == source_desc.Width && scalar_source_desc.Height == source_desc.Height && scalar_source_desc.SampleDesc.Count == 1 && (scalar_source_desc.Format == DXGI_FORMAT_R16_FLOAT || scalar_source_desc.Format == DXGI_FORMAT_R16_TYPELESS) && (scalar_source_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 && scalar_source_view_desc.Format == DXGI_FORMAT_R16_FLOAT && scalar_source_view_desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D && scalar_source_view_desc.Texture2D.MostDetailedMip == 0 && scalar_output_desc.Width == source_desc.Width && scalar_output_desc.Height == source_desc.Height && scalar_output_desc.SampleDesc.Count == 1 && (scalar_output_desc.Format == DXGI_FORMAT_R16_FLOAT || scalar_output_desc.Format == DXGI_FORMAT_R16_TYPELESS) && (scalar_output_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 && scalar_output_view_desc.Format == DXGI_FORMAT_R16_FLOAT && scalar_output_view_desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && scalar_output_view_desc.Texture2D.MipSlice == 0 && scalar_history_desc.Width == source_desc.Width && scalar_history_desc.Height == source_desc.Height && scalar_history_desc.SampleDesc.Count == 1 && (scalar_history_desc.Format == DXGI_FORMAT_R16_FLOAT || scalar_history_desc.Format == DXGI_FORMAT_R16_TYPELESS) && (scalar_history_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 && scalar_history_view_desc.Format == DXGI_FORMAT_R16_FLOAT && scalar_history_view_desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && scalar_history_view_desc.Texture2D.MipSlice == 0) : true);

                  // A pass with unexpected bindings is not recorded as a temporal output, so it can never become
                  // the main view.
                  if (!views_match || !history_matches)
                  {
                     if (game_device_data.dlss_binding_rejection_logs++ < 8)
                     {
                        std::ostringstream message;
                        message << "[Battlefront II Luma] Temporal contract rejected before scene learning: cs="
                                << std::hex << compute_shader_hash << std::dec
                                << " frame=" << cb_luma_global_settings.FrameIndex
                                << " views=" << views_match << " history=" << history_matches
                                << " depth=" << depth_desc.Format << '/' << depth_view_desc.Format
                                << " motion=" << motion_desc.Format << '/' << motion_view_desc.Format
                                << " scene=" << source_desc.Format << '/' << source_view_desc.Format
                                << " output=" << output_desc.Format << '/' << output_view_desc.Format
                                << " history=" << color_history_desc.Format << '/' << color_history_view_desc.Format;
                        reshade::log::message(reshade::log::level::warning, message.str().c_str());
                     }
                     return dispatch_native_temporal()
                               ? DrawOrDispatchOverrideType::Replaced
                               : DrawOrDispatchOverrideType::None;
                  }
                  const size_t temporal_producer = game_device_data.dlss_frame_temporal_outputs.size();
                  game_device_data.dlss_frame_temporal_outputs.push_back({output_color,
                     game_device_data.dlss_frame_scene_edges.size()});

                  // Only an output already confirmed as the main view (see the final composite above) is
                  // evaluated: auxiliary views use the same shader and formats and must not advance NGX history.
                  const bool main_temporal_output =
                     game_device_data.dlss_main_temporal_output_resources.contains(output_color.get()) && game_device_data.dlss_main_view_generation == game_device_data.dlss_generation && game_device_data.dlss_main_view_swapchain_generation == game_device_data.dlss_swapchain_generation && output_desc.Width == game_device_data.dlss_main_temporal_output_width && output_desc.Height == game_device_data.dlss_main_temporal_output_height && output_desc.Format == game_device_data.dlss_main_temporal_output_format;
                  const bool recent_pack = game_device_data.dlss_pack_support.Recent(cb_luma_global_settings.FrameIndex,
                     game_device_data.dlss_generation, game_device_data.dlss_swapchain_generation);
                  const bool supported_pack = recent_pack && game_device_data.dlss_pack_support.supported;
                  if (!main_temporal_output)
                  {
                     return dispatch_native_temporal()
                               ? DrawOrDispatchOverrideType::Replaced
                               : DrawOrDispatchOverrideType::None;
                  }

                  // The main view can be found through a scene packer Luma does not know; replacing its TAA
                  // still needs supported packers on a current route, and the recovery logic's go-ahead.
                  if (!supported_pack || !game_device_data.dlss_recovery.AllowsEvaluation())
                     return dispatch_native_temporal()
                               ? DrawOrDispatchOverrideType::Replaced
                               : DrawOrDispatchOverrideType::None;
                  // The render size comes from the bound resources: the temporal pass can run below the display
                  // size.
                  const UINT input_width = source_desc.Width;
                  const UINT input_height = source_desc.Height;
                  const UINT output_width = output_desc.Width;
                  const UINT output_height = output_desc.Height;
                  const bool input_size_valid = input_width > 0 && input_height > 0;
                  const bool output_size_valid = output_width > 0 && output_height > 0;

                  Battlefront2Jitter::Sample jitter_sample;
                  const uint64_t jitter_frame = cb_luma_global_settings.FrameIndex;
                  const bool jitter_extracted = Battlefront2Jitter::ReadNativeJitter(
                     native_device_context,
                     input_width,
                     input_height,
                     jitter_sample);
                  bool jitter_frame_valid = jitter_extracted;
                  if (jitter_extracted)
                  {
                     if (!game_device_data.dlss_jitter_frame_seen || game_device_data.dlss_jitter_frame != jitter_frame)
                     {
                        game_device_data.dlss_jitter_frame_seen = true;
                        game_device_data.dlss_jitter_frame = jitter_frame;
                     }
                     else if (std::fabs(game_device_data.dlss_jitter_x - jitter_sample.jitter_x) > 1.0e-5f || std::fabs(game_device_data.dlss_jitter_y - jitter_sample.jitter_y) > 1.0e-5f)
                        jitter_frame_valid = false; // Another temporal dispatch this frame must have the same jitter.
                     game_device_data.dlss_jitter_x = jitter_sample.jitter_x;
                     game_device_data.dlss_jitter_y = jitter_sample.jitter_y;
                     if (!jitter_frame_valid && game_device_data.dlss_jitter_logs < 8)
                     {
                        ++game_device_data.dlss_jitter_logs;
                        std::ostringstream jitter_log;
                        jitter_log << "[Battlefront II Luma] Native jitter read frame=" << jitter_frame
                                   << " source=CS0[28..51] range=" << jitter_sample.first_constant << '+' << jitter_sample.num_constants
                                   << " submitted_pixel=" << std::setprecision(6) << jitter_sample.jitter_x << ',' << jitter_sample.jitter_y
                                   << " native_delta=" << std::setprecision(9) << jitter_sample.native_matrix_delta_x << ',' << jitter_sample.native_matrix_delta_y << '.';
                        reshade::log::message(reshade::log::level::warning, jitter_log.str().c_str());
                     }
                  }
                  else
                  {
                     game_device_data.dlss_jitter_x = 0.f;
                     game_device_data.dlss_jitter_y = 0.f;
                     game_device_data.dlss_jitter_frame = jitter_frame;
                     game_device_data.dlss_jitter_frame_seen = true;
                     if (game_device_data.dlss_jitter_logs < 8)
                     {
                        ++game_device_data.dlss_jitter_logs;
                        std::ostringstream jitter_log;
                        jitter_log << "[Battlefront II Luma] Native jitter unavailable frame=" << jitter_frame
                                   << " input=" << input_width << 'x' << input_height
                                   << " reason=" << jitter_sample.failure
                                   << " range=" << jitter_sample.first_constant << '+' << jitter_sample.num_constants << '.';
                        reshade::log::message(reshade::log::level::warning, jitter_log.str().c_str());
                     }
                  }

                  // The DLSS target has the back buffer size measured at present (output_resolution before the
                  // first present).
                  const UINT swapchain_width = game_device_data.dlss_present_swapchain_width > 0
                                                  ? game_device_data.dlss_present_swapchain_width
                                                  : (device_data.output_resolution.x > 1.f ? static_cast<UINT>(device_data.output_resolution.x) : 0);
                  const UINT swapchain_height = game_device_data.dlss_present_swapchain_height > 0
                                                   ? game_device_data.dlss_present_swapchain_height
                                                   : (device_data.output_resolution.y > 1.f ? static_cast<UINT>(device_data.output_resolution.y) : 0);
                  const bool needs_full_resolution_output = swapchain_width > 0 && swapchain_height > 0;
                  // Frostbite renders ceil(output * scale) rounded up to a multiple of 4, so at the game's own native
                  // scale (Luma's DLAA scale rounds down instead) a swapchain that is not 4-aligned gets a padded
                  // render (2052x1152 for 2049x1152). DLAA reconstructs the whole padded render and the packing takes
                  // a centered 1:1 crop. A render beyond the padding is supersampled, which DLSS cannot downscale.
                  const auto padded_extent = [](UINT render, UINT display)
                  {
                     return render > display && render <= ((display + 3u) & ~3u) ? render : display;
                  };
                  const UINT dlss_target_width = needs_full_resolution_output ? padded_extent(input_width, swapchain_width) : output_width;
                  const UINT dlss_target_height = needs_full_resolution_output ? padded_extent(input_height, swapchain_height) : output_height;
                  const UINT display_crop_x = needs_full_resolution_output ? (dlss_target_width - swapchain_width) / 2 : 0;
                  const UINT display_crop_y = needs_full_resolution_output ? (dlss_target_height - swapchain_height) / 2 : 0;
                  const bool target_size_valid = dlss_target_width > 0 && dlss_target_height > 0;
                  const bool scale_valid = input_size_valid && target_size_valid && input_width <= dlss_target_width && input_height <= dlss_target_height;
                  if (input_size_valid && target_size_valid && !scale_valid)
                     game_device_data.dlss_supersampled_frame = cb_luma_global_settings.FrameIndex;
                  const bool full_resolution_output_ready = needs_full_resolution_output && scale_valid && EnsureFullResolutionDLSSOutput(native_device, device_data, game_device_data, source_desc, dlss_target_width, dlss_target_height) && EnsureFullResolutionDLSSDisplayTarget(native_device, game_device_data, swapchain_width, swapchain_height);
                  // The view, history and UAV checks above already passed.
                  const bool dimensions_valid = input_size_valid && output_size_valid && target_size_valid && output_width == input_width && output_height == input_height && depth_desc.Width == input_width && depth_desc.Height == input_height && motion_desc.Width == input_width && motion_desc.Height == input_height && scale_valid && full_resolution_output_ready;
                  // History is reset when sizes or formats change, not when Frostbite rotates the resources of its
                  // ring.
                  const bool resolution_changed = game_device_data.dlss_last_effective_input_width != input_width || game_device_data.dlss_last_effective_input_height != input_height || game_device_data.dlss_last_output_width != output_width || game_device_data.dlss_last_output_height != output_height || game_device_data.dlss_last_target_width != dlss_target_width || game_device_data.dlss_last_target_height != dlss_target_height || game_device_data.dlss_last_display_width != swapchain_width || game_device_data.dlss_last_display_height != swapchain_height || game_device_data.dlss_last_source_format != source_desc.Format || game_device_data.dlss_last_output_format != output_desc.Format;

                  if (resolution_changed)
                  {
                     // After ResetDLSSState the sizes are zero: the main view was learned in this generation and is
                     // current, so the first frame only records the sizes. Clearing it here cost a native frame and
                     // a second history reset after every start or display resize.
                     if (game_device_data.dlss_last_output_width != 0 || game_device_data.dlss_last_output_height != 0)
                     {
                        ++game_device_data.dlss_generation;
                        DLSSMainViewClear(game_device_data);
                     }
                     game_device_data.dlss_frame_edges_frame = cb_luma_global_settings.FrameIndex;
                     device_data.sr_suppressed = false;
                     device_data.force_reset_sr = true;
                     game_device_data.dlss_has_drawn_once = false;
                     game_device_data.dlss_display_handoff_seen = false;
                     game_device_data.dlss_display_handoff_frame = ~uint64_t(0);
                     game_device_data.dlss_output_token = 0;
                     game_device_data.dlss_display_handoff_token = 0;
                     game_device_data.dlss_output_generation = 0;
                     game_device_data.dlss_output_consumed_frame = ~uint64_t(0);
                     game_device_data.dlss_final_rejection_logs = 0;
                     game_device_data.dlss_final_constants_logs = 0;
                     game_device_data.dlss_last_evaluation_succeeded = false;
                     game_device_data.dlss_failure_logged = false;
                     game_device_data.dlss_setup_logged = false;
                     game_device_data.dlss_last_input_width = input_width;
                     game_device_data.dlss_last_input_height = input_height;
                     game_device_data.dlss_last_effective_input_width = input_width;
                     game_device_data.dlss_last_effective_input_height = input_height;
                     game_device_data.dlss_last_output_width = output_width;
                     game_device_data.dlss_last_output_height = output_height;
                     game_device_data.dlss_last_source_format = source_desc.Format;
                     game_device_data.dlss_last_output_format = output_desc.Format;
                  }

                  game_device_data.dlss_last_target_width = dlss_target_width;
                  game_device_data.dlss_last_target_height = dlss_target_height;
                  game_device_data.dlss_last_display_width = swapchain_width;
                  game_device_data.dlss_last_display_height = swapchain_height;
                  game_device_data.dlss_display_crop_x = display_crop_x;
                  game_device_data.dlss_display_crop_y = display_crop_y;
                  // Logged once per size or format change, only when the setup is rejected.
                  if (resolution_changed || !game_device_data.dlss_setup_logged)
                  {
                     game_device_data.dlss_setup_logged = true;
                     if (!dimensions_valid)
                     {
                        std::ostringstream setup_log;
                        setup_log << "[Battlefront II Luma] DLSS temporal contract generation=" << game_device_data.dlss_generation
                                  << " mode=" << GetDLSSQualityModeName(game_device_data.dlss_quality_mode)
                                  << " input=" << input_width << 'x' << input_height
                                  << " native=" << input_width << 'x' << input_height
                                  << " native_pass_output=" << output_width << 'x' << output_height
                                  << " dlss_target=" << dlss_target_width << 'x' << dlss_target_height
                                  << " display=" << swapchain_width << 'x' << swapchain_height
                                  << " display_crop=" << display_crop_x << ',' << display_crop_y
                                  << " scale=" << (dlss_target_width > 0 ? static_cast<float>(input_width) / static_cast<float>(dlss_target_width) : 0.f)
                                  << " depth=" << depth_desc.Width << 'x' << depth_desc.Height << " fmt=" << depth_desc.Format
                                  << " depth_view_fmt=" << depth_view_desc.Format
                                  << " motion=" << motion_desc.Width << 'x' << motion_desc.Height << " fmt=" << motion_desc.Format
                                  << " source_fmt=" << source_desc.Format << " output_fmt=" << output_desc.Format
                                  << " valid=0 reason=";
                        if (!input_size_valid || !output_size_valid)
                           setup_log << "zero-sized-resource";
                        else if (!scale_valid)
                           setup_log << "supersampled-render-exceeds-display";
                        else if (!full_resolution_output_ready)
                           setup_log << "full-resolution-target-unavailable";
                        else
                           setup_log << "ok";
                        setup_log << ".";
                        reshade::log::message(reshade::log::level::warning, setup_log.str().c_str());
                     }
                  }
                  const bool reset_history = resolution_changed || !game_device_data.dlss_has_drawn_once;

                  if (!jitter_frame_valid)
                  {
                     // Never submit a missing or contradictory jitter; the game's TAA is the frame's output.
                     device_data.sr_suppressed = true;
                     device_data.force_reset_sr = true;
                     device_data.has_drawn_sr = false;
                     game_device_data.dlss_has_drawn_once = false;
                     dispatch_native_temporal();
                     return DrawOrDispatchOverrideType::Replaced;
                  }
                  if (device_data.sr_suppressed)
                  {
                     device_data.sr_suppressed = false;
                     device_data.force_reset_sr = true;
                  }

                  if (dimensions_valid)
                  {
                     auto* sr_instance_data = device_data.GetSRInstanceData();
                     auto sr_implementation = sr_implementations.find(device_data.sr_type);
                     if (sr_instance_data == nullptr || sr_implementation == sr_implementations.end() || sr_implementation->second == nullptr)
                        return dispatch_native_temporal() ? DrawOrDispatchOverrideType::Replaced : DrawOrDispatchOverrideType::None;
                     SR::SettingsData settings_data;
                     settings_data.output_width = dlss_target_width;
                     settings_data.output_height = dlss_target_height;
                     settings_data.render_width = input_width;
                     settings_data.render_height = input_height;
                     // The game's Resolution Scale sets the render size; no second dynamic-resolution loop.
                     settings_data.dynamic_resolution = false;
                     // Linear R11G11B10 scene color; the game's own passes do the display encoding later.
                     settings_data.hdr = true;
                     settings_data.inverted_depth = true; // Frostbite's D32 scene depth is reverse-Z here.
                     settings_data.mvs_jittered = false;  // FastStableVelocity carries no projection jitter.
                     // PS 0x37810D54 writes clip-space current-minus-previous velocity, which the game's temporal
                     // CS turns into currentUV - previousUV with (+0.5, -0.5). NGX wants previous - current in
                     // render pixels.
                     settings_data.mvs_x_scale = -0.5f * static_cast<float>(input_width);
                     settings_data.mvs_y_scale = +0.5f * static_cast<float>(input_height);
                     settings_data.auto_exposure = true;
                     // NGX gets no quality mode from the menu: Luma infers it from the render/output ratio, and the
                     // mode only selects the preset NGX uses on Auto. Frostbite rounds the Ultra Performance render up
                     // to a multiple of 4, which can miss NGX's single size for that mode (856x480 for 853x480 at
                     // 1440p), so Auto passes that mode's own default explicitly.
                     settings_data.render_preset = dlss_render_preset.load();
                     if (settings_data.render_preset == 0 && game_device_data.dlss_control_mode == static_cast<int>(Battlefront2DLSSMode::UltraPerformance))
                        settings_data.render_preset = dlss_ultra_performance_preset;

                     auto& pending = game_device_data.dlss_temporal_inputs;
                     pending.color = source_texture;
                     pending.depth = depth_texture;
                     pending.motion = motion_texture;
                     pending.frame = cb_luma_global_settings.FrameIndex;
                     pending.generation = game_device_data.dlss_generation;
                     pending.swapchain_generation = game_device_data.dlss_swapchain_generation;
                     pending.settings = settings_data;
                     pending.reset = reset_history;
                     pending.jitter_x = jitter_sample.jitter_x;
                     pending.jitter_y = jitter_sample.jitter_y;
                     pending.native_output = output_texture;
                     pending.temporal_producer = temporal_producer;

                     // The game's TAA still runs: bloom and other passes read its output (the raw jittered scene
                     // made small lights flicker in the bloom). NGX reads the untouched input at t3.
                     dispatch_native_temporal();
                     pending.late_scene = output_texture;
                     EvaluateTemporalDLSS(native_device, native_device_context, device_data, game_device_data);
                     if (game_device_data.dlss_last_evaluation_succeeded && game_device_data.dlss_output_token != 0)
                     {
                        // Bloom keeps reading the game's TAA output, never NGX's: development DLSS draws its
                        // watermark into its output at about 65000, which bloom turns into a light source. Begin
                        // points the replayed post passes at the private DLSS image and leaves u2 alone. NGX takes
                        // the jitter as minus the samples' offset from the pixel centres (+y down).
                        game_device_data.dlss_post.light_layer = Battlefront2Psycho::LightGamutExpansionAmount(game_device_data.psycho.uploaded) > 0.f;
                        if (!game_device_data.dlss_post.Begin(native_device, output_texture.get(),
                               device_data.sr_output_color.get(), game_device_data.dlss_output_srv.get(), cb_luma_global_settings.FrameIndex,
                               -pending.jitter_x, -pending.jitter_y))
                        {
                           game_device_data.dlss_post.Fail("private-temporal-scene");
                           device_data.has_drawn_sr = false;
                           game_device_data.dlss_output_token = 0;
                        }
                        game_device_data.dlss_post.BuildShares(native_device, native_device_context, device_data, source_texture.get());
                     }
                     // On failure the game's TAA result stands; dispatching again would advance its history twice.
                     return DrawOrDispatchOverrideType::Replaced;
                  }
               }
            }
         }
      }
#endif

      if (dispatch_native_temporal())
         return DrawOrDispatchOverrideType::Replaced;
      return DrawOrDispatchOverrideType::None;
   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      Battlefront2Compare::Expire();
      if (!Battlefront2XeGTAO::enabled.load()) GetGameDeviceData(device_data).xegtao.Reset();
      engine_fixes.Update();
      depth_dither.Update(Battlefront2DepthDither::enabled.load() && device_data.sr_type == SR::Type::DLSS);
      const auto ssr_settings = Battlefront2SSR::Snapshot();
      ssr_resolution.Update(ssr_settings.full_resolution);
      ssr_velocity.Update(ssr_settings.velocity_temporal);
      UpdateDisplay(GetGameDeviceData(device_data).psycho, game_window, device_data.GetMainNativeSwapchain().get());
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
      auto& game_device_data = GetGameDeviceData(device_data);
      const uint64_t recovery_frame = cb_luma_global_settings.FrameIndex;
      if (game_device_data.dlss_last_temporal_dispatch_frame == recovery_frame)
         game_device_data.dlss_scene_frames_without_taa = 0;
      else if (game_device_data.dlss_last_pack_frame == recovery_frame && game_device_data.dlss_scene_frames_without_taa != (std::numeric_limits<uint32_t>::max)())
         ++game_device_data.dlss_scene_frames_without_taa;
      const auto recovery_preset = dlss_render_preset.load();
      if (game_device_data.dlss_recovery_mode != dlss_control_mode || game_device_data.dlss_recovery_preset != recovery_preset)
      {
         game_device_data.dlss_recovery_mode = dlss_control_mode;
         game_device_data.dlss_recovery_preset = recovery_preset;
         game_device_data.dlss_recovery.RequestRetry();
      }
      const bool observed_main_route = game_device_data.dlss_main_view_frame == recovery_frame && game_device_data.dlss_main_view_resource != nullptr && !game_device_data.dlss_main_view_ambiguous && game_device_data.dlss_main_view_generation == game_device_data.dlss_generation && game_device_data.dlss_main_view_swapchain_generation == game_device_data.dlss_swapchain_generation;
      const bool recovery_route = observed_main_route && game_device_data.dlss_pack_support.AllowsEvaluation(
                                                            recovery_frame, game_device_data.dlss_generation, game_device_data.dlss_swapchain_generation);
      game_device_data.dlss_recovery_decision = game_device_data.dlss_recovery.Tick({GetTickCount64(), device_data.sr_type == SR::Type::DLSS && dlss_control_mode != 0,
         game_device_data.dlss_last_pack_frame == recovery_frame || game_device_data.psycho.scene_frame == recovery_frame,
         recovery_route,
         game_device_data.dlss_last_evaluated_frame == recovery_frame && game_device_data.dlss_last_evaluation_succeeded,
         game_device_data.dlss_output_consumed_frame == recovery_frame,
         game_device_data.dlss_swapchain_generation, !game_device_data.dlss_scale_lease.Owned(),
         observed_main_route && !recovery_route, device_data.taa_detected});
      if (game_device_data.dlss_recovery_decision.attempt_started || game_device_data.dlss_recovery_decision.returned_to_native)
      {
         device_data.force_reset_sr = true;
         game_device_data.dlss_has_drawn_once = false;
         game_device_data.dlss_output_token = 0;
      }
      UpdateNativeRenderScale(device_data);
      if (!full_resolution_transparency.load() || device_data.sr_type != SR::Type::DLSS)
         game_device_data.full_transparency.ready = game_device_data.full_transparency.composited = false;
      game_device_data.dlss_post.bypass_dof = dlss_disable_dof.load();
      game_device_data.dlss_post.bypass_motion_blur = Battlefront2Psycho::Snapshot().values[Battlefront2Psycho::MotionBlurEnabled] == 0.f;
      if (game_device_data.dlss_observed_sr_type != device_data.sr_type)
      {
         // The framework keeps the NGX feature while SR is off, but its history is stale after native frames.
         game_device_data.dlss_observed_sr_type = device_data.sr_type;
         ++game_device_data.dlss_generation;
         game_device_data.dlss_has_drawn_once = false;
         game_device_data.dlss_last_evaluation_succeeded = false;
         game_device_data.dlss_last_evaluated_frame = ~uint64_t(0);
         game_device_data.dlss_evaluated_since_reset = false;
         game_device_data.dlss_output_token = 0;
         game_device_data.dlss_display_handoff_token = 0;
         game_device_data.dlss_display_handoff_seen = false;
         game_device_data.dlss_display_handoff_frame = ~uint64_t(0);
         DLSSScenePairClear(game_device_data);
         DLSSMainViewClear(game_device_data);
         device_data.force_reset_sr = true;
         device_data.sr_suppressed = false;
         device_data.has_drawn_sr = false;
      }
      if (!custom_texture_mip_lod_bias_offset)
      {
         const bool dlss_scene_consumed = game_device_data.dlss_output_consumed_frame == cb_luma_global_settings.FrameIndex && game_device_data.dlss_last_effective_input_height > 0 && game_device_data.dlss_last_target_height > 0;
         // The bias follows the measured NGX input (aligned sizes included); native frames get no bias.
         const float new_mip_bias = Battlefront2MipBias::Offset(
            game_device_data.dlss_last_effective_input_width,
            game_device_data.dlss_last_target_width,
            dlss_scene_consumed);
         float previous_mip_bias = 0.f;
         {
            std::unique_lock lock_samplers(s_mutex_samplers);
            previous_mip_bias = device_data.texture_mip_lod_bias_offset;
            device_data.texture_mip_lod_bias_offset = new_mip_bias;
         }
         if (std::abs(previous_mip_bias - new_mip_bias) > 0.001f)
            RefreshMipBiasSamplers(native_device, device_data.primary_command_list.get(), game_device_data.mip_variants, new_mip_bias, game_device_data.native_mip_samplers);
      }

      // The largest back buffer gives the display size; output_resolution can lag behind a resize.
      UINT current_swapchain_width = 0;
      UINT current_swapchain_height = 0;
      for (const uint64_t back_buffer_handle : device_data.back_buffers)
      {
         com_ptr<ID3D11Texture2D> back_buffer_texture;
         if (SUCCEEDED(reinterpret_cast<ID3D11Resource*>(back_buffer_handle)->QueryInterface(&back_buffer_texture)))
         {
            D3D11_TEXTURE2D_DESC back_buffer_desc = {};
            back_buffer_texture->GetDesc(&back_buffer_desc);
            if (back_buffer_desc.Width > current_swapchain_width)
            {
               current_swapchain_width = back_buffer_desc.Width;
               current_swapchain_height = back_buffer_desc.Height;
            }
         }
      }
      if (current_swapchain_width == 0 || current_swapchain_height == 0)
      {
         current_swapchain_width = device_data.output_resolution.x > 1.f ? static_cast<UINT>(device_data.output_resolution.x) : 0;
         current_swapchain_height = device_data.output_resolution.y > 1.f ? static_cast<UINT>(device_data.output_resolution.y) : 0;
      }
      if (game_device_data.dlss_present_swapchain_width != current_swapchain_width || game_device_data.dlss_present_swapchain_height != current_swapchain_height)
      {
         game_device_data.dlss_present_swapchain_width = current_swapchain_width;
         game_device_data.dlss_present_swapchain_height = current_swapchain_height;
         ++game_device_data.dlss_swapchain_generation;
      }
      if (game_device_data.dlss_last_swapchain_width != current_swapchain_width || game_device_data.dlss_last_swapchain_height != current_swapchain_height)
      {
         ResetDLSSState(device_data);
         game_device_data.dlss_last_swapchain_width = current_swapchain_width;
         game_device_data.dlss_last_swapchain_height = current_swapchain_height;
      }
      const bool evaluated_this_frame = game_device_data.dlss_last_evaluated_frame == cb_luma_global_settings.FrameIndex && game_device_data.dlss_last_evaluation_succeeded;
      const bool dlss_scene_consumed = game_device_data.dlss_output_consumed_frame == cb_luma_global_settings.FrameIndex;
      if (!device_data.taa_detected || (device_data.sr_type == SR::Type::DLSS && !evaluated_this_frame))
      {
         device_data.force_reset_sr = true;
         device_data.sr_suppressed = false;
         game_device_data.dlss_has_drawn_once = false;
         game_device_data.dlss_display_handoff_seen = false;
         game_device_data.dlss_display_handoff_frame = ~uint64_t(0);
         game_device_data.dlss_display_handoff_token = 0;
      }

      const uint32_t previous_sr_type = cb_luma_global_settings.SRType;
      const uint32_t current_sr_type = (device_data.sr_type == SR::Type::DLSS && dlss_scene_consumed && !device_data.sr_suppressed) ? (uint32_t(device_data.sr_type) + 1) : 0;
      cb_luma_global_settings.SRType = current_sr_type;
      if (previous_sr_type != current_sr_type)
         device_data.cb_luma_global_settings_dirty = true;

      device_data.has_drawn_sr = false;
      device_data.taa_detected = false;
      game_device_data.dlss_temporal_inputs.native_output = nullptr;
      game_device_data.dlss_temporal_inputs.late_scene = nullptr;
      // Scene and output identities last one frame, consumed or not, so a later pass of the same size can never
      // show stale pixels.
      game_device_data.dlss_display_handoff_seen = false;
      game_device_data.dlss_display_handoff_frame = ~uint64_t(0);
      game_device_data.dlss_display_handoff_token = 0;
      DLSSScenePairClear(game_device_data);
      game_device_data.dlss_frame_pack_edges.clear();
      game_device_data.dlss_frame_edges_frame = ~uint64_t(0);
#endif
   }

   void PrintImGuiAbout() override
   {
      // The buttons keep ReShade's font: it has their icons.
      {
         const Battlefront2UI::MenuFontScope font;
         ImGui::PushTextWrapPos(0.f);
         ImGui::Text(
            "Luma for \"STAR WARS Battlefront II\" is developed by PEQHUB and is open source and free.\n"
            "Requires DirectX 11 (DX12 off) and Anti-Aliasing set to TAA High.");
         ImGui::PopTextWrapPos();
      }

      ImGui::NewLine();
      static const std::string social_link = std::string("Join our \"HDR Den\" Discord ") + std::string(ICON_FK_SEARCH);
      if (ImGui::Button(social_link.c_str()))
      {
         // Unique link for Luma's HDR Den (tracks the origin of people joining); do not share for other purposes.
         static const std::string discord_link = std::string("https://discord.gg/J9fM") + std::string("3EVuEZ");
         ShellExecuteA(nullptr, "open", discord_link.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
      static const std::string contributing_link = std::string("Contribute on Github ") + std::string(ICON_FK_FILE_CODE);
      if (ImGui::Button(contributing_link.c_str()))
      {
         ShellExecuteA(nullptr, "open", "https://github.com/Filoppi/Luma-Framework", nullptr, nullptr, SW_SHOWNORMAL);
      }

      const Battlefront2UI::MenuFontScope font;
      ImGui::NewLine();
      ImGui::Text("Build Date: %s %s", __DATE__, __TIME__);

      ImGui::NewLine();
      ImGui::Text("Credits:"
                  "\n\nMain:"
                  "\nPEQHUB"
                  "\n\nLuma Framework:"
                  "\nPumbo"
                  "\n\nThird Party:"
                  "\nReShade"
                  "\nImGui"
                  "\nRenoDX and PsychoV (ShortFuse)"
                  "\nXeGTAO (Intel)"
                  "\nCORTAO (Daniel Isheden)"
                  "\nGT7 Tone Mapping (Polyphony Digital)"
                  "\nACES"
                  "\nNVIDIA DLSS");
   }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      const char* project_name = PROJECT_NAME;
      const char* cleared_project_name = (project_name[0] == '_') ? (project_name + 1) : project_name;

      // Increment this when settings or shader binaries need an intentional reset.
      constexpr uint32_t mod_version = 1;
      Globals::SetGlobals(cleared_project_name,
         "STAR WARS Battlefront II (2017) Luma mod", "", mod_version);
      Globals::DEVELOPMENT_STATE = Globals::ModDevelopmentState::Finished;

      // Preserve native formats while enabling Luma's independent-flip path. The game's samplers get DLSS's mip bias
      // from OnPushSamplers (samplers.h), not from Luma's generic sampler upgrade, which also changed shadow samplers.
      swapchain_format_upgrade_type = TextureFormatUpgradesType::None;
      swapchain_upgrade_type = SwapchainUpgradeType::None;
      texture_format_upgrades_type = TextureFormatUpgradesType::None;
      texture_upgrade_formats.clear();

#if ALLOW_SHADERS_DUMPING
      auto_dump = false; // Only the shader cache is needed, not dumps on disk.
#endif
      game = new Battlefront2Game();
   }
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
   static bool events_registered = false;
   if (ul_reason_for_call == DLL_PROCESS_DETACH && events_registered)
   {
      static_cast<Battlefront2Game*>(game)->UnregisterEvents();
      events_registered = false;
   }
#endif

   const BOOL result = CoreMain(hModule, ul_reason_for_call, lpReserved);
#if ENABLE_BATTLEFRONT2_DLSS_TEMPORAL_HOOK
   if (ul_reason_for_call == DLL_PROCESS_ATTACH && result)
   {
      static_cast<Battlefront2Game*>(game)->RegisterEvents();
      events_registered = true;
   }
#endif
   return result;
}
