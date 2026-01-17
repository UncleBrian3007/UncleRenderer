#include "SceneConstants.hlsl"

RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthTexture : register(t1);
Texture2D<float4> GBufferA : register(t2);
RWTexture2D<float> ShadowMask : register(u0);

struct ShadowPayload
{
    uint Hit;
};

float3 ReconstructWorldPosition(uint2 pixel, float depth, uint2 dispatchDim)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(dispatchDim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clip = float4(ndc, depth, 1.0f);
    float4 worldPosition = mul(clip, ViewProjectionInverse);
    worldPosition.xyz /= worldPosition.w;
    return worldPosition.xyz;
}

[shader("raygeneration")]
void RayGen()
{
    const uint2 DispatchIndex = DispatchRaysIndex().xy;
    const uint2 DispatchDim = DispatchRaysDimensions().xy;
    if (DispatchIndex.x >= DispatchDim.x || DispatchIndex.y >= DispatchDim.y)
    {
        return;
    }

    const float Depth = DepthTexture.Load(int3(DispatchIndex, 0));
    if (Depth >= 1.0f)
    {
        ShadowMask[DispatchIndex] = 1u;
        return;
    }

    ShadowPayload Payload;
    Payload.Hit = 1;

    RayDesc Ray;
    float3 worldPosition = ReconstructWorldPosition(DispatchIndex, Depth, DispatchDim);
    float4 normalEncoded = GBufferA.Load(int3(DispatchIndex, 0));
    float3 normalView = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float3 worldNormal = normalize(mul(normalView, (float3x3) ViewInverse));
    if (any(isnan(worldNormal)) || all(worldNormal == 0.0f))
    {
        worldNormal = float3(0.0f, 1.0f, 0.0f);
    }

    float3 rayDirection = normalize(LightDirection);
    Ray.Origin = worldPosition + worldNormal * 0.01f;
    Ray.Direction = rayDirection;
    Ray.TMin = 0.0f;
    Ray.TMax = 10000.0f;

    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, Ray, Payload);

    ShadowMask[DispatchIndex] = Payload.Hit;
}

[shader("miss")]
void Miss(inout ShadowPayload Payload)
{
    Payload.Hit = 1;
}

[shader("closesthit")]
void ClosestHit(inout ShadowPayload Payload, in BuiltInTriangleIntersectionAttributes Attributes)
{
    Payload.Hit = 0;
}
