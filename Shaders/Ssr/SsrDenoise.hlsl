#include "../SceneConstants.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

cbuffer SsrDenoiseConstants : register(b0)
{
    uint2 OutputSize;
    float DepthSigma;
    float NormalPower;
};

cbuffer SsrDenoiseBindlessConstants : register(b1)
{
    uint SsrInputIndex;
    uint GBufferAIndex;
    uint LinearDepthIndex;
    uint PointSamplerIndex;
    uint LinearSamplerIndex;
};

VSOutput SsrDenoiseVS(uint VertexId : SV_VertexID)
{
    float2 Positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0f, 1.0f);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

float4 SsrDenoisePS(VSOutput Input) : SV_Target
{
    Texture2D SsrInput = ResourceDescriptorHeap[SsrInputIndex];
    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D LinearDepth = ResourceDescriptorHeap[LinearDepthIndex];
    SamplerState PointSampler = SamplerDescriptorHeap[PointSamplerIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearSamplerIndex];

    const float2 texelSize = 1.0f / max(float2(OutputSize), 1.0f);
    const float centerDepth = LinearDepth.SampleLevel(PointSampler, Input.UV, 0).r;
    if (centerDepth <= 0.0f)
    {
        return 0.0f;
    }

    const float3 centerNormal = normalize(GBufferA.SampleLevel(PointSampler, Input.UV, 0).xyz * 2.0f - 1.0f);
    float3 accum = 0.0f;
    float weightSum = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offsetUv = Input.UV + float2(x, y) * texelSize;
            float depth = LinearDepth.SampleLevel(PointSampler, offsetUv, 0).r;
            float3 normal = normalize(GBufferA.SampleLevel(PointSampler, offsetUv, 0).xyz * 2.0f - 1.0f);
            float depthDiff = abs(depth - centerDepth);
            float depthWeight = exp(-depthDiff / max(DepthSigma, 1e-4f));
            float normalWeight = pow(saturate(dot(normal, centerNormal)), NormalPower);
            float weight = depthWeight * normalWeight;
            float3 color = SsrInput.SampleLevel(LinearSampler, offsetUv, 0).rgb;
            accum += color * weight;
            weightSum += weight;
        }
    }

    const float3 filtered = accum / max(weightSum, 1e-4f);
    const float alpha = SsrInput.SampleLevel(LinearSampler, Input.UV, 0).a;
    return float4(filtered, alpha);
}
