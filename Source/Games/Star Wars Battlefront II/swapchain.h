#pragma once
// Swapchain flags and color space, and NVIDIA Reflex low latency.

#include <dxgi.h>
#include <algorithm>
#include <dxgi1_4.h>
#include <optional>
#include <Windows.h>
#include <d3d11.h>
#include <cstdint>
#pragma warning(push)
#pragma warning(disable : 4828) // Upstream NVAPI has legacy-encoded comments; this project compiles as UTF-8.
#include "../../External/NVAPI/nvapi.h"
#pragma warning(pop)

namespace Battlefront2Presentation
{
   // Flip model and FP16 (scRGB), keeping fullscreen, refresh rate, size and flags. scRGB carries HDR and SDR alike, so
   // the output can follow Windows HDR while the game runs: the game does not rebuild its swapchain when HDR is
   // toggled (measured), and on an SDR display Windows still flips it directly (PresentMon: independent flip, as 8-bit).
   // The framework's flip model change is disabled for this game (DISABLE_SWAPCHAIN_FLIP_MODEL).
   inline bool PrepareFlip(reshade::api::swapchain_desc& desc, bool tearing_supported)
   {
      if (desc.back_buffer.texture.samples != 1 || (desc.present_flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)) return false;
      switch (static_cast<DXGI_FORMAT>(desc.back_buffer.texture.format))
      {
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
      case DXGI_FORMAT_R10G10B10A2_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
         break;
      default:
         return false;
      }
      desc.present_mode = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      desc.back_buffer_count = (std::max)(desc.back_buffer_count, 2u);
      desc.back_buffer.texture.format = static_cast<reshade::api::format>(DXGI_FORMAT_R16G16B16A16_FLOAT);
      if (tearing_supported) desc.present_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
      // True even on a resize whose description is already flip model: ReShade then keeps showing the game the
      // description it asked for.
      return true;
   }
   inline bool PrepareBackbufferView(reshade::api::resource_view_desc& desc, DXGI_FORMAT storage)
   {
      if (storage != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
      switch (static_cast<DXGI_FORMAT>(desc.format))
      {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      case DXGI_FORMAT_R10G10B10A2_UNORM:
         desc.format = static_cast<reshade::api::format>(storage);
         return true;
      default:
         return false;
      }
   }

   // Shared with ReShade's D3D11 swapchain implementation. ReShade reads this
   // marker when its runtime is recreated, after the addon's init_swapchain event.
   inline constexpr GUID color_space_id = {0x18b57e4, 0x1493, 0x4953, {0xad, 0xf2, 0xde, 0x6d, 0x99, 0xcc, 0x5, 0xe5}};

   inline std::optional<DXGI_COLOR_SPACE_TYPE> ColorSpaceForBuffer(DXGI_FORMAT format)
   {
      switch (format)
      {
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
         // Linear transport is also valid for SDR content on an FP16 buffer.
         return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8X8_UNORM:
      case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
         return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
      default:
         // A 10-bit or unknown buffer alone does not identify its transfer curve.
         return std::nullopt;
      }
   }

   inline HRESULT ApplyColorSpace(IDXGISwapChain3* swapchain, DXGI_COLOR_SPACE_TYPE space)
   {
      if (!swapchain) return E_NOINTERFACE;
      UINT support = 0;
      HRESULT result = swapchain->CheckColorSpaceSupport(space, &support);
      if (FAILED(result)) return result;
      if (!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) return DXGI_ERROR_UNSUPPORTED;
      result = swapchain->SetColorSpace1(space);
      if (FAILED(result)) return result;
      // Replace the previous marker on every successful transition, including
      // FP16 -> 8-bit SDR. Otherwise ReShade linearizes its UI into an SDR target.
      return swapchain->SetPrivateData(color_space_id, sizeof(space), &space);
   }
} // namespace Battlefront2Presentation

// NVIDIA low latency through the driver (NvAPI_D3D_SetSleepMode without
// NvAPI_D3D_Sleep): the driver paces frames itself. Engine-side Reflex sleep
// and latency markers are not used.

namespace Battlefront2Reflex
{
   enum class Mode
   {
      off,
      on,
      boost
   };
   struct Api
   {
      decltype(&NvAPI_D3D_SetSleepMode) set_mode = nullptr;
      decltype(&NvAPI_D3D_GetSleepStatus) sleep_status = nullptr;
      bool Complete() const
      {
         return set_mode && sleep_status;
      }
   };

   // Takes the sleep mode only when no other owner has set one, and resets it
   // to Off on destruction. Created only with a complete Api.
   class DriverMode
   {
      Api api_{};
      IUnknown* device_ = nullptr;
      bool owned_ = false;

   public:
      DriverMode(const Api& api, IUnknown* device) : api_(api), device_(device)
      {
         if (device_) device_->AddRef();
      }
      DriverMode(const DriverMode&) = delete;
      DriverMode& operator=(const DriverMode&) = delete;
      ~DriverMode()
      {
         Restore();
         if (device_) device_->Release();
      }
      bool Configure(Mode mode)
      {
         if (!device_) return false;
         if (!owned_)
         {
            NV_GET_SLEEP_STATUS_PARAMS p{};
            p.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
            if (api_.sleep_status(device_, &p) != NVAPI_OK || p.bLowLatencyMode || p.bUseGameSleep || p.sleepIntervalUs) return false;
         }
         NV_SET_SLEEP_MODE_PARAMS p{};
         p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
         p.bLowLatencyMode = mode != Mode::off;
         p.bLowLatencyBoost = mode == Mode::boost;
         p.bUseMarkersToOptimize = 0;
         if (api_.set_mode(device_, &p) != NVAPI_OK) return false;
         owned_ = true;
         return true;
      }
      void Restore()
      {
         if (owned_)
         {
            NV_SET_SLEEP_MODE_PARAMS p{};
            p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
            api_.set_mode(device_, &p);
            owned_ = false;
         }
      }
   };
   class Library
   {
      HMODULE module_ = nullptr;
      Api api_{};

   public:
      Library() = default;
      Library(const Library&) = delete;
      Library& operator=(const Library&) = delete;
      ~Library()
      {
         if (module_) FreeLibrary(module_);
      }
      bool Load()
      {
         if (module_) return api_.Complete();
         module_ = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
         if (!module_) return false;
         using Query = void*(__cdecl*)(unsigned int);
         auto query = reinterpret_cast<Query>(GetProcAddress(module_, "nvapi_QueryInterface"));
         if (!query) return false;
         auto init = reinterpret_cast<decltype(&NvAPI_Initialize)>(query(0x0150e828));
         if (!init || init() != NVAPI_OK) return false;
         // IDs verified against NVIDIA's amd64/nvapi64.lib function stubs.
         // https://github.com/NVIDIA/nvapi/blob/main/amd64/nvapi64.lib
         api_.set_mode = reinterpret_cast<decltype(api_.set_mode)>(query(0xac1ca9e0));
         api_.sleep_status = reinterpret_cast<decltype(api_.sleep_status)>(query(0xaef96ca1));
         return api_.Complete();
      }
      const Api& GetApi() const
      {
         return api_;
      }
   };
} // namespace Battlefront2Reflex
