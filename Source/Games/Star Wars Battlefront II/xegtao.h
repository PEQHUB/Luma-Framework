#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Replaces the game's screen-space AO with Luma's shared XeGTAO (Shaders/Includes/XeGTAO.hlsl), through
// Shaders/Star Wars Battlefront II/Luma_SWBF2_XeGTAO.hlsl, or with CORTAO (Luma_SWBF2_CORTAO.hlsl, on the reference
// implementation in Includes/CORTAO). Both shaders describe the game's inputs.
namespace Battlefront2XeGTAO
{
   using Microsoft::WRL::ComPtr;
   inline std::atomic<bool> enabled{true};             // Replace the game's raw AO (XeGTAO, VB-GTAO or CORTAO)
   inline std::atomic<bool> visibility_bitmask{false}; // VB-GTAO instead of XeGTAO (VISIBILITY_BITMASK)
   inline std::atomic<bool> cortao{true};              // CORTAO instead of either
   inline std::atomic<float> radius_scale{1.f};
   inline std::atomic<unsigned> quality{1}; // 0: XE_GTAO_QUALITY 1 (XeGTAO 7 slices), 1: XE_GTAO_QUALITY 2 (10); VB-GTAO 4 slices of 4 or 6 steps
   // CORTAO's samples per pixel (CORTAO_SAMPLE_COUNT, "BattlefrontIICORTAOSamples"), the reference's quality levels
   // from Minimal to Ultra. Config only.
   inline constexpr std::array<unsigned, 7> cortao_samples = {4, 6, 8, 12, 16, 24, 32};
   inline constexpr unsigned cortao_default_level = 6; // 32 samples (Ultra)
   inline std::atomic<unsigned> cortao_level{cortao_default_level};
   // A new noise seed each frame, as the reference asks for with a temporal filter after it (DLSS or the game's TAA);
   // off keeps the noise still ("BattlefrontIICORTAOTemporalNoise", config only).
   inline std::atomic<bool> cortao_temporal_noise{true};

   inline uint32_t CORTAOSeed(uint64_t frame)
   {
      uint32_t x = uint32_t(frame) * 0x9E3779B9u;
      x ^= x >> 16;
      x *= 0x7FEB352Du;
      x ^= x >> 15;
      x *= 0x846CA68Bu;
      return x ^ (x >> 16);
   }

   // CORTAO's occluder lookup table for 32 rays: 24x24 occluder directions (sphere map) by 16 occluder radii, one ray
   // mask each. The reference's generator (utils/generators/OccluderLookupTableGenerator.py, 24x24x16 R32_UINT) in C++;
   // it matches the published 24x24x16x32_R32_UINT.bin bit for bit.
   inline constexpr UINT cortao_table_directions = 24, cortao_table_radii = 16;
   inline std::vector<uint32_t> CORTAOOccluderTable()
   {
      constexpr int rays = 32, directions = int(cortao_table_directions), radii = int(cortao_table_radii);
      const double golden_angle = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
      double ray[rays][3];
      for (int i = 0; i < rays; ++i)
      {
         const double angle = i * golden_angle, radius = std::sqrt((i + 0.5) / rays);
         ray[i][0] = std::cos(angle) * radius;
         ray[i][1] = std::sin(angle) * radius;
         ray[i][2] = std::sqrt(1.0 - ray[i][0] * ray[i][0] - ray[i][1] * ray[i][1]);
      }
      std::vector<uint32_t> table;
      table.reserve(size_t(directions) * directions * radii);
      for (int z = 0; z < radii; ++z)
      {
         const double occluder_radius = std::sqrt(2.0) * (z + 0.5) / radii;
         const double occluder_cos_angle = 1.0 - occluder_radius * occluder_radius * 0.5;
         // Every ray is also blocked in the direction closest to it, so even the smallest occluder blocks something.
         int closest[rays];
         double closest_cos[rays];
         for (int i = 0; i < rays; ++i)
         {
            closest[i] = -1;
            closest_cos[i] = -1.0;
         }
         const size_t offset = table.size();
         for (int y = 0; y < directions; ++y)
            for (int x = 0; x < directions; ++x)
            {
               // Sphere map decode of the texel center
               const double ex = (x + 0.5) / directions * 2.0 - 1.0, ey = (y + 0.5) / directions * 2.0 - 1.0;
               const double zz = (std::max)(1.0 - ex * ex - ey * ey, 0.0), scale = 2.0 * std::sqrt(zz);
               const double direction[3] = {ex * scale, ey * scale, zz * 2.0 - 1.0};
               uint32_t bits = 0;
               if (direction[2] != -1.0)
                  for (int i = 0; i < rays; ++i)
                  {
                     const double cos_angle = direction[0] * ray[i][0] + direction[1] * ray[i][1] + direction[2] * ray[i][2];
                     if (cos_angle > closest_cos[i])
                     {
                        closest[i] = y * directions + x;
                        closest_cos[i] = cos_angle;
                     }
                     if (cos_angle > occluder_cos_angle) bits |= 1u << i;
                  }
               table.push_back(bits);
            }
         for (int i = 0; i < rays; ++i)
            if (closest[i] >= 0) table[offset + closest[i]] |= 1u << i;
      }
      return table;
   }

