#include "SceneConstants.hlsl"

cbuffer RestirGIConstants : register(b0)
{
    uint OutputWidth;
    uint OutputHeight;
    uint FrameIndex;
    uint SamplesPerPixel;
    float Intensity;
    uint Enabled;
    uint Padding0;
    uint Padding1;
};

cbuffer RestirGIBindless : register(b1)
{
    uint OutputUavIndex;
    uint DepthIndex;
    uint LinearDepthIndex;
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint GBufferDIndex;
};

static const float kPi = 3.14159265f;
static const float3 kSkyTop = float3(0.08f, 0.18f, 0.35f);
static const float3 kSkyHorizon = float3(0.28f, 0.30f, 0.32f);
static const float3 kGroundBounce = float3(0.04f, 0.03f, 0.02f);

struct FReservoir
{
    float3 Sample;
    float WeightSum;
    float SelectedWeight;
    uint SampleCount;
};

uint Hash32(uint X)
{
    X ^= X >> 16;
    X *= 0x7feb352du;
    X ^= X >> 15;
    X *= 0x846ca68bu;
    X ^= X >> 16;
    return X;
}

float2 Random2(uint2 Pixel, uint SampleIndex)
{
    uint Seed = Pixel.x * 1973u + Pixel.y * 9277u + FrameIndex * 26699u + SampleIndex * 31847u + 0x68bc21ebu;
    uint H0 = Hash32(Seed);
    uint H1 = Hash32(H0 ^ 0xa511e9b3u);
    return float2((H0 & 0x00ffffffu) / 16777216.0f, (H1 & 0x00ffffffu) / 16777216.0f);
}

float3 BuildOrthonormalBasis(float3 N)
{
    return (abs(N.z) < 0.999f) ? normalize(cross(N, float3(0.0f, 0.0f, 1.0f))) : normalize(cross(N, float3(0.0f, 1.0f, 0.0f)));
}

float3 SampleCosineHemisphere(float2 Xi)
{
    float Phi = 2.0f * kPi * Xi.x;
    float CosTheta = sqrt(saturate(1.0f - Xi.y));
    float SinTheta = sqrt(saturate(Xi.y));
    return float3(cos(Phi) * SinTheta, sin(Phi) * SinTheta, CosTheta);
}

float3 EvaluateSkyIrradiance(float3 Direction)
{
    float UpFactor = saturate(Direction.y * 0.5f + 0.5f);
    float3 Sky = lerp(kSkyHorizon, kSkyTop, UpFactor * UpFactor);
    float GroundFactor = saturate(-Direction.y);
    return lerp(Sky, kGroundBounce, GroundFactor * GroundFactor);
}

float3 EstimateCandidateGI(float3 Albedo, float Metalness, float3 Normal, uint2 Pixel, uint SampleIndex)
{
    float2 Xi = Random2(Pixel, SampleIndex);

    float3 T = BuildOrthonormalBasis(Normal);
    float3 B = normalize(cross(Normal, T));
    float3 LocalDir = SampleCosineHemisphere(Xi);
    float3 WorldDir = normalize(LocalDir.x * T + LocalDir.y * B + LocalDir.z * Normal);

    float NdotL = saturate(dot(Normal, WorldDir));
    float3 DiffuseColor = Albedo * (1.0f - Metalness);
    float3 Incoming = EvaluateSkyIrradiance(WorldDir);
    return DiffuseColor * Incoming * (NdotL / kPi);
}

float ComputeCandidateWeight(float3 Candidate)
{
    return max(1e-5f, dot(Candidate, float3(0.299f, 0.587f, 0.114f)));
}

void ReservoirUpdate(inout FReservoir Reservoir, float3 Candidate, float CandidateWeight, float RandomValue)
{
    Reservoir.SampleCount += 1u;
    Reservoir.WeightSum += CandidateWeight;
    float SelectionProbability = CandidateWeight / max(Reservoir.WeightSum, 1e-6f);
    if (RandomValue < SelectionProbability)
    {
        Reservoir.Sample = Candidate;
        Reservoir.SelectedWeight = CandidateWeight;
    }
}


float ComputeReservoirNormalization(const FReservoir Reservoir)
{
    float Denominator = max(Reservoir.SelectedWeight * float(max(1u, Reservoir.SampleCount)), 1e-5f);
    float RawFactor = Reservoir.WeightSum / Denominator;
    return clamp(RawFactor, 0.0f, 8.0f);
}

float3 StabilizeReservoirSample(float3 Sample, float3 Albedo, uint SampleCount)
{
    float MaxChannel = max(Albedo.r, max(Albedo.g, Albedo.b));
    float3 RadianceLimit = (0.15f + 2.5f * MaxChannel) * (1.0f + 1.0f / float(max(1u, SampleCount))) * float3(1.0f, 1.0f, 1.0f);
    float3 ClampedSample = min(Sample, RadianceLimit);

    float Confidence = saturate(float(SampleCount) / 16.0f);
    return lerp(0.0f.xxx, ClampedSample, Confidence);
}


