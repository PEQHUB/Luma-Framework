#include "Includes/PsychoSettings.hlsl"
#include "Includes/NativeLUT.hlsl"
#include "Includes/SceneFeatures.hlsl"
#ifndef BF2_NATIVE_POST
#define BF2_NATIVE_POST 0
#endif

Texture2D<float3> Scene : register(t0);
// Frostbite's native LUT includes grading and tonal compression. Its strength
// is independent of scene exposure; display mapping remains the final pass.
Texture3D<float3> NativeGrade : register(t1);
Texture2D<float3> Auxiliary : register(t2); // Distortion or bloom, by variant.
Texture2D<float4> Auxiliary3 : register(t3); // Bloom, film grain or ironsights DOF.
#if BF2_PACK_DISTORTION && BF2_PACK_FILM_GRAIN
Texture2D<float4> FilmGrain : register(t4);
Texture2D<float4> IronsightsDof : register(t5);
SamplerState FilmGrainSampler : register(s4);
SamplerState IronsightsSampler : register(s5);
#else
Texture2D<float4> IronsightsDof : register(t4); // Distortion + ironsights variant.
SamplerState IronsightsSampler : register(s4);
#endif
Texture2D<float3> NativeScene : register(t7); // Free of all native input slots.
Texture3D<float4> SeparatedGrade : register(t8);
#if !BF2_NATIVE_POST
// Blaster bolts and saber blades (`laser_shaders` in dlss.h), drawn once more on their own
// (Battlefront2PostDLSS::State::Replay); unbound without them.
Texture2D<float3> LightLayer : register(t9);
#endif
#if !BF2_NATIVE_POST
// The eye glare's veil (Luma_SWBF2_Glare.hlsl), half the size; unbound without glare.
Texture2D<float3> GlareVeil : register(t10);
SamplerState GlareSampler : register(s6);
#endif
SamplerState SceneSampler : register(s0);
SamplerState GradeSampler : register(s1);
SamplerState AuxiliarySampler : register(s2);
SamplerState Auxiliary3Sampler : register(s3);
cbuffer FrostbiteGlobals : register(b0) { float4 N[18]; };

