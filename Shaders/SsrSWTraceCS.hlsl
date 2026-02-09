#ifndef HZB_ENABLED
#define HZB_ENABLED 1
#endif

#ifndef SSR_REFINE_ENABLED
#define SSR_REFINE_ENABLED 1
#endif

#ifndef SW_SSR_ENABLED
#define SW_SSR_ENABLED 1
#endif

#include "SsrShared.hlsl"

cbuffer SsrSwTraceConstants : register(b1)
{
    uint2 OutputSize;
    uint MaxSteps;
    float MaxDistance;
    float Thickness;
    float Stride;
    float RoughnessCutoff;
    float Intensity;
    uint HZBWidth;
    uint HZBHeight;
    uint HZBMipCount;
    uint HZBAvailable;
    uint MaxRayCount;
};

cbuffer SsrSwTraceBindlessConstants : register(b2)
{
    uint LinearDepthIndex;
    uint SceneColorIndex;
    uint RayCounterPrimaryIndex;
    uint RayListPrimaryIndex;
    uint RayCounterHwMissIndex;
    uint RayListHwMissIndex;
    uint SsrOutputIndex;
    uint HZBIndex;
    uint PointSamplerIndex;
    uint LinearSamplerIndex;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    uint rayIndex = DispatchThreadId.x;
    if (rayIndex >= MaxRayCount)
    {
        return;
    }

    ByteAddressBuffer RayCounterPrimary = ResourceDescriptorHeap[RayCounterPrimaryIndex];
    uint rayCount = RayCounterPrimary.Load(0);
    if (rayIndex >= rayCount)
    {
        return;
    }

    StructuredBuffer<RayItem> RayListPrimary = ResourceDescriptorHeap[RayListPrimaryIndex];
    RayItem item = RayListPrimary[rayIndex];

    if (item.Roughness > RoughnessCutoff)
    {
        return;
    }

    float2 uv = (float2(item.PixelCoord) + 0.5f) / float2(OutputSize);
    float3 viewPos = item.OriginVS;
    float3 rayDir = item.DirVS;
    float thickness = Thickness * rcp(max(abs(rayDir.z), 1e-3f));

    Texture2D LinearDepth = ResourceDescriptorHeap[LinearDepthIndex];
    Texture2D SceneColor = ResourceDescriptorHeap[SceneColorIndex];
    Texture2D<float2> HZBTexture = ResourceDescriptorHeap[HZBIndex];
    SamplerState PointSampler = SamplerDescriptorHeap[PointSamplerIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearSamplerIndex];

#if SW_SSR_ENABLED
    FTraceResult TraceResult = TraceSw(
        viewPos,
        rayDir,
        uv,
        MaxSteps,
        Stride,
        MaxDistance,
        thickness,
        LinearDepth,
        SceneColor,
        PointSampler,
        LinearSampler,
        HZBTexture,
        HZBWidth,
        HZBHeight,
        HZBMipCount,
        HZBAvailable);

    float r01 = saturate(item.Roughness / max(RoughnessCutoff, 1e-4f));
    float roughAtten = 1.0f - r01;
    roughAtten = roughAtten * roughAtten;
    float hitWeight = TraceResult.Weight * roughAtten;

    if (TraceResult.bHit)
    {
        hitWeight = max(hitWeight, 1e-4f);
        RWTexture2D<float4> SsrOutput = ResourceDescriptorHeap[SsrOutputIndex];
        SsrOutput[item.PixelCoord] = float4(TraceResult.Color * Intensity, hitWeight);
        return;
    }
#endif

    RWByteAddressBuffer RayCounterHwMiss = ResourceDescriptorHeap[RayCounterHwMissIndex];
    RWStructuredBuffer<RayItem> RayListHwMiss = ResourceDescriptorHeap[RayListHwMissIndex];
    uint writeIndex = 0;
    RayCounterHwMiss.InterlockedAdd(0, 1, writeIndex);
    if (writeIndex < MaxRayCount)
    {
        RayListHwMiss[writeIndex] = item;
    }
}
