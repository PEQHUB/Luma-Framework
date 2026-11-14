// Frostbite's separable DOF gather, evaluated at each reconstructed pixel.
// Offsets and CoC stay in native-pixel units: changing DLSS quality must not
// change the screen-space blur radius. The zero-radius tap reads the exact
// reconstructed pixel instead of a downsampled copy of the sharp scene.
Texture2D<float4> Color : register(t0);
// The game's blur amount: one jittered sample per render pixel. Each output pixel sits between four of them, with the
// shares of Luma_SWBF2_PostWeights.hlsl. The gather is evaluated on each of the four sample lattices (every tap at a
// whole number of render pixels from the lattice's sample, so only real blur amounts are used) and the four results
// are mixed by the shares. Mixing the blur amounts instead would give a sharp edge a share of its background's
// radius and spread it into a halo; taking one lattice per pixel would step once per render pixel.
Texture2D<float> CoC : register(t1);
Texture2D<float4> Shares : register(t3);
Texture2D<float4> NearColor : register(t2);
SamplerState ColorBorder : register(s0);
RWTexture2D<float4> Gather : register(u0);
#if BF2_DOF_NEAR
RWTexture2D<float4> NearGather : register(u1);
#endif
cbuffer NativeDOF : register(b0)
{
   uint2 FullScreenSize; float2 FullPixelSize;
   uint2 CurrentScreenSize; float2 CurrentPixelSize;
   float EnergyScale, InfocusMultiplier, MinRadiusToLayer4, MinRadiusToLayer8;
   float MaxRadiusToGather, CurrentRadius, MergeRadiusThreshold, MergeColorThreshold;
};
cbuffer Footprint : register(b1)
{
   float2 Scale;        // render size / output size
   float2 SampleOffset; // the samples' offset from the render pixel centres, in render pixels (+y down)
   int2 RenderMax;      // render size - 1
   uint2 Padding;
};
#if BF2_DOF_VERTICAL
static const int2 Along = int2(0, 1), Across = int2(1, 0);
#else
static const int2 Along = int2(1, 0), Across = int2(0, 1);
#endif

// The blur amounts of the two samples across the gather's axis at `texel`.
float2 Pair(int2 texel)
{
   return float2(CoC.Load(int3(clamp(texel, 0, RenderMax), 0)), CoC.Load(int3(clamp(texel + Across, 0, RenderMax), 0)));
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
   uint width, height;
   Gather.GetDimensions(width, height);
   if (thread.x >= width || thread.y >= height) return;
   const float2 uv = (float2(thread.xy) + 0.5) / float2(width, height);
   // The four lattices in the order (along, across) = (0,0), (0,1), (1,0), (1,1).
   const int2 base = int2(floor((float2(thread.xy) + 0.5) * Scale - 0.5 - SampleOffset));
   float4 shares = Shares.Load(int3(thread.xy, 0));
#if !BF2_DOF_VERTICAL
   shares = shares.xzyw;
#endif
   shares /= dot(shares, 1);
   const float4 centerCoC = float4(Pair(base), Pair(base + Along));
   float4 result = 0;
#if BF2_DOF_NEAR
   float4 nearResult = 0;
#endif
   // The 32 verified native kernels differ only in group-shared padding
   // (1..128) and whether they also accumulate foreground coverage.
   const int radius = min(128, max(0, (int)MaxRadiusToGather));
   float2 next = Pair(base - radius * Along);
   [loop] for (int offset = -radius; offset <= radius; ++offset)
   {
#if BF2_DOF_VERTICAL
      const float2 sampleUV = uv + float2(0, offset * FullPixelSize.y);
#else
      const float2 sampleUV = uv + float2(offset * FullPixelSize.x, 0);
#endif
      // This tap on the four lattices: the second pair sits one sample further along.
      const float2 previous = next;
      next = Pair(base + (offset + 1) * Along);
      const float4 coc = float4(previous, next);
      const float4 distance = abs((float)offset) / (abs(coc) + 1);
      const float4 square = distance * distance;
      const float4 weights = shares * max(0, 1 - square) * max(0, 1 - square * square)
         * saturate(MaxRadiusToGather - abs(coc)) / (1 + 1.3 * abs(coc));
      const float weight = dot(weights, 1);
      if (weight <= 0) continue;
      // Border zero matches native Texture.Load outside the scene. In the
      // horizontal pass native alpha is explicitly 1 even outside the image.
      float4 color = offset == 0 ? Color.Load(int3(thread.xy, 0)) : Color.SampleLevel(ColorBorder, sampleUV, 0);
#if !BF2_DOF_VERTICAL
      color.a = 1;
#endif
      result += color * weight;
#if BF2_DOF_NEAR
      const float nearWeight = dot(weights * saturate(-coc) * saturate(abs(coc) - abs(centerCoC) + 0.5), 1);
#if BF2_DOF_VERTICAL
      color = offset == 0 ? NearColor.Load(int3(thread.xy, 0)) : NearColor.SampleLevel(ColorBorder, sampleUV, 0);
#endif
      nearResult += color * nearWeight;
#endif
   }
   Gather[thread.xy] = result;
#if BF2_DOF_NEAR
   NearGather[thread.xy] = nearResult;
#endif
}
