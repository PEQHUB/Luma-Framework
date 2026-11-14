// Fullscreen vertex stage for the Battlefront II scene-only DLSS handoff.
//
// The original Frostbite linear scene pixel shader is retained at runtime.
// Its disassembled input signature consumes TEXCOORD1 from pixel-shader
// register v2. The replacement vertex stage must reproduce the native VS
// output signature (TEXCOORD0 at v1 and TEXCOORD1 at v2); semantic indices,
// not register numbers, define the stage linkage.
//
// Without that linkage v2 is undefined and the game's shader samples
// undefined coordinates. Keeping the original pixel shader preserves
// Frostbite's exact HDR/color/alpha conversion and leaves its UI and mask
// resource bindings untouched.

struct VS_OUTPUT
{
   float4 vertex : SV_Position;
   float4 texcoord0 : TEXCOORD0;
   float2 texcoord1 : TEXCOORD1;
};

#ifdef VERTEXSHADER
VS_OUTPUT main(uint vertex_idx : SV_VertexID)
{
   VS_OUTPUT output;
   const float2 texcoord = float2(vertex_idx & 1, vertex_idx >> 1);
   output.vertex = float4((texcoord.x - 0.5f) * 2.0f, -(texcoord.y - 0.5f) * 2.0f, 0.0f, 1.0f);
   output.texcoord0 = float4(texcoord, 0.0f, 1.0f);
   output.texcoord1 = texcoord;
   return output;
}
#endif
