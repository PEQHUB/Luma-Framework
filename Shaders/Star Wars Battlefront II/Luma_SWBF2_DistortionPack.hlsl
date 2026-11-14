// Private DLSS counterpart of Frostbite PS 0x848FD892. The original game
// draw still runs unchanged. Preserve its distortion, color scale, vignette,
// PQ-to-LUT coordinates and luminance alpha; only scene blur LOD is rescaled.
#include "Includes/SceneFeatures.hlsl"
Texture2D<float3> Scene : register(t0);
Texture3D<float3> ColorGrading : register(t1);
Texture2D<float3> Distortion : register(t2);
Texture2D<float3> Bloom : register(t3);
#if BF2_PACK_FILM_GRAIN
Texture2D<float4> FilmGrain : register(t4);
Texture2D<float4> IronsightsDof : register(t5);
SamplerState FilmGrainSampler : register(s4);
SamplerState IronsightsSampler : register(s5);
#else
Texture2D<float4> IronsightsDof : register(t4);
SamplerState IronsightsSampler : register(s4);
#endif
Texture2D<float3> NativeScene : register(t7); // Dimensions only.
SamplerState SceneSampler : register(s0);
SamplerState ColorGradingSampler : register(s1);
SamplerState DistortionSampler : register(s2);
SamplerState BloomSampler : register(s3);
cbuffer FrostbiteGlobals : register(b0)
{
   float4 NativeConstants[18];
};

float4 main(float4 position : SV_Position, float4 unused : TEXCOORD0,
   float2 uv : TEXCOORD1) : SV_Target
{
   float2 lensUV=uv;
#if BF2_PACK_LENS
   lensUV=BF2LensUV(uv,NativeConstants[4],NativeConstants[5].xy);
#endif
   float3 distortion = Distortion.Sample(DistortionSampler, lensUV);
   // Keep native MAD-then-ADD ordering. Reassociation can shift neutral UVs
   // across a texture-filter precision boundary on high-contrast detail.
   precise float2 displacement = mad(distortion.xy, NativeConstants[17].xy, NativeConstants[17].zw);
   precise float2 sceneUV = lensUV + displacement;

   uint outputWidth, outputHeight, nativeWidth, nativeHeight;
   Scene.GetDimensions(outputWidth, outputHeight);
   NativeScene.GetDimensions(nativeWidth, nativeHeight);
   float resolutionScale = max(float(outputWidth) / max(nativeWidth, 1u),
      float(outputHeight) / max(nativeHeight, 1u));
   // Native LOD = log2(1 + distortion.z^2 * cb0[8].w). Scale the additional
   // blur footprint, retaining LOD 0 when no blur is requested. Adding a
   // constant log2(scale) would blur even unaffected pixels at lower presets.
   float blur = distortion.z * distortion.z * NativeConstants[8].w;
   float sceneLOD = log2(1.0f + max(blur, 0.0f) * resolutionScale);
#if BF2_PACK_CHROMATIC
   precise float4 chromaticUV = mad((sceneUV.xyxy - 0.5f) * 2.0f, NativeConstants[16], 0.5f);
   float3 color = float3(Scene.SampleLevel(SceneSampler, chromaticUV.xy, sceneLOD).r,
      Scene.SampleLevel(SceneSampler, chromaticUV.zw, sceneLOD).g,
      Scene.SampleLevel(SceneSampler, sceneUV, sceneLOD).b);
#else
   float3 color = Scene.SampleLevel(SceneSampler, sceneUV, sceneLOD);
#endif
#if BF2_PACK_IRONSIGHTS
   float4 dof = IronsightsDof.Sample(IronsightsSampler, uv);
   precise float3 sharp = color * (1.0f - dof.a * NativeConstants[3].x);
   color = mad(dof.rgb * dof.rgb * NativeConstants[3].y, dof.a * NativeConstants[3].x, sharp);
#endif
   color += NativeConstants[8].xyz * Bloom.Sample(BloomSampler, sceneUV);

   color = color * NativeConstants[6].xyz - NativeConstants[15].xyz;
   float2 vignetteUV = (uv - 0.5f) * NativeConstants[14].xy;
   float vignette = pow(saturate(1.0f - dot(vignetteUV, vignetteUV) * NativeConstants[15].w),
      NativeConstants[14].z);
   color = color * vignette + NativeConstants[15].xyz;

   // Exact ST2084 constants and 33^3 LUT texel-center mapping from 848FD892.
   float3 pq = exp2(log2(color * 0.01f) * 0.1593017578125f);
   pq = (pq * 18.8515625f + 0.8359375f) / (pq * 18.6875f + 1.0f);
   pq = exp2(log2(pq) * 78.84375f);
   color = ColorGrading.Sample(ColorGradingSampler, pq * (32.0f / 33.0f) + (0.5f / 33.0f));
#if BF2_PACK_FILM_GRAIN
   // One grain texel per output pixel, as at native resolution (the game tiles one per render pixel).
   float2 grainScale=float2(outputWidth,outputHeight)/max(float2(nativeWidth,nativeHeight),1.0f);
   float grain=FilmGrain.Sample(FilmGrainSampler,uv*NativeConstants[2].xy*grainScale+NativeConstants[2].zw).x-0.5f;
   color+=grain*NativeConstants[1].xyz;
#endif
   return float4(color, dot(color, float3(0.212671f, 0.715160f, 0.072169f)));
}
