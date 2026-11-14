#pragma once
// HDR output: settings, the tone mappers (PsychoV, GT7, ACES 2.0, the game's own), the game's color grading LUT, video and comparison screenshots.

#include <array>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <atomic>
#include <memory>
#include <cstring>
#include <functional>
#include <Windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#include <d3d11_1.h>
#include "aces2_output.h"
#include "game_passes.h"
#include "../../External/reshade/source/com_ptr.hpp"

namespace Battlefront2Psycho
{
   // Indexes the float array uploaded as the shaders' PsychoControls cbuffer
   // (Psycho[19], four values per row, Includes/PsychoSettings.hlsl). The order
   // is the shader layout; UnusedN slots are free.
   enum Field : unsigned
   {
      Enabled,
      HDRPeak,
      HDRPaper,
      HDRUI,
      ExposureEV,
      Highlights,
      Shadows,
      Contrast,
      GlareStrength,
      Unused9,
      Unused10,
      Purity,
      Unused12,
      Unused13,
      Unused14,
      GamutCompression,
      HDRGamut,
      Compression,
      Unused18,
      Unused19,
      Unused20,
      Unused21,
      Unused22,
      SDRGamut,
      AdaptiveR,
      AdaptiveG,
      AdaptiveB,
      Grain,
      BackgroundR,
      BackgroundG,
      BackgroundB,
      SDRWhite,
      OutputHDR,
      OutputLinear,
      DisplayHDR,
      LUTStrength,
      NativeUI,
      DOFDisabled,
      BloomStrength,
      VignetteStrength,
      SDRDither,
      PrivatePrecision,
      MotionBlurEnabled,
      DistortionStrength,
      GrainFrame,
      GrainFallback,
      VignetteFallback,
      Unused47,
      NativeLUTMatrix,
      NativeLUTGamma,
      NativeLUTBrightness,
      NativeLUTPreExposure,
      NativeLUTCompression,
      NativeLUTPostExposure,
      // Set by the addon, never saved: whether the game's final pass is its HDR
      // variant, and the Windows SDR content brightness (nits) Native uses.
      NativeHDRDisplay,
      WindowsSDRWhite,
      // GT7 and ACES 2.0 have their own controls; PsychoV's do not apply to them.
      GT7Exposure,
      GT7ChromaSpace,
      GT7PaperWhite,
      ACES2Exposure,
      ACES2Limiting,
      // GT7 curve and chroma parameters (gt7_tone_mapping.cpp initializeCurve / initializeParameters).
      GT7ToeStrength,
      GT7MidPoint,
      GT7LinearSection,
      GT7Alpha,
      GT7BlendRatio,
      GT7FadeStart,
      GT7FadeEnd,
      // ACES 2.0 creative white and the tone scale / chroma constants of init_ODTParams.
      ACES2White,
      ACES2MidGrey,
      ACES2GreyLift,
      ACES2Contrast,
      ACES2Flare,
      ACES2ChromaCompression,
      ACES2ChromaExpansion,
      Unused75,
      Count
   };
   struct alignas(16) Settings
   {
      std::array<float, Count> values{};
   };
   // Enabled selects the tone mapper; any nonzero value runs the Luma display path.
   // 1 was PsychoV31 (removed): saved profiles with it load as PsychoV.
   enum class Mapper : int
   {
      Native = 0,
      PsychoV = 2,
      GT7 = 3,
      ACES2 = 4
   };
   inline constexpr float MapperPsychoV = 2.f, MapperACES2 = 4.f;
   inline constexpr std::array<Mapper, 4> mappers{Mapper::Native, Mapper::PsychoV, Mapper::GT7, Mapper::ACES2};
   inline Mapper MapperOf(const Settings& s)
   {
      const float v = s.values[Enabled];
      return v >= 3.5f ? Mapper::ACES2 : v >= 2.5f ? Mapper::GT7
                                      : v >= 0.5f  ? Mapper::PsychoV
                                                   : Mapper::Native;
   }
   inline const char* MapperName(Mapper m)
   {
      switch (m)
      {
      case Mapper::PsychoV:
         return "PsychoV";
      case Mapper::GT7:
         return "GT7";
      case Mapper::ACES2:
         return "ACES 2.0";
      default:
         return "Native";
      }
   }
   // Exposure compensation of the selected tone mapper; each keeps its own.
   inline Field ExposureField(Mapper m)
   {
      return m == Mapper::GT7 ? GT7Exposure : m == Mapper::ACES2 ? ACES2Exposure
                                                                 : ExposureEV;
   }
   static_assert(sizeof(Settings) == 19 * 16);
   struct Control
   {
      Field field;
      const char* key;
      float value, minimum, maximum;
   };
   // Defaults for a new profile and for the reset controls, with each value's range.
   inline constexpr Control controls[] = {
      {Enabled, "PsychoEnabled", MapperPsychoV, 0, 4},
      {LUTStrength, "PsychoLUTStrength", 1, 0, 1},
      {NativeLUTMatrix, "NativeLUTMatrix", 1, 0, 1},
      {NativeLUTGamma, "NativeLUTGamma", 0, 0, 1},
      {NativeLUTBrightness, "NativeLUTBrightness", 0, 0, 1},
      {NativeLUTPreExposure, "NativeLUTPreExposure", 1, 0, 1},
      {NativeLUTCompression, "NativeLUTCompression", 0, 0, 1},
      {NativeLUTPostExposure, "NativeLUTPostExposure", 0, 0, 1},
      {HDRPeak, "PsychoHDRPeak", 1000, 100, 10000},
      {HDRPaper, "PsychoHDRPaper", 203, 80, 1000},
      {NativeUI, "PsychoNativeUI", 0, 0, 1},
      {HDRUI, "PsychoHDRUI", 203, 80, 1000},
      {ExposureEV, "PsychoExposureEV", 0, -8, 8},
      {Highlights, "PsychoHighlights", 1, 0.1f, 4},
      {Shadows, "PsychoShadows", 1, 0.1f, 4},
      {Contrast, "PsychoContrast", 1, 0.1f, 4},
      {Purity, "PsychoPurity", 1, 0, 2},
      {GamutCompression, "PsychoGamutCompression", 0, 0, 1},
      {HDRGamut, "PsychoHDRGamut", 1, 0, 1},
      {Compression, "PsychoCompression", 1.5f, 1, 8},
      {SDRGamut, "PsychoSDRGamut", 0, 0, 1},
      {AdaptiveR, "PsychoAdaptiveR", 0.18f, 0.001f, 4},
      {AdaptiveG, "PsychoAdaptiveG", 0.18f, 0.001f, 4},
      {AdaptiveB, "PsychoAdaptiveB", 0.18f, 0.001f, 4},
      {Grain, "PsychoGrain", 0, 0, 2},
      {BackgroundR, "PsychoBackgroundR", 0.18f, 0.001f, 0.95f},
      {BackgroundG, "PsychoBackgroundG", 0.18f, 0.001f, 0.95f},
      {BackgroundB, "PsychoBackgroundB", 0.18f, 0.001f, 0.95f},
      {SDRWhite, "PsychoSDRWhite", 100, 80, 300},
      {BloomStrength, "PostBloomStrength", 1, 0, 2},
      {GlareStrength, "PostGlareStrength", 1, 0, 4},
      {VignetteStrength, "PostVignetteStrength", 0, 0, 1},
      {SDRDither, "PostSDRDither", 1, 0, 1},
      {PrivatePrecision, "PostPrivatePrecision", 0, 0, 1},
      {MotionBlurEnabled, "PostMotionBlurEnabled", 1, 0, 1},
      {DistortionStrength, "PostDistortionStrength", 1, 0, 1},
      // Reference defaults: Polyphony's gt7_tone_mapping.cpp (ICtCp, 250-nit
      // paper white) and the ACES 2.0 HDR presets (P3-D65 limited).
      {GT7Exposure, "GT7Exposure", 0, -8, 8},
      {GT7ChromaSpace, "GT7ChromaSpace", 0, 0, 1},
      // HDR only; SDR always uses Polyphony's 250 nits (the sample's SDR path).
      {GT7PaperWhite, "GT7HDRSceneWhite", 250, 80, 1000},
      {GT7ToeStrength, "GT7ToeStrength", 1.28f, .5f, 3.f},
      {GT7MidPoint, "GT7MidPoint", .538f, .1f, 1.5f},
      {GT7LinearSection, "GT7LinearSection", .444f, .1f, .9f},
      {GT7Alpha, "GT7Alpha", .25f, 0.f, .9f},
      {GT7BlendRatio, "GT7BlendRatio", .6f, 0.f, 1.f},
      {GT7FadeStart, "GT7FadeStart", .98f, .5f, 1.5f},
      {GT7FadeEnd, "GT7FadeEnd", 1.16f, .6f, 2.f},
      {ACES2Exposure, "ACES2Exposure", 0, -8, 8},
      {ACES2Limiting, "ACES2LimitingPrimaries", 1, 0, 2},
      {ACES2White, "ACES2CreativeWhite", 0, 0, 1},
      {ACES2MidGrey, "ACES2MidGrey", 10.013f, 5.f, 20.f},
      {ACES2GreyLift, "ACES2GreyLift", .14f, 0.f, .3f},
      {ACES2Contrast, "ACES2Contrast", 1.15f, .9f, 1.5f},
      {ACES2Flare, "ACES2Flare", .04f, 0.f, .1f},
      {ACES2ChromaCompression, "ACES2ChromaCompression", 1, 0, 2},
      {ACES2ChromaExpansion, "ACES2ChromaExpansion", 1, 0, 2},
   };
   inline Settings Defaults()
   {
      Settings s;
      for (const auto& c : controls)
         s.values[c.field] = c.value;
      return s;
   }
   inline bool LUTBypassed(const Settings& s)
   {
      return s.values[LUTStrength] == 0.f && s.values[NativeLUTMatrix] == 0.f && s.values[NativeLUTGamma] == 0.f && s.values[NativeLUTBrightness] == 0.f && s.values[NativeLUTPreExposure] == 0.f && s.values[NativeLUTCompression] == 0.f && s.values[NativeLUTPostExposure] == 0.f;
   }
   inline bool LUTNative(const Settings& s)
   {
      return s.values[LUTStrength] == 1.f && s.values[NativeLUTMatrix] == 1.f && s.values[NativeLUTGamma] == 1.f && s.values[NativeLUTBrightness] == 1.f && s.values[NativeLUTPreExposure] == 1.f && s.values[NativeLUTCompression] == 1.f && s.values[NativeLUTPostExposure] == 1.f;
   }
   // True when any post control differs from the unmodified game, which Luma's
   // defaults do (vignette and film grain off, SDR dithering on).
   inline bool PostControlsActive(const Settings& value)
   {
      return value.values[BloomStrength] != 1.f || value.values[VignetteStrength] != 1.f || value.values[Grain] != 1.f || value.values[SDRDither] != 0.f || value.values[PrivatePrecision] != 0.f || value.values[MotionBlurEnabled] != 1.f || value.values[DistortionStrength] != 1.f;
   }
   inline void Sanitize(Settings& s)
   {
      for (const auto& c : controls)
      {
         auto& v = s.values[c.field];
         v = std::isfinite(v) ? (std::clamp)(v, c.minimum, c.maximum) : c.value;
      }
      // Switches and two-way choices. The gamuts are 0 BT.709, 1 BT.2020;
      // PsychoV31's other targets (3 and 4) were clamped to 1 above.
      for (auto f : {HDRGamut, SDRGamut, GT7ChromaSpace, ACES2White, NativeUI, SDRDither, PrivatePrecision, MotionBlurEnabled})
         s.values[f] = s.values[f] >= 0.5f ? 1.f : 0.f;
      s.values[Enabled] = float(MapperOf(s));
      // The chroma fade is a smoothstep from start to end; keep it ordered.
      s.values[GT7FadeEnd] = (std::max)(s.values[GT7FadeEnd], s.values[GT7FadeStart] + .01f);
      s.values[ACES2Limiting] = std::floor(s.values[ACES2Limiting] + 0.5f);
      s.values[HDRPaper] = (std::min)(s.values[HDRPaper], s.values[HDRPeak]);
      // PsychoV needs its output anchor below the peak.
      const float anchor_limit = (std::min)(1.f, s.values[HDRPeak] / s.values[HDRPaper]) * 0.95f;
      for (auto f : {BackgroundR, BackgroundG, BackgroundB})
         s.values[f] = (std::min)(s.values[f], anchor_limit);
   }
   // Auto follows Windows HDR; SDR shows Luma's SDR picture on an HDR display too. Native always shows the game's own
   // output. (Saved value 2 was an "HDR" choice that behaved exactly like Auto; it loads as Auto.)
   enum class OutputMode : int
   {
      Auto,
      SDR,
   };
   inline std::atomic<OutputMode> output_mode{OutputMode::Auto};
   inline bool HDROutput(bool hdr_available, bool psycho_enabled)
   {
      return hdr_available && (!psycho_enabled || output_mode.load() != OutputMode::SDR);
   }
   // The output the shaders write. The linear transport (FP16 scRGB) carries HDR and SDR alike, so it follows Windows
   // HDR every frame. On an SDR display its white is 1.0 (80 nits) and Windows turns it into the display's 8-bit sRGB:
   // SDR white, the Windows SDR brightness and the gamut follow the display, and the shader dithers those 8 bits. On an
   // HDR display nothing is quantized to 8 bits, so there is no dither.
   inline void SetOutput(Settings& values, bool display_hdr, bool linear_output, float windows_sdr_white)
   {
      values.values[OutputHDR] = HDROutput(display_hdr && linear_output, values.values[Enabled] != 0.f) ? 1.f : 0.f;
      values.values[OutputLinear] = linear_output ? 1.f : 0.f;
      values.values[DisplayHDR] = display_hdr ? 1.f : 0.f;
      values.values[WindowsSDRWhite] = windows_sdr_white;
      if (linear_output && !display_hdr)
      {
         values.values[SDRWhite] = 80.f;
         values.values[SDRGamut] = 0.f;
         values.values[WindowsSDRWhite] = 80.f;
      }
      else if (linear_output)
         values.values[SDRDither] = 0.f;
   }
   inline std::mutex settings_mutex;
   inline Settings settings = Defaults();
   inline Settings Snapshot()
   {
      std::lock_guard lock(settings_mutex);
      return settings;
   }
   inline void Store(Settings value)
   {
      Sanitize(value);
      std::lock_guard lock(settings_mutex);
      settings = value;
   }

