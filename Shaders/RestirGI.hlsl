#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"
#include "RestirGIReservoir.hlsli"
#include "RestirGISamplingCommon.hlsli"
#include "RestirGISh.hlsli"
#include "PathBrdfCommon.hlsli"
#include "GpuDebugLineCommon.hlsl"

RaytracingAccelerationStructure Scene : register(t0);

cbuffer RestirGIConstants : register(b1)
{
    uint FullWidth;
    uint FullHeight;
    uint HalfWidth;
    uint HalfHeight;
    uint FrameIndex;
    uint Enabled;
    uint HistoryValid;
    uint SpatialPassIndex;
    float Intensity;
    float RayLength;
    float ClampThreshold;
    uint UseVisibility;
    uint UseBrdf;
    uint UseHistoryIndirect;
    uint SequenceFrame;
    uint DebugRayEnabled;
    uint DebugPixelX;
    uint DebugPixelY;
};

cbuffer RestirGIBindless : register(b2)
{
    uint OutputTextureUavIndex;         // b2[0]  : final GI output UAV
    uint DepthIndex;                    // b2[1]  : depth SRV
    uint VelocityIndex;                 // b2[2]  : velocity SRV
    uint GBufferAIndex;                 // b2[3]  : gbuffer A SRV
    uint GBufferBIndex;                 // b2[4]  : gbuffer B SRV
    uint GBufferCIndex;                 // b2[5]  : gbuffer C SRV
    uint InstanceDataBufferIndex;       // b2[6]  : instance data buffer SRV
    uint EnvironmentCubeBindlessIndex;  // b2[7]  : environment cube SRV
    uint LinearClampSamplerIndex;       // b2[8]  : linear clamp sampler
    uint InputInitialRadianceSrvIndex;  // b2[9]  : initial sampling radiance SRV
    uint InputInitialRayDirectionSrvIndex; // b2[10] : initial sampling ray direction SRV
    uint HistoryDepthNormalSrvIndex;    // b2[11] : temporal history depth/normal SRV
    uint HistorySampleRadianceSrvIndex; // b2[12] : temporal history sample radiance SRV
    uint HistoryRayDirectionSrvIndex;   // b2[13] : temporal history ray direction SRV
    uint HistoryMWSrvIndex;             // b2[14] : temporal history M/W SRV
    uint OutputDepthNormalUavIndex;     // b2[15] : output depth/normal UAV
    uint OutputSampleRadianceUavIndex;  // b2[16] : output sample radiance UAV
    uint OutputRayDirectionUavIndex;    // b2[17] : output ray direction UAV
    uint OutputMWUavIndex;              // b2[18] : output M/W UAV
    uint InputDepthNormalSrvIndex;      // b2[19] : spatial input depth/normal SRV
    uint InputSampleRadianceSrvIndex;   // b2[20] : spatial input sample radiance SRV
    uint InputRayDirectionSrvIndex;     // b2[21] : spatial input ray direction SRV
    uint InputMWSrvIndex;               // b2[22] : spatial input M/W SRV
    uint ResolveInputSHUavIndex;        // b2[23] : resolve output InputSH UAV
    uint ResolveVarianceUavIndex;       // b2[24] : resolve output Variance UAV
    uint HistoryIrradianceIndex;        // b2[25] : history irradiance SRV
    uint PrevLinearDepthIndex;          // b2[26] : previous linear depth SRV
    uint DebugLineBufferUavIndex;       // b2[27] : debug line buffer UAV
};

#include "RayTracingCommon.hlsl"

static const uint RestirGIRayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
uint RestirGIHash32(uint Value)
{
    Value ^= Value >> 17;
    Value *= 0xed5ad4bbu;
    Value ^= Value >> 11;
    Value *= 0xac4c1b51u;
    Value ^= Value >> 15;
    Value *= 0x31848babu;
    Value ^= Value >> 14;
    return Value;
}

float RestirGIRandom01(uint2 Pixel, uint Salt)
{
    uint Seed = RestirGIHash32(Pixel.x + 0x9e3779b9u);
    Seed = RestirGIHash32(Seed + Pixel.y);
    Seed = RestirGIHash32(Seed + Salt * 1664525u);
    return (Seed & 0x00ffffffu) / 16777216.0f;
}

float2 RestirGIRandom02(uint2 Pixel, uint Salt)
{
    return float2(
        RestirGIRandom01(Pixel, Salt + 11u),
        RestirGIRandom01(Pixel, Salt + 73u));
}

uint RestirGIPackDebugColor(float3 Radiance)
{
    float3 Color = max(Radiance, 0.0f.xxx);
    Color = Color / (1.0f.xxx + Color);
    Color = saturate(pow(Color, 1.0f / 2.2f) * 1.35f);
    const uint3 PackedRgb = (uint3)round(Color * 255.0f);
    return (0xFFu << 24u) | (PackedRgb.b << 16u) | (PackedRgb.g << 8u) | PackedRgb.r;
}

