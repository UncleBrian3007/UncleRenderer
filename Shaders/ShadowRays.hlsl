#include "SceneConstants.hlsl"
#include "Common.hlsli"

RaytracingAccelerationStructure Scene : register(t0);
cbuffer RayTracingBindlessConstants : register(b1)
{
    uint DepthTextureIndex;
    uint GBufferAIndex;
    uint ShadowMaskIndex;
    uint UnusedIndex;
    uint DispatchWidth;
    uint DispatchHeight;
};

static const uint RayQueryThreadGroupSize = 8;
static const uint ShadowRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
    | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
    | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

[numthreads(RayQueryThreadGroupSize, RayQueryThreadGroupSize, 1)]
void ShadowRaysCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= DispatchWidth || DispatchThreadId.y >= DispatchHeight)
    {
        return;
    }

    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthTextureIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    RWTexture2D<float> ShadowMask = ResourceDescriptorHeap[ShadowMaskIndex];

    const uint2 DispatchIndex = DispatchThreadId.xy;
    const uint2 DispatchDim = uint2(DispatchWidth, DispatchHeight);
    const float Depth = DepthTexture.Load(int3(DispatchIndex, 0));
    if (Depth >= 1.0f)
    {
        ShadowMask[DispatchIndex] = 1u;
        return;
    }

    RayDesc Ray;
    float3 worldPosition = ReconstructWorldPosition(DispatchIndex, Depth, DispatchDim, ViewProjectionInverse);
    float4 normalEncoded = GBufferA.Load(int3(DispatchIndex, 0));
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    if (any(isnan(worldNormal)) || all(worldNormal == 0.0f))
    {
        worldNormal = float3(0.0f, 1.0f, 0.0f);
    }

    float3 rayDirection = normalize(LightDirection);
    Ray.Origin = worldPosition + worldNormal * 0.01f;
    Ray.Direction = rayDirection;
    Ray.TMin = 0.0f;
    Ray.TMax = 10000.0f;

    RayQuery<ShadowRayFlags> RayQuery;
    RayQuery.TraceRayInline(Scene, ShadowRayFlags, 0xFF, Ray);
    while (RayQuery.Proceed())
    {
    }

    const bool bHit = RayQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    ShadowMask[DispatchIndex] = bHit ? 0.0f : 1.0f;
}