   struct State
   {
      com_ptr<ID3D11Texture2D> scene;
      com_ptr<ID3D11RenderTargetView> rtv;
      com_ptr<ID3D11ShaderResourceView> srv;
      com_ptr<ID3D11Buffer> constants;
      com_ptr<ID3D11Resource> native_packed;
      Settings frame_settings{}, uploaded{};
      uint64_t settings_frame = ~uint64_t(0), scene_frame = ~uint64_t(0);
      UINT width = 0, height = 0;
      DXGI_FORMAT scene_format = DXGI_FORMAT_UNKNOWN;
      // display_hdr: Windows HDR is on for the game's display, checked every frame. linear_output: the back buffer is
      // the FP16 linear transport. hdr_output: both, so HDR can be shown.
      bool display_hdr = false, display_checked = false, hdr_output = false, linear_output = false, used_dlss = false, constants_valid = false;
      // Whether the game's last final pass was its HDR variant, and the Windows
      // SDR content brightness (100 when unknown).
      bool native_hdr_display = false;
      float windows_sdr_white = 100.f;
      // Tone mapper comparison: the other scene path (native grading for Native,
      // Luma's for the others), drawn in the same frame.
      com_ptr<ID3D11Texture2D> compare_scene;
      com_ptr<ID3D11RenderTargetView> compare_rtv;
      com_ptr<ID3D11ShaderResourceView> compare_srv;
      uint64_t compare_frame = ~uint64_t(0);
      void Reset()
      {
         scene = nullptr;
         rtv = nullptr;
         srv = nullptr;
         constants = nullptr;
         native_packed = nullptr;
         compare_scene = nullptr;
         compare_rtv = nullptr;
         compare_srv = nullptr;
         compare_frame = ~uint64_t(0);
         settings_frame = scene_frame = ~uint64_t(0);
         width = height = 0;
         used_dlss = constants_valid = false;
         scene_format = DXGI_FORMAT_UNKNOWN;
      }
      const Settings& Frame(uint64_t frame)
      {
         if (settings_frame != frame)
         {
            frame_settings = Snapshot();
            settings_frame = frame;
         }
         return frame_settings;
      }
   };
   inline bool Prepare(ID3D11Device* device, ID3D11DeviceContext* context, State& state,
      UINT width, UINT height, uint64_t frame, bool disable_dof)
   {
      if (!width || !height) return false;
      Settings values = state.Frame(frame);
      const DXGI_FORMAT desired_format = values.values[PrivatePrecision] != 0.f
                                            ? DXGI_FORMAT_R32G32B32A32_FLOAT
                                            : DXGI_FORMAT_R16G16B16A16_FLOAT;
      if (!state.scene || state.width != width || state.height != height || state.scene_format != desired_format)
      {
         state.scene = nullptr;
         state.rtv = nullptr;
         state.srv = nullptr;
         state.native_packed = nullptr;
         state.scene_frame = ~uint64_t(0);
         state.scene_format = DXGI_FORMAT_UNKNOWN;
         D3D11_TEXTURE2D_DESC d{};
         d.Width = width;
         d.Height = height;
         d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
         d.Format = desired_format;
         d.Usage = D3D11_USAGE_DEFAULT;
         d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
         if (FAILED(device->CreateTexture2D(&d, nullptr, &state.scene)) || FAILED(device->CreateRenderTargetView(state.scene.get(), nullptr, &state.rtv)) || FAILED(device->CreateShaderResourceView(state.scene.get(), nullptr, &state.srv)))
         {
            state.scene = nullptr;
            state.rtv = nullptr;
            state.srv = nullptr;
            return false;
         }
         state.width = width;
         state.height = height;
         state.scene_format = desired_format;
      }
      if (!state.constants)
      {
         D3D11_BUFFER_DESC d{};
         d.ByteWidth = sizeof(Settings);
         d.Usage = D3D11_USAGE_DEFAULT;
         d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (FAILED(device->CreateBuffer(&d, nullptr, &state.constants))) return false;
         state.constants_valid = false;
      }
      SetOutput(values, state.display_hdr, state.linear_output, state.windows_sdr_white);
      values.values[DOFDisabled] = disable_dof ? 1.f : 0.f;
      values.values[GrainFrame] = values.values[Grain] != 0.f ? float(frame & 65535u) : 0.f;
      values.values[GrainFallback] = 1.f;
      values.values[VignetteFallback] = 1.f;
      values.values[NativeHDRDisplay] = state.native_hdr_display ? 1.f : 0.f;
      if (!state.constants_valid || std::memcmp(&values, &state.uploaded, sizeof(values)))
      {
         context->UpdateSubresource(state.constants.get(), 0, nullptr, &values, 0, 0);
         state.uploaded = values;
         state.constants_valid = true;
      }
      return true;
   }
   // source_* is the input scene extent when it differs from the private
   // scene (a 4-aligned DLSS scene): the viewport keeps 1:1 texel addressing
   // and the private target clips the centered crop.
   inline void DrawScene(ID3D11DeviceContext* context, State& state,
      ID3D11VertexShader* vs, ID3D11PixelShader* ps, ID3D11ShaderResourceView* source, UINT uav_count,
      ID3D11ShaderResourceView* separated_grade = nullptr,
      UINT source_width = 0, UINT source_height = 0, UINT crop_x = 0, UINT crop_y = 0, ID3D11RenderTargetView* target_override = nullptr)
   {
      DrawStateStack<DrawStateStackType::FullGraphics> saved;
      saved.Cache(context, uav_count);
      // FullGraphics does not save the GS/HS/DS stages, so they are restored by
      // hand, as is the separated grade in slot 8.
      com_ptr<ID3D11ShaderResourceView> previous_grade;
      context->PSGetShaderResources(8, 1, &previous_grade);
      context->PSSetShaderResources(8, 1, &separated_grade);
      com_ptr<ID3D11GeometryShader> gs;
      com_ptr<ID3D11HullShader> hs;
      com_ptr<ID3D11DomainShader> ds;
      context->GSGetShader(&gs, nullptr, nullptr);
      context->HSGetShader(&hs, nullptr, nullptr);
      context->DSGetShader(&ds, nullptr, nullptr);
      com_ptr<ID3D11ShaderResourceView> native_scene;
      context->PSGetShaderResources(0, 1, &native_scene);
      ID3D11ShaderResourceView* native = native_scene.get();
      context->PSSetShaderResources(7, 1, &native);
      context->PSSetShaderResources(0, 1, &source);
      ID3D11Buffer* cb = state.constants.get();
      context->PSSetConstantBuffers(11, 1, &cb);
      ID3D11RenderTargetView* target = target_override ? target_override : state.rtv.get();
      context->OMSetRenderTargets(1, &target, nullptr);
      context->OMSetBlendState(nullptr, nullptr, D3D11_DEFAULT_SAMPLE_MASK);
      context->OMSetDepthStencilState(nullptr, 0);
      context->IASetInputLayout(nullptr);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context->RSSetState(nullptr);
      context->RSSetScissorRects(0, nullptr);
      D3D11_VIEWPORT vp{-float(crop_x), -float(crop_y), float(source_width ? source_width : state.width),
         float(source_height ? source_height : state.height), 0, 1};
      context->RSSetViewports(1, &vp);
      context->GSSetShader(nullptr, nullptr, 0);
      context->HSSetShader(nullptr, nullptr, 0);
      context->DSSetShader(nullptr, nullptr, 0);
      context->VSSetShader(vs, nullptr, 0);
      context->PSSetShader(ps, nullptr, 0);
      context->Draw(4, 0);
      saved.Restore(context);
      ID3D11ShaderResourceView* restore_grade = previous_grade.get();
      context->PSSetShaderResources(8, 1, &restore_grade);
      context->GSSetShader(gs.get(), nullptr, 0);
      context->HSSetShader(hs.get(), nullptr, 0);
      context->DSSetShader(ds.get(), nullptr, 0);
   }
   // A second private scene like the first, for one comparison frame.
   inline bool AllocateCompareScene(ID3D11Device* device, State& state)
   {
      if (!state.scene) return false;
      D3D11_TEXTURE2D_DESC d{};
      state.scene->GetDesc(&d);
      if (state.compare_scene)
      {
         D3D11_TEXTURE2D_DESC c{};
         state.compare_scene->GetDesc(&c);
         if (c.Width == d.Width && c.Height == d.Height && c.Format == d.Format) return true;
      }
      state.compare_scene = nullptr;
      state.compare_rtv = nullptr;
      state.compare_srv = nullptr;
      if (FAILED(device->CreateTexture2D(&d, nullptr, &state.compare_scene)) || FAILED(device->CreateRenderTargetView(state.compare_scene.get(), nullptr, &state.compare_rtv)) || FAILED(device->CreateShaderResourceView(state.compare_scene.get(), nullptr, &state.compare_srv)))
      {
         state.compare_scene = nullptr;
         state.compare_rtv = nullptr;
         state.compare_srv = nullptr;
         return false;
      }
      return true;
   }
   inline void ReleaseCompareScene(State& state)
   {
      state.compare_scene = nullptr;
      state.compare_rtv = nullptr;
      state.compare_srv = nullptr;
      state.compare_frame = ~uint64_t(0);
   }
} // namespace Battlefront2Psycho

