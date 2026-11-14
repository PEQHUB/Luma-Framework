// Eye glare: the light a bright source scatters inside the eye, for the part of the scene the display cannot show.
// The viewer's eye scatters what the screen shows; the rest of a source brighter than the display's peak is added
// here as the veil it would have cast, before the tone mapper. The veil is the CIE total glare function
// (CIE 135/1-1999, Vos & van den Berg) as a sum of blurs that double in width, one per level of a pyramid at half
// resolution; the add-on computes each level's share of the light (Battlefront2Glare in tone_mapping.h).
#include "Includes/PsychoSettings.hlsl"

Texture2D<float3> Source : register(t0);
Texture2D<float3> Coarser : register(t1);
RWTexture2D<float4> Target : register(u0);
SamplerState LinearSampler : register(s0);
cbuffer GlareControls : register(b0) { float4 Glare; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
#if BF2_GLARE_PASS == 0
   // The scene's graded light above the display's peak (Glare.x, as PQ in Glare.y), with its colour, averaged
   // over 2x2 pixels.
   float3 excess = 0.0f;
   [unroll] for (uint i = 0; i < 4; ++i)
   {
      float3 pq = Source.Load(int3(id.xy * 2 + uint2(i & 1, i >> 1), 0));
      [branch] if (max(pq.r, max(pq.g, pq.b)) > Glare.y)
      {
         float3 light = BF2PQDecode(min(pq, 1.0f));
         excess += light * max(1.0f - Glare.x / dot(light, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
      }
   }
   Target[id.xy] = float4(excess * 0.25f, 0.0f);
#elif BF2_GLARE_PASS == 1
   // Half the size through [1 5 10 10 5 1] / 32 each way: three bilinear taps a row.
   float2 size;
   Source.GetDimensions(size.x, size.y);
   static const float3 offsets = float3(-5.0f / 3.0f, 0.0f, 5.0f / 3.0f), weights = float3(0.1875f, 0.625f, 0.1875f);
   float3 sum = 0.0f;
   [unroll] for (uint y = 0; y < 3; ++y)
      [unroll] for (uint x = 0; x < 3; ++x)
         sum += weights[x] * weights[y] * Source.SampleLevel(LinearSampler, (id.xy * 2 + 1 + float2(offsets[x], offsets[y])) / size, 0);
   Target[id.xy] = float4(sum, 0.0f);
#else
   // This level's share of the light (Glare.x) plus the veil of the coarser levels (times Glare.y) through a
   // 3x3 tent.
   float2 size;
   Coarser.GetDimensions(size.x, size.y);
   float4 uv = ((id.xy + 0.5f) * 0.5f).xyxy + float4(-0.5f, -0.5f, 0.5f, 0.5f);
   float3 veil = Coarser.SampleLevel(LinearSampler, uv.xy / size, 0) + Coarser.SampleLevel(LinearSampler, uv.zy / size, 0)
      + Coarser.SampleLevel(LinearSampler, uv.xw / size, 0) + Coarser.SampleLevel(LinearSampler, uv.zw / size, 0);
   Target[id.xy] = float4(Glare.x * Source.Load(int3(id.xy, 0)) + Glare.y * 0.25f * veil, 0.0f);
#endif
}
