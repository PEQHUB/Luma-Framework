// Exact native ScreenSpaceRaytraceConstants patch; resource is a private full
// ranged-slice copy. Never writes the game's original constant buffer.
globallycoherent RWByteAddressBuffer PrivateConstants : register(u0);
#ifndef BF2_SSR_CONSTANT_COUNT
#define BF2_SSR_CONSTANT_COUNT 73
#endif
cbuffer NativeConstants : register(b0) { uint4 NativeWords[BF2_SSR_CONSTANT_COUNT]; };
cbuffer PatchParams : register(b13)
{
    uint BaseBytes;
    uint PassKind;
    float ThicknessScale;
    float ResolveSamplesScale;
    float ImportanceScale;
    float ResponsivenessScale;
    uint2 Reserved;
};

bool PositiveFinite(float v) { return v > 0.0 && isfinite(v); }
[numthreads(80, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // Read only the exact effective shader range, never the surrounding ring
    // allocation that the engine can update with WRITE_NO_OVERWRITE.
    [branch] if(tid.x < BF2_SSR_CONSTANT_COUNT)
        PrivateConstants.Store4(tid.x * 16u, NativeWords[tid.x]);
    else
        PrivateConstants.Store4(tid.x * 16u, uint4(0,0,0,0));
    DeviceMemoryBarrierWithGroupSync();
    if(tid.x != 0u) return;
    if (PassKind == 1u)
    {
        float start = asfloat(PrivateConstants.Load(BaseBytes + 672u));
        float inverseWidth = asfloat(PrivateConstants.Load(BaseBytes + 684u));
        // Native confidence: 1-saturate((abs(delta)-start)*inverseWidth).
        // Scale the entire distance ramp coherently, preserving zero starts.
        if (isfinite(start) && start >= 0.0 && PositiveFinite(inverseWidth)
            && isfinite(start * ThicknessScale) && isfinite(inverseWidth / ThicknessScale))
        {
            PrivateConstants.Store(BaseBytes + 672u, asuint(start * ThicknessScale));
            PrivateConstants.Store(BaseBytes + 684u, asuint(inverseWidth / ThicknessScale));
        }
    }
    else if (PassKind == 2u)
    {
        uint samples = PrivateConstants.Load(BaseBytes + 868u);
        // Values beyond the validated conservative work cap remain native.
        if (samples >= 1u && samples <= 64u)
            PrivateConstants.Store(BaseBytes + 868u,
                clamp((uint)round(samples * ResolveSamplesScale), 1u, 64u));
        float bias = asfloat(PrivateConstants.Load(BaseBytes + 856u));
        if (isfinite(bias) && bias >= 0.0 && bias <= 1.0)
            PrivateConstants.Store(BaseBytes + 856u, asuint(saturate(bias * ImportanceScale)));
    }
    else if (PassKind == 3u)
    {
        float response = asfloat(PrivateConstants.Load(BaseBytes + 864u));
        if (PositiveFinite(response) && isfinite(response * ResponsivenessScale))
            PrivateConstants.Store(BaseBytes + 864u, asuint(response * ResponsivenessScale));
        // History validity at byte 872 belongs to the native temporal producer.
        // Response tuning must preserve it, including a game's explicit reset.
    }
}
