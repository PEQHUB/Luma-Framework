#ifndef BF2_PSYCHO_SETTINGS
#define BF2_PSYCHO_SETTINGS
// Battlefront2Psycho::Settings (tone_mapping.h), four values per row.
// ACES 2.0's other settings reach the GPU through its own tables (b10),
// built on the CPU.
cbuffer PsychoControls : register(b11) { float4 Psycho[19]; };
#define BF2_GT7_EXPOSURE Psycho[14].x
#define BF2_GT7_CHROMA_SPACE Psycho[14].y
#define BF2_GT7_PAPER_WHITE Psycho[14].z
#define BF2_ACES2_EXPOSURE Psycho[14].w
#define BF2_GT7_TOE_STRENGTH Psycho[15].y
#define BF2_GT7_MID_POINT Psycho[15].z
#define BF2_GT7_LINEAR_SECTION Psycho[15].w
#define BF2_GT7_ALPHA Psycho[16].x
#define BF2_GT7_BLEND_RATIO Psycho[16].y
#define BF2_GT7_FADE_START Psycho[16].z
#define BF2_GT7_FADE_END Psycho[16].w
#define PSYCHO_HDR (Psycho[8].x != 0.0f)
#define PSYCHO_LINEAR_OUTPUT (Psycho[8].y != 0.0f)
// Windows HDR is on for the game's display (the output itself can be SDR).
#define BF2_DISPLAY_HDR (Psycho[8].z != 0.0f)
#define PSYCHO_LUT_STRENGTH Psycho[8].w
#define PSYCHO_NATIVE_UI (Psycho[9].x != 0.0f)
#define PSYCHO_DOF_DISABLED (Psycho[9].y != 0.0f)
#define BF2_GLARE_STRENGTH Psycho[2].x
#define BF2_LIGHT_GAMUT_EXPANSION Psycho[2].y
#define BF2_BLOOM_STRENGTH Psycho[9].z
#define BF2_VIGNETTE_STRENGTH Psycho[9].w
#define PSYCHO_SDR_DITHER (Psycho[10].x != 0.0f)
#define BF2_DISTORTION_STRENGTH Psycho[10].w
// Psycho[12] and Psycho[13].xy are the six native LUT stage strengths.
#define BF2_LUT_NATIVE (PSYCHO_LUT_STRENGTH>=1.0f && all(Psycho[12]>=1.0f) && all(Psycho[13].xy>=1.0f))
// Set by the addon: the game's final pass is its HDR variant; the Windows
// SDR content brightness in nits (0 when unknown).
#define BF2_NATIVE_HDR_DISPLAY (Psycho[13].z != 0.0f)
float BF2WindowsSDRWhite() { return Psycho[13].w > 0.0f ? Psycho[13].w : 100.0f; }
#define BF2_LUT_BYPASSED (PSYCHO_LUT_STRENGTH<=0.0f && all(Psycho[12]<=0.0f) && all(Psycho[13].xy<=0.0f))
float PsychoPaperNits() { return PSYCHO_HDR ? Psycho[0].z : Psycho[7].w; }
float PsychoUINits() { return PSYCHO_HDR ? Psycho[0].w : Psycho[7].w; }
float3 BF2PQEncode(float3 linear100) {
   float3 p=exp2(log2(max(linear100,0.0f)*0.01f)*0.1593017578125f);
   return exp2(log2((p*18.8515625f+0.8359375f)/(p*18.6875f+1.0f))*78.84375f);
}
float3 BF2PQDecode(float3 pq) {
   float3 p=exp2(log2(max(pq,0.0f))/78.84375f);
   return exp2(log2(max(p-0.8359375f,0.0f)/max(18.8515625f-18.6875f*p,1e-6f))/0.1593017578125f)*100.0f;
}
// Linear BT.709 <-> BT.2020, both D65, derived from the primaries.
static const float3x3 BF2_BT709_TO_BT2020 = float3x3(
   0.6274038959f, 0.3292830384f, 0.0433130657f,
   0.0690972894f, 0.9195403951f, 0.0113623156f,
   0.0163914389f, 0.0880133079f, 0.8955952532f);
static const float3x3 BF2_BT2020_TO_BT709 = float3x3(
   1.6604910021f, -0.5876411388f, -0.0728498633f,
   -0.1245504745f, 1.1328998971f, -0.0083494226f,
   -0.0181507634f, -0.1005788980f, 1.1187296614f);
// Gamut expansion of the light of blaster bolts and sabers: the same luminance and hue line (dominant wavelength),
// with its distance toward the BT.709 edge becoming the same distance toward the BT.2020 edge. Amount 0 leaves it
// unchanged.
float3 BF2ExpandGamut(float3 c, float amount) {
   float y=dot(c,float3(0.2126f,0.7152f,0.0722f));
   float3 w=mul(BF2_BT709_TO_BT2020,c);
   float p709=1.0f-min(c.r,min(c.g,c.b))/y, p2020=1.0f-min(w.r,min(w.g,w.b))/y;
   if(!(amount>0.0f && y>0.0f && p2020>1e-4f)) return c;
   return mul(BF2_BT2020_TO_BT709,y+(w-y)*lerp(1.0f,p709/p2020,amount*saturate(p709)));
}
#endif
