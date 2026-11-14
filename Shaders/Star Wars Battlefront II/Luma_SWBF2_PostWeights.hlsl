// For each output pixel, the shares of the four render samples around it, for the passes replayed after DLSS that
// read render-resolution data (depth, the depth-of-field blur amount). The samples sit at this frame's jitter offset
// from the picture. A share is the bilinear weight times how close the sample's colour (the DLSS input) is to the
// picture's (the DLSS output) at the pixel: across an edge the data follows the picture's edge, elsewhere it is
// bilinear. What is read through the shares changes from output pixel to output pixel, never in render-pixel steps.
Texture2D<float3> Output : register(t0); // DLSS output
Texture2D<float3> Input : register(t1);  // DLSS input colour, render resolution
RWTexture2D<float4> Shares : register(u0); // sample (x, y) of the 2x2 at index x + 2 * y
cbuffer Footprint : register(b0)
{
   float2 Scale;        // render size / output size
   float2 SampleOffset; // the samples' offset from the render pixel centres, in render pixels (+y down)
   int2 RenderMax;      // render size - 1
   uint2 Padding;
};

// Finite for any colour (D3D's min and max return the other operand for NaN), so no share is NaN.
float3 Tone(float3 color)
{
   return log2(min(max(color, 0), 65504) + 0.01);
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
   uint width, height;
   Shares.GetDimensions(width, height);
   if (thread.x >= width || thread.y >= height) return;
   // The pixel centre in sample units: sample k sits at k.
   const float2 position = (float2(thread.xy) + 0.5) * Scale - 0.5 - SampleOffset;
   const int2 base = int2(floor(position));
   const float2 f = position - base;
   const float3 picture = Tone(Output.Load(int3(thread.xy, 0)));
   float4 shares;
   [unroll] for (int i = 0; i < 4; ++i)
   {
      const int2 corner = int2(i & 1, i >> 1);
      const float2 side = lerp(1 - f, f, corner);
      const float3 difference = Tone(Input.Load(int3(clamp(base + corner, 0, RenderMax), 0))) - picture;
      // Half a stop apart on every channel counts for a fifth; the floor keeps bilinear where no sample matches.
      shares[i] = side.x * side.y * (exp(-2 * dot(difference, difference)) + 0.001);
   }
   Shares[thread.xy] = shares / max(dot(shares, 1), 1e-12);
}
