#include "ClusterDagGeometryFetch.hlsl"

#ifndef CLUSTER_DAG_SW_RASTER_HZB_REJECT
#define CLUSTER_DAG_SW_RASTER_HZB_REJECT 0
#endif

cbuffer ClusterDagSoftwareRasterConstants : register(b0)
{
    uint Visibility64UavIndex;
    uint VisibleEntryBufferIndex;
    uint SwVisibleEntryIndexBufferIndex;
    uint VisibleEntryCounterBufferIndex;
    uint ClusterDataBufferIndex;
    uint DrawDataBufferIndex;
    uint SceneDataBufferIndex;
    uint DepthTextureIndex;
    uint ViewportWidth;
    uint ViewportHeight;
    uint HZBTextureIndex;
    uint HZBWidth;
    uint HZBHeight;
    uint HZBMipCount;
    uint PageDataBufferIndex;
};

#if CLUSTER_DAG_SW_RASTER_HZB_REJECT
groupshared uint GroupClusterOccludedByHZB;
#endif

float2 ClipToPixel(float4 clipPosition)
{
    const float2 ndc = clipPosition.xy / max(abs(clipPosition.w), 1e-6f);
    return float2(
        (ndc.x * 0.5f + 0.5f) * ViewportWidth,
        (0.5f - ndc.y * 0.5f) * ViewportHeight);
}