float ComputeSpatialReuseWeight(
    float CenterLD,
    float NeighborLD,
    float3 CenterNormal,
    float3 NeighborNormal,
    float3 CenterAlbedo,
    float3 NeighborAlbedo,
    float CenterRoughness,
    float NeighborRoughness,
    float CenterMetalness,
    float NeighborMetalness)
{
    float DepthWeight = exp(-abs(CenterLD - NeighborLD) / max(0.05f + 0.01f * CenterLD, 1e-4f));
    float AvgRoughness = 0.5f * (CenterRoughness + NeighborRoughness);
    float NormalPower = lerp(16.0f, 4.0f, saturate(AvgRoughness));
    float NormalWeight = pow(saturate(dot(CenterNormal, NeighborNormal)), NormalPower);

    float3 AlbedoDelta = abs(CenterAlbedo - NeighborAlbedo);
    float AlbedoDistance = max(AlbedoDelta.r, max(AlbedoDelta.g, AlbedoDelta.b));
    float AvgMetalness = 0.5f * (CenterMetalness + NeighborMetalness);
    float AlbedoScale = lerp(0.30f, 0.45f, saturate(AvgMetalness));
    float AlbedoWeight = saturate(1.0f - AlbedoDistance / AlbedoScale);

    float RoughnessDelta = abs(CenterRoughness - NeighborRoughness);
    float RoughnessWeight = saturate(1.0f - RoughnessDelta / 0.4f);

    float MetalnessDelta = abs(CenterMetalness - NeighborMetalness);
    float MetalnessWeight = saturate(1.0f - MetalnessDelta / 0.5f);

    return saturate(DepthWeight * NormalWeight * AlbedoWeight * RoughnessWeight * MetalnessWeight);
}


uint ComputeAdaptiveSampleCount(uint BaseSamples, float Roughness)
{
    float RoughnessFactor = lerp(1.5f, 0.75f, saturate(Roughness));
    float TargetSamples = float(max(1u, BaseSamples)) * RoughnessFactor;
    return uint(clamp(round(TargetSamples), 1.0f, 32.0f));
}


uint ComputeAdaptiveNeighborCount(float Roughness)
{
    float NeighborFactor = lerp(1.0f, 0.5f, saturate(Roughness));
    uint Count = uint(clamp(round(8.0f * NeighborFactor), 2.0f, 8.0f));
    return Count;
}


uint MapNeighborSequenceIndex(uint LogicalIndex, uint2 Pixel)
{
    uint Seed = Pixel.x * 73856093u + Pixel.y * 19349663u + FrameIndex * 83492791u;
    uint Start = Hash32(Seed) & 7u;
    return (LogicalIndex + Start) & 7u;
}

