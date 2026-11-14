#ifndef BF2_SDR_DITHER_HLSL
#define BF2_SDR_DITHER_HLSL
// Stochastic rounding in the actual 8-bit sRGB output domain. Unlike adding
// noise then clamping, this preserves exact black/white and has an unbiased
// expected quantized value. Static per-pixel noise avoids temporal sparkle.
uint BF2DitherHash(uint x) {
   x ^= x >> 16; x *= 0x7feb352du;
   x ^= x >> 15; x *= 0x846ca68bu;
   return x ^ (x >> 16);
}
float BF2DitherUniform(uint seed) {
   // 23 bits plus a midpoint fit exactly in float, excluding both endpoints.
   return (float(BF2DitherHash(seed) >> 9) + 0.5f) * (1.0f / 8388608.0f);
}
float3 BF2DitherSDR8(float3 encoded, uint2 pixel) {
   uint seed = pixel.x * 0x9e3779b9u ^ pixel.y * 0x85ebca6bu;
   float3 noise = float3(BF2DitherUniform(seed ^ 0x243f6a88u),
      BF2DitherUniform(seed ^ 0x85a308d3u), BF2DitherUniform(seed ^ 0x13198a2eu));
   float3 codes=saturate(encoded)*255.0f;
   float3 base=floor(codes), fraction=codes-base;
   float3 roundUp=float3(noise.r<fraction.r?1.0f:0.0f,
      noise.g<fraction.g?1.0f:0.0f,noise.b<fraction.b?1.0f:0.0f);
   // Comparing the fraction avoids rounding (255 + almost-one) to 256 in
   // float arithmetic. Exact code values, including white, remain exact.
   return (base+roundUp)*(1.0f/255.0f);
}
#endif
