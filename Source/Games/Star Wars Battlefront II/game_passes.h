#pragma once
// The game passes Luma recognizes (shader hashes) and how the main view's scene flows through them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

// Game shader hashes, checked against the game's DXBC. Recognizing a hash
// does not replace the per-draw checks of the bound resources and constants.
namespace Battlefront2ShaderVariants
{
   template <std::size_t N>
   constexpr bool SortedUnique(const std::array<uint32_t, N>& values)
   {
      for (std::size_t i = 1; i < N; ++i)
         if (values[i - 1] >= values[i]) return false;
      return true;
   }
   // Binary search of a SortedUnique array.
   template <std::size_t N>
   constexpr bool Contains(const std::array<uint32_t, N>& sorted, uint64_t hash)
   {
      std::size_t first = 0, end = N;
      while (first < end)
      {
         const std::size_t mid = first + (end - first) / 2;
         if (sorted[mid] < hash)
            first = mid + 1;
         else
            end = mid;
      }
      return first < N && sorted[first] == hash;
   }

   // Frostbite's temporal resolve is one compute shader compiled into many
   // permutations. The game picks one from its anti-aliasing feature flags
   // (disocclusion rejection, history sharpening, neighbor weights, depth-of-
   // field filtering), and the pick can change when video settings are applied.
   // These are all the permutations in the game's shader set with the
   // bindings DLSS relies on:
   //  - cbuffer ShaderConstants, 304 bytes, g_width at 0 through
   //    g_postSharpenAmount at 300 (jitter matrices at 112 and 176);
   //  - g_depthTexture t0, g_velocityTexture t1, g_inputTexture0 t3,
   //    g_historyInputTexture0 t4, g_outputTexture0 u2, g_historyOutputTexture0 u3.
   // Optional bindings the native pass keeps for itself: point sampler s0,
   // velocity-magnitude history t2/u0. Thread groups 8x8x1 and 8x16x1 carry the
   // same code. Permutations with the debug overlay (u1) or without depth are
   // left to the game.
   inline constexpr std::array<uint32_t, 16> temporal_color = {
      0x0A50E245, 0x0D253015, 0x15E5592E, 0x1E38E5DF, 0x211EF389, 0x48FE89EC,
      0x4A84A874, 0x586AAE0B, 0x6EB33A5A, 0x7C89CCC1, 0xB1716D82, 0xB7802634,
      0xC6E49224, 0xDE2FBDE8, 0xEA9E04FF, 0xEC14BCDC};
   // As above, plus the scalar channel: g_inputTexture1 t5,
   // g_historyInputTexture1 t6, g_outputTexture1 u4, g_historyOutputTexture1 u5.
   inline constexpr std::array<uint32_t, 32> temporal_scalar = {
      0x010D23C7, 0x01D76A2C, 0x0CEFFA2A, 0x151C7F51, 0x178A1AA2, 0x2454C9B2,
      0x31690B05, 0x340CDCA2, 0x382F2026, 0x3A8777DC, 0x498D81C0, 0x527FF43A,
      0x57121565, 0x62BBC3EC, 0x8029F8BF, 0x87993EE0, 0x9E63F529, 0xADB6E489,
      0xB35EC6B6, 0xB83DA05E, 0xB99D5FB8, 0xC099B532, 0xCC55A04E, 0xD5491454,
      0xD6BE801E, 0xE6976B1F, 0xE7F3A952, 0xEA635388, 0xEE499D9E, 0xEE9A2243,
      0xF4DFF646, 0xFC87A5F4};
   static_assert(SortedUnique(temporal_color) && SortedUnique(temporal_scalar));

   inline constexpr bool TemporalScalar(uint64_t hash)
   {
      return Contains(temporal_scalar, hash);
   }
   inline constexpr bool Temporal(uint64_t hash)
   {
      return Contains(temporal_color, hash) || TemporalScalar(hash);
   }