   // The game's AO arrays are the AO resolution deinterleaved 4x4 (16 slices). The AO resolution need not be a
   // multiple of 4 (DLSS Balanced at 4K renders AO at 1114x628), so accept arrays rounded up or down.
   inline bool DeinterleavedSize(UINT array_size, UINT full_size)
   {
      return array_size == (full_size + 3) / 4 || array_size == full_size / 4;
   }

   class State
   {
      UINT width_ = 0, height_ = 0;
      uint64_t depth_frame_ = UINT64_MAX;
      ComPtr<ID3D11Resource> native_depth_arrays_, native_normal_arrays_;
      // Private textures: the copied linear depth, its prefiltered mip chain (XE_GTAO_DEPTH_MIP_LEVELS) and the
      // working AO term (x, visibility / XE_GTAO_OCCLUSION_TERM_SCALE) with packed edges (y).
      ComPtr<ID3D11Texture2D> source_depth_, depth_mips_, working_;
      ComPtr<ID3D11ShaderResourceView> source_depth_srv_, depth_mips_srv_, working_srv_;
      std::array<ComPtr<ID3D11UnorderedAccessView>, 5> depth_mip_uavs_;
      ComPtr<ID3D11UnorderedAccessView> working_uav_;
      // CORTAO's: the packed linear depth and normal with its mips, the unfiltered occlusion and the occluder table.
      UINT cortao_mips_ = 0;
      ComPtr<ID3D11Texture2D> cortao_packed_, cortao_raw_;
      ComPtr<ID3D11Texture3D> cortao_table_;
      ComPtr<ID3D11ShaderResourceView> cortao_packed_srv_, cortao_raw_srv_, cortao_table_srv_;
      std::array<ComPtr<ID3D11UnorderedAccessView>, 8> cortao_packed_uavs_;
      ComPtr<ID3D11UnorderedAccessView> cortao_raw_uav_;
      ComPtr<ID3D11Buffer> params_;
      ComPtr<ID3D11SamplerState> sampler_;

      // The textures both methods share.
      bool Ensure(ID3D11Device* d, UINT w, UINT h)
      {
         if (w == width_ && h == height_ && source_depth_ && sampler_ && params_) return true;
         *this = State{};
         D3D11_TEXTURE2D_DESC td{};
         td.Width = w;
         td.Height = h;
         td.ArraySize = 1;
         td.SampleDesc.Count = 1;
         td.MipLevels = 1;
         td.Format = DXGI_FORMAT_R32_FLOAT;
         td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
         if (FAILED(d->CreateTexture2D(&td, nullptr, &source_depth_)) || FAILED(d->CreateShaderResourceView(source_depth_.Get(), nullptr, &source_depth_srv_))) return false;
         D3D11_BUFFER_DESC bd{};
         bd.ByteWidth = 32; // cbuffer LumaGTAO (16 bytes) / LumaCORTAO (32)
         bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         if (FAILED(d->CreateBuffer(&bd, nullptr, &params_))) return false;
         D3D11_SAMPLER_DESC sd{};
         sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
         sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
         sd.MaxLOD = D3D11_FLOAT32_MAX;
         if (FAILED(d->CreateSamplerState(&sd, &sampler_))) return false;
         width_ = w;
         height_ = h;
         return true;
      }

