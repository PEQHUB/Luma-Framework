#include "Includes/PsychoSettings.hlsl"
// Native YUV layouts, verified against captured DXBC. Variants 0-8 repair
// direct encoded-to-scRGB draws. Variants 9-11 preserve SDR movie appearance
// in the observed sRGB UI intermediate before the HDR UI compositor.
Texture2D<float4> Plane0 : register(t0);
Texture2D<float4> Plane1 : register(t1);
Texture2D<float4> Plane2 : register(t2);
Texture2D<float4> Plane3 : register(t3);
SamplerState S0 : register(s0);
SamplerState S1 : register(s1);
SamplerState S2 : register(s2);
SamplerState S3 : register(s3);
cbuffer NativeVideo : register(b0) { float4 V[47]; };

float3 DecodeVideoSRGB(float3 encoded) {
   float3 hi=pow(max((encoded+0.055f)/1.055f,0.0f),2.4f);
   return float3(encoded.r<=0.04045f?encoded.r/12.92f:hi.r,
                 encoded.g<=0.04045f?encoded.g/12.92f:hi.g,
                 encoded.b<=0.04045f?encoded.b/12.92f:hi.b);
}

#if BF2_VIDEO_VARIANT==1 || BF2_VIDEO_VARIANT==2 || BF2_VIDEO_VARIANT==3 || BF2_VIDEO_VARIANT==6 || BF2_VIDEO_VARIANT==10
float4 main(float4 position:SV_Position,float4 tint:TEXCOORD0,float2 uv:TEXCOORD1):SV_Target
#else
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0,float4 tint:TEXCOORD1):SV_Target
#endif
{
   float4 yvu=float4(Plane0.Sample(S0,uv).r,0,0,1);
#if BF2_VIDEO_VARIANT==2
   yvu.yz=Plane1.Sample(S1,uv).gr;
#elif BF2_VIDEO_VARIANT==3
   yvu.yz=Plane2.Sample(S2,uv).gr;
#elif BF2_VIDEO_VARIANT==11
   // Untinted fullscreen variant uses the opposite chroma texture order.
   yvu.y=Plane2.Sample(S1,uv).r;
   yvu.z=Plane1.Sample(S2,uv).r;
#else
   yvu.y=Plane1.Sample(S1,uv).r;
   yvu.z=Plane2.Sample(S2,uv).r;
#endif
   // Full float32 constants from the native bytecode, including video range
   // offsets. Preserve the original matrix, sampling and authored tint/fades.
   precise float3 rgb;
   rgb.r=dot(yvu.xyw,float3(1.16412353515625f,1.595794677734375f,-0.8706550598144531f));
   rgb.g=dot(yvu,float4(1.16412353515625f,-0.8134765625f,-0.391448974609375f,0.5297050476074219f));
   rgb.b=dot(yvu.xzw,float3(1.16412353515625f,2.017822265625f,-1.0816688537597656f));
#if BF2_VIDEO_VARIANT>=9
   // SDR movie code values were viewed with an SDR display response. Keeping
   // the sRGB linear toe in HDR lifts near-black compression noise. Select
   // that response at video decode, before native linear tint/fade/blending.
   // On an SDR display Windows encodes the output with the sRGB curve, so the
   // exact inverse gives back the game's own 8-bit picture.
   rgb=BF2_DISPLAY_HDR ? pow(max(rgb,0.0f),2.2f) : DecodeVideoSRGB(saturate(rgb));
#endif
#if BF2_VIDEO_VARIANT==8
   if(V[46].x>0.0f) {
      float3 t=saturate(rgb/V[46].x);
      rgb*=t*t*(3.0f-2.0f*t);
   }
#endif
   float4 color=float4(rgb,1.0f);
#if BF2_VIDEO_VARIANT==3
   color.a=(Plane1.Sample(S1,uv).r-0.06274499744176865f)*1.1641443967819214f;
#elif BF2_VIDEO_VARIANT==4
   color.a=Plane3.Sample(S3,uv).r;
#elif BF2_VIDEO_VARIANT==6
   color.a=(Plane3.Sample(S3,uv).r-0.06274499744176865f)*1.1641443967819214f;
#endif
#if BF2_VIDEO_VARIANT!=11
   color*=tint;
#endif
#if BF2_VIDEO_VARIANT==5
   color*=Plane3.Sample(S3,uv);
#elif BF2_VIDEO_VARIANT==7
   color*=V[40]; // Native videoFakeBrightness, not scene exposure.
#endif
#if BF2_VIDEO_VARIANT<9
   // The original output is sRGB code values. Writing it unchanged to scRGB
   // treats dark compression noise as linear light (12.92x at the toe).
   // Restore the display transfer once, then place SDR white at paper white
   // (Native: the Windows SDR content brightness, like the rest of its SDR
   // picture).
   const float white=Psycho[0].x!=0.0f ? PsychoPaperNits() : BF2WindowsSDRWhite();
   color.rgb=DecodeVideoSRGB(saturate(color.rgb))*white/80.0f;
#endif
   return color; // Alpha is coverage, never gamma-decoded or white-scaled.
}
