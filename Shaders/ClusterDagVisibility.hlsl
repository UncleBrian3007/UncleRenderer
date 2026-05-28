#include "SceneConstants.hlsl"
#include "Common.hlsli"
#include "ClusterDag/ClusterDagCommon.hlsl"
#include "ClusterDagPackedVertex.hlsli"

#ifndef CLUSTER_DAG_VISIBILITY_ALPHA_MASK
#define CLUSTER_DAG_VISIBILITY_ALPHA_MASK 0
#endif

struct VSInput
{
    uint VertexId : SV_VertexID;
};

struct VSOutput
{
    float4 Position : SV_Position;
#if CLUSTER_DAG_VISIBILITY_ALPHA_MASK
    float2 UV : TEXCOORD0;
    float ColorAlpha : TEXCOORD1;
#endif
};

cbuffer ClusterDagVisibilityDrawConstants : register(b2)
{
    uint DrawIndexStart;
    uint DrawDataIndex;
    uint Visibility64UavIndex;
    uint DrawDataVisibleEntryIndex;
    uint VisibleEntryBufferIndex;
    uint PageDataBufferIndex;
};

#if CLUSTER_DAG_VISIBILITY_ALPHA_MASK
SamplerState MaterialSampler : register(s0);

float2 LoadVisibilityGlobalClusterDagUv(uint vertexIndex)
{
    if (ClusterDagVertexPackingMode != 0u)
    {
        if (VertexBufferBindlessIndices.z != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedTexCoordBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.z];
            return DecodeClusterDagPackedUV(PackedTexCoordBuffer[vertexIndex]);
        }

        return ClusterDagPackedConstantUV.xy;
    }

    StructuredBuffer<float2> TexCoordBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.z];
    return TexCoordBuffer[vertexIndex];
}

float4 LoadVisibilityGlobalClusterDagColor(uint vertexIndex)
{
    if (ClusterDagVertexPackingMode != 0u)
    {
        if (ExtraBindlessIndices.x != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedColorBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.x];
            return DecodeClusterDagPackedColor(PackedColorBuffer[vertexIndex]);
        }

        return ClusterDagPackedConstantColor;
    }

    StructuredBuffer<float4> ColorBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.x];
    return ColorBuffer[vertexIndex];
}

float2 LoadVisibilityClusterDagUv(ClusterDagVisibleEntry visibleEntry, uint vertexIndex, ByteAddressBuffer PageData)
{
    if (ClusterDagVertexPackingMode != 0u)
    {
        uint packedUv = 0u;
        if (LoadClusterDagPagedPackedScalar(visibleEntry, vertexIndex, kClusterDagGpuPageHeaderPackedUvByteOffsetOffset, kClusterDagGpuPageHeaderPackedUvCountOffset, PageData, packedUv))
        {
            return DecodeClusterDagPackedUV(packedUv);
        }

        if (HasClusterDagPageData(visibleEntry))
        {
            return ClusterDagPackedConstantUV.xy;
        }
    }

    return LoadVisibilityGlobalClusterDagUv(vertexIndex);
}

float4 LoadVisibilityClusterDagColor(ClusterDagVisibleEntry visibleEntry, uint vertexIndex, ByteAddressBuffer PageData)
{
    if (ClusterDagVertexPackingMode != 0u)
    {
        uint packedColor = 0u;
        if (LoadClusterDagPagedPackedScalar(visibleEntry, vertexIndex, kClusterDagGpuPageHeaderPackedColorByteOffsetOffset, kClusterDagGpuPageHeaderPackedColorCountOffset, PageData, packedColor))
        {
            return DecodeClusterDagPackedColor(packedColor);
        }

        if (HasClusterDagPageData(visibleEntry))
        {
            return ClusterDagPackedConstantColor;
        }
    }

    return LoadVisibilityGlobalClusterDagColor(vertexIndex);
}
#endif

VSOutput ClusterDagVisibilityVS(VSInput Input)
{
    VSOutput Output;
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<ClusterDagPackedPosition> PackedPositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.y];
    StructuredBuffer<uint> DrawDataVisibleEntries = ResourceDescriptorHeap[DrawDataVisibleEntryIndex];
    StructuredBuffer<ClusterDagVisibleEntry> VisibleEntries = ResourceDescriptorHeap[VisibleEntryBufferIndex];

    float3 position = 0.0f.xxx;
    bool positionLoaded = false;
    bool allowGlobalFallback = true;
#if CLUSTER_DAG_VISIBILITY_ALPHA_MASK
    float2 uv = ClusterDagPackedConstantUV.xy;
    float colorAlpha = ClusterDagPackedConstantColor.a;
