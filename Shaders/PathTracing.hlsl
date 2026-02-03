#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"

#ifndef PATH_TRACING_DEBUG
#define PATH_TRACING_DEBUG 0
#endif

// "Efficient Construction of Perpendicular Vectors Without Branching"
float3 GetPerpendicularVector(float3 u)
{
	float3 a = abs(u);
	uint xm = ((a.x - a.y) < 0 && (a.x - a.z) < 0) ? 1 : 0;
	uint ym = (a.y - a.z) < 0 ? (1 ^ xm) : 0;
	uint zm = 1 ^ (xm | ym);
	return cross(u, float3(xm, ym, zm));
}

float3 TangentToWorld(float3 v, float3 N)
{
	const float3 B = GetPerpendicularVector(N);
	const float3 T = cross(B, N);
	return T * v.x + B * v.y + N * v.z;
}

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

float3 ReconstructWorldPosition(uint2 pixel, float depth, uint2 dispatchDim)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(dispatchDim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clip = float4(ndc, depth, 1.0f);
    float4 worldPosition = mul(clip, ViewProjectionInverse);
    worldPosition.xyz /= worldPosition.w;
    return worldPosition.xyz;
}

uint Hash(uint value)
{
    value ^= value >> 17;
    value *= 0xed5ad4bb;
    value ^= value >> 11;
    value *= 0xac4c1b51;
    value ^= value >> 15;
    value *= 0x31848bab;
    value ^= value >> 14;
    return value;
}

float Random01(uint2 pixel, uint salt)
{
    uint seed = Hash(pixel.x + 0x9e3779b9u);
    seed = Hash(seed + pixel.y);
    seed = Hash(seed + salt * 1664525u);
    return (seed & 0x00ffffffu) / 16777216.0f;
}

float3 HashToColor(uint value)
{
    uint h = Hash(value);
    float r = ((h >> 0) & 0xff) / 255.0f;
    float g = ((h >> 8) & 0xff) / 255.0f;
    float b = ((h >> 16) & 0xff) / 255.0f;
    return float3(r, g, b);
}

float3 SampleHemisphereCosine(float2 rand, float3 normal)
{
    float phi = 6.2831853f * rand.x;
    float cosTheta = sqrt(1.0f - rand.y);
    float sinTheta = sqrt(rand.y);

    float3 tangent = normalize(abs(normal.z) < 0.999f ? cross(float3(0.0f, 0.0f, 1.0f), normal) : cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);

    float3 sample = tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) + normal * cosTheta;
    return normalize(sample);
}

// PDF = cos / PI = NdotL / PI
float3 SampleCosHemisphere(float2 randVal, float3 N)
{
	float r = sqrt(randVal.x);
	float phi = 2.0 * PI * randVal.y;

	float sinPhi, cosPhi;
	sincos(phi, sinPhi, cosPhi);

	float3 v = float3(r * cos(phi), r * sin(phi), sqrt(1.0 - randVal.x));

	return TangentToWorld(v, N);
}

float3 SampleConeUniform(float2 randVal, float radius, float3 direction)
{
    float cosTheta = cos(radius);
    float r0 = cosTheta + randVal.x * (1.0f - cosTheta);
    float r = sqrt(max(0.0f, 1.0f - r0 * r0));
    float phi = 2.0f * PI * randVal.y;

    float sinPhi;
    float cosPhi;
    sincos(phi, sinPhi, cosPhi);

    float3 v = float3(r * cosPhi, r * sinPhi, r0);
    return TangentToWorld(v, direction);
}

