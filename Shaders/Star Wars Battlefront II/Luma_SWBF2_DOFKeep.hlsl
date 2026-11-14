// After a depth-of-field composite replayed at output resolution: DLSS's own pixels come back next to render samples in
// focus. The composite decides between the sharp picture and the blur from the game's blur amount, one value per render
// pixel, so detail thinner than a render pixel at an in-focus edge (fur strands, highlights) would be erased and the edge
// cut along the render grid. Background within a render pixel of something in focus stays sharp instead.
Texture2D<float3> Sharp : register(t0);  // the composite's input: the DLSS picture before depth of field
Texture2D<float> CoC : register(t1);     // the game's blur amount, render resolution
Texture2D<float4> Near : register(t2);   // the rebuilt foreground gather (alpha: its coverage); unbound without one
cbuffer NativeDOF : register(b0) { float4 DOF[3]; }; // DOF[2].y scales the sharp picture in every composite
cbuffer Keep : register(b13)
{
   float2 Scale;        // render size / output size
   float2 SampleOffset; // the samples' offset from the render pixel centres, in render pixels (+y down)
   int2 RenderMax;      // render size - 1
   int2 Padding;
};

float4 main(float4 position : SV_Position) : SV_Target
{
   // The pixel centre in sample units: sample k sits at k.
   const float2 at = position.xy * Scale - 0.5 - SampleOffset;
   const int2 base = int2(floor(at));
   const float2 f = at - base;
   float keep = 0;
   [unroll] for (int i = 0; i < 4; ++i)
   {
      const int2 corner = int2(i & 1, i >> 1);
      const float coc = CoC.Load(int3(clamp(base + corner, 0, RenderMax), 0));
      // The composite blends towards the blur as the blur amount goes from 0.5 to 1.5.
      const float focus = 1 - saturate(abs(coc) - 0.5);
      // Full up to half a render pixel from the sample, fading out over the next half.
      const float share = (corner.x ? f.x : 1 - f.x) * (corner.y ? f.y : 1 - f.y);
      keep = max(keep, focus * saturate(2 * share));
   }
   uint nearWidth, nearHeight;
   Near.GetDimensions(nearWidth, nearHeight);
   if (nearWidth) keep *= 1 - saturate(Near.Load(int3(position.xy, 0)).a); // foreground blur stays in front
   return float4(Sharp.Load(int3(position.xy, 0)) * DOF[2].y, keep);
}
