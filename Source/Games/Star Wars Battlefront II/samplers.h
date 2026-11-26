#pragma once
// DLSS's texture LOD bias on the game's material samplers.

#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Battlefront2MipBias
{
   inline float Offset(unsigned int render_width, unsigned int output_width, bool active)
   {
      if (!active || render_width == 0 || output_width == 0) return 0.f;
      // NVIDIA DLSS guide, section 3.5. Adjust adds it to the native sampler bias, exactly once. DLAA retains the -1
      // temporal detail bias.
      return std::log2(float(render_width) / float((std::max)(render_width, output_width))) - 1.f;
   }

   // Gives a game sampler DLSS's bias and 16x AF, or returns false to keep it as the game made it. Only the anisotropic
   // material samplers with a mip range change. Shadow, point, linear post-process and fixed-mip samplers stay native:
   // the soft shadow pass reads its shadow map through an ordinary (non-comparison) sampler, so Luma's generic upgrade
   // changed the shadows. Nothing changes without DLSS.
   inline bool Adjust(D3D11_SAMPLER_DESC& desc, float offset)
   {
      if (offset == 0.f || desc.Filter != D3D11_FILTER_ANISOTROPIC || desc.MaxLOD <= desc.MinLOD) return false;
      desc.MaxAnisotropy = D3D11_REQ_MAXANISOTROPY;
      desc.MipLODBias = (std::clamp)(desc.MipLODBias + offset, D3D11_MIP_LOD_BIAS_MIN, D3D11_MIP_LOD_BIAS_MAX);
      return true;
   }

   // The variants of the game's samplers, one per bias. Nothing is released before the device goes, so a pointer handed
   // out stays valid while another thread binds it, and an entry holds the game's sampler, so its address is never reused.
   class Variants
   {
      struct Entry
      {
         com_ptr<ID3D11SamplerState> native;
         std::vector<std::pair<float, com_ptr<ID3D11SamplerState>>> by_bias;
         ID3D11SamplerState* Find(float bias, bool& found) const
         {
            for (const auto& [b, variant] : by_bias)
               if (b == bias)
               {
                  found = true;
                  return variant.get();
               }
            found = false;
            return nullptr;
         }
      };
      std::shared_mutex mutex_;
      std::unordered_map<ID3D11SamplerState*, Entry> entries_;

   public:
      // What to bind instead of `native` at this bias, or null to keep the game's sampler.
      ID3D11SamplerState* Get(ID3D11Device* device, ID3D11SamplerState* native, float bias)
      {
         if (!native || bias == 0.f) return nullptr;
         bool found = false;
         {
            const std::shared_lock lock(mutex_);
            const auto entry = entries_.find(native);
            if (entry != entries_.end())
            {
               auto* variant = entry->second.Find(bias, found);
               if (found) return variant;
            }
         }
         D3D11_SAMPLER_DESC desc{};
         native->GetDesc(&desc);
         com_ptr<ID3D11SamplerState> variant;
         if (Adjust(desc, bias) && FAILED(device->CreateSamplerState(&desc, &variant))) variant = nullptr;
         const std::unique_lock lock(mutex_);
         auto& entry = entries_[native];
         auto* existing = entry.Find(bias, found);
         if (found) return existing; // Another thread made it first.
         entry.native = native;
         entry.by_bias.emplace_back(bias, variant);
         return variant.get();
      }
      // Whether `bound` is one of the variants of `native`.
      bool Owns(ID3D11SamplerState* native, ID3D11SamplerState* bound)
      {
         const std::shared_lock lock(mutex_);
         const auto entry = entries_.find(native);
         if (entry == entries_.end()) return false;
         for (const auto& [bias, variant] : entry->second.by_bias)
            if (variant.get() == bound) return true;
         return false;
      }
   };
} // namespace Battlefront2MipBias