bool IsSpatialNeighborValidLinear(
    float CenterLD,
    float NeighborLD,
    float3 CenterNormal,
    float3 NeighborNormal,
    float3 CenterAlbedo,
    float3 NeighborAlbedo,
    float CenterRoughness,
    float NeighborRoughness,
    float CenterMetalness,
    float NeighborMetalness)
{
    const float kInvalidThreshold = 65500.0f;
    if (NeighborLD >= kInvalidThreshold)
    {
        return false;
    }

    float DepthEps = 0.02f + 0.01f * CenterLD;
    bool DepthOk = abs(CenterLD - NeighborLD) < DepthEps;
    float AvgRoughness = 0.5f * (CenterRoughness + NeighborRoughness);
    float NormalThreshold = lerp(0.93f, 0.75f, saturate(AvgRoughness));
    bool NormalOk = dot(CenterNormal, NeighborNormal) > NormalThreshold;

    float3 AlbedoDelta = abs(CenterAlbedo - NeighborAlbedo);
    float AvgMetalness = 0.5f * (CenterMetalness + NeighborMetalness);
    float AlbedoThreshold = lerp(0.25f, 0.40f, saturate(AvgMetalness));
    bool AlbedoOk = max(AlbedoDelta.r, max(AlbedoDelta.g, AlbedoDelta.b)) < AlbedoThreshold;

    float RoughnessDelta = abs(CenterRoughness - NeighborRoughness);
    bool RoughnessOk = RoughnessDelta < 0.4f;

    float MetalnessDelta = abs(CenterMetalness - NeighborMetalness);
    bool MetalnessOk = MetalnessDelta < 0.5f;

    return DepthOk && NormalOk && AlbedoOk && RoughnessOk && MetalnessOk;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= OutputWidth || DispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    RWTexture2D<float4> OutputTexture = ResourceDescriptorHeap[OutputUavIndex];

    if (Enabled == 0u || Intensity <= 0.0f)
    {
        OutputTexture[DispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D<float4> GBufferC = ResourceDescriptorHeap[GBufferCIndex];
    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float> LinearDepthTexture = ResourceDescriptorHeap[LinearDepthIndex];

    const uint2 Pixel = DispatchThreadId.xy;
    const float kInvalidLD = 65504.0f;
    const float kInvalidGuard = kInvalidLD - 4.0f;

    float CenterLD = LinearDepthTexture[Pixel];
    if (CenterLD >= kInvalidGuard)
    {
        OutputTexture[Pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float Depth = DepthTexture[Pixel];
    if (Depth <= 0.0f || Depth >= 1.0f)
    {
        OutputTexture[Pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 CenterNormal = normalize(GBufferA[Pixel].xyz * 2.0f - 1.0f);
    float4 SrmData = GBufferB[Pixel];
    float3 Albedo = GBufferC[Pixel].rgb;
    float Metalness = saturate(SrmData.y);
    float Roughness = saturate(SrmData.z);

    FReservoir Reservoir;
    Reservoir.Sample = float3(0.0f, 0.0f, 0.0f);
    Reservoir.WeightSum = 0.0f;
    Reservoir.SelectedWeight = 1.0f;
    Reservoir.SampleCount = 0u;

    uint BaseSampleCount = clamp(SamplesPerPixel, 1u, 32u);
    uint EffectiveSampleCount = ComputeAdaptiveSampleCount(BaseSampleCount, Roughness);
    [loop]
    for (uint SampleIndex = 0; SampleIndex < EffectiveSampleCount; ++SampleIndex)
    {
        float3 Candidate = EstimateCandidateGI(Albedo, Metalness, CenterNormal, Pixel, SampleIndex);
        float CandidateWeight = ComputeCandidateWeight(Candidate);
        float RandomValue = Random2(Pixel ^ uint2(0x9e3779b9u, 0x85ebca6bu), SampleIndex + 1337u).x;
        ReservoirUpdate(Reservoir, Candidate, CandidateWeight, RandomValue);
    }

    const int2 NeighborOffsets[8] =
    {
        int2(-1, 0),
        int2(1, 0),
        int2(0, -1),
        int2(0, 1),
        int2(-1, -1),
        int2(1, -1),
        int2(-1, 1),
        int2(1, 1)
    };

    uint EffectiveNeighborCount = ComputeAdaptiveNeighborCount(Roughness);

    [unroll]
    for (uint NeighborIndex = 0; NeighborIndex < 8; ++NeighborIndex)
    {
        if (NeighborIndex >= EffectiveNeighborCount)
        {
            continue;
        }

        uint SequenceIndex = MapNeighborSequenceIndex(NeighborIndex, Pixel);
        int2 NeighborPixelSigned = int2(Pixel) + NeighborOffsets[SequenceIndex];
        if (NeighborPixelSigned.x < 0 || NeighborPixelSigned.y < 0 || NeighborPixelSigned.x >= int(OutputWidth) || NeighborPixelSigned.y >= int(OutputHeight))
        {
            continue;
        }

        uint2 NeighborPixel = uint2(NeighborPixelSigned);
        float NeighborLD = LinearDepthTexture[NeighborPixel];

        float3 NeighborNormal = normalize(GBufferA[NeighborPixel].xyz * 2.0f - 1.0f);
        float3 NeighborAlbedo = GBufferC[NeighborPixel].rgb;
        float4 NeighborSrm = GBufferB[NeighborPixel];
        float NeighborMetalness = saturate(NeighborSrm.y);
        float NeighborRoughness = saturate(NeighborSrm.z);

        if (!IsSpatialNeighborValidLinear(CenterLD, NeighborLD, CenterNormal, NeighborNormal, Albedo, NeighborAlbedo, Roughness, NeighborRoughness, Metalness, NeighborMetalness))
        {
            continue;
        }

        float3 Candidate = EstimateCandidateGI(NeighborAlbedo, NeighborMetalness, NeighborNormal, NeighborPixel, SequenceIndex + EffectiveSampleCount);
        float ReuseWeight = ComputeSpatialReuseWeight(CenterLD, NeighborLD, CenterNormal, NeighborNormal, Albedo, NeighborAlbedo, Roughness, NeighborRoughness, Metalness, NeighborMetalness);
        float CandidateWeight = ComputeCandidateWeight(Candidate) * max(0.1f, ReuseWeight);
        float RandomValue = Random2(Pixel ^ uint2(0x27d4eb2du, 0x165667b1u), SequenceIndex + 9001u).x;
        ReservoirUpdate(Reservoir, Candidate, CandidateWeight, RandomValue);
    }

    float NormalizedFactor = ComputeReservoirNormalization(Reservoir);
    float3 StabilizedSample = StabilizeReservoirSample(Reservoir.Sample * NormalizedFactor, Albedo, Reservoir.SampleCount);
    float3 FinalSample = StabilizedSample * Intensity;
    OutputTexture[Pixel] = float4(max(FinalSample, 0.0f), 1.0f);
}
