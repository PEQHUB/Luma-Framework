#include "Includes/NativeLUT.hlsl"
// Rebuilds the game's color grading LUT with each of its stages scaled by a
// Luma setting (ArtisticStrength is the menu's "Game color grading"). The source LUTs and constants are those of the game's own
// LUT generation pass, copied before that pass runs.
Texture3D<float3> Sources[15] : register(t0);
cbuffer ColorCubeConstants : register(b0) { float4 N[25]; };
cbuffer LumaLUTSources : register(b1) {
   uint SourceCount; float ArtisticStrength; float MatrixStrength; float GammaStrength;
   float BrightnessStrength; float PreExposureStrength; float CompressionStrength; float PostExposureStrength;
};
RWTexture3D<float4> Output : register(u0);
[numthreads(33,3,1)]
void main(uint3 id : SV_DispatchThreadID) {
   if(any(id>=33)) return;
   int4 location=int4(id.x,32-id.y,id.z,0);
   float3 source=0.0f;
   [unroll] for(uint i=0;i<15;++i) {
      if(i<SourceCount) {
         float3 next=Sources[i].Load(location);
         source=i==0 ? next : mad(N[9+i].xyz,next-source,source);
      }
   }
   // The game's HDR mode moves its highlight compression to the display peak:
   // pre-exposure 1/peak and post-exposure peak (100-nit units; measured
   // 0.1023 and 9.775 for its 977-nit setting) instead of SDR's 0.5 and 1.0.
   // Luma's own grade keeps the SDR pair, so its tone mappers receive the same
   // signal whatever the game's HDR switch says. Native (the game's own LUT)
   // is unaffected.
   float2 exposure=N[7].xy;
   if(exposure.y>1.0001f && abs(exposure.x*exposure.y-1.0f)<1e-3f) exposure=float2(0.5f,1.0f);
   float4 s=float4(source,1.0f);
   float3 matrixColor=float3(dot(s,N[0]),dot(s,N[1]),dot(s,N[2]));
   float3 color=lerp(source,matrixColor,MatrixStrength);
   // Below 100%, game color grading fades the game's color changes out and
   // keeps its tone response: the ungraded scene color (the identity LUT is a
   // PQ lattice) takes the luminance of the graded one. The grade is the
   // game's tone curve (measured: about the square of the scene value around
   // 0.47), so fading to the identity would lift shadows by several stops.
   if(ArtisticStrength<1.0f) {
      const float3 weights=float3(0.2126f,0.7152f,0.0722f);
      float3 scene=BF2LUTDecode(float3(id)/32.0f);
      float sceneY=dot(scene,weights), gradedY=dot(BF2LUTDecode(color),weights);
      float3 neutral=sceneY>1e-6f ? scene*(gradedY/sceneY) : gradedY;
      color=lerp(BF2LUTEncode(neutral),color,ArtisticStrength);
   }
   float3 exponent=N[5].xyz;
   if(N[5].w!=0.0f) exponent=mad(min(color,N[5].w)/N[5].w,1.0f-exponent,exponent);
   float3 shaped=pow(max(color,0.0f),exponent);
   shaped=lerp(color,shaped,GammaStrength);
   float3 adjusted=mad(N[6].x-1.0f,N[6].z,shaped);
   adjusted=pow(max(adjusted,0.0f),N[6].y)*min(N[6].w*N[6].x,1.0f);
   color=BF2LUTDecode(lerp(shaped,adjusted,BrightnessStrength))*lerp(1.0f,exposure.x,PreExposureStrength);
   if(CompressionStrength>0.0f) color=lerp(color,BF2NativeLUTCompression(color),CompressionStrength);
   float scale=lerp(1.0f,exposure.y,PostExposureStrength);
   // RGBA32F avoids the native UNORM clamp while retaining PQ interpolation.
   // Alpha < 0 marks invalid generation parameters for the scene's fallback.
   bool valid=all(isfinite(color)) && isfinite(scale) && scale>=0.0f;
   Output[id]=valid ? float4(BF2LUTEncode(color*scale),1.0f) : float4(0.0f,0.0f,0.0f,-1.0f);
}
