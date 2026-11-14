#pragma once
// Screen space reflections at full resolution with DLSS.

#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <d3d11.h>
#include <vector>
#include "game_passes.h"

namespace Battlefront2SSR
{
   using Microsoft::WRL::ComPtr;
   struct Settings
   {
      bool filtered_upsampling = true;
      bool spatial_filter = false;
      bool full_resolution = true;
      bool velocity_temporal = true;
      float thickness = 1.f, resolve_samples = 1.f, importance = 1.f, responsiveness = 1.f;
      bool NativeValid() const
      {
         if (!std::isfinite(thickness) || thickness < .5f || thickness > 10.f) return false;
         for (float v : {resolve_samples, importance, responsiveness})
            if (!std::isfinite(v) || v < .5f || v > 5.f) return false;
         return true;
      }
   };
   // Profiles saved before version 1 stored a different option under the
   // spatial filter's key, so they start with the filter off.
   inline Settings NativeProfile(Settings s, unsigned version)
   {
      if (version == 0) s.spatial_filter = false;
      if (!s.NativeValid()) s = {};
      return s;
   }
   inline bool Active(unsigned pass, const Settings& s)
   {
      return s.NativeValid() && ((pass == 1 && s.thickness != 1.f) || (pass == 2 && (s.resolve_samples != 1.f || s.importance != 1.f)) || (pass == 3 && s.responsiveness != 1.f));
   }

   // Applies the scale factors to a private copy of an SSR pass's constants
   // (on the GPU, with Luma_SWBF2_SSRConstants), bound for that dispatch only.
   // One per immediate context; the caller synchronizes.
   class PrivateConstants
   {
      ComPtr<ID3D11Buffer> raw_, constants_, params_;
      ComPtr<ID3D11UnorderedAccessView> uav_;
      UINT bytes_ = 0;
      bool Ensure(ID3D11Device* device, UINT bytes)
      {
         if (bytes_ == bytes && raw_ && constants_ && params_ && uav_) return true;
         raw_.Reset();
         constants_.Reset();
         params_.Reset();
         uav_.Reset();
         bytes_ = 0;
         D3D11_BUFFER_DESC desc{};
         desc.ByteWidth = bytes;
         desc.Usage = D3D11_USAGE_DEFAULT;
         desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
         desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
         if (FAILED(device->CreateBuffer(&desc, nullptr, &raw_))) return false;
         D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
         view.Format = DXGI_FORMAT_R32_TYPELESS;
         view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
         view.Buffer.NumElements = bytes / 4;
         view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
         if (FAILED(device->CreateUnorderedAccessView(raw_.Get(), &view, &uav_))) return false;
         desc.MiscFlags = 0;
         desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (FAILED(device->CreateBuffer(&desc, nullptr, &constants_))) return false;
         desc.ByteWidth = 32;
         if (FAILED(device->CreateBuffer(&desc, nullptr, &params_))) return false;
         bytes_ = bytes;
         return true;
      }

   public:
      // True: native_dispatch ran once with the patched constants. False:
      // nothing ran, and the caller runs the native dispatch.
      template <class NativeDispatch>
      bool Replay(ID3D11DeviceContext* context, uint32_t hash, const Settings& settings,
         ID3D11ComputeShader* patch_shader, NativeDispatch&& native_dispatch)
      {
         const UINT pass = Pass(hash);
         if (!patch_shader || !Active(pass, settings)) return false;
         const UINT registers = pass == 1 ? 51u : pass == 3 ? 55u
                                                            : 73u;
         ComPtr<ID3D11Buffer> original;
         ComPtr<ID3D11DeviceContext1> ranged;
         context->QueryInterface(IID_PPV_ARGS(&ranged));
         UINT first = 0, count = 0;
         if (ranged)
            ranged->CSGetConstantBuffers1(0, 1, &original, &first, &count);
         else
            context->CSGetConstantBuffers(0, 1, &original);
         if (!original) return false;
         D3D11_BUFFER_DESC desc{};
         original->GetDesc(&desc);
         if (!ranged) count = desc.ByteWidth / 16;
         // Frostbite binds ranges of 1 MiB constant buffers (allowed by D3D11.1).
         if (desc.ByteWidth > 8u * 1024u * 1024u || desc.ByteWidth < registers * 16 || desc.ByteWidth % 16 || (desc.ByteWidth > 65536 && !ranged) || count < registers || uint64_t(first) * 16 + registers * 16 > desc.ByteWidth) return false;
         ComPtr<ID3D11Device> device;
         context->GetDevice(&device);
         if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 || !Ensure(device.Get(), 1280)) return false;

