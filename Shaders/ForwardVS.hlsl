#include "SceneConstants.hlsl"

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

    uint vertexIndex = IndexBuffer[Input.VertexId];
    float3 position = PositionBuffer[vertexIndex];
    float3 normal = NormalBuffer[vertexIndex];
    float2 uv = TexCoordBuffer[vertexIndex];
    float4 tangent = TangentBuffer[vertexIndex];
    float4 color = ColorBuffer[vertexIndex];

    float4 WorldPos = mul(float4(position, 1.0), World);
    float4 ViewPos = mul(WorldPos, View);
    Output.Position = mul(ViewPos, Projection);
    Output.Normal = mul(normal, (float3x3)World);
    Output.UV = uv;
    Output.WorldPos = WorldPos.xyz;
    Output.Tangent = float4(normalize(mul(tangent.xyz, (float3x3)World)), tangent.w);
    Output.Color = color;
    return Output;
}
