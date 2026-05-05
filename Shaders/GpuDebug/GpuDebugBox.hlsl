#include "../SceneConstants.hlsl"
#include "../Common.hlsli"
#include "GpuDebugBoxCommon.hlsl"

cbuffer DebugBoxBindlessConstants : register(b1)
{
    uint DebugBoxBufferIndex;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
};

float3 GetCornerSign(uint CornerIndex)
{
    switch (CornerIndex)
    {
    case 0u: return float3(-1.0f, -1.0f, -1.0f);
    case 1u: return float3(1.0f, -1.0f, -1.0f);
    case 2u: return float3(-1.0f, 1.0f, -1.0f);
    case 3u: return float3(1.0f, 1.0f, -1.0f);
    case 4u: return float3(-1.0f, -1.0f, 1.0f);
    case 5u: return float3(1.0f, -1.0f, 1.0f);
    case 6u: return float3(-1.0f, 1.0f, 1.0f);
    default: return float3(1.0f, 1.0f, 1.0f);
    }
}

uint GetTriangleCornerIndex(uint VertexInBox)
{
    static const uint Indices[36] =
    {
        0u, 2u, 1u, 1u, 2u, 3u,
        4u, 5u, 6u, 5u, 7u, 6u,
        0u, 1u, 4u, 1u, 5u, 4u,
        2u, 6u, 3u, 3u, 6u, 7u,
        0u, 4u, 2u, 2u, 4u, 6u,
        1u, 3u, 5u, 3u, 7u, 5u
    };

    return Indices[VertexInBox];
}

VSOutput GpuDebugBoxVS(uint VertexId : SV_VertexID)
{
    VSOutput Output;
    Output.Position = float4(2.0f, 2.0f, 0.0f, 1.0f);
    Output.Color = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (DebugBoxBufferIndex == 0xffffffffu)
    {
        return Output;
    }

    ByteAddressBuffer DebugBoxBuffer = ResourceDescriptorHeap[DebugBoxBufferIndex];
    const uint boxCount = min(DebugBoxBuffer.Load(kDebugBoxHeaderBoxCountOffset), kDebugBoxMaxEntries);
    const uint boxIndex = VertexId / 36u;
    if (boxIndex >= boxCount)
    {
        return Output;
    }

    const uint baseOffset = kDebugBoxHeaderSize + boxIndex * kDebugBoxEntryStride;
    const float3 center = asfloat(DebugBoxBuffer.Load3(baseOffset + 0));
    const float halfExtentX = asfloat(DebugBoxBuffer.Load(baseOffset + 12));
    const float3 axisX = asfloat(DebugBoxBuffer.Load3(baseOffset + 16));
    const float halfExtentY = asfloat(DebugBoxBuffer.Load(baseOffset + 28));
    const float3 axisY = asfloat(DebugBoxBuffer.Load3(baseOffset + 32));
    const float halfExtentZ = asfloat(DebugBoxBuffer.Load(baseOffset + 44));
    const float3 axisZ = asfloat(DebugBoxBuffer.Load3(baseOffset + 48));
    const uint packedColor = DebugBoxBuffer.Load(baseOffset + 60);

    const uint cornerIndex = GetTriangleCornerIndex(VertexId % 36u);
    const float3 cornerSign = GetCornerSign(cornerIndex);
    const float3 positionWs = center
        + axisX * (cornerSign.x * halfExtentX)
        + axisY * (cornerSign.y * halfExtentY)
        + axisZ * (cornerSign.z * halfExtentZ);

    const float4 positionVs = mul(float4(positionWs, 1.0f), View);
    Output.Position = mul(positionVs, Projection);
    Output.Color = UnpackColor(packedColor);
    return Output;
}

float4 GpuDebugBoxPS(VSOutput Input) : SV_Target
{
    return Input.Color;
}