         ComPtr<ID3D11ComputeShader> shader;
         std::array<ID3D11ClassInstance*, 256> instances{};
         UINT instance_count = UINT(instances.size());
         context->CSGetShader(&shader, instances.data(), &instance_count);
         ComPtr<ID3D11UnorderedAccessView> previous_uav;
         context->CSGetUnorderedAccessViews(0, 1, &previous_uav);
         ComPtr<ID3D11Buffer> previous_params;
         UINT params_first = 0, params_count = 0;
         if (ranged)
            ranged->CSGetConstantBuffers1(13, 1, &previous_params, &params_first, &params_count);
         else
            context->CSGetConstantBuffers(13, 1, &previous_params);
         struct Parameters
         {
            UINT base, pass;
            float thickness, samples, importance, response;
            UINT reserved[2];
         };
         const Parameters parameters{0, pass, settings.thickness, settings.resolve_samples,
            settings.importance, settings.responsiveness, {0, 0}};
         context->UpdateSubresource(params_.Get(), 0, nullptr, &parameters, 0, 0);
         ID3D11Buffer* params = params_.Get();
         context->CSSetConstantBuffers(13, 1, &params);
         ID3D11UnorderedAccessView* uav = uav_.Get();
         const UINT preserve_count = UINT(-1);
         context->CSSetUnorderedAccessViews(0, 1, &uav, &preserve_count);
         context->CSSetShader(patch_shader, nullptr, 0);
         context->Dispatch(1, 1, 1);
         uav = nullptr;
         context->CSSetUnorderedAccessViews(0, 1, &uav, &preserve_count);
         context->CopyResource(constants_.Get(), raw_.Get());
         uav = previous_uav.Get();
         context->CSSetUnorderedAccessViews(0, 1, &uav, &preserve_count);
         context->CSSetShader(shader.Get(), instances.data(), instance_count);
         for (UINT i = 0; i < instance_count; ++i)
            if (instances[i]) instances[i]->Release();
         params = previous_params.Get();
         if (ranged)
            ranged->CSSetConstantBuffers1(13, 1, &params, &params_first, &params_count);
         else
            context->CSSetConstantBuffers(13, 1, &params);
         ID3D11Buffer* replacement = constants_.Get();
         const UINT private_first = 0, private_count = 80;
         if (ranged)
            ranged->CSSetConstantBuffers1(0, 1, &replacement, &private_first, &private_count);
         else
            context->CSSetConstantBuffers(0, 1, &replacement);
         // Native D3D dispatch callbacks must not throw.
         native_dispatch();
         replacement = original.Get();
         if (ranged)
            ranged->CSSetConstantBuffers1(0, 1, &replacement, &first, &count);
         else
            context->CSSetConstantBuffers(0, 1, &replacement);
         return true;
      }
   };

   using Microsoft::WRL::ComPtr;
   // The SSR lighting shaders use s2 only for SSRColor t11 and SSRAlpha t12.
   // Its point filter turns each half-resolution texel into a 2x2 block, so
   // both are sampled bilinearly instead. The native SSR/IBL blend is kept.
   class FilteredUpsampling
   {
      struct Entry
      {
         ComPtr<ID3D11SamplerState> original, filtered;
      };
      std::vector<Entry> cache_;

   public:
      template <class Draw>
      bool Replay(ID3D11Device* device, ID3D11DeviceContext* context, Draw&& draw)
      {
         ComPtr<ID3D11ShaderResourceView> color, alpha;
         context->PSGetShaderResources(11, 1, &color);
         context->PSGetShaderResources(12, 1, &alpha);
         ComPtr<ID3D11RenderTargetView> target;
         context->OMGetRenderTargets(1, &target, nullptr);
         if (!color || !alpha || !target) return false;
         D3D11_SHADER_RESOURCE_VIEW_DESC cv{}, av{};
         color->GetDesc(&cv);
         alpha->GetDesc(&av);
         if (cv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || av.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || cv.Format != DXGI_FORMAT_R11G11B10_FLOAT || av.Format != DXGI_FORMAT_R8_UNORM || cv.Texture2D.MostDetailedMip || av.Texture2D.MostDetailedMip) return false;
         ComPtr<ID3D11Resource> cr, ar, tr;
         color->GetResource(&cr);
         alpha->GetResource(&ar);
         target->GetResource(&tr);
         ComPtr<ID3D11Texture2D> ct, at, tt;
         if (FAILED(cr.As(&ct)) || FAILED(ar.As(&at)) || FAILED(tr.As(&tt))) return false;
         D3D11_TEXTURE2D_DESC cd{}, ad{}, td{};
         ct->GetDesc(&cd);
         at->GetDesc(&ad);
         tt->GetDesc(&td);
         if (cd.Width != ad.Width || cd.Height != ad.Height || cd.Width != td.Width / 2 || cd.Height != td.Height / 2 || cd.SampleDesc.Count != 1 || ad.SampleDesc.Count != 1 || td.SampleDesc.Count != 1)
            return false;
         ComPtr<ID3D11SamplerState> original;
         context->PSGetSamplers(2, 1, &original);
         if (!original) return false;
         D3D11_SAMPLER_DESC desc{};
         original->GetDesc(&desc);
         if (desc.Filter != D3D11_FILTER_MIN_MAG_MIP_POINT) return false;
         ID3D11SamplerState* replacement = nullptr;
         for (const auto& entry : cache_)
            if (entry.original.Get() == original.Get())
            {
               replacement = entry.filtered.Get();
               break;
            }
         if (!replacement)
         {
            // Only min/mag filtering changes; the rest is the native sampler's.
            desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            ComPtr<ID3D11SamplerState> filtered;
            if (FAILED(device->CreateSamplerState(&desc, &filtered))) return false;
            if (cache_.size() >= 8) cache_.clear();
            cache_.push_back({original, filtered});
            replacement = filtered.Get();
         }
         struct Restore
         {
            ID3D11DeviceContext* context;
            ID3D11SamplerState* sampler;
            ~Restore()
            {
               context->PSSetSamplers(2, 1, &sampler);
            }
         } restore{context, original.Get()};
         context->PSSetSamplers(2, 1, &replacement);
         draw();
         return true;
      }
   };

   using Microsoft::WRL::ComPtr;
   // Filters the SSR result for the SSR lighting draw only; the engine's SSR
   // history keeps the unfiltered result.
   class SpatialFilter
   {
      UINT width_ = 0, height_ = 0;
      std::array<ComPtr<ID3D11ShaderResourceView>, 2> views_;
      std::array<ComPtr<ID3D11UnorderedAccessView>, 2> outputs_;
      bool Ensure(ID3D11Device* device, UINT width, UINT height)
      {
         if (width_ == width && height_ == height && views_[0] && views_[1]) return true;
         views_ = {};
         outputs_ = {};
         width_ = height_ = 0;
         const DXGI_FORMAT formats[] = {DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R8_UNORM};
         for (UINT i = 0; i < 2; ++i)
         {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = formats[i];
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            ComPtr<ID3D11Texture2D> texture;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) || FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &views_[i])) || FAILED(device->CreateUnorderedAccessView(texture.Get(), nullptr, &outputs_[i])))
            {
               views_ = {};
               outputs_ = {};
               return false;
            }
         }
         width_ = width;
         height_ = height;
         return true;
      }
      static bool Extent(ID3D11ShaderResourceView* view, UINT width, UINT height, DXGI_FORMAT expected = DXGI_FORMAT_UNKNOWN)
      {
         if (!view) return false;
         D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
         view->GetDesc(&vd);
         if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || vd.Texture2D.MostDetailedMip || (expected != DXGI_FORMAT_UNKNOWN && vd.Format != expected)) return false;
         ComPtr<ID3D11Resource> resource;
         view->GetResource(&resource);
         ComPtr<ID3D11Texture2D> texture;
         if (FAILED(resource.As(&texture))) return false;
         D3D11_TEXTURE2D_DESC d{};
         texture->GetDesc(&d);
         return d.Width == width && d.Height == height && d.SampleDesc.Count == 1;
      }

   public:
      template <class Draw>
      bool Replay(ID3D11Device* device, ID3D11DeviceContext* context,
         ID3D11ComputeShader* shader, Draw&& draw)
      {
         if (!shader || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
         std::array<ComPtr<ID3D11ShaderResourceView>, 5> source;
         const UINT slots[] = {11, 12, 6, 0, 1};
         for (UINT i = 0; i < 5; ++i)
            context->PSGetShaderResources(slots[i], 1, &source[i]);
         ComPtr<ID3D11RenderTargetView> target;
         context->OMGetRenderTargets(1, &target, nullptr);
         if (!target) return false;
         ComPtr<ID3D11Resource> resource;
         target->GetResource(&resource);
         ComPtr<ID3D11Texture2D> texture;
         if (FAILED(resource.As(&texture))) return false;
         D3D11_TEXTURE2D_DESC d{};
         texture->GetDesc(&d);
         if (d.SampleDesc.Count != 1) return false;
         // SSR color and confidence (t11, t12), depth (t6) and G-buffers (t0, t1), all at the target size.
         if (!Extent(source[0].Get(), d.Width, d.Height, DXGI_FORMAT_R11G11B10_FLOAT) || !Extent(source[1].Get(), d.Width, d.Height, DXGI_FORMAT_R8_UNORM) || !Extent(source[2].Get(), d.Width, d.Height, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) || !Extent(source[3].Get(), d.Width, d.Height, DXGI_FORMAT_R10G10B10A2_UNORM) || !Extent(source[4].Get(), d.Width, d.Height, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) || !Ensure(device, d.Width, d.Height)) return false;
         ComPtr<ID3D11ComputeShader> previousShader;
         std::array<ID3D11ClassInstance*, 256> classes{};
         UINT classCount = UINT(classes.size());
         context->CSGetShader(&previousShader, classes.data(), &classCount);
         std::array<ComPtr<ID3D11ShaderResourceView>, 5> previousViews;
         std::array<ComPtr<ID3D11UnorderedAccessView>, 2> previousOutputs;
         for (UINT i = 0; i < 5; ++i)
            context->CSGetShaderResources(i, 1, &previousViews[i]);
         for (UINT i = 0; i < 2; ++i)
            context->CSGetUnorderedAccessViews(i, 1, &previousOutputs[i]);
         ID3D11ShaderResourceView* srv[5];
         for (UINT i = 0; i < 5; ++i)
            srv[i] = source[i].Get();
         ID3D11UnorderedAccessView* uav[] = {outputs_[0].Get(), outputs_[1].Get()};
         const UINT counts[] = {UINT(-1), UINT(-1)};
         context->CSSetShaderResources(0, 5, srv);
         context->CSSetUnorderedAccessViews(0, 2, uav, counts);
         context->CSSetShader(shader, nullptr, 0);
         context->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
         uav[0] = uav[1] = nullptr;
         context->CSSetUnorderedAccessViews(0, 2, uav, counts);
         for (UINT i = 0; i < 5; ++i)
            srv[i] = previousViews[i].Get();
         context->CSSetShaderResources(0, 5, srv);
         for (UINT i = 0; i < 2; ++i)
            uav[i] = previousOutputs[i].Get();
         context->CSSetUnorderedAccessViews(0, 2, uav, counts);
         context->CSSetShader(previousShader.Get(), classes.data(), classCount);
         for (UINT i = 0; i < classCount; ++i)
            if (classes[i]) classes[i]->Release();
         struct Restore
         {
            ID3D11DeviceContext* c;
            ID3D11ShaderResourceView* v[2];
            ~Restore()
            {
               c->PSSetShaderResources(11, 2, v);
            }
         } restore{context, {source[0].Get(), source[1].Get()}};
         ID3D11ShaderResourceView* filtered[] = {views_[0].Get(), views_[1].Get()};
         context->PSSetShaderResources(11, 2, filtered);
         draw();
         return true;
      }
   };
} // namespace Battlefront2SSR
