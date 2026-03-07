#include "SceneConstants.hlsl"

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
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float4 Tangent  : TEXCOORD2;
    float4 Color    : COLOR0;
};


VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.y];
    StructuredBuffer<float2> TexCoordBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.z];
    StructuredBuffer<float4> TangentBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.w];
    StructuredBuffer<float4> ColorBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.x];
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.y];
#if USE_SKINNING
    StructuredBuffer<uint4> JointBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.x];
    StructuredBuffer<float4> WeightBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.y];
    StructuredBuffer<row_major float4x4> BoneMatrices = ResourceDescriptorHeap[SkinningBindlessIndices.z];
    StructuredBuffer<float3> SkinnedPositionBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.w];
#endif

    uint vertexIndex = IndexBuffer[Input.VertexId];
    float3 position = PositionBuffer[vertexIndex];
    float3 normal = NormalBuffer[vertexIndex];
    float2 uv = TexCoordBuffer[vertexIndex];
    float4 tangent = TangentBuffer[vertexIndex];
    float4 color = ColorBuffer[vertexIndex];

#if USE_SKINNING
    uint4 joints = JointBuffer[vertexIndex];
    float4 weights = WeightBuffer[vertexIndex];
    row_major float4x4 skinMatrix =
        weights.x * BoneMatrices[joints.x] +
        weights.y * BoneMatrices[joints.y] +
        weights.z * BoneMatrices[joints.z] +
        weights.w * BoneMatrices[joints.w];
    position = SkinnedPositionBuffer[vertexIndex];
    normal = mul(normal, (float3x3)skinMatrix);
    tangent.xyz = mul(tangent.xyz, (float3x3)skinMatrix);
#endif

    float4 WorldPos = mul(float4(position, 1.0), World);
    float4 ViewPos = mul(WorldPos, View);
    Output.Position = mul(ViewPos, Projection);
    Output.Normal = normalize(mul(normal, (float3x3)WorldInverseTranspose));
    Output.UV = uv;
    Output.WorldPos = WorldPos.xyz;
    Output.Tangent = float4(normalize(mul(tangent.xyz, (float3x3)WorldInverseTranspose)), tangent.w);
    Output.Color = color;
    return Output;
}
