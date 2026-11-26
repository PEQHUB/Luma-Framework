// CORTAO for Battlefront II, on the reference implementation in Includes/CORTAO (v1.0.3, Daniel Isheden, MIT,
// https://github.com/theagentd/CORTAO), unmodified.
//
// Like XeGTAO (Luma_SWBF2_XeGTAO.hlsl), the add-on replaces the game's raw AO dispatch with these passes and writes the
// result into the game's own R8_UNORM AO target, so the game's two AO blur passes and its lighting run unchanged on it.
// - Depth (t0 of the prepass): the copy of the full resolution R32 linear view depth taken at the game's AO deinterleave pass.
// - Normals (t1 of the prepass and the main pass): the game's 4x4 deinterleaved R8G8_SNORM octahedral view space normals.
// - Camera and radius: the raw AO dispatch's own constant buffer (b0), read in place. The radius is the game's own.
// CORTAO_PASS: 0 prepass, 1 main pass (CORTAO_SAMPLE_COUNT samples), 2 filter pass. Entry point "main".
//
// CORTAO's view space looks down -Z with +Z towards the camera. The game's (and XeGTAO's) looks down +Z, with the same X
// and Y, so its positions and normals enter with Z negated.
//
// Shader Model 5: the reference samples and gathers its integer textures (SM 6.7). Here they are read with Load at the
// texels point sampling picks (nearest mip, clamp to edge for the lookup table, a black border for the rest).

#define CORTAO_HLSL 1
#define CORTAO_PERSPECTIVE_PROJECTION 1
#define CORTAO_RAY_COUNT 32
#define CORTAO_OCCLUDER_LOOKUP_TABLE 1
// The reference's filter example: 13 taps with symmetric weights.
#define CORTAO_LARGE_FILTER 1
#define CORTAO_FILTER_SYMMETRY 1