uint2 RestirGIHalfToFull(uint2 HalfPos)
{
    static const uint2 Offsets[4] =
    {
        uint2(1, 1),
        uint2(1, 0),
        uint2(0, 0),
        uint2(0, 1),
    };

    const uint2 FullPos = HalfPos * 2u + Offsets[SequenceFrame % 4u];
    return min(FullPos, uint2(FullWidth - 1u, FullHeight - 1u));
}

float3 RestirGIReconstructWorldPosition(uint2 FullPos, float Depth)
{
    float2 Uv = (float2(FullPos) + 0.5f) / float2(FullWidth, FullHeight);
    float2 Ndc = float2(Uv.x * 2.0f - 1.0f, 1.0f - Uv.y * 2.0f);
    float4 Clip = float4(Ndc, Depth, 1.0f);
    float4 WorldPos = mul(Clip, ViewProjectionInverse);
    WorldPos.xyz /= max(WorldPos.w, 1e-6f);
    return WorldPos.xyz;
}


bool RestirGITraceVisibility(float3 Origin, float3 Direction, float MaxDistance)
{
    RayDesc ShadowRay;
    ShadowRay.Origin = Origin;
    ShadowRay.Direction = Direction;
    ShadowRay.TMin = 1e-3f;
    ShadowRay.TMax = max(1e-3f, MaxDistance);

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> ShadowQuery;
    ShadowQuery.TraceRayInline(Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES, 0xFF, ShadowRay);

    while (ShadowQuery.Proceed())
    {
        const uint InstanceID = ShadowQuery.CandidateInstanceID();
        const uint PrimitiveIndex = ShadowQuery.CandidatePrimitiveIndex();
        const float2 Barycentrics = ShadowQuery.CandidateTriangleBarycentrics();
        if (AlphaTest(InstanceID, PrimitiveIndex, Barycentrics))
        {
            ShadowQuery.CommitNonOpaqueTriangleHit();
        }
    }

    return ShadowQuery.CommittedStatus() == COMMITTED_NOTHING;
}

float3 RestirGISampleHistoryIndirect(uint2 FullPos, float3 HitWorldPos)
{
    if (UseHistoryIndirect == 0u || HistoryIrradianceIndex == 0xFFFFFFFFu || PrevLinearDepthIndex == 0xFFFFFFFFu)
    {
        return 0.0f.xxx;
    }

    Texture2D<float2> VelocityTexture = ResourceDescriptorHeap[VelocityIndex];
    Texture2D<float4> HistoryIrradianceTexture = ResourceDescriptorHeap[HistoryIrradianceIndex];
    Texture2D<float> PrevLinearDepthTexture = ResourceDescriptorHeap[PrevLinearDepthIndex];

    const float2 Uv = (float2(FullPos) + 0.5f) / float2(FullWidth, FullHeight);
    const float2 VelocityNdc = VelocityTexture[FullPos];
    const float2 PrevUv = float2(Uv.x - VelocityNdc.x * 0.5f, Uv.y + VelocityNdc.y * 0.5f);
    if (any(PrevUv <= 0.0f.xx) || any(PrevUv >= 1.0f.xx))
    {
        return 0.0f.xxx;
    }

    const uint2 PrevPixel = min(uint2(PrevUv * float2(FullWidth, FullHeight)), uint2(FullWidth - 1u, FullHeight - 1u));
    const float PrevLinearDepth = PrevLinearDepthTexture[PrevPixel];
    const float CurrentLinearDepth = length(HitWorldPos - CameraPosition);
    if (!isfinite(PrevLinearDepth) || abs(CurrentLinearDepth - PrevLinearDepth) > 0.5f)
    {
        return 0.0f.xxx;
    }

    return max(HistoryIrradianceTexture[PrevPixel].rgb, 0.0f.xxx);
}

