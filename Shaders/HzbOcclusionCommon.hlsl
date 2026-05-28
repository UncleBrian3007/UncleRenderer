#ifndef HZB_OCCLUSION_COMMON_HLSL
#define HZB_OCCLUSION_COMMON_HLSL

bool IsSphereOccludedByHZB(
    float3 center,
    float radius,
    float4x4 viewProjection,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    uint hzbMipCount)
{
    if (hzbWidth == 0u || hzbHeight == 0u || hzbMipCount == 0u)
    {
        return false;
    }

    float3 boundsMin = center - radius;
    float3 boundsMax = center + radius;

    float3 corners[8] =
    {
        float3(boundsMin.x, boundsMin.y, boundsMin.z),
        float3(boundsMax.x, boundsMin.y, boundsMin.z),
        float3(boundsMin.x, boundsMax.y, boundsMin.z),
        float3(boundsMax.x, boundsMax.y, boundsMin.z),
        float3(boundsMin.x, boundsMin.y, boundsMax.z),
        float3(boundsMax.x, boundsMin.y, boundsMax.z),
        float3(boundsMin.x, boundsMax.y, boundsMax.z),
        float3(boundsMax.x, boundsMax.y, boundsMax.z)
    };

    float2 minUv = float2(1.0f, 1.0f);
    float2 maxUv = float2(0.0f, 0.0f);
    float maxDepth = 0.0f;

    bool anyBehind = false;

    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        float4 clip = mul(float4(corners[i], 1.0f), viewProjection);
        if (clip.w <= 0.0f)
        {
            anyBehind = true;
            break;
        }

        float3 ndc = clip.xyz / clip.w;
        float2 uv;
        uv.x = ndc.x * 0.5f + 0.5f;
        uv.y = 1.0f - (ndc.y * 0.5f + 0.5f);

        minUv = min(minUv, uv);
        maxUv = max(maxUv, uv);
        maxDepth = max(maxDepth, ndc.z);
    }

    if (anyBehind)
    {
        return false;
    }

    if (maxUv.x < 0.0f || maxUv.y < 0.0f || minUv.x > 1.0f || minUv.y > 1.0f)
    {
        return false;
    }

    minUv = saturate(minUv);
    maxUv = saturate(maxUv);

    const float2 extent = maxUv - minUv;
    const float2 pixelSize = extent * float2(hzbWidth, hzbHeight);
    const float maxDim = max(pixelSize.x, pixelSize.y);
    uint mipLevel = 0u;
    if (maxDim > 1.0f)
    {
        mipLevel = (uint)clamp(floor(log2(maxDim)), 0.0f, (float)(hzbMipCount - 1u));
    }

    const uint mipWidth = max(1u, hzbWidth >> mipLevel);
    const uint mipHeight = max(1u, hzbHeight >> mipLevel);

    uint2 minCoord = uint2(minUv * float2(mipWidth, mipHeight));
    uint2 maxCoord = uint2(maxUv * float2(mipWidth, mipHeight));
    minCoord = min(minCoord, uint2(mipWidth - 1u, mipHeight - 1u));
    maxCoord = min(maxCoord, uint2(mipWidth - 1u, mipHeight - 1u));

    float hzbDepth = 1.0f;
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(minCoord, mipLevel)).x);
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(maxCoord.x, minCoord.y, mipLevel)).x);
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(minCoord.x, maxCoord.y, mipLevel)).x);
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(maxCoord, mipLevel)).x);

    return maxDepth < hzbDepth;
}

#endif
