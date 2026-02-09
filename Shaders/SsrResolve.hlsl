cbuffer SsrResolveConstants : register(b1)
{
    uint2 OutputSize;
    float DepthWeight;
    float NormalWeight;
    float RoughnessWeight;
};

cbuffer SsrResolveBindlessConstants : register(b2)
{
    uint SsrInputIndex;
    uint SsrOutputIndex;
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint LinearDepthIndex;
    uint PointSamplerIndex;
};

float GetDepth(Texture2D LinearDepth, SamplerState Sampler, float2 uv)
{
    return LinearDepth.SampleLevel(Sampler, uv, 0).r;
}

float3 GetNormal(Texture2D GBufferA, SamplerState Sampler, float2 uv)
{
    float3 normal = GBufferA.SampleLevel(Sampler, uv, 0).xyz * 2.0f - 1.0f;
    return normalize(normal);
}

float GetRoughness(Texture2D GBufferB, SamplerState Sampler, float2 uv)
{
    return GBufferB.SampleLevel(Sampler, uv, 0).z;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = DispatchThreadId.xy;
    if (pixel.x >= OutputSize.x || pixel.y >= OutputSize.y)
    {
        return;
    }

    Texture2D SsrInput = ResourceDescriptorHeap[SsrInputIndex];
    RWTexture2D<float4> SsrOutput = ResourceDescriptorHeap[SsrOutputIndex];
    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D LinearDepth = ResourceDescriptorHeap[LinearDepthIndex];
    SamplerState PointSampler = SamplerDescriptorHeap[PointSamplerIndex];

    float2 uv = (float2(pixel) + 0.5f) / float2(OutputSize);
    float depthCenter = GetDepth(LinearDepth, PointSampler, uv);
    float3 normalCenter = GetNormal(GBufferA, PointSampler, uv);
    float roughnessCenter = GetRoughness(GBufferB, PointSampler, uv);

    uint2 basePixel = pixel & ~1u;
    float4 bestSample = SsrInput[basePixel];
    float bestScore = 1e9f;

    [unroll]
    for (uint offsetY = 0; offsetY < 2; ++offsetY)
    {
        [unroll]
        for (uint offsetX = 0; offsetX < 2; ++offsetX)
        {
            uint2 samplePixel = basePixel + uint2(offsetX, offsetY);
            if (samplePixel.x >= OutputSize.x || samplePixel.y >= OutputSize.y)
            {
                continue;
            }

            float2 sampleUv = (float2(samplePixel) + 0.5f) / float2(OutputSize);
            float depthSample = GetDepth(LinearDepth, PointSampler, sampleUv);
            float3 normalSample = GetNormal(GBufferA, PointSampler, sampleUv);
            float roughnessSample = GetRoughness(GBufferB, PointSampler, sampleUv);

            float depthDiff = abs(depthCenter - depthSample);
            float normalDiff = 1.0f - saturate(dot(normalCenter, normalSample));
            float roughnessDiff = abs(roughnessCenter - roughnessSample);
            float score = depthDiff * DepthWeight + normalDiff * NormalWeight + roughnessDiff * RoughnessWeight;

            float4 sampleValue = SsrInput[samplePixel];
            if (sampleValue.a <= 0.0f)
            {
                score += 1.0f;
            }

            if (score < bestScore)
            {
                bestScore = score;
                bestSample = sampleValue;
            }
        }
    }

    SsrOutput[pixel] = bestSample;
}
