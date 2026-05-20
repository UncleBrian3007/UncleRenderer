#include "SceneConstants.hlsl"
#include "ClusterDag/ClusterDagCommon.hlsl"
#include "ClusterDagPackedVertex.hlsli"

struct VSInput
{
    uint VertexId : SV_VertexID;
};

struct VSOutput
{
    float4 Position : SV_Position;
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
        drawData.ModelIndex = 0u;
        uint pageVertexIndex = 0u;
        if (usePageData)
        {
            allowGlobalFallback = false;
            ByteAddressBuffer PageData = ResourceDescriptorHeap[PageDataBufferIndex];
            if (LoadClusterDagPagedVertexIndex(visibleEntry, drawData, Input.VertexId / 3u, Input.VertexId % 3u, PageData, pageVertexIndex))
            {
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
    }
    const float4 worldPosition = mul(float4(position, 1.0f), World);
    const float4 viewPosition = mul(worldPosition, View);
    Output.Position = mul(viewPosition, Projection);
    return Output;
}

[earlydepthstencil]
void ClusterDagVisibilityPS(
    float4 Position : SV_Position,
    uint PrimitiveId : SV_PrimitiveID)
{
    StructuredBuffer<uint> DrawDataVisibleEntries = ResourceDescriptorHeap[DrawDataVisibleEntryIndex];
    RWTexture2D<uint64_t> Visibility64 = ResourceDescriptorHeap[Visibility64UavIndex];

    const uint visibleEntryIndex = DrawDataVisibleEntries[DrawDataIndex];
    if (visibleEntryIndex == 0xffffffffu || PrimitiveId >= 128u)
    {
        return;
    }

    const uint pixelValue = ((visibleEntryIndex + 1u) << 7u) | PrimitiveId;
    const uint depthInt = asuint(saturate(Position.z));
    const uint64_t packedPixel = (uint64_t(depthInt) << 32u) | uint64_t(pixelValue);
    uint64_t previousValue = 0u;
    InterlockedMax(Visibility64[uint2(Position.xy)], packedPixel, previousValue);
}
