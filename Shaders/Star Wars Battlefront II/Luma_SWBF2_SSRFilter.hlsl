// Spatial reconstruction of native sparse SSR, before lighting and DLSS.
Texture2D<float3> Reflection : register(t0);
Texture2D<float> Coverage : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float4> Surface : register(t3);
Texture2D<float4> Material : register(t4);
RWTexture2D<float3> FilteredReflection : register(u0);
RWTexture2D<float> FilteredCoverage : register(u1);

// An 8x8 output tile plus its two-pixel border. Decode each input once;
// neighboring output pixels share the same radiance and surface samples.
groupshared float4 TileRadiance[144];
groupshared float4 TileNormalSmoothness[144];
groupshared float TileDepth[144];
groupshared uint TileBasis[144];

float3 DecodeNormal(float2 encoded)
{
    float2 n = encoded * 2.0 - 1.0;
    return normalize(float3(n, sqrt(saturate(1.0 - dot(n, n)))));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID,
    uint3 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    uint width, height;
    Reflection.GetDimensions(width, height);
    for (uint index = lane; index < 144; index += 64)
    {
        int2 q = clamp(int2(gid.xy * 8) + int2(index % 12, index / 12) - 2,
            int2(0, 0), int2(width - 1, height - 1));
        float4 surface = Surface.Load(int3(q, 0));
        TileNormalSmoothness[index] = float4(DecodeNormal(surface.xy), surface.z);
        TileDepth[index] = Depth.Load(int3(q, 0));
        TileBasis[index] = (uint)round(Material.Load(int3(q, 0)).w * 6.0);
        TileRadiance[index] = float4(Reflection.Load(int3(q, 0)), saturate(Coverage.Load(int3(q, 0))));
    }
    // Screen-edge lanes still load the halo and participate in this barrier.
    GroupMemoryBarrierWithGroupSync();
    if (tid.x >= width || tid.y >= height) return;
    int2 p = int2(tid.xy);
    uint center = (local.y + 2) * 12 + local.x + 2;
    float z = TileDepth[center];
    float4 surface = TileNormalSmoothness[center];
    float3 normal = surface.xyz;
    uint basis = TileBasis[center];
    float roughness = 1.0 - surface.w;
    float sigma = lerp(0.85, 1.5, saturate(roughness));
    float3 weightedColor = 0.0;
    float weightedCoverage = 0.0, totalWeight = 0.0;
    // A compact kernel removes the coarse ray-grid boundaries. Reject other
    // surfaces by depth, encoded normal basis, normal angle and roughness.
    [loop] for (int y = -2; y <= 2; ++y)
    [loop] for (int x = -2; x <= 2; ++x)
    {
        uint q = (uint)((int)center + y * 12 + x);
        float qz = TileDepth[q];
        float4 qs = TileNormalSmoothness[q];
        uint qb = TileBasis[q];
        float depthDifference = abs(qz - z) / max(max(abs(z), abs(qz)), 1e-6);
        float angle = dot(normal, qs.xyz);
        float roughnessDifference = abs(qs.w - surface.w);
        if (qb != basis || depthDifference > 0.02 || angle < 0.9 || roughnessDifference > 0.1) continue;
        float w = exp2(-float(x*x + y*y) / (2.0 * sigma * sigma)
            - depthDifference * 128.0 - (1.0 - saturate(angle)) * 32.0
            - roughnessDifference * 32.0);
        float a = TileRadiance[q].w;
        float3 c = TileRadiance[q].xyz;
        if (!all(isfinite(c))) continue;
        // Native SSR color already includes coverage: the lighting pass adds it
        // directly, while coverage attenuates the environment fallback.
        // Keep the pair premultiplied; do not apply coverage a second time.
        weightedColor += max(c, 0.0) * w;
        weightedCoverage += a * w;
        totalWeight += w;
    }
    FilteredReflection[p] = totalWeight > 1e-8 ? weightedColor / totalWeight : 0.0;
    FilteredCoverage[p] = totalWeight > 1e-8 ? weightedCoverage / totalWeight : 0.0;
}
