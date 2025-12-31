cbuffer CullingConstants : register(b0)
{
    float4 FrustumPlanes[6];
    float4x4 ViewProjection;
    uint ModelCount;
    uint HZBEnabled;
    uint HZBMipCount;
    uint HZBWidth;
    uint HZBHeight;
};

StructuredBuffer<float4> ModelBounds : register(t0);
Texture2D<float> HZBTexture : register(t1);
RWByteAddressBuffer IndirectArgs : register(u0);

static const uint kCommandStride = 64;
static const uint kInstanceCountOffset = 44;

bool IsAabbVisible(float3 boundsMin, float3 boundsMax)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = FrustumPlanes[i];
        float3 positiveVertex = float3(
            plane.x >= 0.0f ? boundsMax.x : boundsMin.x,
            plane.y >= 0.0f ? boundsMax.y : boundsMin.y,
            plane.z >= 0.0f ? boundsMax.z : boundsMin.z);

        if (dot(plane.xyz, positiveVertex) + plane.w < 0.0f)
        {
            return false;
        }
    }
    return true;
}

float4 ProjectToClip(float3 position)
{
    return mul(float4(position, 1.0f), ViewProjection);
}

bool IsOccluded(float3 boundsMin, float3 boundsMax)
{
    if (HZBEnabled == 0 || HZBWidth == 0 || HZBHeight == 0 || HZBMipCount == 0)
    {
        return false;
    }

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
    bool hasValidCorner = false;

    bool anyBehind = false;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        float4 clip = ProjectToClip(corners[i]);
        if (clip.w <= 0.0f)
        {
            anyBehind = true;
            break;
        }

        float3 ndc = clip.xyz / clip.w;
        float2 uv;
        uv.x = ndc.x * 0.5f + 0.5f;
        uv.y = 1 - (ndc.y * 0.5f + 0.5f);

        minUv = min(minUv, uv);
        maxUv = max(maxUv, uv);
        maxDepth = max(maxDepth, ndc.z);
    }

    if (anyBehind || !hasValidCorner)
    {
        return false;
    }

    if (maxUv.x < 0.0f || maxUv.y < 0.0f || minUv.x > 1.0f || minUv.y > 1.0f)
    {
        return false;
    }

    minUv = saturate(minUv);
    maxUv = saturate(maxUv);

    float2 extent = maxUv - minUv;
    float2 pixelSize = extent * float2(HZBWidth, HZBHeight);
    float maxDim = max(pixelSize.x, pixelSize.y);
    uint mipLevel = 0;
    if (maxDim > 1.0f)
    {
        mipLevel = (uint)clamp(floor(log2(maxDim)), 0.0f, (float)(HZBMipCount - 1));
    }

    uint mipWidth = max(1u, HZBWidth >> mipLevel);
    uint mipHeight = max(1u, HZBHeight >> mipLevel);

    uint2 minCoord = uint2(minUv * float2(mipWidth, mipHeight));
    uint2 maxCoord = uint2(maxUv * float2(mipWidth, mipHeight));
    minCoord = min(minCoord, uint2(mipWidth - 1, mipHeight - 1));
    maxCoord = min(maxCoord, uint2(mipWidth - 1, mipHeight - 1));

    float hzbDepth = 1.0f;
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(minCoord, mipLevel)));
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(maxCoord.x, minCoord.y, mipLevel)));
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(minCoord.x, maxCoord.y, mipLevel)));
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(maxCoord, mipLevel)));

    return maxDepth < hzbDepth;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= ModelCount)
    {
        return;
    }

    uint boundsIndex = index * 2;
    float3 boundsMin = ModelBounds[boundsIndex].xyz;
    float3 boundsMax = ModelBounds[boundsIndex + 1].xyz;
    bool visible = IsAabbVisible(boundsMin, boundsMax);
    if (visible && HZBEnabled != 0)
    {
        visible = !IsOccluded(boundsMin, boundsMax);
    }

    uint baseOffset = index * kCommandStride + kInstanceCountOffset;
    IndirectArgs.Store(baseOffset, visible ? 1u : 0u);
}