// Eye glare (Luma_SWBF2_Glare.hlsl): the veil the eye would cast around the scene light the display cannot show.
namespace Battlefront2Glare
{
   inline constexpr UINT max_levels = 8;
   // The observer: 30 years old with brown eyes (CIE pigmentation factor 0.5), seeing the picture's height under 30
   // degrees (a desk monitor, or a television from 1.9 picture heights).
   inline constexpr double age = 30., pigmentation = .5, view_degrees = 30.;

   // The share of a point's light that the CIE total glare function (CIE 135/1-1999, Vos & van den Berg; 1/sr, angle
   // t in degrees)
   //   (1 - 0.08 (A/70)^4) (9.2e6 / (1 + (t/0.0046)^2)^1.5 + 1.5e5 / (1 + (t/0.045)^2)^1.5)
   //   + (1 + 1.6 (A/70)^4) (400 / (1 + (t/0.1)^2) + p (1300 / (1 + (t/0.1)^2)^1.5 + 0.8 / (1 + (t/0.1)^2)^0.5))
   // spreads as Gaussians from `low` to `high` degrees wide (sigma). Each term a / (1 + (t/b)^2)^n is a mixture of
   // Gaussians exp(-x (t/b)^2) with density x^(n-1) e^-x / Gamma(n), where x = b^2 / (2 sigma^2), and one such
   // Gaussian holds pi b^2 / x square degrees. The function's 3e-8 t^2 and 2.5e-3 p terms, which matter beyond the
   // widest blur, are left out.
   inline double Share(double low, double high)
   {
      constexpr double pi = 3.14159265358979323846;
      const double a4 = std::pow(age / 70., 4.), core = 1. - .08 * a4, scatter = 1. + 1.6 * a4;
      const auto x = [](double b, double sigma)
      {
         return b * b / (2. * sigma * sigma);
      };
      const auto term15 = [&](double a, double b)
      {
         return 2. * pi * a * b * b * (std::erf(std::sqrt(x(b, low))) - std::erf(std::sqrt(x(b, high))));
      };
      const double b = .1, x0 = x(b, high), x1 = x(b, low);
      const double term1 = scatter * 400. * pi * b * b * (std::expint(-x1) - std::expint(-x0));
      const double term05 = scatter * pigmentation * .8 * std::sqrt(pi) * b * b * 2. * (std::exp(-x0) / std::sqrt(x0) - std::exp(-x1) / std::sqrt(x1) - std::sqrt(pi) * (std::erf(std::sqrt(x1)) - std::erf(std::sqrt(x0))));
      return (pi / 180.) * (pi / 180.) * (term15(core * 9.2e6, .0046) + term15(core * 1.5e5, .045) + term15(scatter * pigmentation * 1300., b) + term1 + term05);
   }

   struct State
   {
      UINT width = 0, height = 0, levels = 0;
      // Mip k has the scene size over 2^(k+1): the excess light there, and its veil with those of all coarser ones.
      com_ptr<ID3D11Texture2D> light, veil;
      std::array<com_ptr<ID3D11ShaderResourceView>, max_levels> light_srv, veil_srv;
      std::array<com_ptr<ID3D11UnorderedAccessView>, max_levels> light_uav, veil_uav;
      com_ptr<ID3D11Buffer> constants;
      // Each level's share of the light. The shader's filters blur level k by sqrt(4^(k+1) - 3) scene pixels
      // (sigma), which stands for the Gaussians up to halfway to the next level's width. Narrower ones than the
      // first level's stay inside the source; the last level also stands for the octave above it.
      std::array<double, max_levels> shares{};
      void Reset()
      {
         *this = State{};
      }
      bool Ensure(ID3D11Device* device, UINT scene_width, UINT scene_height)
      {
         if (width == scene_width && height == scene_height) return levels != 0;
         Reset();
         width = scene_width;
         height = scene_height;
         UINT count = 0;
         while (count < max_levels && ((std::min)(width, height) >> (count + 1)) >= 8)
            ++count;
         if (count < 2) return false;
         D3D11_TEXTURE2D_DESC d{};
         d.Width = width / 2;
         d.Height = height / 2;
         d.MipLevels = count;
         d.ArraySize = d.SampleDesc.Count = 1;
         d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
         d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
         D3D11_BUFFER_DESC bd{};
         bd.ByteWidth = 16; // cbuffer GlareControls
         bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (FAILED(device->CreateTexture2D(&d, nullptr, &light)) || FAILED(device->CreateTexture2D(&d, nullptr, &veil)) || FAILED(device->CreateBuffer(&bd, nullptr, &constants))) return false;
         for (UINT i = 0; i < count; ++i)
         {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = d.Format;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MostDetailedMip = i;
            sv.Texture2D.MipLevels = 1;
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = d.Format;
            uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uv.Texture2D.MipSlice = i;
            if (FAILED(device->CreateShaderResourceView(light.get(), &sv, &light_srv[i])) || FAILED(device->CreateUnorderedAccessView(light.get(), &uv, &light_uav[i])) || FAILED(device->CreateShaderResourceView(veil.get(), &sv, &veil_srv[i])) || FAILED(device->CreateUnorderedAccessView(veil.get(), &uv, &veil_uav[i]))) return false;
         }
         const auto sigma = [&](UINT level)
         {
            return std::sqrt(std::ldexp(1., 2 * int(level) + 2) - 3.) * view_degrees / height;
         };
         for (UINT i = 0; i < count; ++i)
            shares[i] = Share(i ? std::sqrt(sigma(i - 1) * sigma(i)) : sigma(0) / std::sqrt(2.), i + 1 < count ? std::sqrt(sigma(i) * sigma(i + 1)) : 2. * sigma(i));
         levels = count;
         return true;
      }
   };

