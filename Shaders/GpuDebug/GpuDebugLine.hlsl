#include "../SceneConstants.hlsl"
#include "../Common.hlsli"
#include "GpuDebugLineCommon.hlsl"

cbuffer DebugLineBindlessConstants : register(b1)
{
    uint DebugLineBufferIndex;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
};

VSOutput GpuDebugLineVS(uint VertexId : SV_VertexID)
{
    VSOutput Output;
    Output.Position = float4(2.0f, 2.0f, 0.0f, 1.0f);
    Output.Color = float4(0.0f, 0.0f, 0.0f, 0.0f);

    if (DebugLineBufferIndex == 0xffffffffu)
    {
        return Output;
    }

    ByteAddressBuffer DebugLineBuffer = ResourceDescriptorHeap[DebugLineBufferIndex];
    const uint lineCount = min(DebugLineBuffer.Load(kDebugLineHeaderLineCountOffset), kDebugLineMaxEntries);
    const uint lineIndex = VertexId / 2;
    if (lineIndex >= lineCount)
    {
        return Output;
    }

    const uint baseOffset = kDebugLineHeaderSize + lineIndex * kDebugLineEntryStride;
    const uint3 p0Packed = DebugLineBuffer.Load3(baseOffset + 0);
    const uint3 p1Packed = DebugLineBuffer.Load3(baseOffset + 16);
    const uint colorPacked = DebugLineBuffer.Load(baseOffset + 28);

    const float3 p0 = asfloat(p0Packed);
    const float3 p1 = asfloat(p1Packed);
    const float3 positionWs = (VertexId & 1u) == 0u ? p0 : p1;

    const float4 positionVs = mul(float4(positionWs, 1.0f), View);
    Output.Position = mul(positionVs, Projection);
    Output.Color = UnpackColor(colorPacked);
    return Output;
}

float4 GpuDebugLinePS(VSOutput Input) : SV_Target
{
    return Input.Color;
}