float EdgeFunction(float2 a, float2 b, float2 c)
{
    return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

#if CLUSTER_DAG_SW_RASTER_HZB_REJECT
float4 WorldToClip(float3 worldPosition, ClusterDagResolveSceneData sceneData)
{
    return mul(mul(float4(worldPosition, 1.0f), sceneData.View), sceneData.Projection);
}

bool IsClusterOccludedByHZB(ClusterDagClusterData cluster, ClusterDagResolveSceneData sceneData, Texture2D<float2> HZBTexture)
{
    const float3 center = cluster.Bounds.xyz;
    const float radius = cluster.Bounds.w;
    if (radius <= 0.0f || HZBMipCount == 0u)
    {
        return false;
    }

    float2 minUv = float2(1.0f, 1.0f);
    float2 maxUv = float2(0.0f, 0.0f);
    float minDepth = 1.0f;
    float maxDepth = 0.0f;

    [unroll]
    for (uint cornerIndex = 0u; cornerIndex < 8u; ++cornerIndex)
    {
        const float3 cornerOffset = float3(
            (cornerIndex & 1u) != 0u ? radius : -radius,
            (cornerIndex & 2u) != 0u ? radius : -radius,
            (cornerIndex & 4u) != 0u ? radius : -radius);
        const float4 clip = WorldToClip(center + cornerOffset, sceneData);
        if (clip.w <= 1e-6f)
        {
            return false;
        }

        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = float2(
            ndc.x * 0.5f + 0.5f,
            0.5f - ndc.y * 0.5f);
        minUv = min(minUv, uv);
        maxUv = max(maxUv, uv);
        minDepth = min(minDepth, ndc.z);
        maxDepth = max(maxDepth, ndc.z);
    }

    if (maxUv.x < 0.0f || maxUv.y < 0.0f || minUv.x > 1.0f || minUv.y > 1.0f)
    {
        return false;
    }
    if (maxDepth < 0.0f || minDepth > 1.0f)
    {
        return false;
    }

    minUv = saturate(minUv);
    maxUv = saturate(maxUv);

    const float2 pixelSize = (maxUv - minUv) * float2(HZBWidth, HZBHeight);
    const float maxDim = max(pixelSize.x, pixelSize.y);
    uint mipLevel = 0u;
    if (maxDim > 1.0f)
    {
        mipLevel = (uint)clamp(floor(log2(maxDim)), 0.0f, (float)(HZBMipCount - 1u));
    }

    const uint mipWidth = max(1u, HZBWidth >> mipLevel);
    const uint mipHeight = max(1u, HZBHeight >> mipLevel);
    uint2 minCoord = min(uint2(minUv * float2(mipWidth, mipHeight)), uint2(mipWidth - 1u, mipHeight - 1u));
    uint2 maxCoord = min(uint2(maxUv * float2(mipWidth, mipHeight)), uint2(mipWidth - 1u, mipHeight - 1u));

    float hzbMinDepth = 1.0f;
    hzbMinDepth = min(hzbMinDepth, HZBTexture.Load(int3(minCoord, mipLevel)).x);
    hzbMinDepth = min(hzbMinDepth, HZBTexture.Load(int3(maxCoord.x, minCoord.y, mipLevel)).x);
    hzbMinDepth = min(hzbMinDepth, HZBTexture.Load(int3(minCoord.x, maxCoord.y, mipLevel)).x);
    hzbMinDepth = min(hzbMinDepth, HZBTexture.Load(int3(maxCoord, mipLevel)).x);

    return saturate(maxDepth) < hzbMinDepth;
}
#endif

void RasterizeTriangle(
    uint visibleEntryIndex,
    uint localTriIndex,
    ClusterDagResolveSceneData sceneData,
    ClusterDagVisibleEntry visibleEntry,
    ClusterDagDrawData drawData,
    ByteAddressBuffer PageData,
    RWTexture2D<uint64_t> Visibility64,
    Texture2D<float> DepthTexture)
{
    uint vertexIndex0 = 0u;
    uint vertexIndex1 = 0u;
    uint vertexIndex2 = 0u;
    if (!LoadClusterDagTriangleIndices(sceneData, visibleEntry, drawData, localTriIndex, PageData, vertexIndex0, vertexIndex1, vertexIndex2))
    {
        return;
    }

    const float3 localPosition0 = LoadClusterDagPosition(sceneData, visibleEntry, vertexIndex0, PageData);
    const float3 localPosition1 = LoadClusterDagPosition(sceneData, visibleEntry, vertexIndex1, PageData);
    const float3 localPosition2 = LoadClusterDagPosition(sceneData, visibleEntry, vertexIndex2, PageData);
    const float4 view0 = mul(mul(float4(localPosition0, 1.0f), sceneData.World), sceneData.View);
    const float4 view1 = mul(mul(float4(localPosition1, 1.0f), sceneData.World), sceneData.View);
    const float4 view2 = mul(mul(float4(localPosition2, 1.0f), sceneData.World), sceneData.View);
    const float4 clip0 = mul(view0, sceneData.Projection);
    const float4 clip1 = mul(view1, sceneData.Projection);
    const float4 clip2 = mul(view2, sceneData.Projection);
    if (clip0.w <= 1e-6f || clip1.w <= 1e-6f || clip2.w <= 1e-6f)
    {
        return;
    }

    const float3 ndc0 = clip0.xyz / clip0.w;
    const float3 ndc1 = clip1.xyz / clip1.w;
    const float3 ndc2 = clip2.xyz / clip2.w;
    if (max(max(ndc0.x, ndc1.x), ndc2.x) < -1.0f || min(min(ndc0.x, ndc1.x), ndc2.x) > 1.0f ||
        max(max(ndc0.y, ndc1.y), ndc2.y) < -1.0f || min(min(ndc0.y, ndc1.y), ndc2.y) > 1.0f ||
        max(max(ndc0.z, ndc1.z), ndc2.z) < 0.0f || min(min(ndc0.z, ndc1.z), ndc2.z) > 1.0f)
    {
        return;
    }

    const float2 p0 = ClipToPixel(clip0);
    const float2 p1 = ClipToPixel(clip1);
    const float2 p2 = ClipToPixel(clip2);
    const float area = EdgeFunction(p0, p1, p2);
    if (abs(area) < 1e-6f)
    {
        return;
    }

    const bool useDoubleSided = (sceneData.ClusterDagMaterialPipelineKey & kClusterDagPipelineKeyDoubleSidedMask) != 0u;
    if (!useDoubleSided && area < 0.0f)
    {
        return;
    }

    const int2 minPixel = max(int2(floor(min(min(p0, p1), p2))), int2(0, 0));
    const int2 maxPixel = min(int2(ceil(max(max(p0, p1), p2))), int2(int(ViewportWidth) - 1, int(ViewportHeight) - 1));
    const bool positiveArea = area > 0.0f;
    const uint pixelValue = ((visibleEntryIndex + 1u) << 7u) | localTriIndex;

    [loop]
    for (int y = minPixel.y; y <= maxPixel.y; ++y)
    {
        [loop]
        for (int x = minPixel.x; x <= maxPixel.x; ++x)
        {
            const float2 pixelCenter = float2(x, y) + 0.5f;
            const float e0 = EdgeFunction(p1, p2, pixelCenter);
            const float e1 = EdgeFunction(p2, p0, pixelCenter);
            const float e2 = EdgeFunction(p0, p1, pixelCenter);
            if (positiveArea ? (e0 < 0.0f || e1 < 0.0f || e2 < 0.0f) : (e0 > 0.0f || e1 > 0.0f || e2 > 0.0f))
            {
                continue;
            }

            const float b0 = e0 / area;
            const float b1 = e1 / area;
            const float b2 = e2 / area;
            const float depth = saturate(b0 * ndc0.z + b1 * ndc1.z + b2 * ndc2.z);
            const float sceneDepth = DepthTexture.Load(int3(x, y, 0));
            if (depth < sceneDepth)
            {
                continue;
            }

            const uint depthInt = asuint(depth);
            const uint64_t packedPixel = (uint64_t(depthInt) << 32u) | uint64_t(pixelValue);
            const uint2 pixelCoord = uint2(x, y);
            const uint64_t currentPixel = Visibility64[pixelCoord];
            if (packedPixel <= currentPixel)
            {
                continue;
            }

            uint64_t previousValue = 0u;
            InterlockedMax(Visibility64[pixelCoord], packedPixel, previousValue);
        }
    }
}

[numthreads(64, 1, 1)]
void RasterizeClusterSWCS(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint swListIndex = groupId.x;
    const uint laneIndex = groupThreadId.x;
    RWTexture2D<uint64_t> Visibility64 = ResourceDescriptorHeap[Visibility64UavIndex];
    StructuredBuffer<ClusterDagVisibleEntry> VisibleEntries = ResourceDescriptorHeap[VisibleEntryBufferIndex];
    StructuredBuffer<uint> SwVisibleEntryIndices = ResourceDescriptorHeap[SwVisibleEntryIndexBufferIndex];
    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterDataBufferIndex];
    StructuredBuffer<ClusterDagDrawData> DrawDatas = ResourceDescriptorHeap[DrawDataBufferIndex];
    StructuredBuffer<ClusterDagResolveSceneData> SceneDatas = ResourceDescriptorHeap[SceneDataBufferIndex];
    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthTextureIndex];
    ByteAddressBuffer PageData = ResourceDescriptorHeap[PageDataBufferIndex];

    const uint visibleEntryIndex = SwVisibleEntryIndices[swListIndex];
    const ClusterDagVisibleEntry visibleEntry = VisibleEntries[visibleEntryIndex];
    ClusterDagVisibleEntry geometryEntry = visibleEntry;
    const ClusterDagClusterData cluster = Clusters[visibleEntry.ClusterIndex];
    ClusterDagDrawData drawData = DrawDatas[visibleEntry.DrawDataIndex];
    ClusterDagDrawData pagedDrawData;
    if (TryLoadClusterDagVisibleEntryDrawData(visibleEntry, visibleEntry.DrawDataIndex, PageData, pagedDrawData))
    {
        drawData = pagedDrawData;
    }
    else
    {
        geometryEntry.PageDataBase = 0xffffffffu;
    }
    const ClusterDagResolveSceneData sceneData = SceneDatas[drawData.ModelIndex];
    const uint triangleCount = min(cluster.TriangleCount, 128u);

#if CLUSTER_DAG_SW_RASTER_HZB_REJECT
    if (laneIndex == 0u)
    {
        Texture2D<float2> HZBTexture = ResourceDescriptorHeap[HZBTextureIndex];
        GroupClusterOccludedByHZB = IsClusterOccludedByHZB(cluster, sceneData, HZBTexture) ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();
    if (GroupClusterOccludedByHZB != 0u)
    {
        return;
    }
#endif

    [loop]
    for (uint localTriIndex = laneIndex; localTriIndex < triangleCount; localTriIndex += 64u)
    {
        RasterizeTriangle(visibleEntryIndex, localTriIndex, sceneData, geometryEntry, drawData, PageData, Visibility64, DepthTexture);
    }
}