   // Draws the veil of the private scene with the three passes of Luma_SWBF2_Glare.hlsl. Returns it for the
   // display shader, or null without glare: off, Native, or unavailable.
   inline ID3D11ShaderResourceView* Draw(ID3D11Device* device, ID3D11DeviceContext* context, State& state,
      const Battlefront2Psycho::State& tone, const std::array<ID3D11ComputeShader*, 3>& passes, ID3D11SamplerState* linear_sampler, UINT uav_count)
   {
      namespace P = Battlefront2Psycho;
      const auto& values = tone.uploaded.values;
      const P::Mapper mapper = P::MapperOf(tone.uploaded);
      const float strength = values[P::GlareStrength];
      if (strength <= 0.f || mapper == P::Mapper::Native || !passes[0] || !passes[1] || !passes[2] || !state.Ensure(device, tone.width, tone.height)) return nullptr;
      // The brightest scene value the display shows, in the scene's units (1 = scene white): the peak over scene
      // white in HDR and white itself in SDR, before the tone mapper's exposure. Light beyond it is the excess.
      const float white = mapper == P::Mapper::GT7 ? values[P::GT7PaperWhite] : values[P::HDRPaper];
      const float peak = (values[P::OutputHDR] != 0.f ? values[P::HDRPeak] / white : 1.f) / std::exp2(values[P::ExposureField(mapper)]);
      const float m = std::pow(peak * .01f, .1593017578125f);
      const float peak_pq = std::pow((.8359375f + 18.8515625f * m) / (1.f + 18.6875f * m), 78.84375f);
      DrawStateStack<DrawStateStackType::Compute> saved;
      saved.Cache(context, uav_count);
      ID3D11Buffer* constants = state.constants.get();
      context->CSSetConstantBuffers(0, 1, &constants);
      context->CSSetSamplers(0, 1, &linear_sampler);
      const auto pass = [&](ID3D11ComputeShader* shader, ID3D11ShaderResourceView* source, ID3D11ShaderResourceView* coarser, ID3D11UnorderedAccessView* target, UINT level, float x, float y)
      {
         const float control[4] = {x, y, 0.f, 0.f};
         context->UpdateSubresource(constants, 0, nullptr, control, 0, 0);
         ID3D11ShaderResourceView* const inputs[2] = {source, coarser};
         context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
         context->CSSetShaderResources(0, 2, inputs);
         context->CSSetShader(shader, nullptr, 0);
         context->Dispatch(((state.width >> (level + 1)) + 7) / 8, ((state.height >> (level + 1)) + 7) / 8, 1);
         ID3D11UnorderedAccessView* const none = nullptr;
         context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
      };
      pass(passes[0], tone.srv.get(), nullptr, state.light_uav[0].get(), 0, peak, peak_pq);
      for (UINT i = 1; i < state.levels; ++i)
         pass(passes[1], state.light_srv[i - 1].get(), nullptr, state.light_uav[i].get(), i, 0.f, 0.f);
      for (UINT i = state.levels - 1; i-- > 0;)
      {
         const bool top = i + 2 == state.levels;
         pass(passes[2], state.light_srv[i].get(), top ? state.light_srv[i + 1].get() : state.veil_srv[i + 1].get(), state.veil_uav[i].get(), i,
            float(strength * state.shares[i]), top ? float(strength * state.shares[i + 1]) : 1.f);
      }
      saved.Restore(context);
      return state.veil_srv[0].get();
   }
} // namespace Battlefront2Glare

namespace Battlefront2ACES2
{
   // The ACES 2.0 display shader reads its parameters and hue tables from b10.
   // They are rebuilt on the CPU only when the target (peak, limiting primaries
   // and white, tuning) changes.
   inline constexpr UINT constant_buffer_slot = 10;

   inline const Chromaticities& LimitingPrimaries(int index)
   {
      return index <= 0 ? Rec709 : index == 1 ? P3D65
                                              : Rec2020;
   }

   // The transform's peak relative to its 100-nit reference white. HDR shows
   // that white at scene white (the shader scales by scene white / 100), so a
   // 100-nit scene white gives the unscaled reference. The transform is
   // defined up to 10,000 nits.
   inline float TransformPeak(float display_peak, float scene_white)
   {
      const float peak = display_peak * 100.f / (std::max)(scene_white, 1.f);
      return std::isfinite(peak) ? (std::clamp)(peak, 100.f, 10000.f) : 100.f;
   }

   // HDR: the selected peak (see TransformPeak) and limiting primaries,
   // encoded for Rec.2100. SDR: the 100-nit Rec.709 sRGB preset. "ACES white"
   // selects the D60 sim presets: limiting white ~D60 with scale_white.
   struct Target
   {
      float peak;
      Chromaticities limiting;
      Tuning tuning;
   };
   inline Target TargetFor(const Battlefront2Psycho::Settings& settings, bool hdr)
   {
      namespace P = Battlefront2Psycho;
      const auto& v = settings.values;
      Target t{};
      t.peak = hdr ? TransformPeak(v[P::HDRPeak], v[P::HDRPaper]) : 100.f;
      t.limiting = hdr ? LimitingPrimaries(int(v[P::ACES2Limiting])) : Rec709;
      t.tuning.grey_nits = v[P::ACES2MidGrey];
      t.tuning.grey_lift = v[P::ACES2GreyLift];
      t.tuning.contrast = v[P::ACES2Contrast];
      t.tuning.flare = v[P::ACES2Flare];
      t.tuning.chroma_compression = v[P::ACES2ChromaCompression];
      t.tuning.chroma_expansion = v[P::ACES2ChromaExpansion];
      if (v[P::ACES2White] != 0.f)
      {
         t.limiting.white[0] = ACESWhite[0];
         t.limiting.white[1] = ACESWhite[1];
         t.tuning.scale_white = true;
         t.tuning.encoding = hdr ? Rec2020 : Rec709;
      }
      return t;
   }
   inline bool SameTarget(const Target& a, const Target& b)
   {
      return a.peak == b.peak && std::memcmp(&a.limiting, &b.limiting, sizeof(Chromaticities)) == 0 && SameTuning(a.tuning, b.tuning);
   }

   struct State
   {
      com_ptr<ID3D11Buffer> buffer;
      std::unique_ptr<Constants> constants = std::make_unique<Constants>();
      Target built{};         // The target last attempted.
      bool attempted = false; // `built` is meaningful.
      bool valid = false;     // The buffer holds usable tables.
      void Reset()
      {
         buffer = nullptr;
         valid = false;
         attempted = false;
      }
   };

   inline ID3D11Buffer* Prepare(ID3D11Device* device, ID3D11DeviceContext* context, State& state,
      const Battlefront2Psycho::Settings& settings, bool hdr)
   {
      const Target target = TargetFor(settings, hdr);
      // valid and attempted start false and Reset() clears them with the buffer.
      if (!state.buffer)
      {
         D3D11_BUFFER_DESC desc{};
         desc.ByteWidth = sizeof(Constants);
         desc.Usage = D3D11_USAGE_DEFAULT;
         desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (FAILED(device->CreateBuffer(&desc, nullptr, &state.buffer))) return nullptr;
      }
      // One build per target. A combination the transform cannot represent
      // keeps the last valid tables rather than dropping the frame to the
      // native path, and is not rebuilt on every frame.
      if (!state.attempted || !SameTarget(state.built, target))
      {
         state.attempted = true;
         state.built = target;
         if (Build(target.peak, target.limiting, *state.constants, target.tuning))
         {
            context->UpdateSubresource(state.buffer.get(), 0, nullptr, state.constants.get(), 0, 0);
            state.valid = true;
         }
      }
      return state.valid ? state.buffer.get() : nullptr;
   }
} // namespace Battlefront2ACES2

namespace Battlefront2NativePresentation
{
   // The final pass runs without TAA, DLSS, a scene packer or a fresh private
   // scene too (menus). Its own constant buffer keeps a menu draw from
   // overwriting the gameplay scene's constants or using its DLSS output.
   struct State
   {
      Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
      Battlefront2Psycho::Settings uploaded{};
      bool valid = false;
      unsigned failure_logs = 0;
      void Reset()
      {
         constants.Reset();
         valid = false;
      }
   };
   enum class Result
   {
      Drawn,
      UnknownShader,
      NotBackBuffer,
      UnsupportedTarget,
      MissingConstants,
      ShortConstants,
      ShaderUnavailable,
      AllocationFailed
   };
   inline const char* Name(Result result)
   {
      switch (result)
      {
      case Result::Drawn:
         return "drawn";
      case Result::UnknownShader:
         return "unknown_shader";
      case Result::NotBackBuffer:
         return "not_backbuffer";
      case Result::UnsupportedTarget:
         return "unsupported_target_or_encoding";
      case Result::MissingConstants:
         return "missing_native_constants";
      case Result::ShortConstants:
         return "short_native_constant_range";
      case Result::ShaderUnavailable:
         return "replacement_shader_unavailable";
      case Result::AllocationFailed:
         return "constant_buffer_allocation_failed";
      }
      return "unknown";
   }

   template <class IsBackBuffer, class Draw>
   Result Present(ID3D11Device* device, ID3D11DeviceContext* context, State& state,
      ID3D11PixelShader* shader, uint64_t native_hash,
      const Battlefront2Psycho::Settings& settings, bool display_hdr,
      bool linear_output, IsBackBuffer&& is_back_buffer, Draw&& draw, float windows_sdr_white)
   {
      namespace P = Battlefront2Psycho;
      if (!Battlefront2ShaderVariants::Display(native_hash)) return Result::UnknownShader;
      if (!shader) return Result::ShaderUnavailable;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
      context->OMGetRenderTargets(1, &target, nullptr);
      if (!target) return Result::NotBackBuffer;
      Microsoft::WRL::ComPtr<ID3D11Resource> resource;
      target->GetResource(&resource);
      if (!is_back_buffer(resource.Get())) return Result::NotBackBuffer;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
      if (FAILED(resource.As(&texture))) return Result::UnsupportedTarget;
      D3D11_TEXTURE2D_DESC td{};
      texture->GetDesc(&td);
      D3D11_RENDER_TARGET_VIEW_DESC vd{};
      target->GetDesc(&vd);
      // Encoding follows the physical RTV, never the game's HDR preference.
      // In particular, reject sRGB RTVs: they would encode our output twice.
      const DXGI_FORMAT expected = linear_output ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
      if (td.Format != expected || vd.Format != expected || td.SampleDesc.Count != 1 || vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0)
         return Result::UnsupportedTarget;
      Battlefront2RenderState::PixelBindings saved(context);
      if (!saved.constants.buffer) return Result::MissingConstants;
      D3D11_BUFFER_DESC bd{};
      saved.constants.buffer->GetDesc(&bd);
      const uint64_t offset = uint64_t(saved.constants.first_constant) * 16;
      if (offset > bd.ByteWidth || bd.ByteWidth - offset < 160 || saved.constants.num_constants < 10) return Result::ShortConstants;
      if (!state.constants)
      {
         D3D11_BUFFER_DESC desc{};
         desc.ByteWidth = sizeof(P::Settings);
         desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (FAILED(device->CreateBuffer(&desc, nullptr, &state.constants))) return Result::AllocationFailed;
         state.valid = false;
      }
      auto values = settings;
      // Native HDR scene values already carry absolute luminance in 100-nit
      // units. SDR scene values are display-referred: the Native tone mapper
      // shows them at the Windows SDR content brightness, like the unmodified
      // game; the other tone mappers use the chosen paper white.
      P::SetOutput(values, display_hdr, linear_output, windows_sdr_white);
      values.values[P::NativeHDRDisplay] = Battlefront2ShaderVariants::HDRDisplay(native_hash) ? 1.f : 0.f;
      if (!state.valid || std::memcmp(&values, &state.uploaded, sizeof(values)))
      {
         context->UpdateSubresource(state.constants.Get(), 0, nullptr, &values, 0, 0);
         state.uploaded = values;
         state.valid = true;
      }
      ID3D11Buffer* buffer = state.constants.Get();
      context->PSSetConstantBuffers(11, 1, &buffer);
      context->PSSetShader(shader, nullptr, 0);
      draw();
      saved.Restore(context);
      return Result::Drawn;
   }
} // namespace Battlefront2NativePresentation

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// "Compare tone mappers": one game frame is rendered once per tone mapper from
// the same scene, UI and settings, and each result is saved as a full-size PNG.
// HDR (FP16 scRGB) output becomes a 16-bit BT.2020 PQ PNG tagged with a BT.2020 PQ
// ICC profile and cLLi (MaxCLL/MaxFALL); SDR output an 8-bit sRGB PNG.
namespace Battlefront2Compare
{
   enum class Status : int
   {
      Idle,
      Pending,
      Saving,
      Saved,
      Failed
   };
   inline std::atomic<Status> status{Status::Idle};
   inline std::atomic<int64_t> requested_at{0};
   inline std::mutex folder_mutex;
   inline std::string saved_folder; // UTF-8, for the menu

