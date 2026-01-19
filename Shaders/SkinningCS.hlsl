#include "SceneConstants.hlsl"

cbuffer SkinningConstants : register(b0)
{
    uint VertexCount;
    uint PositionIndex;
    uint JointIndex;
    uint WeightIndex;
    uint BoneMatrixIndex;
    uint OutputPositionIndex;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint vertexIndex = dispatchThreadId.x;
    if (vertexIndex >= VertexCount)
    {
        return;
    }

    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[PositionIndex];
    StructuredBuffer<uint4> JointBuffer = ResourceDescriptorHeap[JointIndex];
    StructuredBuffer<float4> WeightBuffer = ResourceDescriptorHeap[WeightIndex];
    StructuredBuffer<row_major float4x4> BoneMatrices = ResourceDescriptorHeap[BoneMatrixIndex];
    RWStructuredBuffer<float3> OutputPositions = ResourceDescriptorHeap[OutputPositionIndex];

    const float3 position = PositionBuffer[vertexIndex];
    const uint4 joints = JointBuffer[vertexIndex];
    const float4 weights = WeightBuffer[vertexIndex];
    row_major float4x4 skinMatrix =
        weights.x * BoneMatrices[joints.x] +
        weights.y * BoneMatrices[joints.y] +
        weights.z * BoneMatrices[joints.z] +
        weights.w * BoneMatrices[joints.w];

    OutputPositions[vertexIndex] = mul(float4(position, 1.0f), skinMatrix).xyz;
}
