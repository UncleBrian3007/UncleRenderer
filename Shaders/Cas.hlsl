// MIT License
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

VSOutput VSMain(uint VertexId : SV_VertexID)
{
    float2 Positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0, 1.0);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

cbuffer CasParams : register(b0)
{
    float2 TexelDelta;
    float Sharpness;
    float Padding;
};

Texture2D InputTexture : register(t0);
SamplerState InputSampler : register(s0);

static const float3 LumCoeff = float3(0.2126f, 0.7152f, 0.0722f);
static const float RcasPeak = 8.0f - 3.0f;
static const float RcasInvPeak = 1.0f / RcasPeak;
static const float FsrEps = 0.0001f;

static const float2 CrossOffsets[4] =
{
    float2(0.0f, -1.0f),
    float2(-1.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f)
};

float4 PSMain(VSOutput Input) : SV_Target
{
    float2 uv = Input.UV;
    float3 C = InputTexture.Sample(InputSampler, uv).rgb;
    float CL = dot(C, LumCoeff);

    float3 N = InputTexture.Sample(InputSampler, uv + CrossOffsets[0] * TexelDelta).rgb;
    float3 W = InputTexture.Sample(InputSampler, uv + CrossOffsets[1] * TexelDelta).rgb;
    float3 E = InputTexture.Sample(InputSampler, uv + CrossOffsets[2] * TexelDelta).rgb;
    float3 S = InputTexture.Sample(InputSampler, uv + CrossOffsets[3] * TexelDelta).rgb;

    float NL = dot(N, LumCoeff);
    float WL = dot(W, LumCoeff);
    float EL = dot(E, LumCoeff);
    float SL = dot(S, LumCoeff);

    float3 minRGB = min(min(min(N, W), min(E, S)), C);
    float3 maxRGB = max(max(max(N, W), max(E, S)), C);
    float3 invMax = 1.0f / (maxRGB + FsrEps);
    float3 amp = clamp(min(minRGB, 2.0f - maxRGB) * invMax, 0.0f, 1.0f);
    amp = rsqrt(amp + FsrEps);

    float w = -RcasInvPeak / dot(amp, LumCoeff);

    float sumL = NL + WL + EL + SL;
    float invDen = 1.0f / (4.0f * w + 1.0f);
    float sharpL = clamp((sumL * w + CL) * invDen, 0.0f, 1.0f);

    float3 chroma = C - CL;
    float3 sharpColor = chroma + sharpL;
    float3 outColor = lerp(C, sharpColor, Sharpness);

    return float4(outColor, 1.0f);
}