      bool EnsureGTAO(ID3D11Device* d)
      {
         if (working_) return true;
         D3D11_TEXTURE2D_DESC td{};
         td.Width = width_;
         td.Height = height_;
         td.ArraySize = 1;
         td.SampleDesc.Count = 1;
         td.Format = DXGI_FORMAT_R32_FLOAT;
         td.MipLevels = UINT(depth_mip_uavs_.size());
         td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
         if (FAILED(d->CreateTexture2D(&td, nullptr, &depth_mips_)) || FAILED(d->CreateShaderResourceView(depth_mips_.Get(), nullptr, &depth_mips_srv_))) return false;
         for (UINT i = 0; i < depth_mip_uavs_.size(); ++i)
         {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = td.Format;
            uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uv.Texture2D.MipSlice = i;
            if (FAILED(d->CreateUnorderedAccessView(depth_mips_.Get(), &uv, &depth_mip_uavs_[i]))) return false;
         }
         td.MipLevels = 1;
         td.Format = DXGI_FORMAT_R8G8_UNORM;
         if (FAILED(d->CreateTexture2D(&td, nullptr, &working_)) || FAILED(d->CreateShaderResourceView(working_.Get(), nullptr, &working_srv_)) || FAILED(d->CreateUnorderedAccessView(working_.Get(), nullptr, &working_uav_)))
         {
            working_ = nullptr;
            return false;
         }
         return true;
      }

      bool EnsureCORTAO(ID3D11Device* d)
      {
         if (cortao_raw_uav_) return true;
         // Mips down to 1x1 or the reference's limit of 8, whichever comes first.
         UINT mips = 1;
         while (mips < cortao_packed_uavs_.size() && ((width_ | height_) >> mips) != 0)
            ++mips;
         D3D11_TEXTURE2D_DESC td{};
         td.Width = width_;
         td.Height = height_;
         td.ArraySize = 1;
         td.SampleDesc.Count = 1;
         td.MipLevels = mips;
         td.Format = DXGI_FORMAT_R32_UINT;
         td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
         if (FAILED(d->CreateTexture2D(&td, nullptr, &cortao_packed_)) || FAILED(d->CreateShaderResourceView(cortao_packed_.Get(), nullptr, &cortao_packed_srv_))) return false;
         for (UINT i = 0; i < mips; ++i)
         {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
            uv.Format = td.Format;
            uv.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uv.Texture2D.MipSlice = i;
            if (FAILED(d->CreateUnorderedAccessView(cortao_packed_.Get(), &uv, &cortao_packed_uavs_[i]))) return false;
         }
         static const std::vector<uint32_t> table = CORTAOOccluderTable();
         D3D11_TEXTURE3D_DESC vd{};
         vd.Width = vd.Height = cortao_table_directions;
         vd.Depth = cortao_table_radii;
         vd.MipLevels = 1;
         vd.Format = DXGI_FORMAT_R32_UINT;
         vd.Usage = D3D11_USAGE_IMMUTABLE;
         vd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
         const D3D11_SUBRESOURCE_DATA init{table.data(), cortao_table_directions * 4, cortao_table_directions * cortao_table_directions * 4};
         if (FAILED(d->CreateTexture3D(&vd, &init, &cortao_table_)) || FAILED(d->CreateShaderResourceView(cortao_table_.Get(), nullptr, &cortao_table_srv_))) return false;
         td.MipLevels = 1;
         td.Format = DXGI_FORMAT_R8_UNORM;
         if (FAILED(d->CreateTexture2D(&td, nullptr, &cortao_raw_)) || FAILED(d->CreateShaderResourceView(cortao_raw_.Get(), nullptr, &cortao_raw_srv_))) return false;
         if (FAILED(d->CreateUnorderedAccessView(cortao_raw_.Get(), nullptr, &cortao_raw_uav_))) return false;
         cortao_mips_ = mips;
         return true;
      }

   public:
      // The passes for the selected method and quality.
      struct Kernels
      {
         bool cortao = false;
         ID3D11ComputeShader* prefilter = nullptr; // "prefilter_depths16x16_cs", or CORTAO's prepass (CORTAO_PASS 0)
         ID3D11ComputeShader* main = nullptr;      // "main_pass_cs", or CORTAO's main pass (CORTAO_PASS 1)
         ID3D11ComputeShader* denoise = nullptr;   // "denoise_pass_cs" with XE_GTAO_FINAL_APPLY, or CORTAO's filter (CORTAO_PASS 2)
      };

      // The game's raw AO pass only writes its R8 output, and Replace checks everything before any GPU work, so
      // either XeGTAO replaces the pass or the native dispatch runs, once.
      template <class NativeDispatch>
      bool Dispatch(ID3D11DeviceContext* c, uint64_t frame, const Kernels& kernels, NativeDispatch&& native)
      {
         if (Replace(c, frame, kernels)) return true;
         native();
         return false;
      }