#endif
    const uint visibleEntryIndex = DrawDataVisibleEntries[DrawDataIndex];
    if (visibleEntryIndex != 0xffffffffu)
    {
        const ClusterDagVisibleEntry visibleEntry = VisibleEntries[visibleEntryIndex];
        const bool usePageData = PageDataBufferIndex != 0xffffffffu && HasClusterDagPageData(visibleEntry);
        ClusterDagDrawData drawData;
        drawData.StartIndex = DrawIndexStart;
        drawData.IndexCount = 0u;
        drawData.RangeIndex = 0u;
        drawData.RangeCommandStart = 0u;
        drawData.RangeCommandCount = 0u;
        drawData.DrawSectionIndex = 0u;
        uint pageVertexIndex = 0u;
        if (usePageData)
        {
            allowGlobalFallback = false;
            ByteAddressBuffer PageData = ResourceDescriptorHeap[PageDataBufferIndex];
            if (LoadClusterDagPagedVertexIndex(visibleEntry, drawData, Input.VertexId / 3u, Input.VertexId % 3u, PageData, pageVertexIndex))
            {
#if CLUSTER_DAG_VISIBILITY_ALPHA_MASK
                uv = LoadVisibilityClusterDagUv(visibleEntry, pageVertexIndex, PageData);
                colorAlpha = LoadVisibilityClusterDagColor(visibleEntry, pageVertexIndex, PageData).a;
#endif
                uint packedXy = 0u;
                uint packedZ = 0u;
                if (LoadClusterDagPagedPackedPositionWords(visibleEntry, pageVertexIndex, PageData, packedXy, packedZ))
                {
                    ClusterDagPackedPosition packedPosition;
                    packedPosition.XY = packedXy;
                    packedPosition.Z = packedZ;
                    position = DecodeClusterDagPackedPosition(packedPosition);
                    positionLoaded = true;
                }
            }
        }
    }

    if (!positionLoaded && allowGlobalFallback)
    {
        const uint vertexIndex = IndexBuffer[Input.VertexId + DrawIndexStart];
        position = ClusterDagVertexPackingMode != 0u
            ? DecodeClusterDagPackedPosition(PackedPositionBuffer[vertexIndex])
            : PositionBuffer[vertexIndex];
#if CLUSTER_DAG_VISIBILITY_ALPHA_MASK
        uv = LoadVisibilityGlobalClusterDagUv(vertexIndex);
        colorAlpha = LoadVisibilityGlobalClusterDagColor(vertexIndex).a;
#endif
    }
    const float4 worldPosition = mul(float4(position, 1.0f), World);
    const float4 viewPosition = mul(worldPosition, View);
    Output.Position = mul(viewPosition, Projection);
#if CLUSTER_DAG_VISIBILITY_ALPHA_MASK
    Output.UV = uv;
    Output.ColorAlpha = colorAlpha;
#endif
    return Output;
}

#if !CLUSTER_DAG_VISIBILITY_ALPHA_MASK
[earlydepthstencil]
#endif
void ClusterDagVisibilityPS(
    VSOutput Input,
    uint PrimitiveId : SV_PrimitiveID)
{
    StructuredBuffer<uint> DrawDataVisibleEntries = ResourceDescriptorHeap[DrawDataVisibleEntryIndex];
    RWTexture2D<uint64_t> Visibility64 = ResourceDescriptorHeap[Visibility64UavIndex];

    const uint visibleEntryIndex = DrawDataVisibleEntries[DrawDataIndex];
    if (visibleEntryIndex == 0xffffffffu || PrimitiveId >= 128u)
    {
        return;
    }

#if CLUSTER_DAG_VISIBILITY_ALPHA_MASK
    float alpha = BaseColorAlpha * Input.ColorAlpha;
    if (MaterialTextureIndices0.x != 0xffffffffu)
    {
        const float2 baseUV = ApplyTextureTransform(Input.UV, BaseColorTransformOffsetScale, BaseColorTransformRotation);
        Texture2D AlbedoTexture = ResourceDescriptorHeap[MaterialTextureIndices0.x];
        alpha *= AlbedoTexture.Sample(MaterialSampler, baseUV).a;
    }

    if (alpha < AlphaCutoff)
    {
        clip(alpha - AlphaCutoff);
        return;
    }
#endif

    const uint pixelValue = ((visibleEntryIndex + 1u) << 7u) | PrimitiveId;
    const uint depthInt = asuint(saturate(Input.Position.z));
    const uint64_t packedPixel = (uint64_t(depthInt) << 32u) | uint64_t(pixelValue);
    uint64_t previousValue = 0u;
    InterlockedMax(Visibility64[uint2(Input.Position.xy)], packedPixel, previousValue);
}