// Rebinds the game's samplers with the current mip bias. Called only when the bias changes, on the immediate render
// thread. Native D3D calls bypass the add-on's events, so the tracked original binds stay as they are.
// "native" holds the samplers the game bound on the immediate context, by stage and slot.
inline void RefreshMipBiasSamplers(ID3D11Device* device, ID3D11DeviceContext* context, Battlefront2MipBias::Variants& variants, float bias,
   const com_ptr<ID3D11SamplerState> (&native)[6][D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT])
{
   using GetSamplers = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState**);
   using SetSamplers = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState* const*);
   static const GetSamplers get[6] = {&ID3D11DeviceContext::VSGetSamplers, &ID3D11DeviceContext::HSGetSamplers, &ID3D11DeviceContext::DSGetSamplers,
      &ID3D11DeviceContext::GSGetSamplers, &ID3D11DeviceContext::PSGetSamplers, &ID3D11DeviceContext::CSGetSamplers};
   static const SetSamplers set[6] = {&ID3D11DeviceContext::VSSetSamplers, &ID3D11DeviceContext::HSSetSamplers, &ID3D11DeviceContext::DSSetSamplers,
      &ID3D11DeviceContext::GSSetSamplers, &ID3D11DeviceContext::PSSetSamplers, &ID3D11DeviceContext::CSSetSamplers};
   if (!context) return;
   for (unsigned stage = 0; stage < 6; ++stage)
   {
      ID3D11SamplerState* bound[16]{};
      (context->*get[stage])(0, 16, bound);
      ID3D11SamplerState* next[16]{};
      std::copy(std::begin(bound), std::end(bound), std::begin(next));
      bool changed = false;
      for (unsigned slot = 0; slot < 16; ++slot)
      {
         auto* original = native[stage][slot].get();
         if (!original || !bound[slot]) continue;
         // Something else (a framework or private pass) bound this slot: keep it.
         if (bound[slot] != original && !variants.Owns(original, bound[slot])) continue;
         auto* variant = variants.Get(device, original, bias);
         next[slot] = variant ? variant : original;
         changed |= next[slot] != bound[slot];
      }
      if (changed) (context->*set[stage])(0, 16, next);
      for (auto* sampler : bound)
         if (sampler) sampler->Release();
   }
}

// Captured shield material: scrolling normal/noise at PS t3/t4/t5 via s1,
// Fresnel opacity and blue/red shield variants. Never match the entire
// half-resolution transparency target: it also contains smoke and fog.
namespace Battlefront2ShieldMipBias
{
   inline bool Matches(uint64_t vs, uint64_t ps)
   {
      return vs == 0x62B9A472 && (ps == 0x2D825D95 || ps == 0x9BA8766E);
   }

   struct State
   {
      struct Entry
      {
         com_ptr<ID3D11SamplerState> bound; // Holds a reference so the key's address cannot be reused.
         float native_bias;
         com_ptr<ID3D11SamplerState> native_bias_sampler;
      };
      std::vector<Entry> cache;

      // Draws with the bound s1 sampler's mip bias set back to the native sampler's. Filter, anisotropy, addressing
      // and LOD clamps stay as bound.
      bool Draw(ID3D11Device* device, ID3D11DeviceContext* context,
         ID3D11SamplerState* native, std::function<void()>& draw)
      {
         if (!native) return false;
         com_ptr<ID3D11SamplerState> bound;
         context->PSGetSamplers(1, 1, &bound);
         if (!bound) return false;
         D3D11_SAMPLER_DESC desc{}, native_desc{};
         bound->GetDesc(&desc);
         native->GetDesc(&native_desc);
         if (desc.MipLODBias == native_desc.MipLODBias) return false;
         desc.MipLODBias = native_desc.MipLODBias;
         ID3D11SamplerState* replacement = nullptr;
         for (const auto& entry : cache)
            if (entry.bound == bound && entry.native_bias == desc.MipLODBias)
            {
               replacement = entry.native_bias_sampler.get();
               break;
            }
         if (!replacement)
         {
            com_ptr<ID3D11SamplerState> sampler;
            if (FAILED(device->CreateSamplerState(&desc, &sampler))) return false;
            if (cache.size() >= 32) cache.clear();
            cache.push_back({bound, desc.MipLODBias, sampler});
            replacement = sampler.get();
         }
         context->PSSetSamplers(1, 1, &replacement);
         draw();
         ID3D11SamplerState* restore = bound.get();
         context->PSSetSamplers(1, 1, &restore);
         return true;
      }
   };
} // namespace Battlefront2ShieldMipBias