      // Renderer thread only. Disabled frames release the private allocation.
      void Reset()
      {
         if (source_depth_ || depth_frame_ != UINT64_MAX) *this = State{};
      }

      // Call at the game's AO deinterleave pass (Battlefront2ShaderVariants::AODepth), before it runs: copies this
      // frame's full resolution R32 linear depth (not the saturated R16 arrays it writes).
      void RecordFullDepth(ID3D11Device* d, ID3D11DeviceContext* c, uint64_t frame)
      {
         depth_frame_ = UINT64_MAX;
         if (!enabled.load() || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
         ComPtr<ID3D11ShaderResourceView> srv;
         c->CSGetShaderResources(0, 1, &srv);
         if (!srv) return;
         D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
         srv->GetDesc(&sv);
         if (sv.Format != DXGI_FORMAT_R32_FLOAT || sv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || sv.Texture2D.MostDetailedMip != 0) return;
         ComPtr<ID3D11Resource> r;
         srv->GetResource(&r);
         ComPtr<ID3D11Texture2D> t;
         if (FAILED(r.As(&t))) return;
         D3D11_TEXTURE2D_DESC td{};
         t->GetDesc(&td);
         if (td.SampleDesc.Count != 1 || td.ArraySize != 1 || (td.Format != DXGI_FORMAT_R32_FLOAT && td.Format != DXGI_FORMAT_R32_TYPELESS)) return;
         if (td.Width < 16 || td.Height < 16) return;
         // Its outputs: the R16 depth and R8G8 normal arrays the raw AO pass reads next.
         std::array<ComPtr<ID3D11UnorderedAccessView>, 2> native;
         c->CSGetUnorderedAccessViews(0, 1, &native[0]);
         c->CSGetUnorderedAccessViews(1, 1, &native[1]);
         for (unsigned i = 0; i < 2; ++i)
         {
            if (!native[i]) return;
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            native[i]->GetDesc(&view);
            if (view.Format != (i ? DXGI_FORMAT_R8G8_SNORM : DXGI_FORMAT_R16_FLOAT) || view.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2DARRAY || view.Texture2DArray.MipSlice != 0 || view.Texture2DArray.FirstArraySlice != 0 || view.Texture2DArray.ArraySize != 16) return;
            ComPtr<ID3D11Resource> resource;
            native[i]->GetResource(&resource);
            ComPtr<ID3D11Texture2D> array;
            if (FAILED(resource.As(&array))) return;
            D3D11_TEXTURE2D_DESC array_desc{};
            array->GetDesc(&array_desc);
            if (!DeinterleavedSize(array_desc.Width, td.Width) || !DeinterleavedSize(array_desc.Height, td.Height) || array_desc.ArraySize != 16 || array_desc.SampleDesc.Count != 1) return;
         }
         if (!Ensure(d, td.Width, td.Height) || !(cortao.load() ? EnsureCORTAO(d) : EnsureGTAO(d))) return;
         native[0]->GetResource(&native_depth_arrays_);
         native[1]->GetResource(&native_normal_arrays_);
         c->CopySubresourceRegion(source_depth_.Get(), 0, 0, 0, 0, t.Get(), 0, nullptr);
         depth_frame_ = frame;
      }

      // For the raw HBAO passes (Battlefront2ShaderVariants::RawAO). True means the raw AO was replaced. The game's
      // two AO blur passes still run afterwards, on what XeGTAO's denoise pass or CORTAO's filter writes into its target.
      bool Replace(ID3D11DeviceContext* c, uint64_t frame, const Kernels& kernels)
      {
         const float radius = radius_scale.load(); // Kept in [0.5, 2] by the settings.
         if (!enabled.load()) return false;
         if (depth_frame_ != frame || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
         if (!kernels.prefilter || !kernels.main || !kernels.denoise) return false;
         if (kernels.cortao ? !cortao_raw_uav_ : !working_) return false; // Made for the method at the deinterleave pass
         ComPtr<ID3D11ShaderResourceView> normal;
         c->CSGetShaderResources(1, 1, &normal);
         ComPtr<ID3D11UnorderedAccessView> output;
         c->CSGetUnorderedAccessViews(0, 1, &output);
         if (!normal || !output) return false;
         D3D11_SHADER_RESOURCE_VIEW_DESC ns{};
         normal->GetDesc(&ns);
         D3D11_UNORDERED_ACCESS_VIEW_DESC os{};
         output->GetDesc(&os);
         if (ns.Format != DXGI_FORMAT_R8G8_SNORM || ns.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY || ns.Texture2DArray.MostDetailedMip != 0 || ns.Texture2DArray.FirstArraySlice != 0 || ns.Texture2DArray.ArraySize != 16 || os.Format != DXGI_FORMAT_R8_UNORM || os.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D || os.Texture2D.MipSlice != 0) return false;
         // The arrays must be the ones the deinterleave pass of this frame wrote.
         ComPtr<ID3D11Resource> nr, orr;
         normal->GetResource(&nr);
         output->GetResource(&orr);
         ComPtr<ID3D11ShaderResourceView> native_depth;
         c->CSGetShaderResources(0, 1, &native_depth);
         ComPtr<ID3D11Resource> dr;
         if (native_depth) native_depth->GetResource(&dr);
         if (dr.Get() != native_depth_arrays_.Get() || nr.Get() != native_normal_arrays_.Get()) return false;
         ComPtr<ID3D11Texture2D> nt, ot;
         if (FAILED(nr.As(&nt)) || FAILED(orr.As(&ot))) return false;
         D3D11_TEXTURE2D_DESC nd{}, od{};
         nt->GetDesc(&nd);
         ot->GetDesc(&od);
         if (!DeinterleavedSize(nd.Width, width_) || !DeinterleavedSize(nd.Height, height_) || od.Width != width_ || od.Height != height_ || od.ArraySize != 1 || od.SampleDesc.Count != 1) return false;
         // The native AO constants (b0) must be bound in full: the shared passes read the camera and radius from them.
         ComPtr<ID3D11DeviceContext1> ranged;
         c->QueryInterface(IID_PPV_ARGS(&ranged));
         ComPtr<ID3D11Buffer> cb;
         UINT first = 0, count = 0;
         if (ranged)
            ranged->CSGetConstantBuffers1(0, 1, &cb, &first, &count);
         else
            c->CSGetConstantBuffers(0, 1, &cb);
         if (!cb) return false;
         D3D11_BUFFER_DESC bd{};
         cb->GetDesc(&bd);
         if (!ranged) count = bd.ByteWidth / 16;
         if (count < 5 || uint64_t(first) * 16 + 80 > bd.ByteWidth) return false;

         // Save the compute state of the game's dispatch. Its b0 stays bound.
         constexpr UINT srv_count = 6, uav_count = 8, params_slot = 11; // cbuffer LumaGTAO / LumaCORTAO : register(b11)
         std::array<ID3D11ShaderResourceView*, srv_count> old_srvs{};
         c->CSGetShaderResources(0, srv_count, old_srvs.data());
         std::array<ID3D11UnorderedAccessView*, uav_count> old_uavs{};
         c->CSGetUnorderedAccessViews(0, uav_count, old_uavs.data());
         ComPtr<ID3D11ComputeShader> old_shader;
         std::array<ID3D11ClassInstance*, 256> classes{};
         UINT class_count = 256;
         c->CSGetShader(&old_shader, classes.data(), &class_count);
         ComPtr<ID3D11SamplerState> old_sampler;
         c->CSGetSamplers(0, 1, &old_sampler);
         ComPtr<ID3D11Buffer> old_params;
         UINT pf = 0, pc = 0;
         if (ranged)
            ranged->CSGetConstantBuffers1(params_slot, 1, &old_params, &pf, &pc);
         else
            c->CSGetConstantBuffers(params_slot, 1, &old_params);

         std::array<UINT, uav_count> counters;
         counters.fill(UINT(-1));
         const std::array<ID3D11ShaderResourceView*, srv_count> null_srvs{};
         const std::array<ID3D11UnorderedAccessView*, uav_count> null_uavs{};
         c->CSSetShaderResources(0, srv_count, null_srvs.data());
         c->CSSetUnorderedAccessViews(0, uav_count, null_uavs.data(), counters.data());
         uint32_t parameters[8] = {}; // XeGTAO: RadiusScaleRT. CORTAO: Resolution, RadiusScale, MipCount, RandomSeed.
         if (kernels.cortao)
         {
            parameters[0] = width_;
            parameters[1] = height_;
            std::memcpy(&parameters[2], &radius, sizeof(radius));
            parameters[3] = cortao_mips_;
            parameters[4] = cortao_temporal_noise.load() ? CORTAOSeed(frame) : 0u;
         }
         else
            std::memcpy(&parameters[0], &radius, sizeof(radius));
         c->UpdateSubresource(params_.Get(), 0, nullptr, parameters, 0, 0);
         ID3D11Buffer* params = params_.Get();
         c->CSSetConstantBuffers(params_slot, 1, &params);
         ID3D11SamplerState* sampler = sampler_.Get(); // Point clamp, as the shared XeGTAO passes expect
         c->CSSetSamplers(0, 1, &sampler);
         const auto run = [&](ID3D11ComputeShader* shader, const std::array<ID3D11ShaderResourceView*, 3>& inputs,
                             const std::array<ID3D11UnorderedAccessView*, uav_count>& outputs, UINT groups_x, UINT groups_y)
         {
            c->CSSetShaderResources(0, UINT(inputs.size()), inputs.data());
            c->CSSetUnorderedAccessViews(0, uav_count, outputs.data(), counters.data());
            c->CSSetShader(shader, nullptr, 0);
            c->Dispatch(groups_x, groups_y, 1);
            c->CSSetUnorderedAccessViews(0, uav_count, null_uavs.data(), counters.data());
            c->CSSetShaderResources(0, srv_count, null_srvs.data());
         };
         if (kernels.cortao)
         {
            // Linear depth and normals packed into one R32 texture with its mips (8x8 groups).
            std::array<ID3D11UnorderedAccessView*, uav_count> mips{};
            for (UINT i = 0; i < cortao_mips_; ++i)
               mips[i] = cortao_packed_uavs_[i].Get();
            run(kernels.prefilter, {source_depth_srv_.Get(), normal.Get(), nullptr}, mips, (width_ + 7) / 8, (height_ + 7) / 8);
            // Occlusion, with the game's own normals (t1) and the occluder table (t2); 16x16 groups.
            run(kernels.main, {cortao_packed_srv_.Get(), normal.Get(), cortao_table_srv_.Get()}, {cortao_raw_uav_.Get()}, (width_ + 15) / 16, (height_ + 15) / 16);
            // The 13 tap filter, written straight into the game's R8 AO target; each thread does a 2x2 block.
            run(kernels.denoise, {cortao_packed_srv_.Get(), cortao_raw_srv_.Get(), nullptr}, {output.Get()}, (width_ + 15) / 16, (height_ + 15) / 16);
         }
         else
         {
            // Linear depth to its prefiltered mip chain; each 8x8 group covers 16x16 pixels.
            run(kernels.prefilter, {source_depth_srv_.Get(), nullptr, nullptr},
               {depth_mip_uavs_[0].Get(), depth_mip_uavs_[1].Get(), depth_mip_uavs_[2].Get(), depth_mip_uavs_[3].Get(), depth_mip_uavs_[4].Get()},
               (width_ + 15) / 16, (height_ + 15) / 16);
            // AO term and edges, with the game's own normals (t1).
            run(kernels.main, {depth_mips_srv_.Get(), normal.Get(), nullptr}, {working_uav_.Get()}, (width_ + 7) / 8, (height_ + 7) / 8);
            // Spatial denoise, written straight into the game's R8 AO target; each thread does two horizontal pixels.
            run(kernels.denoise, {working_srv_.Get(), nullptr, nullptr}, {output.Get()}, (width_ + 15) / 16, (height_ + 7) / 8);
         }

         c->CSSetShader(old_shader.Get(), classes.data(), class_count);
         for (UINT i = 0; i < class_count; ++i)
            if (classes[i]) classes[i]->Release();
         params = old_params.Get();
         if (ranged)
            ranged->CSSetConstantBuffers1(params_slot, 1, &params, &pf, &pc);
         else
            c->CSSetConstantBuffers(params_slot, 1, &params);
         sampler = old_sampler.Get();
         c->CSSetSamplers(0, 1, &sampler);
         c->CSSetShaderResources(0, srv_count, old_srvs.data());
         c->CSSetUnorderedAccessViews(0, uav_count, old_uavs.data(), counters.data());
         for (auto* p : old_srvs)
            if (p) p->Release();
         for (auto* p : old_uavs)
            if (p) p->Release();
         return true;
      }
   };
} // namespace Battlefront2XeGTAO
