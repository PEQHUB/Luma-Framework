#include "Includes/PsychoSettings.hlsl"
#include "Includes/SDRDither.hlsl"
#if !BF2_NATIVE_MAPPER && !BF2_PRESENTATION_ONLY
#if BF2_ACES2
#include "Includes/ACES2/output_transform.hlsl"
#elif BF2_GT7
#include "Includes/GT7/gt7_tone_mapping.hlsl"
#else
// PsychoV (RenoDX psychov Test35).
#include "Includes/PsychoV/tonemap/psychov/test35.hlsl"
#endif
#endif
Texture2D<float4> Scene : register(t0);
Texture2D<float4> Overlay : register(t1);
Texture1D<float> PQToLinear : register(t2);
Texture2D<float> Darken : register(t9);
Texture3D<float3> DisplayMapping : register(t10);
Texture2D<float4> DebugOverlay : register(t12);
SamplerState PointSampler : register(s0);
SamplerState LinearSampler : register(s1);
SamplerState OverlaySampler : register(s2);
cbuffer FrostbiteDisplay : register(b0) { float4 N[10]; };

float4 CubicWeights(float t) {
   float t2=t*t,t3=t2*t;
   float4 w=float4(-t3-t+2*t2,t3+1-2*t2,-t3+t2+t,t3-t2);
   return w/dot(w,float4(1,1,1,1));
}
float3 ReadScene(float2 uv) {
   float3 sampled=0.0f;
#if BF2_FINAL_LINEAR
   // Native Low post-process quality (20FE932C, game HDR 188B1E17) uses a
   // single linear tap.
   sampled=Scene.SampleLevel(LinearSampler,uv,0).rgb;
#else
#if !BF2_FINAL_POINT
   // Retain Frostbite's 4x4 cubic reconstruction when native TAA is rendering
   // below the display size. DLSS is already full size and skips these taps.
   if(any(N[0].xy!=N[1].xy)) {
      float2 phase=frac((uv-0.5f*N[0].zw)*N[0].xy);
      float2 base=uv+(-1.0f-phase)*N[0].zw;
      float4 wx=CubicWeights(phase.x),wy=CubicWeights(phase.y);
      float3 result=0;
      [unroll] for(int y=0;y<4;++y) {
         float3 row=0;
         [unroll] for(int x=0;x<4;++x) row+=Scene.SampleLevel(LinearSampler,base+float2(x,y)*N[0].zw,0).rgb*wx[x];
         result+=row*wy[y];
      }
      sampled=result;
   } else
#endif
   { sampled=Scene.SampleLevel(PointSampler,uv,0).rgb; }
#endif
   return sampled;
}

float3 EncodeSRGB(float3 x) {
   float3 hi=exp2(log2(abs(x))*(1.0f/2.4f))*1.055f-0.055f;
   return float3(x.r<=0.0031308f?x.r*12.92f:hi.r,x.g<=0.0031308f?x.g*12.92f:hi.g,x.b<=0.0031308f?x.b*12.92f:hi.b);
}
float3 DecodeSRGB(float3 x) {
   float3 hi=exp2(log2(max((x+0.055f)/1.055f,0.0f))*2.4f);
   return float3(x.r<=0.04045f?x.r/12.92f:hi.r,x.g<=0.04045f?x.g/12.92f:hi.g,x.b<=0.04045f?x.b/12.92f:hi.b);
}
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
   float3 color=ReadScene(uv);
   // Where SDR white sits in the output units below (1 = 100 nits on the
   // linear transport). The game's accessibility filter works on SDR values.
   float white=1.0f;
   uint flags=asuint(N[7].x);
#if BF2_NATIVE_MAPPER || BF2_PRESENTATION_ONLY
   color*=N[2].x; // Frostbite fade operates on the graded PQ signal.
   if (flags&2) color=DisplayMapping.SampleLevel(LinearSampler,color*(31.0f/32.0f)+(0.5f/32.0f),0);
   else color=float3(PQToLinear.SampleLevel(LinearSampler,color.r,0),PQToLinear.SampleLevel(LinearSampler,color.g,0),PQToLinear.SampleLevel(LinearSampler,color.b,0));
