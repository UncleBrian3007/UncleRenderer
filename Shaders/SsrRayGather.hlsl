#include "SceneConstants.hlsl"

struct RayItem
{
    float3 OriginVS;
    float TMin;
    float3 DirVS;
    float TMax;
    uint2 PixelCoord;
    float Roughness;
    float Padding;
};

cbuffer SsrRayGatherConstants : register(b1)
{
    uint2 OutputSize;
    uint FrameIndex;
    uint SamplesPerQuad;
    uint MaxRayCount;
    float MaxDistance;
    float RoughnessCutoff;
    float NormalBiasScale;
    float TMinBias;
    uint PatternRotate;
    uint Padding;
};

cbuffer SsrRayGatherBindlessConstants : register(b2)
{
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint LinearDepthIndex;
    uint RayCounterIndex;
    uint RayListIndex;
    uint GBufferPointSamplerIndex;
};

float ReconstructViewZ(float depth)
{
    return Projection._43 / max(depth, 1e-6f);
}

float3 ReconstructViewPosition(float2 uv, float viewZ)
{
    float2 ndc = float2(uv * 2.0f - 1.0f);
    float viewX = ndc.x * viewZ / Projection._11;
    float viewY = -ndc.y * viewZ / Projection._22;
    return float3(viewX, viewY, viewZ);
}

bool ShouldSampleQuad(uint2 pixel, uint samplesPerQuad, uint patternRotate)
{
    uint2 quad = pixel & 1u;
    uint selector = quad.x | (quad.y << 1);
    uint patternIndex = (selector + patternRotate) & 3u;
    if (samplesPerQuad <= 1u)
    {
        return patternIndex == 0u;
    }
    if (samplesPerQuad == 2u)
    {
        return patternIndex < 2u;
    }
    return true;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = DispatchThreadId.xy;
    if (pixel.x >= OutputSize.x || pixel.y >= OutputSize.y)
    {
        return;
    }

    if (!ShouldSampleQuad(pixel, SamplesPerQuad, PatternRotate))
    {
        return;
    }

    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D LinearDepth = ResourceDescriptorHeap[LinearDepthIndex];
    SamplerState GBufferPointSampler = SamplerDescriptorHeap[GBufferPointSamplerIndex];

    float2 uv = (float2(pixel) + 0.5f) / float2(OutputSize);
    float4 normalEncoded = GBufferA.SampleLevel(GBufferPointSampler, uv, 0);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.SampleLevel(GBufferPointSampler, uv, 0);
    float roughness = smr.z;
    if (roughness > RoughnessCutoff)
    {
        return;
    }

    float viewZ = LinearDepth.SampleLevel(GBufferPointSampler, uv, 0).r;
    if (viewZ <= 0.0f)
    {
        return;
    }

    float3 viewPos = ReconstructViewPosition(uv, viewZ);
    float3 viewDir = normalize(-viewPos);
    float3 viewNormal = normalize(mul(worldNormal, (float3x3)View));
    float3 rayDir = normalize(reflect(-viewDir, viewNormal));
    float normalBiasVS = max(0.01f, NormalBiasScale * abs(viewPos.z));

    if (RayCounterIndex == 0xFFFFFFFFu || RayListIndex == 0xFFFFFFFFu)
    {
        return;
    }

    RWByteAddressBuffer RayCounter = ResourceDescriptorHeap[RayCounterIndex];
    RWStructuredBuffer<RayItem> RayList = ResourceDescriptorHeap[RayListIndex];
    uint writeIndex = 0;
    RayCounter.InterlockedAdd(0, 1, writeIndex);
    if (writeIndex < MaxRayCount)
    {
        RayItem item;
        item.OriginVS = viewPos + viewNormal * normalBiasVS;
        item.TMin = TMinBias;
        item.DirVS = rayDir;
        item.TMax = MaxDistance;
        item.PixelCoord = pixel;
        item.Roughness = roughness;
        item.Padding = 0.0f;
        RayList[writeIndex] = item;
    }
}