   inline constexpr bool AODepth(uint32_t hash)
   {
      // Full linear R32 depth at t0; matching 16-slice depth/normal outputs.
      // 125 reconstructs normals, 9AC encodes stored normals, AC blends the two.
      // The native producer runs unchanged; XeGTAO decodes its normal array.
      return hash == 0x125D7EFA || hash == 0x9AC5BF02 || hash == 0xAC217033;
   }
   // The game's raw HBAO passes (one per permutation), which XeGTAO replaces.
   inline constexpr bool RawAO(uint32_t hash)
   {
      switch (hash)
      {
      case 0x02F095E5:
      case 0x2F1F35C2:
      case 0x3FA6FF6F:
      case 0x51AAC490:
      case 0x6411646D:
      case 0x7135CDEA:
      case 0x95F7D83D:
      case 0xB84F0F99:
      case 0xC1D7BCCC:
      case 0xE1B560A6:
      case 0xF1CBCC73:
      case 0xFAC758D3:
         return true;
      }
      return false;
   }
   // Final display passes: SDR / game-HDR pairs per post-process quality.
   // Point (equal sizes): 09927C51 / 3F23BFCA. Cubic upscale: 1A9A9123 /
   // DAB9FEA5. Low post-process quality, one linear tap: 20FE932C / 188B1E17
   // (identical to the point pair apart from sampler s1).
   inline constexpr bool LinearDisplay(uint64_t hash)
   {
      return hash == 0x20FE932C || hash == 0x188B1E17;
   }
   inline constexpr bool CubicDisplay(uint64_t hash)
   {
      return hash == 0x1A9A9123 || hash == 0xDAB9FEA5;
   }
   inline constexpr bool HDRDisplay(uint64_t hash)
   {
      return hash == 0x3F23BFCA || hash == 0xDAB9FEA5 || hash == 0x188B1E17;
   }
   inline constexpr bool Display(uint64_t hash)
   {
      // Packed scene t0, native UI t1, PQ decode t2, display CB0[10].
      return hash == 0x1A9A9123 || hash == 0xDAB9FEA5 || hash == 0x09927C51 || hash == 0x3F23BFCA || LinearDisplay(hash);
   }
} // namespace Battlefront2ShaderVariants

namespace Battlefront2SSR
{
   // The SSR trace (1), resolve (2) and temporal (3) compute passes, velocity
   // history variants included.
   inline constexpr unsigned Pass(uint32_t hash)
   {
      if (hash == 0x10F0590C || hash == 0xE7C0FA9C) return 1;
      if (hash == 0x75C70DC8 || hash == 0xF893B2F1 || hash == 0xF5B2185A || hash == 0xED56C080) return 2;
      if (hash == 0xAA13AF5A || hash == 0xE66B4684 || hash == 0xC1FCFFB3 || hash == 0x6AB18BE4) return 3;
      return 0;
   }
   inline constexpr bool Lighting(uint64_t hash)
   {
      // Same G-buffer/depth decode and SSR inputs t11/t12; s2 is used only for
      // those two, at mip zero.
      return hash == 0x6EBE06CD || hash == 0x7603E42B || hash == 0xE12C9864;
   }
} // namespace Battlefront2SSR

