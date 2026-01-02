#define DEBUG_PRINT_READ_ONLY
#include "DebugPrintCommon.hlsl"

cbuffer DebugPrintConstants : register(b0)
{
    float2 ScreenSize;
    uint FirstChar;
    uint CharCount;
};

struct DebugGlyph
{
    float2 UvMin;
    float2 UvMax;
    float2 Size;
    float2 Offset;
    float Advance;
    float Padding;
};

StructuredBuffer<DebugGlyph> Glyphs : register(t0);
Texture2D FontAtlas : register(t1);
ByteAddressBuffer DebugPrintBuffer : register(t2);
SamplerState FontSampler : register(s0);

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float4 Color : TEXCOORD1;
};

float4 UnpackColor(uint color)
{
    float r = (color & 0xFFu) / 255.0f;
    float g = ((color >> 8) & 0xFFu) / 255.0f;
    float b = ((color >> 16) & 0xFFu) / 255.0f;
    float a = ((color >> 24) & 0xFFu) / 255.0f;
    return float4(r, g, b, a);
}

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.Position = float4(2.0f, 2.0f, 0.0f, 1.0f);
    output.UV = float2(0.0f, 0.0f);
    output.Color = float4(1.0f, 1.0f, 1.0f, 1.0f);

    const uint totalChars = DebugPrintBuffer.Load(0);
    const uint charIndex = vertexId / 6;
    if (charIndex >= totalChars)
    {
        return output;
    }

    const uint localIndex = vertexId % 6;
    const uint offset = kDebugPrintHeaderSize + charIndex * kDebugPrintEntryStride;
    const uint posX = DebugPrintBuffer.Load(offset + 0);
    const uint posY = DebugPrintBuffer.Load(offset + 4);
    const uint code = DebugPrintBuffer.Load(offset + 8);
    const uint color = DebugPrintBuffer.Load(offset + 12);

    if (code < FirstChar || code >= FirstChar + CharCount)
    {
        return output;
    }

    DebugGlyph glyph = Glyphs[code];
    float2 basePos = float2(posX, posY) + glyph.Offset;
    float2 size = glyph.Size;
    float2 posMin = basePos;
    float2 posMax = basePos + size;

    float2 uvMin = glyph.UvMin;
    float2 uvMax = glyph.UvMax;

    float2 pos;
    float2 uv;
    if (localIndex == 0)
    {
        pos = float2(posMin.x, posMin.y);
        uv = float2(uvMin.x, uvMin.y);
    }
    else if (localIndex == 1)
    {
        pos = float2(posMax.x, posMin.y);
        uv = float2(uvMax.x, uvMin.y);
    }
    else if (localIndex == 2)
    {
        pos = float2(posMin.x, posMax.y);
        uv = float2(uvMin.x, uvMax.y);
    }
    else if (localIndex == 3)
    {
        pos = float2(posMin.x, posMax.y);
        uv = float2(uvMin.x, uvMax.y);
    }
    else if (localIndex == 4)
    {
        pos = float2(posMax.x, posMin.y);
        uv = float2(uvMax.x, uvMin.y);
    }
    else
    {
        pos = float2(posMax.x, posMax.y);
        uv = float2(uvMax.x, uvMax.y);
    }

    float2 ndc;
    ndc.x = (pos.x / ScreenSize.x) * 2.0f - 1.0f;
    ndc.y = 1.0f - (pos.y / ScreenSize.y) * 2.0f;

    output.Position = float4(ndc, 0.0f, 1.0f);
    output.UV = uv;
    output.Color = UnpackColor(color);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target
{
    float alpha = FontAtlas.Sample(FontSampler, input.UV).r;
    return float4(input.Color.rgb, input.Color.a * alpha);
}
