#pragma once
// Texture LOD bias for the game's samplers at the DLSS render resolution.

#include <d3d11.h>
#include <algorithm>
#include <cmath>

namespace Battlefront2MipBias
{
   inline float Offset(unsigned int render_width, unsigned int output_width, bool active)
   {
      if (!active || render_width == 0 || output_width == 0) return 0.f;
      // NVIDIA DLSS guide, section 3.5. Luma's sampler upgrade adds it to the
      // native sampler bias, exactly once. DLAA retains the -1 temporal detail bias.
      return std::log2(float(render_width) / float((std::max)(render_width, output_width))) - 1.f;
   }
} // namespace Battlefront2MipBias

// Rebinds the game's samplers with the current mip bias. Called only when the bias changes, on the immediate render
// thread. Native D3D calls bypass the add-on's events, so the tracked original binds stay as they are.
// "native" holds the samplers the game bound on the immediate context, by stage and slot.
inline void RefreshMipBiasSamplers(ID3D11Device* device, DeviceData& data,
   const com_ptr<ID3D11SamplerState> (&native)[6][D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT])
{
   using GetSamplers = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState**);
   using SetSamplers = void (STDMETHODCALLTYPE ID3D11DeviceContext::*)(UINT, UINT, ID3D11SamplerState* const*);
   static const GetSamplers get[6] = {&ID3D11DeviceContext::VSGetSamplers, &ID3D11DeviceContext::HSGetSamplers, &ID3D11DeviceContext::DSGetSamplers,
      &ID3D11DeviceContext::GSGetSamplers, &ID3D11DeviceContext::PSGetSamplers, &ID3D11DeviceContext::CSGetSamplers};
   static const SetSamplers set[6] = {&ID3D11DeviceContext::VSSetSamplers, &ID3D11DeviceContext::HSSetSamplers, &ID3D11DeviceContext::DSSetSamplers,
      &ID3D11DeviceContext::GSSetSamplers, &ID3D11DeviceContext::PSSetSamplers, &ID3D11DeviceContext::CSSetSamplers};
   auto* context = data.primary_command_list.get();
   if (!context) return;
   for (unsigned stage = 0; stage < 6; ++stage)
   {
      ID3D11SamplerState* bound[16]{};
      (context->*get[stage])(0, 16, bound);
      ID3D11SamplerState* next[16]{};
      std::copy(std::begin(bound), std::end(bound), std::begin(next));
      bool changed = false;
      {
         std::unique_lock lock(s_mutex_samplers);
         for (unsigned slot = 0; slot < 16; ++slot)
         {
            auto* original = native[stage][slot].get();
            if (!original || !bound[slot]) continue;
            auto found = data.custom_sampler_by_original_sampler.find(reinterpret_cast<uint64_t>(original));
            if (found == data.custom_sampler_by_original_sampler.end()) continue;
            auto& variants = found->second;
            bool owned = bound[slot] == original;
            for (const auto& [bias, sampler] : variants)
               owned |= sampler.get() == bound[slot];
            // Something else (a framework or private pass) bound this slot: keep it.
            if (!owned) continue;
            auto current = variants.find(data.texture_mip_lod_bias_offset);
            if (current == variants.end())
            {
               D3D11_SAMPLER_DESC desc{};
               original->GetDesc(&desc);
               current = variants.emplace(data.texture_mip_lod_bias_offset, CreateCustomSampler(data, device, desc)).first;
            }
            next[slot] = current->second ? current->second.get() : original;
            changed |= next[slot] != bound[slot];
         }
         if (changed) (context->*set[stage])(0, 16, next);
      }
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