#if BF2_PRESENTATION_ONLY
   // No scene tone mapper or auto exposure on display-referred loading/menu
   // content. Preserve native PQ/LUT decoding and fade. SDR artwork stays SDR
   // in range, with its white placed at the selected reference luminance.
   // A native HDR composite already uses absolute 100-nit units.
   if(!PSYCHO_HDR || !BF2_NATIVE_HDR_DISPLAY) {
      color=saturate(color);
      if(PSYCHO_LINEAR_OUTPUT) white=(Psycho[0].x!=0.0f ? PsychoPaperNits() : 100.0f)/100.0f;
      color*=white;
   }
#endif
#else
   // The game's decoded grade is linear BT.709 in 100-nit units, where the
   // game's own SDR output has its white at 1.0. Each tone mapper takes that
   // signal; scene/paper white scales its OUTPUT, not its input.
   color=BF2PQDecode(color);
   // The scene's alpha is the share of the pixel's light from blaster bolts and
   // sabers (Luma_SWBF2_PsychoScene.hlsl); in HDR with a BT.2020 target their
   // colour is expanded toward BT.2020.
   [branch] if(PSYCHO_HDR && Psycho[4].x!=0.0f && BF2_LIGHT_GAMUT_EXPANSION>0.0f)
      color=BF2ExpandGamut(color,BF2_LIGHT_GAMUT_EXPANSION*saturate(Scene.SampleLevel(PointSampler,uv,0).a));
   // The game fades in PQ. Mirrored for negative channels, which the gamut expansion can carry.
   [branch] if(N[2].x!=1.0f) color=sign(color)*BF2PQDecode(BF2PQEncode(abs(color))*N[2].x);
   float paper=PsychoPaperNits();
#if BF2_GT7
   // Polyphony's sample: linear Rec.2020 in and out, in frame-buffer units of
   // 100 nits. Its frame buffer is exposed for its paper white: the SDR path
   // maps the value 2.5 (250 nits) to SDR white (initializeAsSDR). The game
   // signal has its SDR white at 1.0, so it enters GT7 scaled by the paper
   // white. Fed unscaled, the picture is 1.32 stops under GT7's exposure and
   // its toe, which ends at 0.538, covers every midtone.
   // SDR is the sample's SDR path as published (250 nits). HDR targets the
   // display peak (documented range 250-10,000 nits) and places scene white
   // at the scene brightness control, whose default is the same 250 nits.
   // Wide-gamut output is carried as negative BT.709 on the linear transport
   // and clipped for 8-bit SDR.
   const float gt7White=PSYCHO_HDR ? BF2_GT7_PAPER_WHITE : GT7::GRAN_TURISMO_SDR_PAPER_WHITE;
   color*=exp2(BF2_GT7_EXPOSURE)*GT7::PhysicalValueToFrameBufferValue(gt7White);
   const int ucs=BF2_GT7_CHROMA_SPACE!=0.0f ? GT7::UCS_JZAZBZ : GT7::UCS_ICTCP;
   // The sample's curve/blend parameters; Luma's defaults are its values.
   GT7::Parameters parameters;
   parameters.alpha=BF2_GT7_ALPHA; parameters.grayPoint=BF2_GT7_MID_POINT;
   parameters.linearSection=BF2_GT7_LINEAR_SECTION; parameters.toeStrength=BF2_GT7_TOE_STRENGTH;
   parameters.blendRatio=BF2_GT7_BLEND_RATIO; parameters.fadeStart=BF2_GT7_FADE_START; parameters.fadeEnd=BF2_GT7_FADE_END;
   GT7::ToneMapping gt7;
   if(PSYCHO_HDR) gt7=GT7::InitializeAsHDR(clamp(Psycho[0].y,250.0f,10000.0f),ucs,parameters);
   else gt7=GT7::InitializeAsSDR(GT7::GRAN_TURISMO_SDR_PAPER_WHITE,ucs,parameters);
   color=mul(BF2_BT2020_TO_BT709,GT7::ApplyToneMapping(gt7,mul(BF2_BT709_TO_BT2020,color)));
   // HDR output is absolute, with scene white near the scene brightness. SDR
   // on the linear transport: 1 = SDR white.
   if(PSYCHO_HDR) white=BF2_GT7_PAPER_WHITE/100.0f;
   else if(PSYCHO_LINEAR_OUTPUT) { white=paper/100.0f; color*=white; }
