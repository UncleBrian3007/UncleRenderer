#ifndef RESTIR_GI_SH_HLSLI
#define RESTIR_GI_SH_HLSLI

static const float SH_L0 = 0.282095f;
static const float SH_L1 = 0.488603f;

struct FRestirGiPackedSh
{
    float3 Irradiance;
    float3 DominantDirection;
};

float RestirGiLuminance(float3 Value)
{
    return dot(Value, float3(0.2126f, 0.7152f, 0.0722f));
}

uint RestirGiEncodeDirection16x2(float3 Direction)
{
    float3 N = normalize(Direction);
    N /= (abs(N.x) + abs(N.y) + abs(N.z) + 1e-6f);

    float2 Enc = N.xy;
    if (N.z < 0.0f)
    {
        const float2 SignVec = lerp(-1.0f.xx, 1.0f.xx, step(0.0f.xx, Enc));
        Enc = (1.0f - abs(Enc.yx)) * SignVec;
    }

    Enc = Enc * 0.5f + 0.5f;
    uint2 Packed = (uint2)round(saturate(Enc) * 65535.0f);
    return (Packed.x & 0xFFFFu) | ((Packed.y & 0xFFFFu) << 16u);
}

float3 RestirGiDecodeDirection16x2(uint Packed)
{
    float2 Enc = float2(Packed & 0xFFFFu, Packed >> 16u) / 65535.0f;
    Enc = Enc * 2.0f - 1.0f;

    float3 N = float3(Enc.xy, 1.0f - abs(Enc.x) - abs(Enc.y));
    float2 T = saturate(-N.zz);
    N.xy += lerp(T, -T, step(0.0f.xx, N.xy));
    return normalize(N);
}

FRestirGiPackedSh RestirGiProjectSh(float3 Irradiance, float3 Direction)
{
    FRestirGiPackedSh Sh;
    const float3 D = normalize(Direction);
    const float DirectionWeight = SH_L0 + SH_L1 * max(0.0f, D.z);
    Sh.Irradiance = max(Irradiance, 0.0f.xxx) * DirectionWeight;
    Sh.DominantDirection = D;
    return Sh;
}

uint4 RestirGiPackSh(FRestirGiPackedSh Sh)
{
    return uint4(asuint(Sh.Irradiance.x), asuint(Sh.Irradiance.y), asuint(Sh.Irradiance.z), RestirGiEncodeDirection16x2(Sh.DominantDirection));
}

FRestirGiPackedSh RestirGiUnpackSh(uint4 Packed)
{
    FRestirGiPackedSh Sh;
    Sh.Irradiance = float3(asfloat(Packed.x), asfloat(Packed.y), asfloat(Packed.z));
    Sh.DominantDirection = RestirGiDecodeDirection16x2(Packed.w);
    return Sh;
}

float3 RestirGiUnprojectIrradiance(FRestirGiPackedSh Sh, float3 SurfaceNormal)
{
    const float NdotL = saturate(dot(normalize(SurfaceNormal), normalize(Sh.DominantDirection)));
    const float ReconstructWeight = SH_L0 + SH_L1 * NdotL;
    return max(Sh.Irradiance * ReconstructWeight, 0.0f.xxx);
}

float RestirGiShVariance(FRestirGiPackedSh Sh)
{
    const float Energy = max(RestirGiLuminance(Sh.Irradiance), 0.0f);
    const float Directional = 1.0f - saturate(abs(Sh.DominantDirection.z));
    return saturate(Energy * 0.1f + Directional * 0.25f);
}

#endif