// The game's raw AO constant buffer (80 bytes), as its raw AO shaders read it.
cbuffer NativeAO : register(b0)
{
   float2 NativeNDCScale;  // Texture coordinates to view space multiply pair (per unit of view depth)
   float2 NativeNDCOffset; // Texture coordinates to view space add pair
   float2 NativeInvSliceSize;
   float2 NativeSliceSize;
   float NativeFromHalf;
   float NativeRadiusFactor;
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
cbuffer LumaCORTAO : register(b11)
{
   uint2 Resolution;  // The AO resolution
   float RadiusScale; // Scales the game's own AO radius ("BattlefrontIIXeGTAORadiusScale", 0.5 to 2)
   uint MipCount;     // Mips of the packed data buffer (1 to 8)
   // The reference's noise seed: a new random value each frame lets the temporal filter (DLSS or the game's TAA)
   // average CORTAO's noise; a constant one keeps the noise still.
   uint RandomSeed;
   uint3 LumaCORTAOPadding;
}

// The reference's recommended defaults (CORTAOParameters.h).
static const float MaxScreenspaceRadiusHeightRatio = 0.2;
static const float MipBias = -4.0;
static const float MaxSlopeCompensation = 4.0;
static const float BackfaceThreshold = 0.5;
static const float BackfaceBoost = 0.0;
static const float FilterDepthSensitivity = 512.0;
static const float FilterNormalSensitivity = 128.0;

static const uint2 OccluderTableSize = uint2(24, 16); // 24x24 directions, 16 radii, 32 rays (R32_UINT)

uint PackedResolution()
{
   return Resolution.x | (Resolution.y << 16u);
}

Texture2DArray<float2> NativeNormals : register(t1);

// The game's normal for a pixel, in CORTAO's view space.
float3 LoadNativeNormal(int2 pixelCoords)
{
   // 4x4 deinterleaved: pixel (x, y) is in slice (y % 4) * 4 + (x % 4), at (x / 4, y / 4).
   const uint2 p = uint2(pixelCoords);
   const float2 oct = NativeNormals.Load(int4(p >> 2u, (p.y & 3u) * 4u + (p.x & 3u), 0));
   // Octahedral decode
   float3 normal = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
   if (normal.z < 0.0)
   {
      normal.xy = (1.0 - abs(oct.yx)) * float2(oct.x >= 0.0 ? 1.0 : -1.0, oct.y >= 0.0 ? 1.0 : -1.0);
   }
   normal = normalize(normal);
   return float3(normal.xy, -normal.z);
}

#if CORTAO_PASS == 0

#include "Includes/CORTAO/CORTAOPrepass.h"

Texture2D<float> LinearDepth : register(t0);
RWTexture2D<uint> PackedData[8] : register(u0);

// The prepass linearizes 1 / (depth * depthScale + depthOffset). With a scale of 1 and an offset of 0, the game's linear
// depth enters as its inverse, and 0 (CORTAO's sky) marks pixels without a valid depth.
float cortao_loadDepth(int2 pixelCoords)
{
   const float z = LinearDepth.Load(int3(pixelCoords, 0));
   return (z > 0.0 && z < 3.0e38) ? 1.0 / z : 0.0;
}

float3 cortao_loadNormal(int2 pixelCoords)
{
   return LoadNativeNormal(pixelCoords);
}

void cortao_writePackedData(int2 pixelCoords, int mipLevel, uint data)
{
   PackedData[mipLevel][pixelCoords] = data;
}

[numthreads(8, 8, 1)]
void main(uint2 pixelCoords : SV_DispatchThreadID)
{
   CORTAOPrepassParameters params;
   params.packedResolution = PackedResolution();
   params.depthScale = 1.0;
   params.depthOffset = 0.0;
   params.randomSeed = RandomSeed;
   params.mipCount = MipCount;
   params._padding0 = 0u;
   params._padding1 = 0u;
   params._padding2 = 0u;
   cortao_prepass(pixelCoords, params);
}

#elif CORTAO_PASS == 1

#include "Includes/CORTAO/CORTAOMainPass.h"

Texture2D<uint> PackedData : register(t0);
Texture3D<uint> OccluderLookupTable : register(t2);
RWTexture2D<unorm float> Occlusion : register(u0);

cortao_half3 cortao_loadNormal(cortao_int2 pixelCoords)
{
   return LoadNativeNormal(pixelCoords);
}

// Nearest mip, nearest texel, 0 outside the texture.
cortao_uint cortao_samplePackedData(cortao_float2 texCoords, cortao_float mipmap)
{
   const uint level = uint(clamp(floor(mipmap + 0.5), 0.0, float(MipCount - 1u)));
   const uint2 size = max(Resolution >> level, 1u);
   const float2 texel = floor(texCoords * float2(size));
   if (!(all(texel >= 0.0) && all(texel < float2(size)))) return 0u; // Also NaN
   return PackedData.Load(int3(texel, level));
}

// Nearest texel, clamped to the edge.
cortao_uint cortao_sampleOccluderLookupTable(cortao_float3 texCoords)
{
   const int3 size = int3(OccluderTableSize.xx, OccluderTableSize.y);
   const int3 texel = clamp(int3(floor(texCoords * float3(size))), 0, size - 1);
   return OccluderLookupTable.Load(int4(texel, 0));
}

void cortao_writeResult(cortao_int2 pixelCoords, cortao_half occlusion, cortao_half3 bentNormal, cortao_half3x3 tangentSpaceTransform, cortao_uint rayBits)
{
   Occlusion[pixelCoords] = occlusion;
}

// The reference's thread order (v1.0.1): Morton order inside each 16x16 group, for cache coherency.
uint2 Deinterleave(uint index)
{
   uint2 p = uint2(index, index) >> uint2(0, 1);
   p &= 0x5555u;
   p = (p ^ (p >> 1u)) & 0x3333u;
   p = (p ^ (p >> 2u)) & 0x0f0fu;
   p = (p ^ (p >> 4u)) & 0x00ffu;
   return p;
}

[numthreads(16, 16, 1)]
void main(uint2 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
   const uint2 pixelCoords = groupID * 16u + Deinterleave(localIndex);
   if (any(pixelCoords >= Resolution)) return;
   // CORTAO leaves its sky pixels unwritten; the game's AO target needs no occlusion there, and none at all when the
   // game's AO radius is zero.
   if (!(NativeRadiusSquared > 0.0) || PackedData.Load(int3(pixelCoords, 0)) == 0u)
   {
      Occlusion[pixelCoords] = 1.0;
      return;
   }

   // CORTAOParameters.h's populateMainPassParameters, from the game's camera: its texture-to-view pair is CORTAO's
   // unprojection, the projection is its inverse, and the tangents of the half field of view are half its multipliers.
   const float2 tanHalfFOV = 0.5 * abs(NativeNDCScale);
   CORTAOMainPassParameters params;
   params.packedResolution = PackedResolution();
   params.randomSeed = RandomSeed;
   params.normalBias = tanHalfFOV.x / float(Resolution.x) * sqrt(2.0);
   params.radius = sqrt(NativeRadiusSquared) * RadiusScale;
   params.unprojectParams = float4(NativeNDCScale, NativeNDCOffset);
   params.projectionParams = float4(1.0 / NativeNDCScale, NativeNDCOffset / NativeNDCScale);
   const float maxApparentCircleRadius = tanHalfFOV.y * MaxScreenspaceRadiusHeightRatio * 2.0;
   params.maxApparentCircleRadiusCoefficient = maxApparentCircleRadius * rsqrt(maxApparentCircleRadius * maxApparentCircleRadius + 1.0);
   params.mipBias = log2(float(Resolution.y) * 0.5 / tanHalfFOV.y) + MipBias;
   params.inverseMaxSlopeCompensation = 1.0 / MaxSlopeCompensation;
   params.backfaceThreshold = BackfaceThreshold;
   params.backfaceBoost = BackfaceBoost;
   params._padding0 = 0u;
   params._padding1 = 0u;
   params._padding2 = 0u;
   cortao_mainPass(pixelCoords, params);
}

#elif CORTAO_PASS == 2

#include "Includes/CORTAO/CORTAOFilterPass.h"

Texture2D<uint> PackedData : register(t0);
Texture2D<unorm float> RawOcclusion : register(t1);
RWTexture2D<unorm float> Occlusion : register(u0);

// What Gather returns at the corner between texels (base) and (base + 1): x (0, 1), y (1, 1), z (1, 0), w (0, 0).
// Loads outside the texture return 0, like a black border.
int2 GatherBase(float2 texCoords, int2 offset)
{
   return int2(round(texCoords * float2(Resolution))) - 1 + offset;
}

cortao_uint4 cortao_gatherPackedData(cortao_float2 texCoords, cortao_int2 offset)
{
   const int2 b = GatherBase(texCoords, offset);
   return uint4(PackedData.Load(int3(b + int2(0, 1), 0)), PackedData.Load(int3(b + int2(1, 1), 0)),
      PackedData.Load(int3(b + int2(1, 0), 0)), PackedData.Load(int3(b, 0)));
}

cortao_half4 cortao_gatherSSAO(cortao_float2 texCoords, cortao_int2 offset)
{
   const int2 b = GatherBase(texCoords, offset);
   return float4(RawOcclusion.Load(int3(b + int2(0, 1), 0)), RawOcclusion.Load(int3(b + int2(1, 1), 0)),
      RawOcclusion.Load(int3(b + int2(1, 0), 0)), RawOcclusion.Load(int3(b, 0)));
}

void cortao_writeFilterOutput(cortao_int2 pixelCoords, cortao_half ssao)
{
   Occlusion[pixelCoords] = ssao;
}

// Each thread filters a 2x2 block; 8x8 groups cover 16x16 pixels.
[numthreads(8, 8, 1)]
void main(uint2 threadCoords : SV_DispatchThreadID)
{
   // The filter only writes pixels with a depth: give the others of this block no occlusion.
   const uint2 pixelCoords = (threadCoords << 1u) | 1u;
   [unroll] for (uint i = 0u; i < 4u; i++)
   {
      const uint2 p = pixelCoords - uint2(i & 1u, i >> 1u);
      if (all(p < Resolution) && PackedData.Load(int3(p, 0)) == 0u) Occlusion[p] = 1.0;
   }
   CORTAOFilterPassParameters params;
   params.packedResolution = PackedResolution();
   params.depthSensitivity = FilterDepthSensitivity;
   params.normalSensitivity = FilterNormalSensitivity;
   params._padding0 = 0u;
   cortao_filterPass(threadCoords, params);
}

#endif