float3 RestirGIEvaluateHitRadiance(uint InstanceID, float2 UV, float3 HitNormal, float3 HitAlbedo, float HitMetalness, float HitRoughness, float3 OutDirection, float3 HitWorldPos, uint2 FullPos)
{
    const float3 Diffuse = HitAlbedo * (1.0f - saturate(HitMetalness));
    const float3 Specular = lerp(0.04f.xxx, HitAlbedo, saturate(HitMetalness));
    const float Roughness = max(HitRoughness, 0.03f);

    const float3 Wi = normalize(LightDirection);
    const float3 Wo = normalize(OutDirection);
    const float NdotL = saturate(dot(HitNormal, Wi));

    float Visibility = 1.0f;
    if (UseVisibility > 0u && NdotL > 0.0f)
    {
        Visibility = RestirGITraceVisibility(HitWorldPos + HitNormal * 0.01f, Wi, max(0.1f, RayLength)) ? 1.0f : 0.0f;
    }

    float3 Direct = 0.0f.xxx;
    if (UseBrdf > 0u)
    {
        Direct = BRDF(Wi, Wo, HitNormal, Diffuse, Specular, Roughness) * (LightColor * LightIntensity) * NdotL * Visibility;
    }
    else
    {
        Direct = Diffuse * (LightColor * LightIntensity) * (NdotL / PI) * Visibility;
    }

    const float3 HistoryIndirect = RestirGISampleHistoryIndirect(FullPos, HitWorldPos);
    const float3 Emissive = max(SampleEmissive(InstanceID, UV), 0.0f.xxx);
    return max(Direct + HistoryIndirect + Emissive, 0.0f.xxx);
}

float3 RestirGISampleCandidate(float3 WorldPos, float3 Normal, uint2 FullPos, uint2 HalfPos, out float3 OutDirection, out float OutHitDistance, out bool bOutHit)
{
    const float2 Xi = RestirGIRandom02(HalfPos, SequenceFrame * 1999u + 17u);
    const float3 Direction = SampleHemisphereCosine(Xi, Normal);
    OutDirection = Direction;
    OutHitDistance = max(0.1f, RayLength);
    bOutHit = false;

    RayDesc Ray;
    Ray.Origin = WorldPos + Normal * 0.01f;
    Ray.Direction = Direction;
    Ray.TMin = 1e-3f;
    Ray.TMax = max(0.1f, RayLength);

    RayQuery<RestirGIRayFlags> Query;
    Query.TraceRayInline(Scene, RestirGIRayFlags, 0xFF, Ray);

    while (Query.Proceed())
    {
        const uint InstanceID = Query.CandidateInstanceID();
        const uint PrimitiveIndex = Query.CandidatePrimitiveIndex();
        const float2 Barycentrics = Query.CandidateTriangleBarycentrics();
        if (AlphaTest(InstanceID, PrimitiveIndex, Barycentrics))
        {
            Query.CommitNonOpaqueTriangleHit();
        }
    }

    float3 Incoming = 0.0f.xxx;
    if (Query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        const uint InstanceID = Query.CommittedInstanceID();
        const uint PrimitiveIndex = Query.CommittedPrimitiveIndex();
        const float2 Barycentrics = Query.CommittedTriangleBarycentrics();

        const float2 UV = GetInterpolatedUV(InstanceID, PrimitiveIndex, Barycentrics);
        const float3 HitAlbedo = SampleAlbedo(InstanceID, UV);
        const float2 HitMR = SampleMetallicRoughness(InstanceID, UV);
        float3 HitNormal = GetInterpolatedNormal(InstanceID, PrimitiveIndex, Barycentrics);
        if (dot(HitNormal, -Direction) < 0.0f)
        {
            HitNormal = -HitNormal;
        }

        const float HitT = Query.CommittedRayT();
        OutHitDistance = HitT;
        bOutHit = true;
        const float3 HitWorldPos = WorldPos + Direction * HitT;
        Incoming = RestirGIEvaluateHitRadiance(InstanceID, UV, HitNormal, HitAlbedo, HitMR.x, HitMR.y, -Direction, HitWorldPos, FullPos);
    }
    else
    {
        Incoming = EvaluateSky(Direction);
    }

    // NOTE: Keep any RestirGIRandom01 test hook behavior unchanged in this pass;
    // restore to production path when running strict Legacy/New comparisons.
    return max(Incoming, 0.0f.xxx);
}

uint RestirGIEncodeNormal16x2(float3 N)
{
    N /= (abs(N.x) + abs(N.y) + abs(N.z) + 1e-6f);
    float2 Enc = N.xy;
    if (N.z < 0.0f)
    {
        const float2 SignVec = lerp(-1.0f.xx, 1.0f.xx, step(0.0f.xx, Enc));
        Enc = (1.0f - abs(Enc.yx)) * SignVec;
    }

    Enc = Enc * 0.5f + 0.5f;
    uint2 Packed = (uint2)round(saturate(Enc) * 65535.0f);
    return (Packed.x & 0xFFFFu) | ((Packed.y & 0xFFFFu) << 16u);
}

float3 RestirGIDecodeNormal16x2(uint Packed)
{
    float2 Enc = float2(Packed & 0xFFFFu, Packed >> 16u) / 65535.0f;
    Enc = Enc * 2.0f - 1.0f;

    float3 N = float3(Enc.xy, 1.0f - abs(Enc.x) - abs(Enc.y));
    float2 T = saturate(-N.zz);
    N.xy += lerp(T, -T, step(0.0f.xx, N.xy));
    return normalize(N);
}

