#include "SceneConstants.hlsl"
#include "ClusterDagPackedVertex.hlsli"

#ifndef USE_SKINNING
#define USE_SKINNING 0
#endif

struct VSInput
{
    uint VertexId : SV_VertexID;
};


struct VSOutput
{
    float4 Position : SV_Position;
};

cbuffer DrawCommandConstants : register(b2)
{
    uint DrawIndexStart;
    uint DrawDataIndex;
};

VSOutput ShadowMapVS(VSInput Input)
{
    VSOutput Output;
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<ClusterDagPackedPosition> PackedPositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.y];
#if USE_SKINNING
    StructuredBuffer<uint4> JointBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.x];
    StructuredBuffer<float4> WeightBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.y];
    StructuredBuffer<row_major float4x4> BoneMatrices = ResourceDescriptorHeap[SkinningBindlessIndices.z];
    StructuredBuffer<float3> SkinnedPositionBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.w];
#endif

    uint vertexIndex = IndexBuffer[Input.VertexId + DrawIndexStart];
    float3 position = ClusterDagVertexPackingMode != 0u
        ? DecodeClusterDagPackedPosition(PackedPositionBuffer[vertexIndex])
        : PositionBuffer[vertexIndex];

#if USE_SKINNING
    uint4 joints = JointBuffer[vertexIndex];
    float4 weights = WeightBuffer[vertexIndex];
    row_major float4x4 skinMatrix =
        weights.x * BoneMatrices[joints.x] +
        weights.y * BoneMatrices[joints.y] +
        weights.z * BoneMatrices[joints.z] +
        weights.w * BoneMatrices[joints.w];
    position = SkinnedPositionBuffer[vertexIndex];
#endif

    float4 WorldPos = mul(float4(position, 1.0), World);
    Output.Position = mul(WorldPos, LightViewProjection);
    return Output;
}
