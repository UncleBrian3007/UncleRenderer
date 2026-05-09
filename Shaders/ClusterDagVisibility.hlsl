#include "SceneConstants.hlsl"
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
};

VSOutput ClusterDagVisibilityVS(VSInput Input)
{
    VSOutput Output;
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<ClusterDagPackedPosition> PackedPositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.y];

    const uint vertexIndex = IndexBuffer[Input.VertexId + DrawIndexStart];
    const float3 position = ClusterDagVertexPackingMode != 0u
        ? DecodeClusterDagPackedPosition(PackedPositionBuffer[vertexIndex])
        : PositionBuffer[vertexIndex];
    const float4 worldPosition = mul(float4(position, 1.0f), World);
    const float4 viewPosition = mul(worldPosition, View);
    Output.Position = mul(viewPosition, Projection);
    return Output;
}

struct PSOutput
{
    uint2 Visibility : SV_Target0;
    float2 Barycentrics : SV_Target1;
};

PSOutput ClusterDagVisibilityPS(
    uint PrimitiveId : SV_PrimitiveID,
    float3 Barycentrics : SV_Barycentrics)
{
    PSOutput Output;
    Output.Visibility = uint2(DrawDataIndex + 1u, PrimitiveId);
    Output.Barycentrics = Barycentrics.yz;
    return Output;
}
