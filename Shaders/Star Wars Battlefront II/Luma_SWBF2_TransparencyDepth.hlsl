// Native 199CF6D5 writes this linear distance to its R32 color output;
// shield/smoke soft-particle comparisons require it, rather than raw Z.
Texture2D<float> RawDepth : register(t0);
cbuffer NativeProjection : register(b13) { float4 Projection; };
float main(float4 position : SV_Position) : SV_Target0
{
   float z = RawDepth.Load(int3(int2(position.xy), 0));
   return Projection.x / (z - Projection.y);
}
