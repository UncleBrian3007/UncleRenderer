#include "SceneConstants.hlsl"
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

float4 UnpackColor(uint PackedColor)
{
    const float r = (PackedColor & 0xFFu) / 255.0f;
    const float g = ((PackedColor >> 8) & 0xFFu) / 255.0f;
    const float b = ((PackedColor >> 16) & 0xFFu) / 255.0f;
    const float a = ((PackedColor >> 24) & 0xFFu) / 255.0f;
    return float4(r, g, b, a);
}

VSOutput VSMain(uint VertexId : SV_VertexID)
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

float4 PSMain(VSOutput Input) : SV_Target
{
    return Input.Color;
}