namespace Battlefront2SceneLineage
{
   // Frostbite's DOF compositing permutations (layers, gather, bokeh and
   // distortion; not the reduced-resolution gather/filter passes): scene
   // g_RGBTexture at t0, g_InfocusMultiplier in cb0, one RGB output.
   inline constexpr std::array<uint32_t, 36> dof_compositors = {
      0x00670380, 0x113D1C87, 0x1775EB92, 0x1953A18B, 0x24DFBCB4, 0x2F2E3DFE,
      0x357B259D, 0x3A5B3FCC, 0x431CDB02, 0x4831636F, 0x5EB51697, 0x5F4F606C,
      0x6F2D7A75, 0x771BBE88, 0x7CE3D155, 0x92130014, 0x9267D341, 0x9549CC61,
      0x96D4B425, 0x97A9927D, 0x98990B1A, 0x9D7D83E4, 0xA25D2014, 0xAF0D2726,
      0xB2A1A668, 0xB49FACA5, 0xBE246271, 0xCE5A1BFE, 0xD5EAC2C5, 0xD9433742,
      0xEA66CA99, 0xEB58CEBD, 0xEBC68F52, 0xEFE8A8FE, 0xF9823524, 0xF9E18709};
   // The ones above that blend by saturate(max(blur amount, 0) - 0.5), the far blur only; the others blend by
   // saturate(|blur amount| - 0.5).
   inline constexpr std::array<uint32_t, 12> far_dof_compositors = {0x6F2D7A75, 0x9267D341, 0x9549CC61, 0x96D4B425, 0x97A9927D,
      0xB2A1A668, 0xB49FACA5, 0xD9433742, 0xEA66CA99, 0xEBC68F52, 0xEFE8A8FE, 0xF9E18709};
   // Full-scene motion/radial blur permutations: scene mainTexture at t1 (t0
   // is depth), one RGB target, no discard. The pass also runs, at zero blur
   // intensity, when the game's separate MotionBlurEnabled option is on.
   inline constexpr std::array<uint32_t, 112> motion_blur_compositors = {
      0x00F0C295, 0x09867BA7, 0x0BEBE97E, 0x0DDC53D0, 0x117CB996, 0x11A442CC,
      0x11A8189C, 0x120890E0, 0x130453EA, 0x15C4C89D, 0x191FF2CF, 0x195E64F4,
      0x1D82647D, 0x20205201, 0x240CD0B0, 0x251009C7, 0x2AE9D23F, 0x305EE059,
      0x31EE2B91, 0x33263AEA, 0x33299982, 0x3778402D, 0x384BC672, 0x3A900F13,
      0x3B7A33FD, 0x3C5C5B82, 0x3D600D9A, 0x3D8974E2, 0x3DB5BA92, 0x3FD5D70E,
      0x400F9AA4, 0x42112FEC, 0x4354D00E, 0x43A34D28, 0x45A0FF69, 0x464C6905,
      0x47CE1BA6, 0x49BC49E1, 0x49DFE105, 0x4BD08DF3, 0x4DA6DE2D, 0x4DD47249,
      0x5522C7B1, 0x55FA5E70, 0x56BC81D8, 0x59B88E15, 0x5EC01F4A, 0x60E19978,
      0x6159340F, 0x6D77B098, 0x6F572D1A, 0x6FC5E6F3, 0x7129DBC8, 0x71FC1C93,
      0x734B260C, 0x75B68F0E, 0x7A5A62C2, 0x7A6F4428, 0x7A9D76FC, 0x7E489CE9,
      0x7F94FE51, 0x80A3E6D7, 0x85179921, 0x86DC40BA, 0x893A2F41, 0x8A98ADD6,
      0x8C028C00, 0x8CAF1A0B, 0x8CE244D4, 0x8D29F7E0, 0x90AA8AB4, 0x91517A8A,
      0x91ACF41B, 0x94412D39, 0xA0454A2E, 0xA223FD92, 0xA394FFB6, 0xA4BA44B0,
      0xA55704A7, 0xA70F04E9, 0xB04929AA, 0xB193B897, 0xB3AC5284, 0xB63C0FAC,
      0xB852CAB4, 0xBAC3E8B0, 0xBC5AA6B6, 0xBE4C73ED, 0xC1F75A87, 0xC43A026F,
      0xC53B1D41, 0xC9EF0746, 0xCA845338, 0xCD73B46C, 0xD57D8918, 0xD6384181,
      0xD94AAFEC, 0xDDB38948, 0xDEB0765B, 0xDEB7F502, 0xE009E28B, 0xE13B2743,
      0xE480FF18, 0xE6F580AD, 0xE72C43B7, 0xE93C5FBB, 0xEDADA2A6, 0xF127C020,
      0xF2D00E45, 0xF3DED3ED, 0xF6B85637, 0xF6E1DF5C};
   static_assert(Battlefront2ShaderVariants::SortedUnique(dof_compositors) && Battlefront2ShaderVariants::SortedUnique(far_dof_compositors) &&
                 Battlefront2ShaderVariants::SortedUnique(motion_blur_compositors));
   constexpr bool BlendsFarBlurOnly(uint64_t hash)
   {
      return Battlefront2ShaderVariants::Contains(far_dof_compositors, hash);
   }
   constexpr bool IsMotionBlurComposite(uint64_t hash)
   {
      return Battlefront2ShaderVariants::Contains(motion_blur_compositors, hash);
   }
   constexpr int CompositeSourceSlot(uint64_t vertex, uint64_t pixel)
   {
      if (vertex == 0xFA088C75 && Battlefront2ShaderVariants::Contains(dof_compositors, pixel)) return 0;
      if (vertex == 0xECE610C8 && IsMotionBlurComposite(pixel)) return 1;
      return -1;
   }

