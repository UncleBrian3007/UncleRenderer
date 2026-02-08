#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"

#ifndef PATH_TRACING_DEBUG
#define PATH_TRACING_DEBUG 0
#endif

#ifndef PATH_TRACING_USE_VNDF
#define PATH_TRACING_USE_VNDF 1
#endif

RaytracingAccelerationStructure Scene : register(t0);
cbuffer RayTracingBindlessConstants : register(b1)
{
    uint DepthTextureIndex;
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint OutputTextureIndex;
    uint DispatchWidth;
    uint DispatchHeight;
    uint FrameIndex;
    uint InstanceDataBufferIndex;
    uint MaxBounces;
    uint LinearClampSamplerIndex;
    uint EnvironmentCubeBindlessIndex;
    int DebugMode; // 0=Normal PT, 1=GBuffer Albedo, 2=First Hit Albedo, 3=Texture Index Hash, 4=Direct Light, 5=Diffuse Probability, 6=Hit/Miss Mask, 7=Throughput Over Pdf, 8=Firefly Metric, 9=First Hit Distance, 10=Sky Miss Contribution, 11=First Hit NdotV, 12=Bounce1 NdotV
};

#include "RayTracingCommon.hlsl"
#include "PathTracingCommon.hlsl"

// Constants for ray tracing
static const uint RayQueryThreadGroupSize = 8;
static const uint PathRayFlags = RAY_FLAG_NONE;
static const uint ShadowRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
static const float MaxRayDistance = 1000.0f;
static const float FireflyThreshold = 100.0f;

bool TraceVisibilityRay(RayDesc ray)
{
    RayQuery<ShadowRayFlags> ShadowQuery;
    ShadowQuery.TraceRayInline(Scene, ShadowRayFlags, 0xFF, ray);
    while (ShadowQuery.Proceed())
    {
        uint instanceID = ShadowQuery.CandidateInstanceID();
        uint primitiveIndex = ShadowQuery.CandidatePrimitiveIndex();
        float2 barycentrics = ShadowQuery.CandidateTriangleBarycentrics();
        if (AlphaTest(instanceID, primitiveIndex, barycentrics))
        {
            ShadowQuery.CommitNonOpaqueTriangleHit();
        }
    }

    return ShadowQuery.CommittedStatus() == COMMITTED_NOTHING;
}

float ProbabilityToSampleDiffuse(float3 diffuse, float3 specular)
{
    float lumDiffuse = Luminance(diffuse);
    float lumSpecular = Luminance(specular);
    return lumDiffuse / max(lumDiffuse + lumSpecular, 0.0001f);
}

