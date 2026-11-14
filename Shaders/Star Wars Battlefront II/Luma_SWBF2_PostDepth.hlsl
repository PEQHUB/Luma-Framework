// Private copy of the depth/stencil buffer that draws replayed at another resolution test against; the native one is
// never changed. After DLSS (Guided) depth is read through the shares of Luma_SWBF2_PostWeights.hlsl, so a surface
// stays a plane for the output-resolution geometry tested against it and a silhouette follows the picture's;
// stencil comes from the nearest sample.
Texture2D<float> NativeDepth : register(t0);
Texture2D<uint2> NativeStencil : register(t1);
Texture2D<float4> Shares : register(t2);
cbuffer CopyParams : register(b13) { float2 Scale; float2 SampleOffset; uint StencilBit; uint Guided; };
float main(float4 position : SV_Position) : SV_Depth
{
   uint width, height;
   NativeDepth.GetDimensions(width, height);
   const int2 last = int2(width, height) - 1;
   // The pixel centre in sample units: sample k sits at k.
   const float2 at = position.xy * Scale - 0.5 - SampleOffset;
   const int2 nearest = clamp(int2(floor(at + 0.5)), 0, last);
#if BF2_COPY_STENCIL
   if ((NativeStencil.Load(int3(nearest, 0)).y & StencilBit) == 0) discard;
#endif
   if (!Guided) return NativeDepth.Load(int3(nearest, 0));
   const float4 shares = Shares.Load(int3(position.xy, 0));
   float depth = 0;
   [unroll] for (int i = 0; i < 4; ++i)
      depth += shares[i] * NativeDepth.Load(int3(clamp(int2(floor(at)) + int2(i & 1, i >> 1), 0, last), 0));
   return depth / dot(shares, 1);
}