#elif BF2_ACES2
   // aces-core 2.0 Output Transform: ACES scene light in (1.0 = 100 nits),
   // limiting-primaries RGB out with 1.0 = the transform's 100-nit reference
   // white, clamped to its peak and re-expressed as linear BT.709. The addon
   // builds the tables and binds them at b10 (tone_mapping.h).
   // SDR is the 100-nit Rec.709 preset, shown at SDR white. HDR shows the
   // 100-nit reference white at scene white, as Psycho does: the tables are
   // built for peak * 100 / scene white and the result is scaled back, so
   // the output still ends at the display peak. A scene white of 100 nits
   // is the unscaled reference transform.
   color*=exp2(BF2_ACES2_EXPOSURE);
   color=ACES2_LimitToRec709(ACES2_OutputTransform(ACES2_Rec709ToAP0(color)));
   if(PSYCHO_LINEAR_OUTPUT) white=paper/100.0f; // Linear transport: 1 = 100 nits.
   color*=white;
#else
   float peak=PSYCHO_HDR ? Psycho[0].y/paper : 1.0f;
   // UNORM/sRGB can represent BT.709 only. Wide-gamut targets are available
   // through the native FP16/scRGB transport, including its SDR mode.
   int gamut=PSYCHO_LINEAR_OUTPUT ? int(PSYCHO_HDR ? Psycho[4].x : Psycho[5].w) : 0;
   // PsychoV (Test35). Its adaptation anchors use the 100-nit input
   // convention. It fits to BT.709 or BT.2020 only, so every wide-gamut
   // target selects BT.2020. The remaining arguments are its reserved
   // placeholders and defaults: unit cone exponent, exact fit, no boundary
   // conditioning.
   color=renodx::tonemap::psychov::psychotm_test35(
      color,peak,exp2(Psycho[1].x),Psycho[1].y,Psycho[1].z,Psycho[1].w,Psycho[2].w,
      1.0f,100.0f,1.0f,1.0f,0,1.0f,Psycho[6].xyz,Psycho[7].xyz,
      Psycho[3].w,gamut!=0?1:0,1.0f,Psycho[4].y);
   if(PSYCHO_LINEAR_OUTPUT) white=paper/100.0f; // Linear transport: 1 = 100 nits.
   color*=white;