   // A request waits for a frame with a gameplay (or menu) scene.
   inline constexpr int64_t timeout_ms = 5000;
   inline int64_t NowMs()
   {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
   }
   inline bool Pending()
   {
      return status.load(std::memory_order_relaxed) == Status::Pending;
   }
   inline bool Busy()
   {
      const auto s = status.load();
      return s == Status::Pending || s == Status::Saving;
   }
   inline bool Request()
   {
      auto current = status.load();
      if (current == Status::Pending || current == Status::Saving) return false;
      requested_at = NowMs();
      return status.compare_exchange_strong(current, Status::Pending);
   }
   // Called once per present: a request without a usable frame fails.
   inline void Expire()
   {
      if (!Pending()) return;
      if (NowMs() - requested_at.load() > timeout_ms)
      {
         auto expected = Status::Pending;
         status.compare_exchange_strong(expected, Status::Failed);
      }
   }
   inline std::string Folder()
   {
      std::lock_guard lock(folder_mutex);
      return saved_folder;
   }

   // <game>\Luma\Comparisons (set by the addon), and this session's latest comparison.
   inline std::filesystem::path root;
   inline std::filesystem::path saved_path;
   // The latest comparison if it still exists, else the comparisons folder (created if needed).
   inline std::filesystem::path FolderToOpen()
   {
      std::filesystem::path latest;
      {
         std::lock_guard lock(folder_mutex);
         latest = saved_path;
      }
      std::error_code error;
      if (!latest.empty() && std::filesystem::is_directory(latest, error)) return latest;
      if (root.empty()) return {};
      std::filesystem::create_directories(root, error);
      return std::filesystem::is_directory(root, error) ? root : std::filesystem::path{};
   }
   // Runs a task on its own thread. The thread holds a reference to this add-on, so
   // ReShade unloading it cannot free the code the task is running.
   struct DetachedTask
   {
      HMODULE module = nullptr;
      std::function<void()> run;
   };
   inline void RunDetached(std::function<void()> run)
   {
      auto* task = new DetachedTask{nullptr, std::move(run)};
      if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&RunDetached), &task->module))
      {
         delete task;
         return;
      }
      const auto proc = [](void* parameter) -> DWORD
      {
         auto* task = static_cast<DetachedTask*>(parameter);
         const HMODULE module = task->module;
         task->run();
         delete task;
         FreeLibraryAndExitThread(module, 0);
      };
      if (const HANDLE thread = CreateThread(nullptr, 0, proc, task, 0, nullptr))
         CloseHandle(thread);
      else
      {
         FreeLibrary(task->module);
         delete task;
      }
   }
   // The shell needs an STA thread, and the render thread's COM state is the game's.
   inline bool OpenFolder()
   {
      const auto folder = FolderToOpen();
      if (folder.empty()) return false;
      RunDetached([folder]
         {
         const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
         ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
         if (SUCCEEDED(com)) CoUninitialize(); });
      return true;
   }

   struct Image
   {
      std::wstring name;
      UINT width = 0, height = 0;
      bool hdr = false;            // RGBA16F (scRGB) when true, else RGBA8 (sRGB)
      std::vector<uint8_t> pixels; // tightly packed rows
   };

   inline float HalfToFloat(uint16_t h)
   {
      const uint32_t sign = uint32_t(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
      if (exponent == 31) return 0.f; // Inf/NaN never belong in a picture.
      float value;
      if (exponent == 0)
         value = std::ldexp(float(mantissa), -24);
      else
         value = std::ldexp(float(mantissa | 0x400u), int(exponent) - 25);
      return sign ? -value : value;
   }
   // SMPTE ST 2084 of absolute luminance (nits), 0..1.
   inline float PQEncode(float nits)
   {
      const float y = (std::min)((std::max)(nits, 0.f) / 10000.f, 1.f);
      const float p = std::pow(y, 0.1593017578125f);
      return std::pow((0.8359375f + 18.8515625f * p) / (1.f + 18.6875f * p), 78.84375f);
   }
   // scRGB (BT.709 linear, 1.0 = 80 nits) to 16-bit BT.2020 PQ RGB; colors outside
   // BT.2020 are clipped. MaxCLL and MaxFALL are in nits.
   struct PQImage
   {
      std::vector<uint8_t> data;
      UINT stride = 0;
      float max_cll = 0.f, max_fall = 0.f;
   };
   inline PQImage ToPQ(const Image& image)
   {
      static constexpr float m[9] = {0.6274038959f, 0.3292830384f, 0.0433130657f,
         0.0690972894f, 0.9195403951f, 0.0113623156f, 0.0163914389f, 0.0880133079f, 0.8955952532f};
      const size_t count = size_t(image.width) * image.height;
      PQImage out;
      out.stride = image.width * 6u;
      out.data.resize(size_t(out.stride) * image.height);
      double sum = 0.0;
      for (size_t i = 0; i < count; ++i)
      {
         const auto* h = reinterpret_cast<const uint16_t*>(image.pixels.data()) + i * 4;
         const float c[3] = {HalfToFloat(h[0]) * 80.f, HalfToFloat(h[1]) * 80.f, HalfToFloat(h[2]) * 80.f};
         float rgb[3], largest = 0.f;
         for (int k = 0; k < 3; ++k)
         {
            rgb[k] = (std::max)(m[k * 3] * c[0] + m[k * 3 + 1] * c[1] + m[k * 3 + 2] * c[2], 0.f);
            if (!std::isfinite(rgb[k])) rgb[k] = 0.f;
            largest = (std::max)(largest, rgb[k]);
         }
         out.max_cll = (std::max)(out.max_cll, (std::min)(largest, 10000.f));
         sum += (std::min)(largest, 10000.f);
         auto* o = reinterpret_cast<uint16_t*>(out.data.data()) + i * 3;
         for (int k = 0; k < 3; ++k)
            o[k] = uint16_t(PQEncode(rgb[k]) * 65535.f + .5f);
      }
      out.max_fall = count ? float(sum / double(count)) : 0.f;
      return out;
   }
   inline HRESULT WritePNG(IWICImagingFactory* factory, const std::filesystem::path& file,
      const uint8_t* data, UINT width, UINT height, UINT stride, const WICPixelFormatGUID& wanted)
   {
      using Microsoft::WRL::ComPtr;
      ComPtr<IWICStream> stream;
      HRESULT hr = factory->CreateStream(&stream);
      if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
      ComPtr<IWICBitmapEncoder> encoder;
      if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
      if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
      ComPtr<IWICBitmapFrameEncode> frame;
      ComPtr<IPropertyBag2> options;
      if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
      if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
      if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
      WICPixelFormatGUID pixel_format = wanted;
      if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixel_format);
      if (SUCCEEDED(hr) && pixel_format != wanted) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
      if (SUCCEEDED(hr)) hr = frame->WritePixels(height, stride, UINT(size_t(stride) * height), const_cast<BYTE*>(data));
      if (SUCCEEDED(hr)) hr = frame->Commit();
      if (SUCCEEDED(hr)) hr = encoder->Commit();
      return hr;
   }
   inline uint32_t CRC32(const uint8_t* data, size_t size, uint32_t crc = 0)
   {
      static const auto table = []
      {
         std::array<uint32_t, 256> t{};
         for (uint32_t n = 0; n < 256; ++n)
         {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
               c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
         }
         return t;
      }();
      crc = ~crc;
      for (size_t i = 0; i < size; ++i)
         crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
      return ~crc;
   }
   // ICC v4 RGB profile of BT.2020 PQ: BT.2020 primaries adapted to D50, the ST 2084 curve as 4096 points with
   // 1.0 = 10,000 nits, a cicp tag (9/16/0/1) and a luminance tag at the 203-nit HDR reference white. Ported from
   // Renobot's Discord HDR converter (MIT, see THIRD_PARTY_NOTICES.txt).
   inline const std::vector<uint8_t>& BT2020PQProfile()
   {
      static const std::vector<uint8_t> profile = []
      {
         const auto put = [](std::vector<uint8_t>& bytes, size_t offset, size_t count, uint32_t value)
         {
            for (size_t i = count; i > 0; --i, value >>= 8)
               bytes[offset + i - 1] = uint8_t(value);
         };
         const auto text = [](std::vector<uint8_t>& bytes, size_t offset, const char* value)
         { std::memcpy(bytes.data() + offset, value, std::strlen(value)); };
         const auto xyz = [&](double x, double y, double z)
         {
            std::vector<uint8_t> bytes(20);
            text(bytes, 0, "XYZ ");
            const double values[3] = {x, y, z};
            for (size_t i = 0; i < 3; ++i)
               put(bytes, 8 + i * 4, 4, uint32_t(int32_t(std::lround(values[i] * 65536))));
            return bytes;
         };
         std::vector<uint8_t> curve(12 + 4096 * 2);
         text(curve, 0, "curv");
         put(curve, 8, 4, 4096);
         for (size_t i = 0; i < 4096; ++i)
         {
            const double power = std::pow(double(i) / 4095, 32.0 / 2523);
            const double ratio = (std::max)(power - 3424.0 / 4096, 0.0) / (2413.0 / 128 - 2392.0 / 128 * power);
            put(curve, 12 + i * 2, 2, uint32_t(std::lround(std::pow(ratio, 16384.0 / 2610) * 65535)));
         }
         static constexpr char name[] = "BT.2020 PQ";
         std::vector<uint8_t> description(28 + (sizeof(name) - 1) * 2);
         text(description, 0, "mluc");
         put(description, 8, 4, 1);
         put(description, 12, 4, 12);
         text(description, 16, "enUS");
         put(description, 20, 4, (sizeof(name) - 1) * 2);
         put(description, 24, 4, 28);
         for (size_t i = 0; i < sizeof(name) - 1; ++i)
            description[29 + i * 2] = uint8_t(name[i]);
         // Bradford D65 to D50.
         static constexpr double bradford[9] = {1.047882, 0.022919, -0.050201, 0.029587, 0.990479, -0.017059, -0.009232, 0.015076, 0.751678};
         std::vector<uint8_t> adaptation(44);
         text(adaptation, 0, "sf32");
         for (size_t i = 0; i < 9; ++i)
            put(adaptation, 8 + i * 4, 4, uint32_t(int32_t(std::lround(bradford[i] * 65536))));
         const std::pair<const char*, std::vector<uint8_t>> tags[] = {{"desc", description}, {"cprt", description},
            {"wtpt", xyz(0.9642, 1, 0.8249)}, {"chad", adaptation}, {"cicp", {'c', 'i', 'c', 'p', 0, 0, 0, 0, 9, 16, 0, 1}},
            {"lumi", xyz(0, 203, 0)}, {"rXYZ", xyz(0.673459, 0.279033, -0.001931)}, {"gXYZ", xyz(0.165661, 0.675338, 0.029979)},
            {"bXYZ", xyz(0.125100, 0.045629, 0.797162)}, {"rTRC", curve}, {"gTRC", curve}, {"bTRC", curve}};
         std::vector<uint8_t> bytes(132 + std::size(tags) * 12);
         put(bytes, 128, 4, uint32_t(std::size(tags)));
         for (size_t i = 0; i < std::size(tags); ++i)
         {
            text(bytes, 132 + i * 12, tags[i].first);
            put(bytes, 136 + i * 12, 4, uint32_t(bytes.size()));
            put(bytes, 140 + i * 12, 4, uint32_t(tags[i].second.size()));
            bytes.insert(bytes.end(), tags[i].second.begin(), tags[i].second.end());
            bytes.resize((bytes.size() + 3) & ~size_t(3));
         }
         put(bytes, 0, 4, uint32_t(bytes.size()));
         put(bytes, 8, 4, 0x04300000);
         text(bytes, 12, "mntrRGB XYZ ");
         static constexpr uint16_t date[6] = {2026, 11, 13, 0, 0, 0};
         for (size_t i = 0; i < 6; ++i)
            put(bytes, 24 + i * 2, 2, date[i]);
         text(bytes, 36, "acsp");
         std::memcpy(bytes.data() + 68, xyz(0.9642, 1, 0.8249).data() + 8, 12);
         return bytes;
      }();
      return profile;
   }
   // A zlib stream of stored deflate blocks: PNG's iCCP holds zlib data, and the profile is small.
   inline std::vector<uint8_t> ZlibStored(const std::vector<uint8_t>& data)
   {
      std::vector<uint8_t> out{0x78, 0x01};
      size_t at = 0;
      do
      {
         const size_t size = (std::min)(data.size() - at, size_t(65535));
         out.push_back(at + size == data.size() ? 1 : 0);
         out.push_back(uint8_t(size));
         out.push_back(uint8_t(size >> 8));
         out.push_back(uint8_t(~size));
         out.push_back(uint8_t(~size >> 8));
         out.insert(out.end(), data.begin() + at, data.begin() + at + size);
         at += size;
      } while (at < data.size());
      uint32_t a = 1, b = 0;
      for (const uint8_t byte : data)
      {
         a = (a + byte) % 65521;
         b = (b + a) % 65521;
      }
      for (int k = 3; k >= 0; --k)
         out.push_back(uint8_t((b << 16 | a) >> (k * 8)));
      return out;
   }
   // Tags a PNG as HDR: a BT.2020 PQ ICC profile (iCCP) and cLLi right after IHDR. Not cICP: Discord shows a cICP
   // PNG washed out and a profiled one in HDR. Color chunks the encoder wrote (sRGB, gAMA, cHRM, iCCP) are
   // dropped so no reader treats the PQ values as sRGB.
   inline bool TagHDR(const std::filesystem::path& file, float max_cll, float max_fall)
   {
      std::vector<uint8_t> png;
      {
         std::ifstream in(file, std::ios::binary);
         png.assign(std::istreambuf_iterator<char>(in), {});
      }
      static constexpr uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
      if (png.size() < 33 || std::memcmp(png.data(), signature, 8) != 0 || std::memcmp(png.data() + 12, "IHDR", 4) != 0) return false;
      auto be32 = [](const uint8_t* p)
      { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
      auto put32 = [](std::vector<uint8_t>& v, uint32_t x)
      { for (int k = 3; k >= 0; --k) v.push_back(uint8_t(x >> (k * 8))); };
      auto chunk = [&](std::vector<uint8_t>& v, const char type[4], const std::vector<uint8_t>& body)
      {
         put32(v, uint32_t(body.size()));
         const size_t start = v.size();
         v.insert(v.end(), type, type + 4);
         v.insert(v.end(), body.begin(), body.end());
         put32(v, CRC32(v.data() + start, v.size() - start));
      };
      std::vector<uint8_t> out(png.begin(), png.begin() + 33);
      static constexpr char profile_name[] = "BT.2020 PQ";
      std::vector<uint8_t> iccp(profile_name, profile_name + sizeof(profile_name)); // name, its terminating zero
      iccp.push_back(0);                                                            // zlib
      const auto compressed = ZlibStored(BT2020PQProfile());
      iccp.insert(iccp.end(), compressed.begin(), compressed.end());
      chunk(out, "iCCP", iccp);
      std::vector<uint8_t> clli;
      put32(clli, uint32_t((std::min)(max_cll, 429496.f) * 10000.f + .5f));
      put32(clli, uint32_t((std::min)(max_fall, 429496.f) * 10000.f + .5f));
      chunk(out, "cLLi", clli);
      for (size_t at = 33; at + 12 <= png.size();)
      {
         const uint32_t length = be32(png.data() + at);
         if (at + 12 + size_t(length) > png.size()) return false;
         const char* type = reinterpret_cast<const char*>(png.data() + at + 4);
         const bool drop = !std::memcmp(type, "sRGB", 4) || !std::memcmp(type, "gAMA", 4) || !std::memcmp(type, "cHRM", 4) || !std::memcmp(type, "iCCP", 4) || !std::memcmp(type, "cICP", 4) || !std::memcmp(type, "cLLi", 4);
         if (!drop) out.insert(out.end(), png.begin() + at, png.begin() + at + 12 + length);
         at += 12 + size_t(length);
      }
      std::ofstream(file, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
      return true;
   }
   inline HRESULT WriteImage(IWICImagingFactory* factory, const std::filesystem::path& file, const Image& image)
   {
      if (image.hdr)
      {
         const auto pq = ToPQ(image);
         HRESULT hr = WritePNG(factory, file, pq.data.data(), image.width, image.height, pq.stride, GUID_WICPixelFormat48bppRGB);
         if (SUCCEEDED(hr) && !TagHDR(file, pq.max_cll, pq.max_fall)) hr = E_FAIL;
         return hr;
      }
      const size_t count = size_t(image.width) * image.height;
      std::vector<uint8_t> bgr(count * 3);
      for (size_t i = 0; i < count; ++i)
      {
         const uint8_t* rgba = image.pixels.data() + i * 4;
         bgr[i * 3] = rgba[2];
         bgr[i * 3 + 1] = rgba[1];
         bgr[i * 3 + 2] = rgba[0];
      }
      return WritePNG(factory, file, bgr.data(), image.width, image.height, image.width * 3, GUID_WICPixelFormat24bppBGR);
   }

   inline bool SaveAll(const std::filesystem::path& folder, std::vector<Image>& images, const std::string& notes)
   {
      std::error_code error;
      std::filesystem::create_directories(folder, error);
      if (error) return false;
      const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      bool ok = !images.empty();
      {
         Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
         if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            ok = false;
         else
            for (auto& image : images)
            {
               const auto file = folder / (image.name + L".png");
               if (FAILED(WriteImage(factory.Get(), file, image)))
               {
                  ok = false;
                  std::filesystem::remove(file, error);
               }
               std::vector<uint8_t>().swap(image.pixels);
            }
      }
      std::ofstream(folder / L"comparison.txt") << notes;
      if (SUCCEEDED(com)) CoUninitialize();
      return ok;
   }
   // Encoding several 4K images takes about a second, so it runs on a worker that owns the data.
   inline void Save(std::filesystem::path folder, std::string display_folder, std::vector<Image> images, std::string notes)
   {
      status = Status::Saving;
      // std::function needs a copyable callable: share the images instead of copying them.
      auto shared = std::make_shared<std::vector<Image>>(std::move(images));
      RunDetached([folder = std::move(folder), display_folder = std::move(display_folder), shared, notes = std::move(notes)]
         {
         const bool ok = SaveAll(folder, *shared, notes);
         { std::lock_guard lock(folder_mutex); saved_folder = display_folder; if (ok) saved_path = folder; }
         status = ok ? Status::Saved : Status::Failed; });
   }
} // namespace Battlefront2Compare

namespace Battlefront2Video
{
   // The first nine variants write encoded RGB directly to a display target.
   // The last three are linear-output movies drawn into an sRGB UI intermediate.
   // World-space video/material shaders are never recognized.
   struct Variant
   {
      uint32_t hash;
      unsigned planes, constants;
      bool linear_ui = false;
   };
   inline constexpr std::array<Variant, 12> variants = {{{0x3EFF61B7, 3, 0}, {0xF391275C, 3, 0},
      {0xE922D0A3, 2, 0}, {0x4EBDB45A, 3, 0}, {0x93FF1C45, 4, 0}, {0x86AC7FD1, 4, 0},
      {0x9E8CBC5C, 4, 0}, {0x1A5A18EE, 3, 41}, {0x62EED219, 3, 47},
      {0xD99078EB, 3, 0, true}, {0xED10858B, 3, 0, true}, {0xE30F2EE0, 3, 0, true}}};
   inline int Find(uint64_t hash)
   {
      for (unsigned i = 0; i < variants.size(); ++i)
         if (variants[i].hash == hash) return int(i);
      return -1;
   }
   struct State
   {
      Microsoft::WRL::ComPtr<ID3D11Buffer> constants;
      Battlefront2Psycho::Settings uploaded{};
      bool valid = false;
      void Reset()
      {
         constants.Reset();
         valid = false;
      }
   };
   enum class Result
   {
      Drawn,
      UnknownShader,
      NativeTarget,
      InvalidInputs,
      ShaderUnavailable,
      AllocationFailed
   };
   template <class IsBackBuffer, class Draw>
   Result Present(ID3D11Device* device, ID3D11DeviceContext* context, State& state,
      ID3D11PixelShader* shader, uint64_t hash, const Battlefront2Psycho::Settings& settings,
      bool display_hdr, bool linear_output, IsBackBuffer&& is_back_buffer, Draw&& draw, float windows_sdr_white)
   {
      namespace P = Battlefront2Psycho;
      const int index = Find(hash);
      if (index < 0) return Result::UnknownShader;
      if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return Result::NativeTarget;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
      context->OMGetRenderTargets(1, &target, nullptr);
      if (!target) return Result::NativeTarget;
      Microsoft::WRL::ComPtr<ID3D11Resource> resource;
      target->GetResource(&resource);
      const auto& variant = variants[index];
      const bool backbuffer = is_back_buffer(resource.Get());
      Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
      if (FAILED(resource.As(&texture))) return Result::NativeTarget;
      D3D11_TEXTURE2D_DESC td{};
      texture->GetDesc(&td);
      D3D11_RENDER_TARGET_VIEW_DESC vd{};
      target->GetDesc(&vd);
      if (td.SampleDesc.Count != 1 || vd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || vd.Texture2D.MipSlice != 0) return Result::NativeTarget;
      if (variant.linear_ui)
      {
         // An 8-bit output keeps the game's own SDR response. Correct only the
         // linear transport, and only the observed sRGB UI route.
         if (!linear_output || backbuffer || vd.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || (td.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) || !(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return Result::NativeTarget;
      }
      else if (!backbuffer || td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || vd.Format != td.Format)
         return Result::NativeTarget;
      if (!shader) return Result::ShaderUnavailable;
      for (unsigned slot = 0; slot < variant.planes; ++slot)
      {
         Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
         context->PSGetShaderResources(slot, 1, &view);
         Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
         context->PSGetSamplers(slot, 1, &sampler);
         if (!view || !sampler) return Result::InvalidInputs;
         D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
         view->GetDesc(&sv);
         // Video planes must be sampled as code values, never through sRGB SRVs.
         if (sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return Result::InvalidInputs;
         switch (sv.Format)
         {
         case DXGI_FORMAT_R8_UNORM:
         case DXGI_FORMAT_R8G8_UNORM:
         case DXGI_FORMAT_R8G8B8A8_UNORM:
         case DXGI_FORMAT_B8G8R8A8_UNORM:
         case DXGI_FORMAT_R16_UNORM:
         case DXGI_FORMAT_R16G16_UNORM:
            break;
         default:
            return Result::InvalidInputs;
         }
      }
      Battlefront2RenderState::PixelBindings saved(context);
      if (variant.constants)
      {
         if (!saved.constants.buffer) return Result::InvalidInputs;
         D3D11_BUFFER_DESC bd{};
         saved.constants.buffer->GetDesc(&bd);
         const uint64_t offset = uint64_t(saved.constants.first_constant) * 16;
         if (saved.constants.num_constants < variant.constants || offset > bd.ByteWidth || bd.ByteWidth - offset < variant.constants * 16) return Result::InvalidInputs;
      }
      // A UI movie stays normalized linear RGB (the native UI compositor
      // applies paper white); it reads only whether the display is HDR.
      if (!state.constants)
      {
         D3D11_BUFFER_DESC bd{};
         bd.ByteWidth = sizeof(P::Settings);
         bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (FAILED(device->CreateBuffer(&bd, nullptr, &state.constants))) return Result::AllocationFailed;
         state.valid = false;
      }
      auto values = settings;
      P::SetOutput(values, display_hdr, true, windows_sdr_white);
      if (!state.valid || std::memcmp(&values, &state.uploaded, sizeof(values)))
      {
         context->UpdateSubresource(state.constants.Get(), 0, nullptr, &values, 0, 0);
         state.uploaded = values;
         state.valid = true;
      }
      auto* buffer = state.constants.Get();
      context->PSSetConstantBuffers(11, 1, &buffer);
      context->PSSetShader(shader, nullptr, 0);
      draw();
      saved.Restore(context);
      return Result::Drawn;
   }
} // namespace Battlefront2Video

namespace Battlefront2NativeLUT
{
   // The game's PQ-output LUT generators, indexed by how many source LUTs they
   // blend; everything else in them is the same. Other generators (sRGB output
   // included) are not supported.
   inline constexpr std::array<uint32_t, 16> GeneratorHashes = {
      0x3FF573DA, 0xFDB2BE2E, 0x678C48B1, 0x812F58B9, 0x9AE0B362, 0x1B83B9BC,
      0x676F90F7, 0x0720338B, 0x423A3E64, 0xB1CBC4CD, 0xE53D7B61, 0xB017B898,
      0x1D83C31D, 0x2BA3527F, 0xE19BC2E4, 0xE7E34A57};
   inline int SourceCount(uint64_t hash)
   {
      for (unsigned i = 0; i < GeneratorHashes.size(); ++i)
         if (hash == GeneratorHashes[i]) return int(i);
      return -1;
   }
   enum class Status : uint32_t
   {
      Waiting,
      Ready,
      PendingKernel,
      UnknownGenerator,
      UnsupportedBindings,
      DeferredContext,
      AllocationFailed
   };
   inline const char* StatusText(Status status)
   {
      switch (status)
      {
      case Status::Ready:
         return "Native grading separated from compression";
      case Status::PendingKernel:
         return "Native grading waiting for shader compilation";
      case Status::UnknownGenerator:
         return "Native grading unavailable: unsupported LUT generator";
      case Status::UnsupportedBindings:
         return "Native grading unavailable: unsupported LUT bindings";
      case Status::DeferredContext:
         return "Native grading unavailable: deferred LUT generation";
      case Status::AllocationFailed:
         return "Native grading unavailable: private resource creation failed";
      default:
         return "Native grading waiting for LUT generation";
      }
   }
   // For the menu only; everything else happens on the immediate context.
   inline std::atomic<Status> status{Status::Waiting};
   // The kernel's LumaLUTSources cbuffer (b1).
   struct Parameters
   {
      uint32_t source_count = 0;
      float artistic = 0, matrix = 0, gamma = 0, brightness = 0, pre_exposure = 0, compression = 0, post_exposure = 0;
   };
   static_assert(sizeof(Parameters) == 32);
   inline Parameters GetParameters(uint32_t sources, const Battlefront2Psycho::Settings& settings)
   {
      using namespace Battlefront2Psycho;
      return {sources, settings.values[LUTStrength], settings.values[NativeLUTMatrix], settings.values[NativeLUTGamma],
         settings.values[NativeLUTBrightness], settings.values[NativeLUTPreExposure], settings.values[NativeLUTCompression], settings.values[NativeLUTPostExposure]};
   }

   struct Entry
   {
      com_ptr<ID3D11Resource> native_output;
      std::array<com_ptr<ID3D11Texture3D>, 15> sources;
      std::array<com_ptr<ID3D11ShaderResourceView>, 15> source_views;
      com_ptr<ID3D11Buffer> constants, counts;
      com_ptr<ID3D11Texture3D> texture;
      com_ptr<ID3D11ShaderResourceView> view;
      com_ptr<ID3D11UnorderedAccessView> uav;
      uint64_t sequence = 0;
      uint32_t source_count = 0;
      Parameters uploaded{};
      Status state = Status::Waiting;
      bool captured = false, ready = false;
   };
   struct State
   {
      static constexpr size_t Capacity = 4;
      std::vector<Entry> entries;
      uint64_t sequence = 0;
      std::atomic<bool> deferred_unsupported{false};
      // A LUT generated in a deferred command list cannot be matched with this
      // immediate-context cache, so grading separation stays off for the device.
      void MarkDeferredUnsupported()
      {
         deferred_unsupported.store(true);
         status.store(Status::DeferredContext);
      }
      void Invalidate(ID3D11Resource* resource)
      {
         for (auto& entry : entries)
            if (entry.native_output.get() == resource)
            {
               entry.captured = entry.ready = false;
               entry.state = Status::UnknownGenerator;
               status.store(entry.state);
            }
      }
      Entry& Replace(ID3D11Resource* resource)
      {
         for (auto& entry : entries)
            if (entry.native_output.get() == resource)
            {
               entry.captured = entry.ready = false;
               entry.state = Status::Waiting;
               entry.sequence = ++sequence;
               return entry;
            }
         if (entries.size() == Capacity)
         {
            auto oldest = std::min_element(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
               { return a.sequence < b.sequence; });
            *oldest = Entry{};
            oldest->native_output = resource;
            oldest->sequence = ++sequence;
            return *oldest;
         }
         entries.emplace_back();
         auto& entry = entries.back();
         entry.native_output = resource;
         entry.sequence = ++sequence;
         return entry;
      }
      bool Generate(ID3D11Device* device, ID3D11DeviceContext* context, Entry& entry, ID3D11ComputeShader* kernel,
         const Battlefront2Psycho::Settings& settings)
      {
         const auto params = GetParameters(entry.source_count, settings);
         if (entry.ready && std::memcmp(&params, &entry.uploaded, sizeof(params)) == 0) return true;
         entry.ready = false;
         if (!entry.captured) return false; // Observe() creates both buffers before it sets this.
         if (!kernel)
         {
            entry.state = Status::PendingKernel;
            return false;
         }
         if (!entry.texture)
         {
            D3D11_TEXTURE3D_DESC d{};
            d.Width = d.Height = d.Depth = 33;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(device->CreateTexture3D(&d, nullptr, &entry.texture)) || FAILED(device->CreateShaderResourceView(entry.texture.get(), nullptr, &entry.view)) || FAILED(device->CreateUnorderedAccessView(entry.texture.get(), nullptr, &entry.uav)))
            {
               entry.texture = nullptr;
               entry.view = nullptr;
               entry.uav = nullptr;
               entry.state = Status::AllocationFailed;
               return false;
            }
         }
         // Save the game's compute state (class instances, both ranged constant
         // buffers, the SRVs and u0) and restore it after the dispatch.
         com_ptr<ID3D11ComputeShader> old_shader;
         ID3D11ClassInstance* classes[256]{};
         UINT class_count = 256;
         context->CSGetShader(&old_shader, classes, &class_count);
         com_ptr<ID3D11DeviceContext1> context1;
         context->QueryInterface(&context1);
         com_ptr<ID3D11Buffer> old_cb[2];
         UINT first[2]{}, count[2]{4096, 4096};
         for (UINT i = 0; i < 2; ++i)
         {
            if (context1)
               context1->CSGetConstantBuffers1(i, 1, &old_cb[i], &first[i], &count[i]);
            else
               context->CSGetConstantBuffers(i, 1, &old_cb[i]);
         }
         std::array<com_ptr<ID3D11ShaderResourceView>, 15> old_views;
         ID3D11ShaderResourceView* new_views[15]{};
         for (UINT i = 0; i < 15; ++i)
         {
            context->CSGetShaderResources(i, 1, &old_views[i]);
            new_views[i] = i < entry.source_count ? entry.source_views[i].get() : nullptr;
         }
         com_ptr<ID3D11UnorderedAccessView> old_uav;
         context->CSGetUnorderedAccessViews(0, 1, &old_uav);
         context->UpdateSubresource(entry.counts.get(), 0, nullptr, &params, 0, 0);
         ID3D11Buffer* cb[]{entry.constants.get(), entry.counts.get()};
         context->CSSetConstantBuffers(0, 2, cb);
         context->CSSetShaderResources(0, 15, new_views);
         ID3D11UnorderedAccessView* output = entry.uav.get();
         context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
         context->CSSetShader(kernel, nullptr, 0);
         context->Dispatch(1, 11, 33);
         output = nullptr;
         context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
         for (UINT i = 0; i < 15; ++i)
            new_views[i] = old_views[i].get();
         context->CSSetShaderResources(0, 15, new_views);
         output = old_uav.get();
         context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
         for (UINT i = 0; i < 2; ++i)
         {
            ID3D11Buffer* saved = old_cb[i].get();
            if (context1)
               context1->CSSetConstantBuffers1(i, 1, &saved, &first[i], &count[i]);
            else
               context->CSSetConstantBuffers(i, 1, &saved);
         }
         context->CSSetShader(old_shader.get(), classes, class_count);
         for (UINT i = 0; i < class_count; ++i)
            if (classes[i]) classes[i]->Release();
         entry.uploaded = params;
         entry.ready = true;
         entry.state = Status::Ready;
         return true;
      }
      // Called before the game's dispatch, which always runs unchanged. The sources
      // and constants are copied now, so a later generation cannot mix them with
      // newer data.
      void Observe(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t hash, UINT groups_x, UINT groups_y, UINT groups_z)
      {
         const int sources = SourceCount(hash);
         if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
         {
            if (sources >= 0) MarkDeferredUnsupported();
            return;
         }
         if (deferred_unsupported.load()) return;
         if (sources < 0 && entries.empty()) return;
         com_ptr<ID3D11UnorderedAccessView> output;
         context->CSGetUnorderedAccessViews(0, 1, &output);
         if (!output) return;
         D3D11_UNORDERED_ACCESS_VIEW_DESC ov{};
         output->GetDesc(&ov);
         if (ov.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE3D) return;
         com_ptr<ID3D11Resource> resource;
         output->GetResource(&resource);
         if (sources < 0)
         {
            // Another shader writing a LUT we track invalidates it; other 3D writes are ignored.
            Invalidate(resource.get());
            return;
         }
         com_ptr<ID3D11Texture3D> native;
         if (FAILED(resource->QueryInterface(&native))) return;
         D3D11_TEXTURE3D_DESC nd{};
         native->GetDesc(&nd);
         if (nd.Width != 33 || nd.Height != 33 || nd.Depth != 33) return;
         auto& entry = Replace(resource.get());
         auto fail = [&](Status reason)
         {
            entry.state = reason;
            status.store(reason);
         };
         if (groups_x != 1 || groups_y != 11 || groups_z != 33 || ov.Format != DXGI_FORMAT_R10G10B10A2_UNORM || ov.Texture3D.MipSlice != 0 || ov.Texture3D.FirstWSlice != 0 || (ov.Texture3D.WSize != 33 && ov.Texture3D.WSize != UINT(-1)))
         {
            fail(Status::UnsupportedBindings);
            return;
         }
         com_ptr<ID3D11Buffer> cb;
         com_ptr<ID3D11DeviceContext1> context1;
         UINT first = 0, count = 4096;
         if (SUCCEEDED(context->QueryInterface(&context1)))
            context1->CSGetConstantBuffers1(0, 1, &cb, &first, &count);
         else
            context->CSGetConstantBuffers(0, 1, &cb);
         const UINT bytes = (sources < 2 ? 8 : 9 + sources) * 16;
         D3D11_BUFFER_DESC bd{};
         if (cb) cb->GetDesc(&bd);
         if (!cb || uint64_t(first) * 16 + bytes > bd.ByteWidth || uint64_t(count) * 16 < bytes)
         {
            fail(Status::UnsupportedBindings);
            return;
         }
         // Copy each source and its exact typed view. The shader handles the
         // sources' reversed Y.
         for (int i = 0; i < sources; ++i)
         {
            com_ptr<ID3D11ShaderResourceView> view;
            context->CSGetShaderResources(UINT(i), 1, &view);
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            if (view) view->GetDesc(&vd);
            if (!view || vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE3D || vd.Texture3D.MostDetailedMip != 0)
            {
               fail(Status::UnsupportedBindings);
               return;
            }
            com_ptr<ID3D11Resource> source;
            view->GetResource(&source);
            com_ptr<ID3D11Texture3D> volume;
            if (FAILED(source->QueryInterface(&volume)) || source.get() == resource.get())
            {
               fail(Status::UnsupportedBindings);
               return;
            }
            D3D11_TEXTURE3D_DESC d{};
            volume->GetDesc(&d);
            if (d.Width != 33 || d.Height != 33 || d.Depth != 33 || d.MipLevels != 1)
            {
               fail(Status::UnsupportedBindings);
               return;
            }
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            d.CPUAccessFlags = d.MiscFlags = 0;
            D3D11_TEXTURE3D_DESC existing{};
            if (entry.sources[i]) entry.sources[i]->GetDesc(&existing);
            D3D11_SHADER_RESOURCE_VIEW_DESC existing_view{};
            if (entry.source_views[i]) entry.source_views[i]->GetDesc(&existing_view);
            if (!entry.sources[i] || existing.Format != d.Format || std::memcmp(&existing_view, &vd, sizeof(vd)) != 0)
            {
               entry.sources[i] = nullptr;
               entry.source_views[i] = nullptr;
               if (FAILED(device->CreateTexture3D(&d, nullptr, &entry.sources[i])) || FAILED(device->CreateShaderResourceView(entry.sources[i].get(), &vd, &entry.source_views[i])))
               {
                  fail(Status::AllocationFailed);
                  return;
               }
            }
            context->CopyResource(entry.sources[i].get(), volume.get());
         }
         std::array<float, 100> zero{};
         D3D11_SUBRESOURCE_DATA initial{zero.data(), 0, 0};
         bd = {};
         bd.ByteWidth = sizeof(zero);
         bd.Usage = D3D11_USAGE_DEFAULT;
         bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (!entry.constants && FAILED(device->CreateBuffer(&bd, &initial, &entry.constants)))
         {
            fail(Status::AllocationFailed);
            return;
         }
         D3D11_BOX range{first * 16, 0, 0, first * 16 + bytes, 1, 1};
         context->CopySubresourceRegion(entry.constants.get(), 0, 0, 0, 0, cb.get(), 0, &range);
         bd.ByteWidth = sizeof(Parameters);
         if (!entry.counts && FAILED(device->CreateBuffer(&bd, nullptr, &entry.counts)))
         {
            fail(Status::AllocationFailed);
            return;
         }
         entry.source_count = UINT(sources);
         entry.captured = true;
         // Generated on demand in Find(); a bypassed or fully native grade never needs it.
         entry.state = Status::PendingKernel;
         status.store(entry.state);
      }
      ID3D11ShaderResourceView* Find(ID3D11Device* device, ID3D11DeviceContext* context,
         ID3D11ShaderResourceView* native_grade, ID3D11ComputeShader* kernel,
         const Battlefront2Psycho::Settings& settings)
      {
         if (deferred_unsupported.load() || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
         {
            status.store(Status::DeferredContext);
            return nullptr;
         }
         if (!native_grade)
         {
            status.store(Status::Waiting);
            return nullptr;
         }
         D3D11_SHADER_RESOURCE_VIEW_DESC view{};
         native_grade->GetDesc(&view);
         if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE3D || view.Format != DXGI_FORMAT_R10G10B10A2_UNORM || view.Texture3D.MostDetailedMip != 0)
         {
            status.store(Status::UnsupportedBindings);
            return nullptr;
         }
         com_ptr<ID3D11Resource> resource;
         native_grade->GetResource(&resource);
         for (auto& entry : entries)
            if (entry.native_output.get() == resource.get())
            {
               Generate(device, context, entry, kernel, settings);
               status.store(entry.state);
               return entry.ready ? entry.view.get() : nullptr;
            }
         status.store(Status::Waiting);
         return nullptr;
      }
   };
} // namespace Battlefront2NativeLUT
