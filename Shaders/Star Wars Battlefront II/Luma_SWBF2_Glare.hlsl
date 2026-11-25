// Eye glare: the light a bright source scatters inside the eye, for the part of the scene the display cannot show.
// The viewer's eye scatters what the screen shows; the rest of the light brighter than the display's peak is added
// here as the veil it would have cast. The veil is the CIE total glare function (CIE 135/1-1999, Vos & van den Berg)
// as a sum of blurs that double in width, one per level of a pyramid at half resolution; the mod computes each
// level's share of the light (Battlefront2Glare in tone_mapping.h).
//
// The veil joins the scene light the grade takes in, after the game's exposure, like bloom (the scene shader,
// Luma_SWBF2_PsychoScene.hlsl). Pass 3 finds the scene levels the grade maps to the display's peak and to the limit,
// the brightest light a pixel may count for as the display would show it; pass 0 takes the light between them.
#include "Includes/PsychoSettings.hlsl"

#if BF2_GLARE_PASS == 0
Texture2D<float3> Scene : register(t0); // The scene the grade takes in, before the game's exposure.
Texture2D<float> Cutoff : register(t3);
cbuffer FrostbiteGlobals : register(b1) { float4 N[18]; }; // The scene pass's own constants: N[6] is the exposure.
#elif BF2_GLARE_PASS == 3
Texture3D<float4> Grade : register(t2); // The grade the scene pass applies: PQ in, PQ out.
#else
Texture2D<float3> Source : register(t0);
#endif
Texture2D<float3> Coarser : register(t1);
#if BF2_GLARE_PASS == 3
RWTexture2D<float> CutoffTarget : register(u0);
#else
RWTexture2D<float4> Target : register(u0);
#endif
SamplerState LinearSampler : register(s0);
// Pass 0: zw the crop of the scene (pixels). Pass 3: x the peak, y 1 when there is a grade, z the limit. Passes 1 and
// 2: the level's share of the light, and the weight of the coarser veil.
cbuffer GlareControls : register(b0) { float4 Glare; };

#if BF2_GLARE_PASS == 3
groupshared float GreyAxis[64];
groupshared float2 Range;

// The luminance the grade gives a grey of this scene level.
float GradedGrey(float level)
{
   if (Glare.y == 0.0f) return level;
   float3 pq = BF2PQEncode(float3(level, level, level));
   return dot(BF2PQDecode(Grade.SampleLevel(LinearSampler, pq * (32.0f / 33.0f) + 0.5f / 33.0f, 0).rgb), float3(0.2126f, 0.7152f, 0.0722f));
}

// The first of the 64 guesses at or above the target (64 when none is).
uint FirstAt(float target)
{
   uint k = 0;
   [loop] while (k < 64 && GreyAxis[k] < target)
      ++k;
   return k;
}

// One group per target: group 0 finds the scene level the grade maps to the display's peak (Glare.x), group 1 the
// level it maps to the limit (Glare.z). Both are 1e30 when the grade never gets there.
[numthreads(64, 1, 1)]
void main(uint gi : SV_GroupIndex, uint3 group : SV_GroupID)
{
   float target = group.x == 0 ? Glare.x : Glare.z;
   if (gi == 0) Range = float2(-8.0f, 12.0f); // log2 of the scene level
   GroupMemoryBarrierWithGroupSync();
   // Two rounds of 64 guesses along the grey axis, the second inside the bracket of the first.
   [unroll] for (uint iteration = 0; iteration < 2; ++iteration)
   {
      GreyAxis[gi] = GradedGrey(exp2(min(lerp(Range.x, Range.y, gi / 63.0f), 100.0f)));
      GroupMemoryBarrierWithGroupSync();
      if (gi == 0)
      {
         uint k = FirstAt(target);
         float2 range = Range;
         float step = (range.y - range.x) / 63.0f;
         if (k == 64)
            Range = float2(1e30f, 1e30f); // The grade never reaches the target.
         else if (k == 0)
            Range = float2(range.x, range.x);
         else if (iteration == 0)
            Range = float2(range.x + step * (k - 1), range.x + step * k);
         else
         {
            float a = GreyAxis[k - 1], b = GreyAxis[k];
            Range = float2(range.x + step * (k - 1 + saturate((target - a) / max(b - a, 1e-12f))), 0.0f);
         }
      }
      GroupMemoryBarrierWithGroupSync();
   }
   if (gi == 0) CutoffTarget[uint2(group.x, 0)] = Range.x >= 1e29f ? 1e30f : exp2(Range.x);
}
#else
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
#if BF2_GLARE_PASS == 0
   // The light above the cutoff, a pixel counting for no more than the scene level the grade maps to the limit,
   // averaged over 2x2 pixels.
   float cutoff = Cutoff.Load(int3(0, 0, 0));
   float cap = Cutoff.Load(int3(1, 0, 0));
   uint2 crop = uint2(Glare.zw);
   float3 excess = 0.0f;
   [unroll] for (uint i = 0; i < 4; ++i)
   {
      float3 light = max(Scene.Load(int3(id.xy * 2 + uint2(i & 1, i >> 1) + crop, 0)), 0.0f) * N[6].xyz;
      if (!all(isfinite(light))) light = 0.0f;
      float luminance = max(dot(light, float3(0.2126f, 0.7152f, 0.0722f)), 1e-6f);
      excess += light * (max(min(luminance, cap) - cutoff, 0.0f) / luminance);
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
#endif
