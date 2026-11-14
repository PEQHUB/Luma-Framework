// XeGTAO / VB-GTAO for Battlefront II, on Luma's shared implementation ("Includes/XeGTAO.hlsl").
//
// The game computes HBAO on 4x4 deinterleaved depth and normal arrays. The add-on replaces its raw AO dispatch
// (Battlefront2XeGTAO::State::Replace) with the three shared passes and writes the result into the game's own
// R8_UNORM AO target, so the game's two AO blur passes and its lighting run unchanged on it.
// - Depth (tex0 of the prefilter): a copy of the full resolution R32 linear view depth that the game's AO deinterleave
//   pass reads, taken at that pass. It is already view space Z, so the depth conversion below is the identity.
// - Normals (t1 of the main pass): the game's R8G8_SNORM octahedral view space normal array, as the raw AO dispatch binds it.
// - Camera and radius: the raw AO dispatch's own constant buffer (b0), read in place.
// Entry points (see main.cpp): "prefilter_depths16x16_cs", "main_pass_cs" and "denoise_pass_cs" (XE_GTAO_FINAL_APPLY=1).
// Permutations: XE_GTAO_QUALITY (slice count) and VISIBILITY_BITMASK (VB-GTAO).

// The game's raw AO constant buffer (80 bytes), as its raw AO shaders read it.
cbuffer NativeAO : register(b0)
{
   float2 NativeNDCScale;  // NDC to view space multiply pair
   float2 NativeNDCOffset; // NDC to view space add pair
   float2 NativeInvSliceSize;
   float2 NativeSliceSize;
   float NativeFromHalf;
   float NativeRadiusFactor;  // The AO radius in pixels at a view depth of 1
   float NativeMaxPixels;
   float NativeRadiusSquared; // The AO radius in view space units, squared
   float NativeSinBias;
   float NativeTanBias;
   float NativeAttenuation;
   float NativeContrast;
   uint NativeRandomOffset;
   float NativeSteps;
   uint NativeDirections;
   uint NativePadding;
}

// Set by the add-on for each replacement.
cbuffer LumaGTAO : register(b11)
{
   float RadiusScaleRT; // Scales the game's own AO radius ("BattlefrontIIXeGTAORadiusScale", 0.5 to 2)
   float3 LumaGTAOPadding;
}

#define XE_GTAO_PIXEL_SIZE_FROM_SOURCE 1 // tex0 is full resolution in the main and denoise passes
#define NDC_TO_VIEW_MUL                NativeNDCScale
#define NDC_TO_VIEW_ADD                NativeNDCOffset

// The game's own AO radius, so the radius follows its per level settings.
#define RadiusOverrideRT  (sqrt(max(NativeRadiusSquared, 0.0)) * RadiusScaleRT)
// Intel's default for XeGTAO, tuned together with its other defaults (all unchanged here) against ray traced ground truth.
// VB-GTAO's reference implementation applies no power. Its object thickness (view space units, meters in this game) and
// steps per slice side were fitted the same way: against cosine weighted ray traced AO (true thickness, bounded at the
// game's AO radius) on synthetic scenes with the game's camera. The shared defaults (0.2, 3) left most of the occlusion out.
#if VISIBILITY_BITMASK
#define FinalValuePowerRT 1.0
#define THICKNESS         0.75
#define STEPS_PER_SLICE   8.0
#else
#define FinalValuePowerRT 2.2
#endif
#define DebugViewRT 0
// Stable spatial noise: the game's AO blur passes run after this.
#define NoiseIndexRT 0

// No AO for invalid depth (sky) or when the game's AO radius is zero. Small projected radii fade out inside XeGTAO.
#define XE_GTAO_ADJUST_VISIBILITY(visibility, viewspaceZ)          \
   if (!(viewspaceZ > 0.0) || !(NativeRadiusSquared > 0.0)) \
      visibility = 1.0;

#define XE_GTAO_FINAL_OUTPUT_TYPE unorm float // The game's R8_UNORM AO target
#define XE_GTAO_ENCODE_FINAL(v)   (v)

float XeGTAO_ScreenSpaceToViewSpaceDepth(const float screenDepth)
{
   return screenDepth; // Already linear view space depth
}

Texture2DArray<float2> NativeNormals : register(t1);

float3 XeGTAO_LoadViewspaceNormal(uint2 pixCoord)
{
   // 4x4 deinterleaved: pixel (x, y) is in slice (y % 4) * 4 + (x % 4), at (x / 4, y / 4).
   const float2 oct = NativeNormals.Load(int4(pixCoord >> 2, (pixCoord.y & 3u) * 4u + (pixCoord.x & 3u), 0));
   // Octahedral decode
   float3 normal = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
   if (normal.z < 0.0)
   {
      normal.xy = (1.0 - abs(oct.yx)) * float2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
   }
   return normalize(normal);
}

#include "../Includes/XeGTAO.hlsl"