[numthreads(RayQueryThreadGroupSize, RayQueryThreadGroupSize, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= DispatchWidth || DispatchThreadId.y >= DispatchHeight)
    {
        return;
    }

    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthTextureIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D<float4> GBufferC = ResourceDescriptorHeap[GBufferCIndex];
    RWTexture2D<float4> PathOutput = ResourceDescriptorHeap[OutputTextureIndex];

    const uint2 DispatchIndex = DispatchThreadId.xy;
    const uint2 DispatchDim = uint2(DispatchWidth, DispatchHeight);
    
#if PATH_TRACING_DEBUG
    // Debug Mode 1: Visualize GBuffer albedo (rasterized result)
    // This is placed BEFORE depth check to show albedo for all pixels including sky
    if (DebugMode == 1)
    {
        float3 dbgAlbedo = GBufferC.Load(int3(DispatchIndex, 0)).rgb;
        PathOutput[DispatchIndex] = float4(dbgAlbedo, 1);
        return;
    }
#endif
    
    const float Depth = DepthTexture.Load(int3(DispatchIndex, 0));
    if (Depth >= 1.0f)
    {
        float3 worldPos = ReconstructWorldPosition(DispatchIndex, 1.0f, DispatchDim);
        float3 worldDir = normalize(worldPos - CameraPosition);
        PathOutput[DispatchIndex] = float4(EvaluateSky(worldDir), 1.0f);
        return;
    }

    float3 worldPosition = ReconstructWorldPosition(DispatchIndex, Depth, DispatchDim);
    float4 normalEncoded = GBufferA.Load(int3(DispatchIndex, 0));
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    if (any(isnan(worldNormal)) || all(worldNormal == 0.0f))
    {
        worldNormal = float3(0.0f, 1.0f, 0.0f);
    }

    float3 albedo = GBufferC.Load(int3(DispatchIndex, 0)).rgb;

    // Load material properties from GBufferB
    // GBufferB format: float4(specular, metallic, roughness, 1.0)
    float4 materialProps = GBufferB.Load(int3(DispatchIndex, 0));
    float currentMetallic = materialProps.g;   // Green channel = metallic
    float currentRoughness = max(materialProps.b, 0.03f);  // Blue channel = roughness, min 0.03 for stability

    float3 position = worldPosition;
    float3 wo = normalize(CameraPosition - worldPosition);
    float3 N = worldNormal;
    if (dot(N, wo) < 0.0f)
    {
        N = -N;
    }

#if PATH_TRACING_DEBUG
    if (DebugMode == 11)
    {
        float ndv = dot(N, wo);
        float3 c = (ndv < 0.0f) ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
        PathOutput[DispatchIndex] = float4(c, 1.0f);
        return;
    }
#endif

    float3 diffuse = albedo * (1.0f - currentMetallic);
    float3 specular = lerp(float3(0.04f, 0.04f, 0.04f), albedo, currentMetallic);
    float roughness = currentRoughness;

    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    float pdf = 1.0f;

    for (uint bounce = 0; bounce < MaxBounces + 1; ++bounce)
    {
        float2 randLight = float2(
            Random01(DispatchIndex, FrameIndex * (MaxBounces + 1u) * 5u + bounce * 5u + 1u),
            Random01(DispatchIndex, FrameIndex * (MaxBounces + 1u) * 5u + bounce * 5u + 2u)
        );
		float3 lightDirSample = SampleConeUniform(randLight, LightRadius, LightDirection);
		float3 wi = lightDirSample;
        RayDesc ShadowRay;
        ShadowRay.Origin = position + N * 0.01f + wi * 0.01f;
        ShadowRay.Direction = wi;
        ShadowRay.TMin = 1e-3f;
        ShadowRay.TMax = MaxRayDistance;

        float visibility = TraceVisibilityRay(ShadowRay) ? 1.0f : 0.0f;
        float NdotL = saturate(dot(N, wi));
        float3 directLight = BRDF(wi, wo, N, diffuse, specular, roughness) * visibility * LightColor * LightIntensity * NdotL;

#if PATH_TRACING_DEBUG
        if (DebugMode == 4 && bounce == 0)
        {
            PathOutput[DispatchIndex] = float4(directLight, 1.0f);
            return;
        }
#endif
        float invPdf = rcp(max(pdf, 1e-6f));
		radiance += directLight * throughput * invPdf;

        if (bounce == MaxBounces)
        {
            break;
        }

        float probDiffuse = ProbabilityToSampleDiffuse(diffuse, specular);
        float randSelect = Random01(DispatchIndex, FrameIndex * (MaxBounces + 1u) * 5u + bounce * 5u + 3u);
        float2 randSample = float2(
            Random01(DispatchIndex, FrameIndex * (MaxBounces + 1u) * 5u + bounce * 5u + 4u),
            Random01(DispatchIndex, FrameIndex * (MaxBounces + 1u) * 5u + bounce * 5u + 5u)
        );

#if PATH_TRACING_DEBUG
        if (DebugMode == 5 && bounce == 0)
        {
            PathOutput[DispatchIndex] = float4(probDiffuse, probDiffuse, probDiffuse, 1.0f);
            return;
        }
#endif

        if (randSelect < probDiffuse)
        {
            wi = SampleCosHemisphere(randSample, N);

            float3 diffuseBrdf = DiffuseBRDF(diffuse);
            float NdotL = saturate(dot(N, wi));

            throughput *= diffuseBrdf * NdotL;
            pdf *= (NdotL / 3.14159265f) * probDiffuse;
        }
        else
        {
            float alpha = roughness * roughness;
#if PATH_TRACING_USE_VNDF
            float3 Vt = WorldToTangent(wo, N);
            float3 Ht = SampleGGX_VNDF(Vt, alpha, randSample);
            float3 H = TangentToWorld(Ht, N);
#else
            float3 H = TangentToWorld(SampleGGX(randSample, alpha), N);
#endif
            wi = reflect(-wo, H);

            float3 F;
            float NdotL = saturate(dot(N, wi));
            if (NdotL <= 0.0f)
            {
                throughput = 0.0f;
                pdf = 0.0f;
                break;
            }

            float3 specularBrdf = SpecularBRDF(N, wo, wi, specular, roughness, F);

            throughput *= specularBrdf * NdotL;

#if PATH_TRACING_USE_VNDF
            float pdfSpec = Pdf_GGX_VNDF_Reflection(N, wo, H, alpha);
            pdf *= pdfSpec * (1.0f - probDiffuse);
#else
            float D = D_GGX(saturate(dot(N, H)), alpha);
            float NdotH = saturate(dot(N, H));
            float LdotH = saturate(dot(wi, H));
            pdf *= (D * NdotH / max(4.0f * LdotH, 0.0001f)) * (1.0f - probDiffuse);
#endif
        }

        float throughputOverPdf = max(throughput.x, max(throughput.y, throughput.z)) / max(pdf, 0.0001f);
#if PATH_TRACING_DEBUG
        if (DebugMode == 7 && bounce == 0)
        {
            PathOutput[DispatchIndex] = float4(throughputOverPdf, throughputOverPdf, throughputOverPdf, 1.0f);
            return;
        }

        if (DebugMode == 8 && bounce == 0)
        {
            float fireflyMetric = throughputOverPdf / FireflyThreshold;
            PathOutput[DispatchIndex] = float4(fireflyMetric, fireflyMetric, fireflyMetric, 1.0f);
            return;
        }
#endif

        if (throughputOverPdf > FireflyThreshold)
        {
            break;
        }

        RayDesc Ray;
        Ray.Origin = position + N * 0.01f;
        Ray.Direction = wi;
        Ray.TMin = 0.001f;
        Ray.TMax = MaxRayDistance;

        RayQuery<PathRayFlags> RayQuery;
        RayQuery.TraceRayInline(Scene, PathRayFlags, 0xFF, Ray);
        while (RayQuery.Proceed())
        {
            uint instanceID = RayQuery.CandidateInstanceID();
            uint primitiveIndex = RayQuery.CandidatePrimitiveIndex();
            float2 barycentrics = RayQuery.CandidateTriangleBarycentrics();
            if (AlphaTest(instanceID, primitiveIndex, barycentrics))
            {
                RayQuery.CommitNonOpaqueTriangleHit();
            }
        }

#if PATH_TRACING_DEBUG
        if (DebugMode == 6 && bounce == 0)
        {
            if (RayQuery.CommittedStatus() == COMMITTED_NOTHING)
                PathOutput[DispatchIndex] = float4(0, 0, 0, 1);
            else
                PathOutput[DispatchIndex] = float4(1, 1, 1, 1);
            return;
        }
#endif

        if (RayQuery.CommittedStatus() == COMMITTED_NOTHING)
        {
			invPdf = rcp(max(pdf, 1e-6f));
#if PATH_TRACING_DEBUG
            if (DebugMode == 10 && bounce == 0)
            {
                float3 skyContribution = EvaluateSky(wi) * throughput * invPdf;
                PathOutput[DispatchIndex] = float4(skyContribution, 1.0f);
                return;
            }

            if (DebugMode == 12 && bounce == 0)
            {
                PathOutput[DispatchIndex] = float4(0.0f, 0.0f, 1.0f, 1.0f);
                return;
            }
#endif

            radiance += EvaluateSky(wi) * throughput * invPdf;
            break;
        }

        uint instanceID = RayQuery.CommittedInstanceID();
        uint primitiveIndex = RayQuery.CommittedPrimitiveIndex();
        float2 barycentrics = RayQuery.CommittedTriangleBarycentrics();
        float hitT = RayQuery.CommittedRayT();
        bool frontFace = RayQuery.CommittedTriangleFrontFace();
        FInstanceData instData = GetInstanceData(instanceID);

#if PATH_TRACING_DEBUG
        if (DebugMode == 9 && bounce == 0)
        {
            float hitDistance = saturate(hitT / MaxRayDistance);
            PathOutput[DispatchIndex] = float4(hitDistance, hitDistance, hitDistance, 1.0f);
            return;
        }
#endif
        float3 nextPos = RayQuery.WorldRayOrigin() + RayQuery.WorldRayDirection() * RayQuery.CommittedRayT();
        float3 nextNormal = GetInterpolatedNormal(instanceID, primitiveIndex, barycentrics);
        if (IsDoubleSided(instData) && !frontFace)
        {
            nextNormal = -nextNormal;
        }
        float2 uv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
        float3 nextAlbedo = SampleAlbedo(instanceID, uv);
        float2 nextMetallicRoughness = SampleMetallicRoughness(instanceID, uv);
        float nextMetallic = nextMetallicRoughness.x;
        float nextRoughness = nextMetallicRoughness.y;

#if PATH_TRACING_DEBUG
        if (DebugMode == 3 && bounce == 0)
        {
            float3 hashColor = HashToColor(instData.BaseColorTextureIndex);
            PathOutput[DispatchIndex] = float4(hashColor, 1);
            return;
        }

        if (DebugMode == 2 && bounce == 0)
        {
            PathOutput[DispatchIndex] = float4(nextAlbedo, 1);
            return;
        }
#endif

        position = nextPos;
        N = nextNormal;
        diffuse = nextAlbedo * (1.0f - nextMetallic);
        specular = lerp(float3(0.04f, 0.04f, 0.04f), nextAlbedo, nextMetallic);
        roughness = max(nextRoughness, 0.03f);
        wo = -wi;
        if (dot(N, wo) < 0.0f)
        {
            N = -N;
        }

#if PATH_TRACING_DEBUG
        if (DebugMode == 12 && bounce == 0)
        {
            float ndv = dot(N, wo);
            float3 c = (ndv < 0.0f) ? float3(1.0f, 0.0f, 0.0f) : float3(0.0f, 1.0f, 0.0f);
            PathOutput[DispatchIndex] = float4(c, 1.0f);
            return;
        }
#endif
    }

    PathOutput[DispatchIndex] = float4(radiance, 1.0f);
}