float RestirGIResolveLinearDepth(float Depth)
{
    return Projection._43 / max(Depth, 1e-6f);
}

float RestirGIResolveGeometryWeight(float CenterDepth, float3 CenterNormal, float SampleDepth, float3 SampleNormal)
{
    const float CenterLinearDepth = RestirGIResolveLinearDepth(CenterDepth);
    const float SampleLinearDepth = RestirGIResolveLinearDepth(SampleDepth);
    const float DepthWeight = exp(-abs(CenterLinearDepth - SampleLinearDepth));
    const float NormalWeight = saturate(dot(CenterNormal, SampleNormal));
    return DepthWeight * NormalWeight;
}

FRestirGISample RestirGILoadSample(Texture2D<float4> RadianceTexture, Texture2D<uint> RayDirTexture, uint2 Pos)
{
    FRestirGISample S;
    S.Radiance = RadianceTexture[Pos].xyz;
    S.RayDirection = RayDirTexture[Pos];
    return S;
}

FRestirGIReservoir RestirGILoadReservoir(Texture2D<float4> SampleRadianceTexture, Texture2D<uint> RayDirectionTexture, Texture2D<float2> MWTexture, uint2 Pos)
{
    FRestirGIReservoir R;
    R.Sample.Radiance = SampleRadianceTexture[Pos].xyz;
    R.Sample.RayDirection = RayDirectionTexture[Pos];
    R.M = MWTexture[Pos].x;
    R.W = MWTexture[Pos].y;
    R.SumWeight = R.W * R.M * RestirGITarget(R.Sample.Radiance);
    return R;
}

void RestirGIStoreReservoir(
    uint2 Pos,
    FRestirGIReservoir Reservoir,
    float Depth,
    float3 Normal,
    RWTexture2D<uint2> OutDepthNormal,
    RWTexture2D<float4> OutSampleRadiance,
    RWTexture2D<uint> OutRayDirection,
    RWTexture2D<float2> OutMW)
{
    OutDepthNormal[Pos] = uint2(asuint(Depth), RestirGIEncodeNormal16x2(Normal));
    OutSampleRadiance[Pos] = float4(max(Reservoir.Sample.Radiance, 0.0f.xxx), 0.0f);
    OutRayDirection[Pos] = Reservoir.Sample.RayDirection;
    OutMW[Pos] = float2(Reservoir.M, Reservoir.W);
}

[numthreads(8, 8, 1)]
void CSInitialSampling(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 HalfPos = DispatchThreadId.xy;
    if (HalfPos.x >= HalfWidth || HalfPos.y >= HalfHeight)
    {
        return;
    }

    RWTexture2D<float4> InitialRadianceOut = ResourceDescriptorHeap[OutputSampleRadianceUavIndex];
    RWTexture2D<uint> InitialRayDirOut = ResourceDescriptorHeap[OutputRayDirectionUavIndex];

    if (Enabled == 0u)
    {
        InitialRadianceOut[HalfPos] = 0.0f.xxxx;
        InitialRayDirOut[HalfPos] = 0u;
        return;
    }

    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];

    const uint2 FullPos = RestirGIHalfToFull(HalfPos);
    const float Depth = DepthTexture[FullPos];
    if (Depth <= 0.0f || Depth >= 1.0f)
    {
        InitialRadianceOut[HalfPos] = 0.0f.xxxx;
        InitialRayDirOut[HalfPos] = 0u;
        return;
    }

    const float3 Normal = normalize(GBufferA[FullPos].xyz * 2.0f - 1.0f);
    const float3 WorldPos = RestirGIReconstructWorldPosition(FullPos, Depth);
    float3 SampleDirection = 0.0f.xxx;
    float DebugHitDistance = max(0.1f, RayLength);
    bool bDebugHit = false;
    const float3 Candidate = RestirGISampleCandidate(WorldPos, Normal, FullPos, HalfPos, SampleDirection, DebugHitDistance, bDebugHit);

    if (DebugRayEnabled != 0u && DebugLineBufferUavIndex != 0xFFFFFFFFu && all(HalfPos == uint2(DebugPixelX, DebugPixelY)))
    {
        const float TraceDistance = bDebugHit ? max(1e-3f, DebugHitDistance) : max(0.1f, RayLength);
        const uint DebugColor = RestirGIPackDebugColor(Candidate);
        DebugDrawLine(DebugLineBufferUavIndex, WorldPos, WorldPos + SampleDirection * TraceDistance, DebugColor);
    }

    InitialRadianceOut[HalfPos] = float4(Candidate, 0.0f);
    InitialRayDirOut[HalfPos] = RestirGiEncodeDirection16x2(SampleDirection);
}

