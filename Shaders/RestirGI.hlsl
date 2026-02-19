#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"
#include "RestirGIReservoir.hlsli"
#include "RestirGISamplingCommon.hlsli"
#include "PathBrdfCommon.hlsli"

RaytracingAccelerationStructure Scene : register(t0);

cbuffer RestirGIConstants : register(b1)
{
    uint OutputWidth;
    uint OutputHeight;
    uint FrameIndex;
    uint SamplesPerPixel;
    float Intensity;
    float RayLength;
    float ClampThreshold;
    uint Enabled;
    uint HistoryValid;
    uint TemporalReuseEnabled;
    float TemporalAdditionalScale;
    float SpatialAdditionalScale;
    float ResolveMinDenominator;
    float ResolveMaxNormalization;
    float ResolveLowSampleBoostGuard;
    uint ResolveUseConfidence;
};

cbuffer RestirGIBindless : register(b2)
{
    uint OutputOrReservoirUavIndex;
    uint ReservoirSrvIndex;
    uint DepthIndex;
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint InstanceDataBufferIndex;
    uint EnvironmentCubeBindlessIndex;
    uint LinearClampSamplerIndex;
    uint VelocityIndex;
    uint HistoryTextureIndex;
    uint HistoryGeomAIndex;
    uint HistoryGeomBIndex;
};

#include "RayTracingCommon.hlsl"

static const uint RestirGIRayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;


static const float kMaxReservoirWeightSum = 1e6f;
static const float kAcceptanceDepthThreshold = 0.01f;
static const float kAcceptanceNormalSimilarityThreshold = 0.85f;
static const float kAcceptanceAlbedoDeltaThreshold = 0.35f;
static const float kAcceptanceMaterialDeltaThreshold = 0.2f;

static const uint kMaxReservoirSampleCount = 65535u;

uint ClampSampleCount(uint value)
{
    return min(value, kMaxReservoirSampleCount);
}

bool IsFiniteFloat3(float3 value)
{
    return all(isfinite(value));
}

bool IsReservoirStateValid(FRestirGIReservoir reservoir)
{
    return reservoir.SampleCount > 0u
        && isfinite(reservoir.WeightSum) && reservoir.WeightSum > 0.0f
        && isfinite(reservoir.SelectedWeight) && reservoir.SelectedWeight > 0.0f
        && IsFiniteFloat3(reservoir.SampleRadiance);
}

float ClampReservoirWeightSum(float value)
{
    if (!isfinite(value) || value < 0.0f)
    {
        return 0.0f;
    }

    return min(value, kMaxReservoirWeightSum);
}

bool IsReservoirNearSaturation(FRestirGIReservoir reservoir)
{
    return (reservoir.WeightSum >= kMaxReservoirWeightSum * 0.95f)
        || (reservoir.SampleCount >= (kMaxReservoirSampleCount - 16u));
}

float ComputeSafeNormalization(FRestirGIReservoir reservoir)
{
    float safeMinDenominator = max(ResolveMinDenominator, 1e-6f);
    float safeMaxNormalization = max(ResolveMaxNormalization, 1.0f);
    float denom = max(reservoir.SelectedWeight * float(max(1u, reservoir.SampleCount)), safeMinDenominator);
    float normRaw = ClampReservoirWeightSum(reservoir.WeightSum) / denom;
    if (!isfinite(normRaw) || normRaw < 0.0f)
    {
        normRaw = 0.0f;
    }

    return min(normRaw, safeMaxNormalization);
}

uint Hash32(uint value)
{
    value ^= value >> 17;
    value *= 0xed5ad4bbu;
    value ^= value >> 11;
    value *= 0xac4c1b51u;
    value ^= value >> 15;
    value *= 0x31848babu;
    value ^= value >> 14;
    return value;
}

float Random01(uint2 pixel, uint salt)
{
    uint seed = Hash32(pixel.x + 0x9e3779b9u);
    seed = Hash32(seed + pixel.y);
    seed = Hash32(seed + salt * 1664525u);
    return (seed & 0x00ffffffu) / 16777216.0f;
}