   // Follows the scene back through this frame's compositor edges (in draw
   // order, each a complete overwrite) to the temporal output it came from.
   // Walking back in time, not just by resource, copes with A -> B -> A
   // ping-pong: only the latest write to a resource counts.
   template <class Edges, class Resource, class IsTemporalOutput>
   Resource Resolve(const Edges& edges, Resource scene, uint64_t frame,
      uint64_t generation, IsTemporalOutput is_temporal_output)
   {
      for (std::size_t end = edges.size();;)
      {
         bool found = false;
         std::size_t index = end;
         while (index != 0)
         {
            const auto& edge = edges[--index];
            if (edge.frame != frame || edge.generation != generation || edge.packed_resource != scene) continue;
            found = true;
            break;
         }
         // A temporal output can itself be overwritten by a later compositor,
         // so the check gets the range of edges the write must fall in.
         if (is_temporal_output(scene, end, found ? index + 1 : 0)) return scene;
         if (!found) return Resource{}; // No temporal producer.
         scene = edges[index].temporal_resource;
         end = index;
      }
   }

   // Whether every scene packer of the frame is supported. An unknown packer
   // still proves the route to the back buffer, but DLSS and the private
   // replays need supported ones. Only the current or previous frame of the
   // same generation and swapchain counts.
   struct PackSupport
   {
      uint64_t frame = ~uint64_t(0), generation = 0, swapchain = 0;
      bool supported = false;
      void Confirm(uint64_t at, uint64_t gen, uint64_t layout, bool known)
      {
         if (frame != at || generation != gen || swapchain != layout)
            *this = {at, gen, layout, known};
         else if (!known)
            supported = false;
      }
      bool Recent(uint64_t at, uint64_t gen, uint64_t layout) const
      {
         return frame != ~uint64_t(0) && at >= frame && at - frame <= 1 && generation == gen && swapchain == layout;
      }
      bool AllowsEvaluation(uint64_t at, uint64_t gen, uint64_t layout) const
      {
         return supported && Recent(at, gen, layout);
      }
   };
} // namespace Battlefront2SceneLineage

namespace Battlefront2RenderState
{
   struct PixelConstantBufferBinding
   {
      Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1;
      UINT slot = 0, first_constant = 0;
      UINT num_constants = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
      bool Capture(ID3D11DeviceContext* context, UINT binding_slot = 0)
      {
         slot = binding_slot;
         buffer.Reset();
         context1.Reset();
         first_constant = 0;
         num_constants = D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
         if (!context) return false;
         if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1))))
            context1->PSGetConstantBuffers1(slot, 1, &buffer, &first_constant, &num_constants);
         else
            context->PSGetConstantBuffers(slot, 1, &buffer);
         return buffer != nullptr;
      }
      void Restore(ID3D11DeviceContext* context) const
      {
         ID3D11Buffer* original = buffer.Get();
         if (context1)
            context1->PSSetConstantBuffers1(slot, 1, &original, &first_constant, &num_constants);
         else
            context->PSSetConstantBuffers(slot, 1, &original);
      }
   };

   // The final Psycho draw changes only PS, t0, b0 and b11. Preserve their
   // exact ranges/linkage without allocating a FullGraphics state stack.
   struct PixelBindings
   {
      Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source;
      PixelConstantBufferBinding constants, psycho_constants;
      ID3D11ClassInstance* instances[256]{};
      UINT instance_count = 256;
      explicit PixelBindings(ID3D11DeviceContext* context)
      {
         context->PSGetShader(&shader, instances, &instance_count);
         context->PSGetShaderResources(0, 1, &source);
         constants.Capture(context, 0);
         psycho_constants.Capture(context, 11);
      }
      PixelBindings(const PixelBindings&) = delete;
      ~PixelBindings()
      {
         for (UINT i = 0; i < instance_count; ++i)
            if (instances[i]) instances[i]->Release();
      }
      void Restore(ID3D11DeviceContext* context) const
      {
         ID3D11ShaderResourceView* original = source.Get();
         context->PSSetShaderResources(0, 1, &original);
         constants.Restore(context);
         psycho_constants.Restore(context);
         context->PSSetShader(shader.Get(), instances, instance_count);
      }
   };
} // namespace Battlefront2RenderState