float Luminance(float3 color)
{
    return dot(max(color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}

float3 DiffuseBRDF(float3 diffuse)
{
    return diffuse / 3.14159265f;
}

float3 SpecularBRDF(float3 N, float3 V, float3 L, float3 specular, float roughness, out float3 F)
{
    float3 H = normalize(V + L);

    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float alpha = roughness * roughness;
    float D = D_GGX(NdotH, alpha);
    float V_G = V_SmithGGX(NdotV, NdotL, alpha);
    F = FresnelSchlick(VdotH, specular);

    return D * V_G * F;
}

float3 BRDF(float3 wi, float3 wo, float3 N, float3 diffuse, float3 specular, float roughness)
{
    float3 F;
    float3 specularBrdf = SpecularBRDF(N, wo, wi, specular, roughness, F);
    float3 diffuseBrdf = DiffuseBRDF(diffuse);
    return diffuseBrdf + specularBrdf;
}

// Sample GGX distribution for importance sampling
// Returns sampled half-vector H in tangent space
float3 SampleGGX(float2 rand, float alpha)
{
    float phi = 6.2831853f * rand.x;
    float cosTheta = sqrt((1.0f - rand.y) / (1.0f + (alpha * alpha - 1.0f) * rand.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 EvaluateSky(float3 direction)
{
    TextureCube EnvironmentMap = ResourceDescriptorHeap[EnvironmentCubeBindlessIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return EnvironmentMap.SampleLevel(LinearSampler, direction, 0).rgb;
}

// Structure to hold per-instance geometry buffer indices
struct FInstanceData
{
    uint PositionBufferIndex;
    uint NormalBufferIndex;
    uint UVBufferIndex;
    uint IndexBufferIndex;
    uint TangentBufferIndex;
    uint BaseColorTextureIndex;
    uint NormalTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint Flags;
    float4 BaseColorFactorAndAlpha;
    float4 MetallicRoughnessAlphaCutoff;
    row_major float4x4 WorldInverseTranspose;
};

static const uint INSTANCE_FLAG_DOUBLE_SIDED = 1u;

bool IsDoubleSided(FInstanceData instData)
{
    return (instData.Flags & INSTANCE_FLAG_DOUBLE_SIDED) != 0u;
}

// Fetch instance data for a hit
FInstanceData GetInstanceData(uint instanceID)
{
    StructuredBuffer<FInstanceData> InstanceDataBuffer = ResourceDescriptorHeap[InstanceDataBufferIndex];
    return InstanceDataBuffer[instanceID];
}

// Fetch triangle indices
uint3 GetTriangleIndices(uint instanceID, uint primitiveIndex)
{
    FInstanceData instData = GetInstanceData(instanceID);
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[instData.IndexBufferIndex];
    
    uint baseIndex = primitiveIndex * 3;
    uint3 indices;
    indices.x = IndexBuffer[baseIndex + 0];
    indices.y = IndexBuffer[baseIndex + 1];
    indices.z = IndexBuffer[baseIndex + 2];
    return indices;
}

// Interpolate vertex attribute using barycentric coordinates
float3 InterpolateFloat3(float3 v0, float3 v1, float3 v2, float2 barycentrics)
{
    return v0 * (1.0f - barycentrics.x - barycentrics.y) + v1 * barycentrics.x + v2 * barycentrics.y;
}

float2 InterpolateFloat2(float2 v0, float2 v1, float2 v2, float2 barycentrics)
{
    return v0 * (1.0f - barycentrics.x - barycentrics.y) + v1 * barycentrics.x + v2 * barycentrics.y;
}

// Get interpolated UV at hit point
float2 GetInterpolatedUV(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    FInstanceData instData = GetInstanceData(instanceID);
    uint3 indices = GetTriangleIndices(instanceID, primitiveIndex);
    
    StructuredBuffer<float2> UVBuffer = ResourceDescriptorHeap[instData.UVBufferIndex];
    
    float2 uv0 = UVBuffer[indices.x];
    float2 uv1 = UVBuffer[indices.y];
    float2 uv2 = UVBuffer[indices.z];

    return InterpolateFloat2(uv0, uv1, uv2, barycentrics);
}

float4 GetInterpolatedTangent(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    FInstanceData instData = GetInstanceData(instanceID);
    uint3 indices = GetTriangleIndices(instanceID, primitiveIndex);

    StructuredBuffer<float4> TangentBuffer = ResourceDescriptorHeap[instData.TangentBufferIndex];

    float4 t0 = TangentBuffer[indices.x];
    float4 t1 = TangentBuffer[indices.y];
    float4 t2 = TangentBuffer[indices.z];

    float3 tangent = InterpolateFloat3(t0.xyz, t1.xyz, t2.xyz, barycentrics);
    float handedness = t0.w * (1.0f - barycentrics.x - barycentrics.y) + t1.w * barycentrics.x + t2.w * barycentrics.y;

    return float4(tangent, handedness);
}

// Get interpolated normal at hit point (transforms from object space to world space)
float3 GetInterpolatedNormal(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    FInstanceData instData = GetInstanceData(instanceID);
    uint3 indices = GetTriangleIndices(instanceID, primitiveIndex);
    
    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[instData.NormalBufferIndex];
    
    float3 n0 = NormalBuffer[indices.x];
    float3 n1 = NormalBuffer[indices.y];
    float3 n2 = NormalBuffer[indices.z];
    
    // Interpolate object-space normal
    float3 objectNormal = InterpolateFloat3(n0, n1, n2, barycentrics);
    
    // Transform to world space using inverse transpose
    float3 worldNormal = mul((float3x3)instData.WorldInverseTranspose, objectNormal);

    worldNormal = normalize(worldNormal);

    if (instData.NormalTextureIndex == 0xFFFFFFFF)
    {
        return worldNormal;
    }

    float4 tangentPacked = GetInterpolatedTangent(instanceID, primitiveIndex, barycentrics);
    float3 worldTangent = normalize(mul((float3x3)instData.WorldInverseTranspose, tangentPacked.xyz));
    worldTangent = normalize(worldTangent - worldNormal * dot(worldNormal, worldTangent));
    float3 worldBitangent = cross(worldNormal, worldTangent) * tangentPacked.w;

    float2 uv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
    Texture2D<float4> NormalTexture = ResourceDescriptorHeap[instData.NormalTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    float3 tangentNormal = NormalTexture.SampleLevel(LinearSampler, uv, 0).xyz * 2.0f - 1.0f;

    float3x3 TBN = float3x3(worldTangent, worldBitangent, worldNormal);
    return normalize(mul(tangentNormal, TBN));
}

// Sample albedo from texture
float3 SampleAlbedo(uint instanceID, float2 uv)
{
    FInstanceData instData = GetInstanceData(instanceID);
    float3 baseColorFactor = instData.BaseColorFactorAndAlpha.rgb;
    
    if (instData.BaseColorTextureIndex == 0xFFFFFFFF)
    {
        return baseColorFactor;
    }
    
    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[instData.BaseColorTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return baseColorFactor * BaseColorTexture.SampleLevel(LinearSampler, uv, 0).rgb;
}

float SampleOpacity(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    FInstanceData instData = GetInstanceData(instanceID);
    float alpha = instData.BaseColorFactorAndAlpha.a;

    if (instData.BaseColorTextureIndex == 0xFFFFFFFF)
    {
        return alpha;
    }

    float2 uv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[instData.BaseColorTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return alpha * BaseColorTexture.SampleLevel(LinearSampler, uv, 0).a;
}

bool AlphaTest(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    if (AlphaMode == 0u)
    {
        return true;
    }

    if (AlphaMode == 2u)
    {
        return false;
    }

    FInstanceData instData = GetInstanceData(instanceID);
    float alpha = SampleOpacity(instanceID, primitiveIndex, barycentrics);
    return alpha >= instData.MetallicRoughnessAlphaCutoff.z;
}

// Sample metallic and roughness from texture
float2 SampleMetallicRoughness(uint instanceID, float2 uv)
{
    FInstanceData instData = GetInstanceData(instanceID);
    float metallic = instData.MetallicRoughnessAlphaCutoff.x;
    float roughness = instData.MetallicRoughnessAlphaCutoff.y;
    
    if (instData.MetallicRoughnessTextureIndex == 0xFFFFFFFF)
    {
        return float2(saturate(metallic), max(roughness, 0.03f));
    }
    
    Texture2D<float4> MetallicRoughnessTexture = ResourceDescriptorHeap[instData.MetallicRoughnessTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    float2 mr = MetallicRoughnessTexture.SampleLevel(LinearSampler, uv, 0).bg; // Blue=metallic, Green=roughness
    
    // Clamp to valid ranges
    metallic = saturate(metallic * mr.x);  // [0, 1]
    roughness = max(roughness * mr.y, 0.03f);  // Minimum roughness to avoid numerical issues
    
    return float2(metallic, roughness);
}

// Constants for ray tracing
static const uint RayQueryThreadGroupSize = 8;
static const uint PathRayFlags = RAY_FLAG_NONE;
static const uint ShadowRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
static const float MaxRayDistance = 1000.0f;
static const float FireflyThreshold = 5.0f;

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
        float3 wi = LightDirection;
        float2 randLight = float2(
            Random01(DispatchIndex, FrameIndex * (MaxBounces + 1u) * 5u + bounce * 5u + 1u),
            Random01(DispatchIndex, FrameIndex * (MaxBounces + 1u) * 5u + bounce * 5u + 2u)
        );
        RayDesc ShadowRay;
        ShadowRay.Origin = position + N * 0.01f;
        ShadowRay.Direction = SampleConeUniform(randLight, LightRadius, wi);
        ShadowRay.TMin = 0.0f;
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
            float3 H = TangentToWorld(SampleGGX(randSample, roughness * roughness), N);
            wi = reflect(-wo, H);

            float3 F;
            float3 specularBrdf = SpecularBRDF(N, wo, wi, specular, roughness, F);
            float NdotL = saturate(dot(N, wi));

            throughput *= specularBrdf * NdotL;

            float D = D_GGX(saturate(dot(N, H)), roughness * roughness);
            float NdotH = saturate(dot(N, H));
            float LdotH = saturate(dot(wi, H));
            pdf *= (D * NdotH / max(4.0f * LdotH, 0.0001f)) * (1.0f - probDiffuse);
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
