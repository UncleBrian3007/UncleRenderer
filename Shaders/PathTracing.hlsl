#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"

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
    int DebugMode; // 0=Normal PT, 1=GBuffer Albedo, 2=First Hit Albedo, 3=Texture Index Hash
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

// Sample GGX distribution for importance sampling
// Returns sampled half-vector H in tangent space
float3 SampleGGX(float2 rand, float alpha)
{
    float phi = 6.2831853f * rand.x;
    float cosTheta = sqrt((1.0f - rand.y) / (1.0f + (alpha * alpha - 1.0f) * rand.y));
    float sinTheta = sqrt(1.0f - cosTheta * cosTheta);
    
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

// Transform vector from tangent space to world space
float3 TangentToWorld(float3 v, float3 N)
{
    float3 T = normalize(abs(N.z) < 0.999f ? cross(float3(0.0f, 0.0f, 1.0f), N) : cross(float3(0.0f, 1.0f, 0.0f), N));
    float3 B = cross(N, T);
    return v.x * T + v.y * B + v.z * N;
}

// PDF for GGX importance sampling
float PDF_GGX(float NdotH, float VdotH, float alpha)
{
    float D = D_GGX(NdotH, alpha);
    return (D * NdotH) / (4.0f * VdotH);
}

// Sample BRDF using importance sampling (GGX for specular, cosine-weighted for diffuse)
// Returns sampled direction, PDF of the selected lobe only, lobe selection probability, and which lobe was sampled
// Sampling probability is based on roughness, NOT on Fresnel energy (keeps probability and BRDF energy separate)
float3 SampleBRDF(float2 rand, float3 N, float3 V, float3 albedo, float metallic, float roughness, out float pdf, out float lobeProbability, out bool sampledSpecular)
{
    float alpha = roughness * roughness;
    
    // Lobe selection probability based on roughness (NOT Fresnel energy)
    // Rough surfaces: favor diffuse sampling
    // Smooth surfaces: favor specular sampling
    // This is purely for sampling efficiency, NOT for BRDF energy weighting
    float specularProbability = 0.5f + 0.3f * (1.0f - roughness); // Range: 0.5 (rough) to 0.8 (smooth)
    specularProbability = lerp(specularProbability, 1.0f, metallic); // Metals always prefer specular
    specularProbability = clamp(specularProbability, 0.1f, 0.9f); // Keep both lobes accessible
    
    float3 L;
    
    if (rand.x < specularProbability)
    {
        // Sample specular (GGX)
        sampledSpecular = true;
        lobeProbability = specularProbability;
        
        float2 u = float2((rand.x - 0.0f) / specularProbability, rand.y);
        float3 H_tangent = SampleGGX(u, alpha);
        float3 H = TangentToWorld(H_tangent, N);
        
        // Reflect view direction around half-vector to get light direction
        L = reflect(-V, H);
        
        float NdotL = dot(N, L);
        if (NdotL <= 0.0f)
        {
            pdf = 0.0f;
            return float3(0.0f, 0.0f, 0.0f);
        }
        
        float NdotH = saturate(dot(N, H));
        float VdotH = saturate(dot(V, H));
        
        // Use only the PDF of the specular lobe (NOT mixture PDF)
        pdf = PDF_GGX(NdotH, VdotH, alpha);
    }
    else
    {
        // Sample diffuse (cosine-weighted hemisphere)
        sampledSpecular = false;
        lobeProbability = 1.0f - specularProbability;
        
        float2 u = float2((rand.x - specularProbability) / (1.0f - specularProbability), rand.y);
        L = SampleHemisphereCosine(u, N);
        
        float NdotL = dot(N, L);
        if (NdotL <= 0.0f)
        {
            pdf = 0.0f;
            return float3(0.0f, 0.0f, 0.0f);
        }
        
        // Use only the PDF of the diffuse lobe (NOT mixture PDF)
        pdf = NdotL / 3.14159265f;
    }
    
    return L;
}

float3 EvaluateSky(float3 direction)
{
    float t = saturate(direction.y * 0.5f + 0.5f);
    return lerp(float3(0.5f, 0.6f, 0.8f), float3(0.1f, 0.2f, 0.4f), 1.0f - t);
}

// Structure to hold per-instance geometry buffer indices
struct FInstanceData
{
    uint PositionBufferIndex;
    uint NormalBufferIndex;
    uint UVBufferIndex;
    uint IndexBufferIndex;
    uint BaseColorTextureIndex;
    uint NormalTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint Padding;
};

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

// Get interpolated normal at hit point (transforms from object space to world space)
float3 GetInterpolatedNormal(uint instanceID, uint primitiveIndex, float2 barycentrics, float3x4 objectToWorld)
{
    FInstanceData instData = GetInstanceData(instanceID);
    uint3 indices = GetTriangleIndices(instanceID, primitiveIndex);
    
    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[instData.NormalBufferIndex];
    
    float3 n0 = NormalBuffer[indices.x];
    float3 n1 = NormalBuffer[indices.y];
    float3 n2 = NormalBuffer[indices.z];
    
    // Interpolate object-space normal
    float3 objectNormal = InterpolateFloat3(n0, n1, n2, barycentrics);
    
    // Transform to world space (use upper 3x3 for normal transformation)
    float3 worldNormal = mul((float3x3)objectToWorld, objectNormal);
    
    return normalize(worldNormal);
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

// Sample albedo from texture
float3 SampleAlbedo(uint instanceID, float2 uv)
{
    FInstanceData instData = GetInstanceData(instanceID);
    
    if (instData.BaseColorTextureIndex == 0xFFFFFFFF)
    {
        return float3(1.0f, 0.0f, 1.0f); // Default magenta if no texture (highly visible)
    }
    
    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[instData.BaseColorTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return BaseColorTexture.SampleLevel(LinearSampler, uv, 0).rgb;
}

// Sample metallic and roughness from texture
float2 SampleMetallicRoughness(uint instanceID, float2 uv)
{
    FInstanceData instData = GetInstanceData(instanceID);
    
    if (instData.MetallicRoughnessTextureIndex == 0xFFFFFFFF)
    {
        // Default values: non-metallic, moderate roughness
        return float2(0.0f, 0.5f);
    }
    
    Texture2D<float4> MetallicRoughnessTexture = ResourceDescriptorHeap[instData.MetallicRoughnessTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    float2 mr = MetallicRoughnessTexture.SampleLevel(LinearSampler, uv, 0).bg; // Blue=metallic, Green=roughness
    
    // Clamp to valid ranges
    float metallic = saturate(mr.x);  // [0, 1]
    float roughness = max(mr.y, 0.03f);  // Minimum roughness to avoid numerical issues
    
    return float2(metallic, roughness);
}

// Constants for ray tracing
static const uint RayQueryThreadGroupSize = 8;
static const uint PathRayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
    | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
static const uint ShadowRayFlags = RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
    | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES
    | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
static const float MaxRayDistance = 1000.0f;
static const float SecondaryBounceAlbedo = 0.5f;
static const float RussianRouletteThreshold = 0.1f;
static const float FireflyThreshold = 5.0f;

// Evaluate direct lighting using PBR (Next Event Estimation)
// Traces a shadow ray to the sun and returns the direct light contribution
float3 EvaluateDirectLighting(float3 hitPoint, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
    // Sun direction from SceneConstants (normalized)
    float3 lightDir = normalize(LightDirection);
    
    // Check if surface faces the light
    float NdotL = dot(normal, lightDir);
    if (NdotL <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    
    // Trace shadow ray to check visibility
    // Offset origin along normal to avoid self-intersection
    RayDesc ShadowRay;
    ShadowRay.Origin = hitPoint + normal * 0.01f;
    ShadowRay.Direction = lightDir;
    ShadowRay.TMin = 0.001f;
    ShadowRay.TMax = MaxRayDistance;
    
    RayQuery<ShadowRayFlags> ShadowQuery;
    ShadowQuery.TraceRayInline(Scene, ShadowRayFlags, 0xFF, ShadowRay);
    while (ShadowQuery.Proceed())
    {
    }
    
    // If ray hit something, the point is in shadow
    if (ShadowQuery.CommittedStatus() != COMMITTED_NOTHING)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    
    // Calculate F0 for PBR (base reflectivity at normal incidence)
    // Default dielectric F0 is 0.04, metals use albedo as F0
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
    
    // Evaluate PBR BRDF: combines diffuse and specular
    // Returns (diffuse + specular) * NdotL
    float3 brdf = EvaluatePBR(albedo, metallic, roughness, F0, normal, viewDir, lightDir);
    
    // Apply light color and intensity
    float3 directLight = brdf * LightColor * LightIntensity;
    return directLight;
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
    
    // Debug Mode 1: Visualize GBuffer albedo (rasterized result)
    // This is placed BEFORE depth check to show albedo for all pixels including sky
    if (DebugMode == 1)
    {
        float3 dbgAlbedo = GBufferC.Load(int3(DispatchIndex, 0)).rgb;
        PathOutput[DispatchIndex] = float4(dbgAlbedo, 1);
        return;
    }
    
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
    float3 normalView = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float3 worldNormal = normalize(mul(normalView, (float3x3)ViewInverse));
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

    // Path tracing with multiple bounces - Direct Lighting First approach
    // Initialize current surface data from G-Buffer before loop
    float3 currentPos = worldPosition;
    float3 currentNormal = worldNormal;
    float3 currentAlbedo = albedo;
    
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f); // Start with full energy
    
    // Track incoming ray direction - direction from which light arrives at surface
    // Initially from camera to first surface point
    float3 incomingDir = normalize(worldPosition - CameraPosition);
    
    for (uint bounce = 0; bounce < MaxBounces; ++bounce)
    {
        // View direction is opposite of incoming ray direction
        float3 viewDir = -incomingDir;
        
        // Direct Lighting: Evaluate PBR at current surface point
        float3 directLight = EvaluateDirectLighting(currentPos, currentNormal, viewDir, currentAlbedo, currentMetallic, currentRoughness);
        
        // Debug Mode 5: Indirect Lighting Only - skip direct lighting accumulation
        if (DebugMode != 5)
        {
            radiance += throughput * directLight; // PBR already includes albedo and BRDF
        }
        
        // Debug Mode 4: Direct Lighting Only - terminate path after accumulating direct lighting
        if (DebugMode == 4)
        {
            break;
        }
        
        // Generate random values for indirect lighting BRDF importance sampling
        float2 rand = float2(
            Random01(DispatchIndex, FrameIndex * MaxBounces * 2u + bounce * 2u + 1u),
            Random01(DispatchIndex, FrameIndex * MaxBounces * 2u + bounce * 2u + 2u)
        );
        
        // Sample BRDF using importance sampling (GGX for specular, cosine-weighted for diffuse)
        float pdf;
        float lobeProbability;
        bool sampledSpecular;
        float3 rayDirection = SampleBRDF(rand, currentNormal, viewDir, currentAlbedo, currentMetallic, currentRoughness, pdf, lobeProbability, sampledSpecular);
        
        if (pdf <= 0.0f)
        {
            break; // Invalid sample
        }
        
        // CRITICAL: Save previous surface properties BEFORE ray tracing
        // These will be used for throughput calculation (BRDF was sampled from this surface)
        float3 prevNormal = currentNormal;
        float3 prevAlbedo = currentAlbedo;
        float prevMetallic = currentMetallic;
        float prevRoughness = currentRoughness;
        float3 prevViewDir = viewDir;
        
        // Trace indirect ray
        RayDesc Ray;
        Ray.Origin = currentPos + currentNormal * 0.01f; // Offset to avoid self-intersection
        Ray.Direction = rayDirection;
        Ray.TMin = 0.001f;
        Ray.TMax = MaxRayDistance;
        
        RayQuery<PathRayFlags> RayQuery;
        RayQuery.TraceRayInline(Scene, PathRayFlags, 0xFF, Ray);
        while (RayQuery.Proceed())
        {
        }
        
        // Debug Mode 6: Hit/Miss Mask - visualize ray coverage
        if (DebugMode == 6 && bounce == 0)
        {
            if (RayQuery.CommittedStatus() == COMMITTED_NOTHING)
                PathOutput[DispatchIndex] = float4(0, 0, 0, 1); // Black = miss (sky)
            else
                PathOutput[DispatchIndex] = float4(1, 1, 1, 1); // White = hit (geometry)
            return;
        }
        
        // Check if ray hit anything
        if (RayQuery.CommittedStatus() == COMMITTED_NOTHING)
        {
            // Ray escaped to sky - add sky contribution and terminate
            radiance += throughput * EvaluateSky(rayDirection);
            break;
        }
        
        // Ray hit something - fetch next surface data
        uint instanceID = RayQuery.CommittedInstanceID();
        uint primitiveIndex = RayQuery.CommittedPrimitiveIndex();
        float2 barycentrics = RayQuery.CommittedTriangleBarycentrics();
        float hitT = RayQuery.CommittedRayT();
        float3x4 objectToWorld = RayQuery.CommittedObjectToWorld3x4();
        
        // Get next hit point position
        float3 nextPos = Ray.Origin + rayDirection * hitT;
        
        // Get interpolated normal and UV from actual geometry data
        // Normal is transformed from object space to world space
        float3 nextNormal = GetInterpolatedNormal(instanceID, primitiveIndex, barycentrics, objectToWorld);
        float2 uv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
        
        // Sample albedo from texture for next surface
        float3 nextAlbedo = SampleAlbedo(instanceID, uv);
        
        // Sample metallic and roughness from texture for next surface
        float2 nextMetallicRoughness = SampleMetallicRoughness(instanceID, uv);
        float nextMetallic = nextMetallicRoughness.x;
        float nextRoughness = nextMetallicRoughness.y;
        
        // Debug Mode 3: Visualize BaseColorTextureIndex per instance (first hit only)
        // Shows hash colors to verify that different instances have different texture bindings
        if (DebugMode == 3 && bounce == 0)
        {
            FInstanceData instData = GetInstanceData(instanceID);
            float3 hashColor = HashToColor(instData.BaseColorTextureIndex);
            PathOutput[DispatchIndex] = float4(hashColor, 1);
            return;
        }
        
        // Debug Mode 2: Visualize first ray hit albedo
        if (DebugMode == 2 && bounce == 0)
        {
            PathOutput[DispatchIndex] = float4(nextAlbedo, 1);
            return;
        }
        
        // NOW evaluate BRDF using PREVIOUS surface properties
        // (the surface from which we sampled the ray direction)
        // Calculate F0 for PBR
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), prevAlbedo, prevMetallic);
        float NdotL = saturate(dot(prevNormal, rayDirection));
        float NdotV = saturate(dot(prevNormal, prevViewDir));
        float3 H = normalize(prevViewDir + rayDirection);
        float NdotH = saturate(dot(prevNormal, H));
        float VdotH = saturate(dot(prevViewDir, H));
        
        float3 brdf;
        if (sampledSpecular)
        {
            // Evaluate only specular lobe (Cook-Torrance)
            float alpha = prevRoughness * prevRoughness;
            float D = D_GGX(NdotH, alpha);
            float G = V_SmithGGX(NdotV, NdotL, alpha);
            float3 F = FresnelSchlick(VdotH, F0);
            
            brdf = (D * G * F) / max(4.0f * NdotV * NdotL, 0.001f) * NdotL;
        }
        else
        {
            // Evaluate only diffuse lobe (Lambert)
            // Pure Lambert without Fresnel for color stability across bounces
            float3 kD = (1.0f - prevMetallic);
            brdf = kD * prevAlbedo / 3.14159265f * NdotL;
        }
        
        // Update throughput with correct Monte Carlo estimator:
        // throughput *= (f_lobe * cosTheta) / (pdf_lobe * lobeProbability)
        // Note: brdf already includes NdotL, so brdf = f * NdotL = f * cosTheta
        // Therefore: throughput *= brdf / (pdf * lobeProbability)
        // The lobeProbability compensation is CRITICAL: without it, paths are statistically underweighted
        throughput *= brdf / (pdf * lobeProbability);
        
        // Firefly rejection: terminate paths with unstable throughput
        // This reduces high-energy outliers (bright white pixels) without biasing the estimator
        // Early path termination is mathematically valid - it doesn't introduce bias
        float luminanceThroughput = max(throughput.r, max(throughput.g, throughput.b));
        if (luminanceThroughput > FireflyThreshold)
        {
            break; // Terminate this path early to prevent fireflies
        }
        
        // ONLY NOW update current surface state to next surface for next iteration
        currentPos = nextPos;
        currentNormal = nextNormal;
        currentAlbedo = nextAlbedo;
        currentMetallic = nextMetallic;
        currentRoughness = nextRoughness;
        
        // Update incoming direction for next bounce
        incomingDir = rayDirection;
        
        // Unbiased Russian roulette termination
        // Calculate survival probability based on throughput luminance
        float maxThroughput = max(throughput.r, max(throughput.g, throughput.b));
        if (maxThroughput < RussianRouletteThreshold && bounce > 1)
        {
            // Use throughput as survival probability (clamped to reasonable range)
            float survivalProbability = clamp(maxThroughput, 0.1f, 1.0f);
            
            // Generate random value for Russian roulette decision
            float rrRand = Random01(DispatchIndex, FrameIndex * MaxBounces * 3u + bounce * 3u);
            
            if (rrRand >= survivalProbability)
            {
                // Terminate path
                break;
            }
            
            // Path survived - compensate throughput to maintain unbiased estimator
            // Divide by survival probability: E[throughput/p] = throughput
            throughput /= survivalProbability;
        }
    }

    PathOutput[DispatchIndex] = float4(radiance, 1.0f);
}