float3 ReconstructWorldPosition(uint2 pixel, float depth, uint2 dispatchDim)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(dispatchDim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clip = float4(ndc, depth, 1.0f);
    float4 worldPosition = mul(clip, ViewProjectionInverse);
    worldPosition.xyz /= worldPosition.w;
    return worldPosition.xyz;
}

float Luminance(float3 color)
{
    return dot(max(color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
}


bool TryGetReprojectedHistoryPixel(uint2 pixel, uint2 resolution, Texture2D<float2> velocityTexture, out uint2 historyPixel)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(resolution);
    float2 velocityNdc = velocityTexture[pixel];
    float2 previousUv = float2(
        uv.x - velocityNdc.x * 0.5f,
        uv.y + velocityNdc.y * 0.5f);
    if (previousUv.x <= 0.0f || previousUv.y <= 0.0f || previousUv.x >= 1.0f || previousUv.y >= 1.0f)
    {
        historyPixel = 0u.xx;
        return false;
    }

    historyPixel = uint2(previousUv * float2(resolution));
    historyPixel = min(historyPixel, resolution - 1u);
    return true;
}

bool IsTemporalHistoryAccepted(
    uint2 centerPixel,
    uint2 historyPixel,
    Texture2D<float> depthTexture,
    Texture2D<float4> gBufferA,
    Texture2D<float4> gBufferB,
    Texture2D<float4> gBufferC,
    Texture2D<float4> historyTexture,
    Texture2D<float4> historyGeomA,
    Texture2D<float4> historyGeomB,
    FRestirGIReservoir historyReservoir)
{
    float centerDepth = depthTexture[centerPixel];
    float historyDepth = historyTexture[historyPixel].a;
    if (centerDepth <= 0.0f || centerDepth >= 1.0f || historyDepth <= 0.0f || historyDepth >= 1.0f)
    {
        return false;
    }

    float depthDelta = abs(centerDepth - historyDepth);
    if (depthDelta > kAcceptanceDepthThreshold)
    {
        return false;
    }

    float3 centerNormal = normalize(gBufferA[centerPixel].xyz * 2.0f - 1.0f);
    float4 historyGeomAValue = historyGeomA[historyPixel];
    float3 historyNormal = normalize(historyGeomAValue.xyz * 2.0f - 1.0f);
    float normalSimilarity = dot(centerNormal, historyNormal);
    if (!isfinite(normalSimilarity) || normalSimilarity < kAcceptanceNormalSimilarityThreshold)
    {
        return false;
    }

    float3 centerAlbedo = gBufferC[centerPixel].rgb;
    float4 historyGeomBValue = historyGeomB[historyPixel];
    float3 historyAlbedo = historyGeomBValue.rgb;
    float albedoDelta = length(centerAlbedo - historyAlbedo);
    if (!isfinite(albedoDelta) || albedoDelta > kAcceptanceAlbedoDeltaThreshold)
    {
        return false;
    }

    float centerRoughness = saturate(gBufferB[centerPixel].z);
    float centerMetalness = saturate(gBufferB[centerPixel].y);
    float historyRoughness = saturate(historyGeomAValue.w);
    float historyMetalness = saturate(historyGeomBValue.w);
    float2 centerMaterial = float2(centerRoughness, centerMetalness);
    float2 historyMaterial = float2(historyRoughness, historyMetalness);
    float materialDelta = length(centerMaterial - historyMaterial);
    if (!isfinite(materialDelta) || materialDelta > kAcceptanceMaterialDeltaThreshold)
    {
        return false;
    }

    bool centerValid = all(isfinite(centerNormal)) && all(isfinite(centerAlbedo)) && all(isfinite(centerMaterial));
    bool historyValid = all(isfinite(historyNormal)) && all(isfinite(historyAlbedo)) && all(isfinite(historyMaterial));
    if (!centerValid || !historyValid)
    {
        return false;
    }

    float historyWeightSum = historyReservoir.WeightSum;
    float historySelectedWeight = historyReservoir.SelectedWeight;
    float historyLum = Luminance(historyReservoir.SampleRadiance);
    return historyReservoir.SampleCount > 0u
        && isfinite(historyWeightSum) && historyWeightSum > 0.0f
        && isfinite(historySelectedWeight) && historySelectedWeight > 0.0f
        && isfinite(historyLum) && historyLum >= 0.0f;
}

float3 EvaluateHitIncomingRadiance(uint instanceID, float2 hitUv, float3 hitNormal, float3 hitAlbedo, float hitMetallic, float hitRoughness, float3 outgoingDirection)
{
    float3 diffuse = hitAlbedo * (1.0f - saturate(hitMetallic));
    float3 specular = lerp(0.04f.xxx, hitAlbedo, saturate(hitMetallic));
    float roughness = max(hitRoughness, 0.03f);

    float3 wi = normalize(LightDirection);
    float3 wo = normalize(outgoingDirection);
    float NdotL = saturate(dot(hitNormal, wi));
    float3 directBrdf = BRDF(wi, wo, hitNormal, diffuse, specular, roughness);
    float3 direct = directBrdf * (LightColor * LightIntensity) * NdotL;

    float3 emissive = max(SampleEmissive(instanceID, hitUv), 0.0f.xxx);
    return max(direct + emissive, 0.0f);
}

float3 SampleCandidateGI(float3 worldPos, float3 normal, uint2 pixel, uint sampleIndex)
{
    float2 Xi = float2(
        Random01(pixel, FrameIndex * 1021u + sampleIndex * 97u + 1u),
        Random01(pixel, FrameIndex * 1303u + sampleIndex * 131u + 7u));

    float3 direction = SampleHemisphereCosine(Xi, normal);

    RayDesc ray;
    ray.Origin = worldPos + normal * 0.01f;
    ray.Direction = direction;
    ray.TMin = 1e-3f;
    ray.TMax = max(0.1f, RayLength);

    RayQuery<RestirGIRayFlags> query;
    query.TraceRayInline(Scene, RestirGIRayFlags, 0xFF, ray);

    while (query.Proceed())
    {
        uint instanceID = query.CandidateInstanceID();
        uint primitiveIndex = query.CandidatePrimitiveIndex();
        float2 barycentrics = query.CandidateTriangleBarycentrics();
        if (AlphaTest(instanceID, primitiveIndex, barycentrics))
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    float3 incomingRadiance = 0.0f.xxx;
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        uint instanceID = query.CommittedInstanceID();
        uint primitiveIndex = query.CommittedPrimitiveIndex();
        float2 barycentrics = query.CommittedTriangleBarycentrics();

        float2 uv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
        float3 hitAlbedo = SampleAlbedo(instanceID, uv);
        float2 hitMR = SampleMetallicRoughness(instanceID, uv);
        float3 hitNormal = GetInterpolatedNormal(instanceID, primitiveIndex, barycentrics);
        if (dot(hitNormal, -direction) < 0.0f)
        {
            hitNormal = -hitNormal;
        }

        incomingRadiance = EvaluateHitIncomingRadiance(instanceID, uv, hitNormal, hitAlbedo, hitMR.x, hitMR.y, -direction);
    }
    else
    {
        incomingRadiance = EvaluateSky(direction);
    }

    return max(incomingRadiance, 0.0f.xxx);
}

float CandidateWeight(float3 candidate)
{
    return min(max(1e-5f, Luminance(candidate)), 4.0f);
}

void ReservoirUpdate(inout FRestirGIReservoir reservoir, float3 candidate, float weight, float randomValue)
{
    bool bReservoirFinite = isfinite(reservoir.WeightSum)
        && isfinite(reservoir.SelectedWeight)
        && IsFiniteFloat3(reservoir.SampleRadiance);
    if (!bReservoirFinite)
    {
        reservoir = CreateEmptyReservoir();
    }

    if (!isfinite(weight) || weight <= 0.0f || !IsFiniteFloat3(candidate))
    {
        return;
    }

    if (!isfinite(randomValue))
    {
        return;
    }

    randomValue = saturate(randomValue);

    reservoir.SampleCount = ClampSampleCount(reservoir.SampleCount + 1u);
    reservoir.WeightSum = ClampReservoirWeightSum(reservoir.WeightSum + weight);

    float probability = saturate(weight / max(reservoir.WeightSum, 1e-6f));
    if (randomValue < probability)
    {
        reservoir.SampleRadiance = candidate;
        reservoir.SelectedWeight = weight;
    }
}

float3 ResolveReservoir(FRestirGIReservoir reservoir)
{
    if (!isfinite(reservoir.WeightSum) || reservoir.WeightSum <= 0.0f
        || !isfinite(reservoir.SelectedWeight) || reservoir.SelectedWeight <= 0.0f
        || reservoir.SampleCount == 0u
        || !IsFiniteFloat3(reservoir.SampleRadiance))
    {
        return 0.0f.xxx;
    }

    float normalization = ComputeSafeNormalization(reservoir);
    if (!isfinite(normalization) || normalization < 0.0f)
    {
        return 0.0f.xxx;
    }

    float3 resolved = reservoir.SampleRadiance * normalization;
    if (!IsFiniteFloat3(resolved))
    {
        return 0.0f.xxx;
    }

    if (ResolveUseConfidence > 0u)
    {
        static const float kConfidenceSampleCountTarget = 12.0f;
        float sampleFactor = saturate(float(reservoir.SampleCount) / kConfidenceSampleCountTarget);
        float weightFactor = saturate(reservoir.SelectedWeight / max(reservoir.WeightSum, 1e-6f));
        float confidence = saturate(lerp(sampleFactor, sampleFactor * weightFactor, saturate(ResolveLowSampleBoostGuard)));
        if (!isfinite(confidence))
        {
            return 0.0f.xxx;
        }

        resolved *= confidence;
    }

    if (!IsFiniteFloat3(resolved))
    {
        return 0.0f.xxx;
    }

    resolved = max(resolved, 0.0f);
    return min(resolved, ClampThreshold.xxx);
}


void SpatialReuse(
    inout FRestirGIReservoir reservoir,
    uint2 pixel,
    uint2 resolution,
    StructuredBuffer<FRestirGIReservoir> reservoirBuffer,
    Texture2D<float> depthTexture,
    Texture2D<float4> gBufferA,
    Texture2D<float4> gBufferB,
    Texture2D<float4> gBufferC)
{
    float centerDepth = depthTexture[pixel];
    if (!isfinite(centerDepth) || centerDepth <= 0.0f || centerDepth >= 1.0f)
    {
        return;
    }

    float3 centerNormal = normalize(gBufferA[pixel].xyz * 2.0f - 1.0f);
    float2 centerMaterial = saturate(float2(gBufferB[pixel].z, gBufferB[pixel].y));
    float3 centerAlbedo = saturate(gBufferC[pixel].rgb);
    if (!IsFiniteFloat3(centerNormal) || any(!isfinite(centerMaterial)) || !IsFiniteFloat3(centerAlbedo))
    {
        return;
    }
    static const int2 Offsets[4] =
    {
        int2(-1, 0),
        int2(1, 0),
        int2(0, -1),
        int2(0, 1)
    };

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        int2 samplePixel = int2(pixel) + Offsets[i];
        if (samplePixel.x < 0 || samplePixel.y < 0 || samplePixel.x >= int(resolution.x) || samplePixel.y >= int(resolution.y))
        {
            continue;
        }

        uint2 neighborPixel = uint2(samplePixel);
        float neighborDepth = depthTexture[neighborPixel];
        if (!isfinite(neighborDepth) || neighborDepth <= 0.0f || neighborDepth >= 1.0f)
        {
            continue;
        }

        float depthDelta = abs(centerDepth - neighborDepth);
        if (!isfinite(depthDelta) || depthDelta > kAcceptanceDepthThreshold)
        {
            continue;
        }

        float3 neighborNormal = normalize(gBufferA[neighborPixel].xyz * 2.0f - 1.0f);
        if (!IsFiniteFloat3(neighborNormal))
        {
            continue;
        }

        float normalSimilarity = dot(centerNormal, neighborNormal);
        if (!isfinite(normalSimilarity) || normalSimilarity < kAcceptanceNormalSimilarityThreshold)
        {
            continue;
        }

        float3 neighborAlbedo = saturate(gBufferC[neighborPixel].rgb);
        if (!IsFiniteFloat3(neighborAlbedo))
        {
            continue;
        }

        float albedoDelta = length(centerAlbedo - neighborAlbedo);
        if (!isfinite(albedoDelta) || albedoDelta > kAcceptanceAlbedoDeltaThreshold)
        {
            continue;
        }

        float2 neighborMaterial = saturate(float2(gBufferB[neighborPixel].z, gBufferB[neighborPixel].y));
        if (any(!isfinite(neighborMaterial)))
        {
            continue;
        }

        float materialDelta = length(centerMaterial - neighborMaterial);
        if (!isfinite(materialDelta) || materialDelta > kAcceptanceMaterialDeltaThreshold)
        {
            continue;
        }

        uint neighborIndex = neighborPixel.y * resolution.x + neighborPixel.x;
        FRestirGIReservoir neighbor = reservoirBuffer[neighborIndex];
        if (!IsReservoirStateValid(neighbor) || IsReservoirNearSaturation(neighbor))
        {
            continue;
        }

        float neighborWeight = max(1e-5f, neighbor.SelectedWeight);
        float randomValue = Random01(pixel, FrameIndex * 3253u + i * 173u + 19u);
        ReservoirUpdate(reservoir, neighbor.SampleRadiance, neighborWeight, randomValue);

        float additionalWeight = ClampReservoirWeightSum((neighbor.WeightSum - neighborWeight) * SpatialAdditionalScale);
        reservoir.WeightSum = ClampReservoirWeightSum(reservoir.WeightSum + additionalWeight);
        reservoir.SampleCount = ClampSampleCount(reservoir.SampleCount + min(max(0u, neighbor.SampleCount - 1u), 4u));
    }
}

