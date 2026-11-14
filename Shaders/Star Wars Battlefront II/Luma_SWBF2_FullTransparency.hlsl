// The private transparency image already matches scene input resolution.
// Retain Frostbite's premultiplied color/opacity and the original OM blend.
Texture2D<float4> Transparency : register(t0);
float4 main(float4 position : SV_Position) : SV_Target0
{
   return Transparency.Load(int3(int2(position.xy), 0));
}
