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

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

cbuffer SSRConstants : register(b1)
{
    uint2 OutputSize;
    uint MaxSteps;
    float Thickness;
    float MaxDistance;
    float Stride;
    float RoughnessCutoff;
    float Intensity;
    uint MaxRayCount;
    uint UseHistory;
    uint HZBWidth;
    uint HZBHeight;
    uint HZBMipCount;
    uint HZBAvailable;
    uint HwEnabled;
};

cbuffer SSRBindlessConstants : register(b2)
{
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint LinearDepthIndex;
    uint SceneColorIndex;
    uint HZBIndex;
    uint GBufferPointSamplerIndex;
    uint SceneColorLinearSamplerIndex;
    uint RayCounterIndex;
    uint RayListIndex;
};

VSOutput VSMain(uint VertexId : SV_VertexID)
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

float4 PSMain(VSOutput Input) : SV_Target
{
    if (UseHistory == 0)
    {
        return 0.0f;
    }

#if !SW_SSR_ENABLED
    if (HwEnabled == 0)
    {
        return 0.0f;
    }
#endif

    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D LinearDepth = ResourceDescriptorHeap[LinearDepthIndex];
    Texture2D SceneColor = ResourceDescriptorHeap[SceneColorIndex];
    Texture2D<float2> HZBTexture = ResourceDescriptorHeap[HZBIndex];
    SamplerState GBufferPointSampler = SamplerDescriptorHeap[GBufferPointSamplerIndex];
    SamplerState SceneColorLinearSampler = SamplerDescriptorHeap[SceneColorLinearSamplerIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferPointSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferPointSampler, Input.UV);
    float roughness = smr.z;

    if (roughness > RoughnessCutoff)
    {
        return 0.0f;
    }

    float viewZ = LinearDepth.SampleLevel(GBufferPointSampler, Input.UV, 0).r;
    if (viewZ <= 0.0f)
    {
        return 0.0f;
    }

    float3 viewPos = ReconstructViewPosition(Input.UV, viewZ);
    float3 viewDir = normalize(-viewPos);
    float3 viewNormal = normalize(mul(worldNormal, (float3x3)View));
    float3 rayDir = normalize(reflect(-viewDir, viewNormal));
    float thickness = Thickness * rcp(max(abs(rayDir.z), 1e-3f));
    float normalBiasVS = max(0.01, 0.001f * abs(viewPos.z));
    float tMinBias = 0.01f;

#if !SW_SSR_ENABLED
    if (MaxRayCount > 0 && RayCounterIndex != 0xFFFFFFFFu && RayListIndex != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer RayCounter = ResourceDescriptorHeap[RayCounterIndex];
        RWStructuredBuffer<RayItem> RayList = ResourceDescriptorHeap[RayListIndex];
        uint writeIndex = 0;
        RayCounter.InterlockedAdd(0, 1, writeIndex);
        if (writeIndex < MaxRayCount)
        {
            RayItem item;
            item.OriginVS = viewPos + viewNormal * normalBiasVS;
            item.TMin = tMinBias;
            item.DirVS = rayDir;
            item.TMax = MaxDistance;
            item.PixelCoord = uint2(Input.Position.xy);
            item.Roughness = roughness;
            item.Padding = 0.0f;
            RayList[writeIndex] = item;
        }
    }
    return 0.0f;
#endif

    FTraceResult TraceResult = TraceSw(
        viewPos,
        rayDir,
        Input.UV,
        MaxSteps,
        Stride,
        MaxDistance,
        thickness,
        LinearDepth,
        SceneColor,
        GBufferPointSampler,
        SceneColorLinearSampler,
        HZBTexture,
        HZBWidth,
        HZBHeight,
        HZBMipCount,
        HZBAvailable);

    bool bHit = TraceResult.bHit;
    float3 hitColor = TraceResult.Color;
    float hitWeight = TraceResult.Weight;

    float r01 = saturate(roughness / max(RoughnessCutoff, 1e-4f));
    float roughAtten = 1.0f - r01;
    roughAtten = roughAtten * roughAtten;
    hitWeight *= roughAtten;

    if (bHit)
    {
        hitWeight = max(hitWeight, 1e-4f);
    }

    if (HwEnabled != 0 && !bHit && UseHistory != 0 && MaxRayCount > 0
        && RayCounterIndex != 0xFFFFFFFFu && RayListIndex != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer RayCounter = ResourceDescriptorHeap[RayCounterIndex];
        RWStructuredBuffer<RayItem> RayList = ResourceDescriptorHeap[RayListIndex];
        uint writeIndex = 0;
        RayCounter.InterlockedAdd(0, 1, writeIndex);
        if (writeIndex < MaxRayCount)
        {
            RayItem item;
            item.OriginVS = viewPos + viewNormal * normalBiasVS;
            item.TMin = tMinBias;
            item.DirVS = rayDir;
            item.TMax = MaxDistance;
            item.PixelCoord = uint2(Input.Position.xy);
            item.Roughness = roughness;
            item.Padding = 0.0f;
            RayList[writeIndex] = item;
        }
    }

    return float4(hitColor * Intensity, hitWeight);
}