[numthreads(8, 8, 1)]
void CSTemporal(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= OutputWidth || DispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    RWStructuredBuffer<FRestirGIReservoir> reservoirOut = ResourceDescriptorHeap[OutputOrReservoirUavIndex];
    StructuredBuffer<FRestirGIReservoir> reservoirHistory = ResourceDescriptorHeap[ReservoirSrvIndex];

    if (Enabled == 0u)
    {
        reservoirOut[DispatchThreadId.y * OutputWidth + DispatchThreadId.x] = CreateEmptyReservoir();
        return;
    }

    Texture2D<float> depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> gBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> gBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D<float4> gBufferC = ResourceDescriptorHeap[GBufferCIndex];
    Texture2D<float2> velocityTexture = ResourceDescriptorHeap[VelocityIndex];
    Texture2D<float4> historyTexture = ResourceDescriptorHeap[HistoryTextureIndex];
    Texture2D<float4> historyGeomA = ResourceDescriptorHeap[HistoryGeomAIndex];
    Texture2D<float4> historyGeomB = ResourceDescriptorHeap[HistoryGeomBIndex];

    const uint2 pixel = DispatchThreadId.xy;
    float depth = depthTexture[pixel];
    if (depth <= 0.0f || depth >= 1.0f)
    {
        reservoirOut[pixel.y * OutputWidth + pixel.x] = CreateEmptyReservoir();
        return;
    }

    float3 normal = normalize(gBufferA[pixel].xyz * 2.0f - 1.0f);

    float3 worldPos = ReconstructWorldPosition(pixel, depth, uint2(OutputWidth, OutputHeight));

    FRestirGIReservoir reservoir = CreateEmptyReservoir();

    const uint effectiveSamples = clamp(SamplesPerPixel, 1u, 32u);
    [loop]
    for (uint sampleIndex = 0; sampleIndex < effectiveSamples; ++sampleIndex)
    {
        float3 candidate = SampleCandidateGI(worldPos, normal, pixel, sampleIndex);
        float weight = CandidateWeight(candidate);
        float randomValue = Random01(pixel, FrameIndex * 1741u + sampleIndex * 313u + 11u);
        ReservoirUpdate(reservoir, candidate, weight, randomValue);
    }

    const uint reservoirIndex = pixel.y * OutputWidth + pixel.x;
    if (TemporalReuseEnabled > 0u && HistoryValid > 0u)
    {
        uint2 historyPixel = 0u.xx;
        if (TryGetReprojectedHistoryPixel(pixel, uint2(OutputWidth, OutputHeight), velocityTexture, historyPixel))
        {
            const uint historyIndex = historyPixel.y * OutputWidth + historyPixel.x;
            FRestirGIReservoir historyReservoir = reservoirHistory[historyIndex];
            if (IsReservoirStateValid(historyReservoir)
                && !IsReservoirNearSaturation(historyReservoir)
                && IsTemporalHistoryAccepted(pixel, historyPixel, depthTexture, gBufferA, gBufferB, gBufferC, historyTexture, historyGeomA, historyGeomB, historyReservoir))
            {
                float historySelectedWeight = max(1e-5f, historyReservoir.SelectedWeight);
                ReservoirUpdate(
                    reservoir,
                    historyReservoir.SampleRadiance,
                    historySelectedWeight,
                    Random01(pixel, FrameIndex * 2143u + 23u));

                float additionalHistoryWeight = ClampReservoirWeightSum((historyReservoir.WeightSum - historySelectedWeight) * TemporalAdditionalScale);
                reservoir.WeightSum = ClampReservoirWeightSum(reservoir.WeightSum + additionalHistoryWeight);
                reservoir.SampleCount = ClampSampleCount(reservoir.SampleCount + min(max(0u, historyReservoir.SampleCount - 1u), 8u));
            }
        }
    }

    reservoirOut[reservoirIndex] = reservoir;
}