float4 main(float4 position : SV_Position, float4 unused : TEXCOORD0, float2 uv : TEXCOORD1) : SV_Target
{
   float3 color;
   float2 lensUV=uv;
#if BF2_PACK_LENS
   lensUV=BF2LensUV(uv,N[4],N[5].xy);
#endif
#if BF2_PACK_DISTORTION
   float3 distortion=Auxiliary.Sample(AuxiliarySampler,lensUV);
   precise float2 offset=mad(distortion.xy,N[17].xy,N[17].zw);
   // Scale decoded screen displacement, retaining its neutral encoded value.
   offset*=BF2_DISTORTION_STRENGTH;
   precise float2 sceneUV=lensUV+offset;
   uint w,h,nw,nh; Scene.GetDimensions(w,h); NativeScene.GetDimensions(nw,nh);
   float scale=max(float(w)/max(nw,1u),float(h)/max(nh,1u));
   float lod=log2(1.0f+max(distortion.z*distortion.z*N[8].w,0.0f)*scale);
#if BF2_PACK_CHROMATIC
   // Native C3825BD6 shifts red and green about the screen center; blue
   // keeps the distorted scene UV. Preserve those authored channel scales.
   precise float4 chromaticUV=mad((sceneUV.xyxy-0.5f)*2.0f,N[16],0.5f);
   color=float3(Scene.SampleLevel(SceneSampler,chromaticUV.xy,lod).r,
      Scene.SampleLevel(SceneSampler,chromaticUV.zw,lod).g,
      Scene.SampleLevel(SceneSampler,sceneUV,lod).b);
#else
   color=Scene.SampleLevel(SceneSampler,sceneUV,lod);
#endif
#else
#if BF2_PACK_CHROMATIC
   precise float4 chromaticUV=mad((lensUV.xyxy-0.5f)*2.0f,N[16],0.5f);
   color=float3(Scene.Sample(SceneSampler,chromaticUV.xy).r,
      Scene.Sample(SceneSampler,chromaticUV.zw).g,Scene.Sample(SceneSampler,lensUV).b);
#elif BF2_PACK_LENS
   color=Scene.Sample(SceneSampler,lensUV);
#else
   color=Scene.SampleLevel(SceneSampler,uv,0);
#endif
#endif
#if !BF2_NATIVE_POST
   // The share of the pixel's light from the light layer, for the display's gamut expansion.
#if BF2_PACK_DISTORTION
   float3 layer=LightLayer.SampleLevel(SceneSampler,sceneUV,0);
#else
   float3 layer=LightLayer.SampleLevel(SceneSampler,lensUV,0);
#endif
   const float3 luminance=float3(0.2126f,0.7152f,0.0722f);
   float lightShare=saturate(dot(layer,luminance)/max(dot(color,luminance),1e-6f));
#endif
#if BF2_PACK_IRONSIGHTS
   if (!PSYCHO_DOF_DISABLED) {
   // Native 0x7DE1009D blends its squared DOF color before adding bloom.
   // Keep this blur after DLSS: its depth/motion input describes the sharp scene.
#if BF2_PACK_DISTORTION || BF2_PACK_FILM_GRAIN
   float4 dof=IronsightsDof.Sample(IronsightsSampler,uv);
#else
   float4 dof=Auxiliary3.Sample(Auxiliary3Sampler,uv);
#endif
   float blend=dof.a*N[3].x;
   // Match the native operation order; lerp's a+(b-a)*t loses dark blur
   // precision when a bright source is fully masked out.
   precise float3 sharp=color*(1.0f-blend);
   color=mad(dof.rgb*dof.rgb*N[3].y,blend,sharp);
   }
#endif
#if BF2_PACK_DISTORTION
   color+=BF2_BLOOM_STRENGTH*N[8].xyz*Auxiliary3.Sample(Auxiliary3Sampler,sceneUV).xyz;
#else
   color+=BF2_BLOOM_STRENGTH*N[8].xyz*Auxiliary.Sample(AuxiliarySampler,lensUV);
#endif
   float2 vignetteUV=(uv-0.5f)*N[14].xy;
   float vignette=pow(saturate(1.0f-dot(vignetteUV,vignetteUV)*N[15].w),N[14].z);
   // The engine can zero the coefficient in gameplay even though menu
   // characters retain their authored vignette. Keep the independent control
   // useful in that case without altering an existing authored vignette.
   if(N[15].w==0.0f && Psycho[11].z!=0.0f) {
      float2 fallbackUV=(uv-0.5f)*2.0f;
      vignette=saturate(1.0f-dot(fallbackUV,fallbackUV)*0.194f);
   }
   vignette=lerp(1.0f,vignette,BF2_VIGNETTE_STRENGTH);
   color=(color*N[6].xyz-N[15].xyz)*vignette+N[15].xyz;
#if !BF2_NATIVE_POST
   // Eye glare is scene light like bloom: it joins the light the grade takes in.
   [branch] if(BF2_GLARE_STRENGTH>0.0f) {
      uint veilWidth,veilHeight; GlareVeil.GetDimensions(veilWidth,veilHeight);
      if(veilWidth>0 && veilHeight>0) color+=GlareVeil.SampleLevel(GlareSampler,position.xy/(2.0f*float2(veilWidth,veilHeight)),0);
   }
#endif
   float3 ungradedPQ=BF2PQEncode(color);
   float3 gradeUV=ungradedPQ*(32.0f/33.0f)+(0.5f/33.0f);
   color=ungradedPQ;
#if BF2_NATIVE_POST
   color=NativeGrade.SampleLevel(GradeSampler,gradeUV,0);
#else
   if(BF2_LUT_NATIVE) {
      // Explicit native endpoint retains its quantized/interpolated result.
      color=NativeGrade.SampleLevel(GradeSampler,gradeUV,0);
   } else if(!BF2_LUT_BYPASSED) {
      uint gw,gh,gd;SeparatedGrade.GetDimensions(gw,gh,gd);
      if(gw==33 && gh==33 && gd==33) {
         float4 separated=SeparatedGrade.SampleLevel(GradeSampler,gradeUV,0);
         if(all(isfinite(separated)) && separated.a>=0.0f)
            color=separated.rgb;
      }
      // Unknown generation deliberately bypasses both native stages. The UI
      // reports unavailable grading; never silently reintroduce compression.
   }
#endif
   // Grain is applied after reconstruction at this output's pixel density.
   // Preserve the native texture when it is supplied; never sample an unbound
   // grain resource (zero would create a constant negative PQ color offset).
   bool nativeGrain=false;
#if BF2_PACK_FILM_GRAIN
#if BF2_PACK_DISTORTION
   uint grainWidth=0,grainHeight=0; FilmGrain.GetDimensions(grainWidth,grainHeight);
#else
   uint grainWidth=0,grainHeight=0; Auxiliary3.GetDimensions(grainWidth,grainHeight);
#endif
   nativeGrain=grainWidth!=0 && grainHeight!=0;
   if(nativeGrain) {
      // The game tiles one grain texel per render pixel; after DLSS, one per output pixel, as at native resolution.
      uint sw,sh,rw,rh; Scene.GetDimensions(sw,sh); NativeScene.GetDimensions(rw,rh);
      float2 grainUV=uv*N[2].xy*(float2(sw,sh)/max(float2(rw,rh),1.0f))+N[2].zw;
#if BF2_PACK_DISTORTION
      float grain=FilmGrain.Sample(FilmGrainSampler,grainUV).x-0.5f;
#else
      float grain=Auxiliary3.Sample(Auxiliary3Sampler,grainUV).x-0.5f;
#endif
      color+=grain*N[1].xyz*Psycho[6].w;
   }
#endif
   if(!nativeGrain && Psycho[11].y!=0.0f && Psycho[6].w!=0.0f) {
      uint2 pixel=uint2(position.xy);
      uint hash=pixel.x*0x9e3779b9u ^ pixel.y*0x85ebca6bu ^ uint(Psycho[11].x)*0xc2b2ae35u;
      hash^=hash>>16; hash*=0x7feb352du; hash^=hash>>15;
      hash*=0x846ca68bu; hash^=hash>>16;
      float grain=float(hash & 0x00ffffffu)*(1.0f/16777216.0f)-0.5f;
      color+=grain*(0.002f*Psycho[6].w);
   }
   // Private FP16 retains graded BT.709 PQ. Reconstruct/fade this signal in
   // the final pass before decoding and applying Psycho once, ahead of UI.
#if BF2_NATIVE_POST
   return float4(color,dot(color,float3(0.212671f,0.715160f,0.072169f)));
#else
   return float4(color,lightShare);
#endif
}
