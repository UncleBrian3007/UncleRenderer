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

VSOutput CasVS(uint VertexId : SV_VertexID)
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

cbuffer CasBindlessConstants : register(b1)
{
    uint InputTextureIndex;
};

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

// Based on rs_rcas.fsh from AMD FidelityFX CAS
float4 CasPS(VSOutput Input) : SV_Target
{
    Texture2D InputTexture = ResourceDescriptorHeap[InputTextureIndex];
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

    // 샤프닝을 해도 안전한 곳을 찾는 것, 이미 너무 어둡거나/밝거나/대비가 심한 곳은 제외 
	float3 minRGB = min(min(min(N, W), min(E, S)), C); // 중심과 주변의 가장 어두운 색상
	float3 maxRGB = max(max(max(N, W), max(E, S)), C); // 중심과 주변의 가장 밝은 색상
    float3 invMax = 1.0f / (maxRGB + FsrEps);
    float3 amp = clamp(min(minRGB, 2.0f - maxRGB) * invMax, 0.0f, 1.0f);
    // 여기서 amp 는 픽셀이 중간톤(0.5)에 가까울수록 1.0에 가까워지고, 극단적인 밝기(0.0 또는 1.0)에 가까울수록 0.0에 가까워짐
    
    // rsqrt 로 중간은 1.0, 극단은 1/sqrt(0 + 0.0001) -> 100.0 에 가까워짐
    amp = rsqrt(amp + FsrEps); // 1 ~ 100

    float w = -RcasInvPeak / dot(amp, LumCoeff); // -0.2(중간) ~ -0.002(극단)

    float sumL = NL + WL + EL + SL;
    float invDen = 1.0f / (4.0f * w + 1.0f);
    float sharpL = clamp((sumL * w + CL) * invDen, 0.0f, 1.0f);
    // sharpL = ((NL + WL + EL + SL) * w  + CL) / (4w + 1) // 주변 픽셀(N,W,E,S)의 기여도는 각각 w, 중심 픽셀(C)의 기여도는 1
    // 샤프닝 최대 w(중간,-0.2), 샤프닝 최소 w(극단,-0.002)
    // 현재 픽셀이 주변 대비 밝으면 더 밝게(+, [CL-0.2*SumL]/(4w+1) > 0), 어두우면 더 어둡게(-,[CL-0.2*SumL]/(4w+1) < 0) 조정하는 것이 샤프닝 효과 
    
    float3 chroma = C - CL; // 색상 성분 분리
    float3 sharpColor = chroma + sharpL; // 색상은 그대로 두고 휘도만 샤프닝 적용
    float3 outColor = lerp(C, sharpColor, Sharpness);

    return float4(outColor, 1.0f);
}