[numthreads(8, 8, 1)]
void CSSpatial(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= OutputWidth || DispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    RWStructuredBuffer<FRestirGIReservoir> reservoirOut = ResourceDescriptorHeap[OutputOrReservoirUavIndex];
    StructuredBuffer<FRestirGIReservoir> reservoirIn = ResourceDescriptorHeap[ReservoirSrvIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> gBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> gBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D<float4> gBufferC = ResourceDescriptorHeap[GBufferCIndex];

    const uint2 pixel = DispatchThreadId.xy;
    float depth = depthTexture[pixel];
    if (Enabled == 0u || depth <= 0.0f || depth >= 1.0f)
    {
        reservoirOut[pixel.y * OutputWidth + pixel.x] = CreateEmptyReservoir();
        return;
    }

    const uint reservoirIndex = pixel.y * OutputWidth + pixel.x;
    FRestirGIReservoir reservoir = reservoirIn[reservoirIndex];
    if (!IsReservoirStateValid(reservoir) || IsReservoirNearSaturation(reservoir))
    {
        reservoir = CreateEmptyReservoir();
    }

    SpatialReuse(reservoir, pixel, uint2(OutputWidth, OutputHeight), reservoirIn, depthTexture, gBufferA, gBufferB, gBufferC);
    reservoirOut[reservoirIndex] = reservoir;
}

[numthreads(8, 8, 1)]
void CSResolve(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= OutputWidth || DispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[OutputOrReservoirUavIndex];
    StructuredBuffer<FRestirGIReservoir> reservoirBuffer = ResourceDescriptorHeap[ReservoirSrvIndex];
    Texture2D<float> depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> gBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> gBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D<float4> gBufferC = ResourceDescriptorHeap[GBufferCIndex];
    RWTexture2D<float4> historyGeomAOut = ResourceDescriptorHeap[HistoryGeomAIndex];
    RWTexture2D<float4> historyGeomBOut = ResourceDescriptorHeap[HistoryGeomBIndex];

    const uint2 pixel = DispatchThreadId.xy;
    float3 packedNormal = normalize(gBufferA[pixel].xyz * 2.0f - 1.0f) * 0.5f + 0.5f;
    float roughness = saturate(gBufferB[pixel].z);
    float metalness = saturate(gBufferB[pixel].y);
    historyGeomAOut[pixel] = float4(packedNormal, roughness);
    historyGeomBOut[pixel] = float4(saturate(gBufferC[pixel].rgb), metalness);

    if (Enabled == 0u)
    {
        outputTexture[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    const uint reservoirIndex = pixel.y * OutputWidth + pixel.x;
    FRestirGIReservoir reservoir = reservoirBuffer[reservoirIndex];
    float3 finalSample = ResolveReservoir(reservoir) * max(0.0f, Intensity);
    float depth = depthTexture[pixel];
    outputTexture[pixel] = float4(finalSample, saturate(depth));
}
