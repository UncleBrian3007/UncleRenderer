#ifndef OCTAHEDRAL_ENCODING_HLSLI
#define OCTAHEDRAL_ENCODING_HLSLI

uint EncodeOctahedral16x2(float3 V)
{
    float3 N = normalize(V);
    N /= (abs(N.x) + abs(N.y) + abs(N.z) + 1e-6f);
    float2 Enc = N.xy;
    if (N.z < 0.0f)
    {
        const float2 SignVec = lerp(-1.0f.xx, 1.0f.xx, step(0.0f.xx, Enc));
        Enc = (1.0f - abs(Enc.yx)) * SignVec;
    }

    Enc = Enc * 0.5f + 0.5f;
    const uint2 Packed = (uint2)round(saturate(Enc) * 65535.0f);
    return (Packed.x & 0xFFFFu) | ((Packed.y & 0xFFFFu) << 16u);
}

float3 DecodeOctahedral16x2(uint Packed)
{
    float2 Enc = float2(Packed & 0xFFFFu, Packed >> 16u) / 65535.0f;
    Enc = Enc * 2.0f - 1.0f;

    float3 N = float3(Enc.xy, 1.0f - abs(Enc.x) - abs(Enc.y));
    const float2 T = saturate(-N.zz);
    N.xy += lerp(T, -T, step(0.0f.xx, N.xy));
    return normalize(N);
}

#endif
