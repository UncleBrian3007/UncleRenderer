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
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float4 Tangent  : TEXCOORD2;
    float4 Color    : COLOR0;
};

cbuffer DrawCommandConstants : register(b2)
{
    uint DrawIndexStart;
    uint DrawDataIndex;
};


VSOutput ForwardVS(VSInput Input)
{
    VSOutput Output;
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.y];
    StructuredBuffer<ClusterDagPackedPosition> PackedPositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<uint> PackedNormalBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.y];
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.y];
#if USE_SKINNING
    StructuredBuffer<uint4> JointBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.x];
    StructuredBuffer<float4> WeightBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.y];
    StructuredBuffer<row_major float4x4> BoneMatrices = ResourceDescriptorHeap[SkinningBindlessIndices.z];
    StructuredBuffer<float3> SkinnedPositionBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.w];
#endif

    uint vertexIndex = IndexBuffer[Input.VertexId + DrawIndexStart];
    const bool usePackedClusterDagVertices = ClusterDagVertexPackingMode != 0u;
    float3 position = usePackedClusterDagVertices
        ? DecodeClusterDagPackedPosition(PackedPositionBuffer[vertexIndex])
        : PositionBuffer[vertexIndex];
    float3 normal = usePackedClusterDagVertices
        ? DecodeOctahedral16x2(PackedNormalBuffer[vertexIndex])
        : NormalBuffer[vertexIndex];
    float2 uv = 0.0f.xx;
    float4 tangent = 0.0f.xxxx;
    float4 color = 1.0f.xxxx;
    if (usePackedClusterDagVertices)
    {
        if (VertexBufferBindlessIndices.z != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedTexCoordBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.z];
            uv = DecodeClusterDagPackedUV(PackedTexCoordBuffer[vertexIndex]);
        }
        else
        {
            uv = ClusterDagPackedConstantUV.xy;
        }

        if (VertexBufferBindlessIndices.w != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedTangentBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.w];
            tangent = DecodeClusterDagPackedTangent(PackedTangentBuffer[vertexIndex]);
        }
        else
        {
            tangent = BuildClusterDagFallbackTangent(normal);
        }

        if (ExtraBindlessIndices.x != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedColorBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.x];
            color = DecodeClusterDagPackedColor(PackedColorBuffer[vertexIndex]);
        }
        else
        {
            color = ClusterDagPackedConstantColor;
        }
    }
    else
    {
        StructuredBuffer<float2> TexCoordBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.z];
        StructuredBuffer<float4> TangentBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.w];
        StructuredBuffer<float4> ColorBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.x];
        uv = TexCoordBuffer[vertexIndex];
        tangent = TangentBuffer[vertexIndex];
        color = ColorBuffer[vertexIndex];
    }

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
