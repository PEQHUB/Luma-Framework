// A render-resolution input of a pass replayed after DLSS (depth, velocity, the depth-of-field blur amount), read
// per output pixel through the shares of Luma_SWBF2_PostWeights.hlsl.
Texture2D<float4> Source : register(t0);
Texture2D<float4> Shares : register(t1);
RWTexture2D<float4> Result : register(u0);
cbuffer Footprint : register(b0)
{
   float2 Scale;
   float2 SampleOffset;
   int2 RenderMax;
   // A DOF composite blends the sharp scene with the blurred layers by saturate(max(blur amount, 0) - 0.5), the far
   // blur only (1), or by saturate(|blur amount| - 0.5) (2). That weight is what has to follow the pixel: a blur
   // amount averaged across an edge would count as fully blurred. The result is a blur amount that gives the
   // composite the averaged weight in either form.
   uint BlendWeight;
   uint Padding;
};

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
   uint width, height;
   Result.GetDimensions(width, height);
   if (thread.x >= width || thread.y >= height) return;
   const int2 base = int2(floor((float2(thread.xy) + 0.5) * Scale - 0.5 - SampleOffset));
   const float4 shares = Shares.Load(int3(thread.xy, 0));
   float4 result = 0;
   [unroll] for (int i = 0; i < 4; ++i)
   {
      float4 value = Source.Load(int3(clamp(base + int2(i & 1, i >> 1), 0, RenderMax), 0));
      if (BlendWeight) value.x = saturate((BlendWeight == 1 ? max(value.x, 0) : abs(value.x)) - 0.5);
      result += shares[i] * value;
   }
   result /= dot(shares, 1);
   if (BlendWeight) result.x += 0.5;
   Result[thread.xy] = result;
}