[numthreads(8, 8, 1)]
void CSTemporalResampling(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 HalfPos = DispatchThreadId.xy;
    if (HalfPos.x >= HalfWidth || HalfPos.y >= HalfHeight)
    {
        return;
    }

    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float2> VelocityTexture = ResourceDescriptorHeap[VelocityIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];

    Texture2D<float4> InitialRadiance = ResourceDescriptorHeap[InputInitialRadianceSrvIndex];
    Texture2D<uint> InitialRayDir = ResourceDescriptorHeap[InputInitialRayDirectionSrvIndex];

    Texture2D<uint2> HistoryDepthNormal = ResourceDescriptorHeap[HistoryDepthNormalSrvIndex];
    Texture2D<float4> HistorySampleRadiance = ResourceDescriptorHeap[HistorySampleRadianceSrvIndex];
    Texture2D<uint> HistoryRayDirection = ResourceDescriptorHeap[HistoryRayDirectionSrvIndex];
    Texture2D<float2> HistoryMW = ResourceDescriptorHeap[HistoryMWSrvIndex];

    RWTexture2D<uint2> OutDepthNormal = ResourceDescriptorHeap[OutputDepthNormalUavIndex];
    RWTexture2D<float4> OutSampleRadiance = ResourceDescriptorHeap[OutputSampleRadianceUavIndex];
    RWTexture2D<uint> OutRayDirection = ResourceDescriptorHeap[OutputRayDirectionUavIndex];
    RWTexture2D<float2> OutMW = ResourceDescriptorHeap[OutputMWUavIndex];

    const uint2 FullPos = RestirGIHalfToFull(HalfPos);
    const float Depth = DepthTexture[FullPos];
    const float3 Normal = normalize(GBufferA[FullPos].xyz * 2.0f - 1.0f);

    if (Enabled == 0u || Depth <= 0.0f || Depth >= 1.0f)
    {
        FRestirGIReservoir Empty = (FRestirGIReservoir)0;
        RestirGIStoreReservoir(HalfPos, Empty, Depth, Normal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
        return;
    }

    FRestirGISample Current = RestirGILoadSample(InitialRadiance, InitialRayDir, HalfPos);

    FRestirGIReservoir Reservoir = (FRestirGIReservoir)0;
    Reservoir.Sample = Current;
    Reservoir.SumWeight = 0.0f;
    Reservoir.M = 0.0f;
    Reservoir.W = 0.0f;

    if (HistoryValid > 0u)
    {
        const float2 Uv = (float2(FullPos) + 0.5f) / float2(FullWidth, FullHeight);
        const float2 VelocityNdc = VelocityTexture[FullPos];
        const float2 PrevUv = float2(Uv.x - VelocityNdc.x * 0.5f, Uv.y + VelocityNdc.y * 0.5f);

        if (all(PrevUv > 0.0f.xx) && all(PrevUv < 1.0f.xx))
        {
            const uint2 PrevHalfPos = min(uint2(PrevUv * float2(HalfWidth, HalfHeight)), uint2(HalfWidth - 1u, HalfHeight - 1u));
            const uint2 PackedHistory = HistoryDepthNormal[PrevHalfPos];
            const float PrevDepth = asfloat(PackedHistory.x);
            const float3 PrevNormal = RestirGIDecodeNormal16x2(PackedHistory.y);

            const float DepthDelta = abs(Depth - PrevDepth);
            const float NormalSimilarity = dot(Normal, PrevNormal);

            if (PrevDepth > 0.0f && PrevDepth < 1.0f && DepthDelta < 0.01f && NormalSimilarity > 0.8f)
            {
                FRestirGIReservoir History = RestirGILoadReservoir(HistorySampleRadiance, HistoryRayDirection, HistoryMW, PrevHalfPos);
                const float Target = RestirGITarget(History.Sample.Radiance);
                if (History.M > 0.0f && History.W > 0.0f && Target > 0.0f)
                {
                    RestirGIMerge(Reservoir, History, Target, RestirGIRandom01(HalfPos, SequenceFrame * 1543u + 3u));
                }
            }
        }
    }

    const float CurrentWeight = max(1e-5f, RestirGITarget(Current.Radiance));
    RestirGIUpdate(Reservoir, Current, CurrentWeight, RestirGIRandom01(HalfPos, SequenceFrame * 1531u + 41u));

    const float SelectedTarget = max(1e-5f, RestirGITarget(Reservoir.Sample.Radiance));
    // Normalization factor W used in resolve: SampleRadiance * W.
    // Compensates for the selection bias of reservoir sampling (brighter samples are picked more often),
    Reservoir.W = Reservoir.SumWeight / max(1e-5f, Reservoir.M * SelectedTarget);

    RestirGIStoreReservoir(HalfPos, Reservoir, Depth, Normal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
}

[numthreads(8, 8, 1)]
void CSReservoirBootstrap(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 HalfPos = DispatchThreadId.xy;
    if (HalfPos.x >= HalfWidth || HalfPos.y >= HalfHeight)
    {
        return;
    }

    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> InitialRadiance = ResourceDescriptorHeap[InputInitialRadianceSrvIndex];
    Texture2D<uint> InitialRayDir = ResourceDescriptorHeap[InputInitialRayDirectionSrvIndex];

    RWTexture2D<uint2> OutDepthNormal = ResourceDescriptorHeap[OutputDepthNormalUavIndex];
    RWTexture2D<float4> OutSampleRadiance = ResourceDescriptorHeap[OutputSampleRadianceUavIndex];
    RWTexture2D<uint> OutRayDirection = ResourceDescriptorHeap[OutputRayDirectionUavIndex];
    RWTexture2D<float2> OutMW = ResourceDescriptorHeap[OutputMWUavIndex];

    const uint2 FullPos = RestirGIHalfToFull(HalfPos);
    const float Depth = DepthTexture[FullPos];
    const float3 Normal = normalize(GBufferA[FullPos].xyz * 2.0f - 1.0f);

    if (Enabled == 0u || Depth <= 0.0f || Depth >= 1.0f)
    {
        FRestirGIReservoir Empty = (FRestirGIReservoir)0;
        RestirGIStoreReservoir(HalfPos, Empty, Depth, Normal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
        return;
    }

    const FRestirGISample Current = RestirGILoadSample(InitialRadiance, InitialRayDir, HalfPos);
    FRestirGIReservoir Reservoir = (FRestirGIReservoir)0;
    Reservoir.Sample = Current;

    const float CurrentWeight = max(1e-5f, RestirGITarget(Current.Radiance));
    RestirGIUpdate(Reservoir, Current, CurrentWeight, RestirGIRandom01(HalfPos, SequenceFrame * 1531u + 41u));

    const float SelectedTarget = max(1e-5f, RestirGITarget(Reservoir.Sample.Radiance));
    Reservoir.W = Reservoir.SumWeight / max(1e-5f, Reservoir.M * SelectedTarget);
    Reservoir.M = min(Reservoir.M, 30.0f);

    RestirGIStoreReservoir(HalfPos, Reservoir, Depth, Normal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
}

[numthreads(8, 8, 1)]
void CSSpatialResampling(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 HalfPos = DispatchThreadId.xy;
    if (HalfPos.x >= HalfWidth || HalfPos.y >= HalfHeight)
    {
        return;
    }

    Texture2D<uint2> InDepthNormal = ResourceDescriptorHeap[InputDepthNormalSrvIndex];
    Texture2D<float4> InSampleRadiance = ResourceDescriptorHeap[InputSampleRadianceSrvIndex];
    Texture2D<uint> InRayDirection = ResourceDescriptorHeap[InputRayDirectionSrvIndex];
    Texture2D<float2> InMW = ResourceDescriptorHeap[InputMWSrvIndex];

    RWTexture2D<uint2> OutDepthNormal = ResourceDescriptorHeap[OutputDepthNormalUavIndex];
    RWTexture2D<float4> OutSampleRadiance = ResourceDescriptorHeap[OutputSampleRadianceUavIndex];
    RWTexture2D<uint> OutRayDirection = ResourceDescriptorHeap[OutputRayDirectionUavIndex];
    RWTexture2D<float2> OutMW = ResourceDescriptorHeap[OutputMWUavIndex];

    FRestirGIReservoir Reservoir = RestirGILoadReservoir(InSampleRadiance, InRayDirection, InMW, HalfPos);
    const uint2 PackedCenter = InDepthNormal[HalfPos];
    const float CenterDepth = asfloat(PackedCenter.x);
    const float3 CenterNormal = RestirGIDecodeNormal16x2(PackedCenter.y);

    if (Enabled == 0u || CenterDepth <= 0.0f || CenterDepth >= 1.0f)
    {
        FRestirGIReservoir Empty = (FRestirGIReservoir)0;
        RestirGIStoreReservoir(HalfPos, Empty, CenterDepth, CenterNormal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
        return;
    }

    const uint MaxIterations = (SpatialPassIndex == 0u) ? 8u : 5u;
    const float SearchRadius = (SpatialPassIndex == 0u) ? 16.0f : 8.0f;

    [loop]
    for (uint Iteration = 0u; Iteration < MaxIterations; ++Iteration)
    {
        const float2 Jitter = RestirGIRandom02(HalfPos, SequenceFrame * 2467u + Iteration * 17u) * 2.0f - 1.0f;
        const int2 CandidatePos = int2(HalfPos) + int2(round(Jitter * SearchRadius));
        if (CandidatePos.x < 0 || CandidatePos.y < 0 || CandidatePos.x >= int(HalfWidth) || CandidatePos.y >= int(HalfHeight))
        {
            continue;
        }

        const uint2 NeighborPos = uint2(CandidatePos);
        const uint2 PackedNeighbor = InDepthNormal[NeighborPos];
        const float NeighborDepth = asfloat(PackedNeighbor.x);
        const float3 NeighborNormal = RestirGIDecodeNormal16x2(PackedNeighbor.y);
        if (NeighborDepth <= 0.0f || NeighborDepth >= 1.0f)
        {
            continue;
        }

        if (abs(CenterDepth - NeighborDepth) > 0.01f || dot(CenterNormal, NeighborNormal) < 0.9f)
        {
            continue;
        }

        FRestirGIReservoir Neighbor = RestirGILoadReservoir(InSampleRadiance, InRayDirection, InMW, NeighborPos);
        const float Target = RestirGITarget(Neighbor.Sample.Radiance);
        if (Target <= 0.0f || Neighbor.M <= 0.0f || Neighbor.W <= 0.0f)
        {
            continue;
        }

        RestirGIMerge(Reservoir, Neighbor, Target, RestirGIRandom01(HalfPos, SequenceFrame * 4513u + Iteration * 53u));
    }

    const float SelectedTarget = max(1e-5f, RestirGITarget(Reservoir.Sample.Radiance));
    Reservoir.W = Reservoir.SumWeight / max(1e-5f, Reservoir.M * SelectedTarget);

	RestirGIStoreReservoir(HalfPos, Reservoir, CenterDepth, CenterNormal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
}

[numthreads(8, 8, 1)]
void CSResolve(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= FullWidth || Pixel.y >= FullHeight)
    {
        return;
    }

    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<uint2> HalfDepthNormal = ResourceDescriptorHeap[InputDepthNormalSrvIndex];
    Texture2D<float4> ReservoirSampleRadiance = ResourceDescriptorHeap[InputSampleRadianceSrvIndex];
    Texture2D<uint> ReservoirRayDirection = ResourceDescriptorHeap[InputRayDirectionSrvIndex];
    Texture2D<float2> ReservoirMW = ResourceDescriptorHeap[InputMWSrvIndex];

    RWTexture2D<float4> OutputTexture = ResourceDescriptorHeap[OutputTextureUavIndex];
    RWTexture2D<uint4> InputSHOut = ResourceDescriptorHeap[ResolveInputSHUavIndex];
    RWTexture2D<float> VarianceOut = ResourceDescriptorHeap[ResolveVarianceUavIndex];
    const float Depth = DepthTexture[Pixel];

    if (Enabled == 0u || Depth <= 0.0f || Depth >= 1.0f)
    {
        OutputTexture[Pixel] = float4(0.0f, 0.0f, 0.0f, saturate(Depth));
        InputSHOut[Pixel] = uint4(0u, 0u, 0u, 0u);
        VarianceOut[Pixel] = 0.0f;
        return;
    }

    const float2 Uv = (float2(Pixel) + 0.5f) / float2(FullWidth, FullHeight);
    const float2 HalfCoord = Uv * float2(HalfWidth, HalfHeight) - 0.5f;
    const int2 HalfBase = int2(floor(HalfCoord));
    const float2 Frac = frac(HalfCoord);

    const int2 Tap00 = clamp(HalfBase + int2(0, 0), int2(0, 0), int2((int)HalfWidth - 1, (int)HalfHeight - 1));
    const int2 Tap10 = clamp(HalfBase + int2(1, 0), int2(0, 0), int2((int)HalfWidth - 1, (int)HalfHeight - 1));
    const int2 Tap01 = clamp(HalfBase + int2(0, 1), int2(0, 0), int2((int)HalfWidth - 1, (int)HalfHeight - 1));
    const int2 Tap11 = clamp(HalfBase + int2(1, 1), int2(0, 0), int2((int)HalfWidth - 1, (int)HalfHeight - 1));

    const float4 BilinearWeights = float4(
        (1.0f - Frac.x) * (1.0f - Frac.y),
        Frac.x * (1.0f - Frac.y),
        (1.0f - Frac.x) * Frac.y,
        Frac.x * Frac.y);

    const float3 CenterNormal = normalize(GBufferA[Pixel].xyz * 2.0f - 1.0f);
    const uint2 Packed00 = HalfDepthNormal[uint2(Tap00)];
    const uint2 Packed10 = HalfDepthNormal[uint2(Tap10)];
    const uint2 Packed01 = HalfDepthNormal[uint2(Tap01)];
    const uint2 Packed11 = HalfDepthNormal[uint2(Tap11)];
    const float D00 = asfloat(Packed00.x);
    const float D10 = asfloat(Packed10.x);
    const float D01 = asfloat(Packed01.x);
    const float D11 = asfloat(Packed11.x);
    const float3 N00 = RestirGIDecodeNormal16x2(Packed00.y);
    const float3 N10 = RestirGIDecodeNormal16x2(Packed10.y);
    const float3 N01 = RestirGIDecodeNormal16x2(Packed01.y);
    const float3 N11 = RestirGIDecodeNormal16x2(Packed11.y);

    const float4 GeometryWeights = float4(
        RestirGIResolveGeometryWeight(Depth, CenterNormal, D00, N00),
        RestirGIResolveGeometryWeight(Depth, CenterNormal, D10, N10),
        RestirGIResolveGeometryWeight(Depth, CenterNormal, D01, N01),
        RestirGIResolveGeometryWeight(Depth, CenterNormal, D11, N11));
    const float4 FinalWeights = BilinearWeights * GeometryWeights;
    const float WeightSum = max(1e-6f, dot(FinalWeights, 1.0f.xxxx));

    const float2 MW00 = ReservoirMW[uint2(Tap00)];
    const float2 MW10 = ReservoirMW[uint2(Tap10)];
    const float2 MW01 = ReservoirMW[uint2(Tap01)];
    const float2 MW11 = ReservoirMW[uint2(Tap11)];
    const float3 R00 = ReservoirSampleRadiance[uint2(Tap00)].xyz;
    const float3 R10 = ReservoirSampleRadiance[uint2(Tap10)].xyz;
    const float3 R01 = ReservoirSampleRadiance[uint2(Tap01)].xyz;
    const float3 R11 = ReservoirSampleRadiance[uint2(Tap11)].xyz;
    const float3 SampleRadiance00 = min(max(R00 * max(0.0f, MW00.y) * max(0.0f, Intensity), 0.0f.xxx), ClampThreshold.xxx);
    const float3 SampleRadiance10 = min(max(R10 * max(0.0f, MW10.y) * max(0.0f, Intensity), 0.0f.xxx), ClampThreshold.xxx);
    const float3 SampleRadiance01 = min(max(R01 * max(0.0f, MW01.y) * max(0.0f, Intensity), 0.0f.xxx), ClampThreshold.xxx);
    const float3 SampleRadiance11 = min(max(R11 * max(0.0f, MW11.y) * max(0.0f, Intensity), 0.0f.xxx), ClampThreshold.xxx);
    const float3 WeightedRadiance =
        SampleRadiance00 * FinalWeights.x +
        SampleRadiance10 * FinalWeights.y +
        SampleRadiance01 * FinalWeights.z +
        SampleRadiance11 * FinalWeights.w;

    const float3 Dir00 = RestirGiDecodeDirection16x2(ReservoirRayDirection[uint2(Tap00)]);
    const float3 Dir10 = RestirGiDecodeDirection16x2(ReservoirRayDirection[uint2(Tap10)]);
    const float3 Dir01 = RestirGiDecodeDirection16x2(ReservoirRayDirection[uint2(Tap01)]);
    const float3 Dir11 = RestirGiDecodeDirection16x2(ReservoirRayDirection[uint2(Tap11)]);
    const FRestirGiPackedSh Sh00 = RestirGiProjectSh(SampleRadiance00, Dir00);
    const FRestirGiPackedSh Sh10 = RestirGiProjectSh(SampleRadiance10, Dir10);
    const FRestirGiPackedSh Sh01 = RestirGiProjectSh(SampleRadiance01, Dir01);
    const FRestirGiPackedSh Sh11 = RestirGiProjectSh(SampleRadiance11, Dir11);
    FRestirGiPackedSh Sh = RestirGiScaleSh(
        RestirGiAddSh(
            RestirGiAddSh(RestirGiScaleSh(Sh00, FinalWeights.x), RestirGiScaleSh(Sh10, FinalWeights.y)),
            RestirGiAddSh(RestirGiScaleSh(Sh01, FinalWeights.z), RestirGiScaleSh(Sh11, FinalWeights.w))),
        rcp(WeightSum));

    const float3 Resolved = max(WeightedRadiance / WeightSum, 0.0f.xxx);
    const float SampleCount = (
        MW00.x * FinalWeights.x +
        MW10.x * FinalWeights.y +
        MW01.x * FinalWeights.z +
        MW11.x * FinalWeights.w) / WeightSum;
    OutputTexture[Pixel] = float4(Resolved, saturate(Depth));
    InputSHOut[Pixel] = RestirGiPackSh(Sh);
    const float Variance = 1.0f - saturate(SampleCount / 500.0f);
    VarianceOut[Pixel] = Variance * Variance;
}
