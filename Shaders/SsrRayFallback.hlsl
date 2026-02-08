#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"

RaytracingAccelerationStructure Scene : register(t0);

cbuffer RayTracingBindlessConstants : register(b1)
{
    uint RayListIndex;
    uint RayCounterIndex;
    uint FallbackUavIndex;
    uint SceneColorIndex;
    uint InstanceDataBufferIndex;
    uint EnvironmentCubeBindlessIndex;
    uint LinearClampSamplerIndex;
    uint MaxRayCount;
    uint OutputWidth;
    uint OutputHeight;
    float SsrIntensity;
    float RoughnessCutoff;
    uint SsrPadding2;
};

#include "RayTracingCommon.hlsl"

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

static const uint RayQueryThreadGroupSize = 64;
static const uint SsrFallbackRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

[numthreads(RayQueryThreadGroupSize, 1, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint rayIndex = DispatchThreadId.x;
    if (rayIndex >= MaxRayCount)
    {
        return;
    }

    StructuredBuffer<RayItem> RayList = ResourceDescriptorHeap[RayListIndex];
    ByteAddressBuffer RayCounter = ResourceDescriptorHeap[RayCounterIndex];
    RWTexture2D<float4> FallbackTexture = ResourceDescriptorHeap[FallbackUavIndex];
    Texture2D SceneColor = ResourceDescriptorHeap[SceneColorIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];

    const uint rayCount = min(MaxRayCount, RayCounter.Load(0));
    if (rayIndex >= rayCount)
    {
        return;
    }

    RayItem item = RayList[rayIndex];
    if (item.PixelCoord.x >= OutputWidth || item.PixelCoord.y >= OutputHeight)
    {
        return;
    }

    float3 worldOrigin = mul(float4(item.OriginVS, 1.0f), ViewInverse).xyz;
    float3 worldDir = normalize(mul(item.DirVS, (float3x3)ViewInverse));

    RayDesc Ray;
    Ray.Origin = worldOrigin;
    Ray.Direction = worldDir;
    Ray.TMin = max(item.TMin, 1e-3f);
    Ray.TMax = item.TMax;

    RayQuery<SsrFallbackRayFlags> RayQuery;
    RayQuery.TraceRayInline(Scene, SsrFallbackRayFlags, 0xFF, Ray);
    while (RayQuery.Proceed())
    {
    }

    if (RayQuery.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        return;
    }

    const uint instanceID = RayQuery.CommittedInstanceID();
    const uint primitiveIndex = RayQuery.CommittedPrimitiveIndex();
    const float2 barycentrics = RayQuery.CommittedTriangleBarycentrics();
    if (!AlphaTest(instanceID, primitiveIndex, barycentrics))
    {
        return;
    }

    const float tHit = RayQuery.CommittedRayT();
    float3 worldHit = worldOrigin + worldDir * tHit;
    float3 viewHit = mul(float4(worldHit, 1.0f), View).xyz;
    float4 clip = mul(float4(viewHit, 1.0f), Projection);
    bool bUvValid = false;
    float2 uv = 0.0f;
    if (clip.w > 0.0f)
    {
        uv = clip.xy / clip.w;
        uv = uv * 0.5f + 0.5f;
        uv.y = 1.0f - uv.y;
        bUvValid = all(uv >= 0.0f) && all(uv <= 1.0f);
    }

    float3 color = 0.0f;
    if (bUvValid)
    {
        color = SceneColor.SampleLevel(LinearSampler, uv, 0).rgb;
    }
    else
    {
        if (InstanceDataBufferIndex == 0xFFFFFFFFu || EnvironmentCubeBindlessIndex == 0xFFFFFFFFu)
        {
            return;
        }

        float3 worldNormal = GetInterpolatedNormal(instanceID, primitiveIndex, barycentrics);
        float2 surfaceUv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
        float3 albedo = SampleAlbedo(instanceID, surfaceUv);
        float2 mr = SampleMetallicRoughness(instanceID, surfaceUv);
        float metallic = mr.x;
        float roughness = mr.y;
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

        float3 V = normalize(-worldDir);
        float3 L = normalize(LightDirection);
        float3 direct = EvaluatePBR(albedo, metallic, roughness, F0, worldNormal, V, L) * LightIntensity * LightColor;
        float3 envSpec = EvaluateSky(reflect(-V, worldNormal)) * F0;
        float3 envDiffuse = EvaluateSky(worldNormal) * albedo * (1.0f - metallic);
        color = direct + envSpec + envDiffuse;
    }

    float hitWeight = 1.0f - saturate(tHit / max(item.TMax, 1e-3f));
    float r01 = saturate(item.Roughness / max(RoughnessCutoff, 1e-4f));
    float roughAtten = 1.0f - r01;
    roughAtten = roughAtten * roughAtten;
    hitWeight *= roughAtten;
    FallbackTexture[item.PixelCoord] = float4(color * SsrIntensity, hitWeight);
}