#endif
#endif
   if(N[2].y>0.0f) {
      float4 overlay=Overlay.SampleLevel(OverlaySampler,uv,0);
      if(asuint(N[5].z)) {
         float4 shifted=(uv.xyxy-0.5f)*N[6]+0.5f;
         overlay.r=Overlay.SampleLevel(LinearSampler,shifted.xy,0).r;
         overlay.g=Overlay.SampleLevel(LinearSampler,shifted.zw,0).g;
      }
      uint direction=asuint(N[4].z), width=asuint(N[4].w);
      uint coordinate=direction==1?uint(position.y):direction==2?uint(position.x):0u;
      uint remainder=width?coordinate%width:0xffffffffu;
      float scan=remainder<(width>>1)?1.0f:N[5].x;
      float3 ui=overlay.rgb*scan;
      float luminance=min(dot(color,float3(0.2126f,0.7152f,0.0722f)),1.0f);
      float darken=Darken.SampleLevel(LinearSampler,uv,0)*luminance*luminance;
      float3 shiftedUI=Overlay.SampleLevel(OverlaySampler,max(uv-N[1].zw,0.0f),0).rgb;
      float3 edge=max(shiftedUI-ui,0.0f);
      float uiLuma=dot(ui,float3(0.2126f,0.7152f,0.0722f));
      float shadow=(1.0f-uiLuma*uiLuma)*dot(edge,float3(0.2126f,0.7152f,0.0722f))*luminance*N[7].y;
      float alpha=(max(shadow,darken)*(1.0f-overlay.a)+overlay.a)*N[2].z;
      if(asuint(N[3].y)) alpha=1.0f-(1.0f-alpha)*(1.0f-alpha);
#if BF2_NATIVE_MAPPER
      color=color*(1.0f-alpha)+ui*N[2].y;
#else
      // The native FP16 route can request HDR UI brightness even when Luma
      // deliberately renders SDR. Its white must follow the SDR reference.
      float uiScale=(PSYCHO_NATIVE_UI && PSYCHO_HDR) ? N[2].y : PSYCHO_LINEAR_OUTPUT ? PsychoUINits()/100.0f : 1.0f;
#if BF2_PRESENTATION_ONLY
      // Disabling the custom mapper preserves native UI brightness, too.
      if(Psycho[0].x==0.0f) uiScale=N[2].y;
#endif
      color=color*(1.0f-alpha)+ui*uiScale;
#endif
   }
   float4 debug=DebugOverlay.SampleLevel(PointSampler,uv,0);
   color=color*(1.0f-debug.a)+debug.rgb;
   // Native color-blind / accessibility controls, including their original
   // SDR-domain correction, applied relative to SDR white. Keep them separate
   // from Psycho's scene grading.
   if(flags&1) {
      float3 encoded=EncodeSRGB(color/white);
      encoded=(encoded-0.5f)*(1.0f+N[8].w*0.112f)+0.5f-N[8].w*0.075f;
      float l=dot(encoded,float3(17.882401f,43.516102f,4.119350f));
      float m=dot(encoded,float3(3.455650f,27.155399f,3.867140f));
      float s=dot(encoded,float3(0.029957f,0.184309f,1.467090f));
      float simL=lerp(l,2.023440f*m-2.528100f*s,N[8].x);
      float simM=lerp(m,0.494207f*l+1.248270f*s,N[8].y);
      float simS=lerp(s,-0.395913f*l+0.801109f*m,N[8].z);
      float3 simulation=simL*float3(0.080944f,-0.010249f,-0.000365f)
         +simM*float3(-0.130504f,0.054019f,-0.004122f)+simS*float3(0.116721f,-0.113615f,0.693511f);
      float3 difference=encoded-simulation;
      float3 correction=float3(0.0f,difference.y+0.7f*difference.x,difference.z+0.7f*difference.x);
      encoded=lerp(encoded,saturate(encoded+correction),N[8].w);
      encoded=(encoded-0.5f)*(1.0f+N[9].y)+0.5f+N[9].x+N[8].w*0.08f;
      color=DecodeSRGB(encoded)*white;
   }
#if BF2_NATIVE_MAPPER || BF2_PRESENTATION_ONLY
   // Native is 1:1 with the unmodified game. Its SDR picture (1.0 = SDR
   // white, scene and UI alike) is shown at the Windows SDR content
   // brightness on an HDR display, as Windows shows the game's own SDR
   // output, and at the display's white (80 nits) on an SDR display.
   // The game's own HDR output keeps its absolute luminance.
   if(PSYCHO_LINEAR_OUTPUT && Psycho[0].x==0.0f && !BF2_NATIVE_HDR_DISPLAY)
      color*=BF2WindowsSDRWhite()/100.0f;
#endif
   // Native FP16/scRGB uses 80 nit units. SDR UNORM uses sRGB encoding.
   // No SDR clamp occurs in the HDR transport, including negative BT.709 RGB.
   // On an SDR display Windows turns the FP16 output into 8-bit sRGB; the
   // dither (only on then) applies to those code values.
   if(PSYCHO_LINEAR_OUTPUT && (PSYCHO_HDR || !PSYCHO_SDR_DITHER)) return float4(color*1.25f,1.0f);
   float3 encodedOutput=EncodeSRGB(saturate(PSYCHO_LINEAR_OUTPUT ? color*1.25f : color));
   if(PSYCHO_SDR_DITHER) encodedOutput=BF2DitherSDR8(encodedOutput,uint2(position.xy));
   return float4(PSYCHO_LINEAR_OUTPUT ? DecodeSRGB(encodedOutput) : encodedOutput,1.0f);
}
