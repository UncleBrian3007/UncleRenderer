#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"
#include "RestirGINewReservoir.hlsli"
#include "RestirGISamplingCommon.hlsli"
#include "PathBrdfCommon.hlsli"
#include "GpuDebugLineCommon.hlsl"

RaytracingAccelerationStructure Scene : register(t0);

cbuffer RestirGINewConstants : register(b1)
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
    uint TemporalReuseEnabled;
    uint UseVisibility;
    uint UseBrdf;
    uint UseHistoryIndirect;
    uint SequenceFrame;
    uint DebugRayEnabled;
    uint DebugPixelX;
    uint DebugPixelY;
};

cbuffer RestirGINewBindless : register(b2)
{
    uint OutputTextureUavIndex;
    uint DepthIndex;
    uint VelocityIndex;
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint InstanceDataBufferIndex;
    uint EnvironmentCubeBindlessIndex;
    uint LinearClampSamplerIndex;
    uint InputInitialRadianceSrvIndex;
    uint InputInitialRayDirectionSrvIndex;
    uint HistoryDepthNormalSrvIndex;
    uint HistorySampleRadianceSrvIndex;
    uint HistoryRayDirectionSrvIndex;
    uint HistoryMWSrvIndex;
    uint OutputDepthNormalUavIndex;
    uint OutputSampleRadianceUavIndex;
    uint OutputRayDirectionUavIndex;
    uint OutputMWUavIndex;
    uint InputDepthNormalSrvIndex;
    uint InputSampleRadianceSrvIndex;
    uint InputRayDirectionSrvIndex;
    uint InputMWSrvIndex;
    uint OutputHistoryGeomAUavIndex;
    uint OutputHistoryGeomBUavIndex;
    uint HistoryIrradianceIndex;
    uint PrevLinearDepthIndex;
    uint DebugLineBufferUavIndex;
};

#include "RayTracingCommon.hlsl"

static const uint RestirGINewRayFlags = RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;
uint RestirGINewHash32(uint Value)
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

float RestirGINewRandom01(uint2 Pixel, uint Salt)
{
    uint Seed = RestirGINewHash32(Pixel.x + 0x9e3779b9u);
    Seed = RestirGINewHash32(Seed + Pixel.y);
    Seed = RestirGINewHash32(Seed + Salt * 1664525u);
    return (Seed & 0x00ffffffu) / 16777216.0f;
}

float2 RestirGINewRandom02(uint2 Pixel, uint Salt)
{
    return float2(
        RestirGINewRandom01(Pixel, Salt + 11u),
        RestirGINewRandom01(Pixel, Salt + 73u));
}

uint RestirGINewPackDebugColor(float3 Radiance)
{
    float3 Color = max(Radiance, 0.0f.xxx);
    Color = Color / (1.0f.xxx + Color);
    Color = saturate(pow(Color, 1.0f / 2.2f) * 1.35f);
    const uint3 PackedRgb = (uint3)round(Color * 255.0f);
    return (0xFFu << 24u) | (PackedRgb.b << 16u) | (PackedRgb.g << 8u) | PackedRgb.r;
}

uint2 RestirGINewHalfToFull(uint2 HalfPos)
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

float3 RestirGINewReconstructWorldPosition(uint2 FullPos, float Depth)
{
    float2 Uv = (float2(FullPos) + 0.5f) / float2(FullWidth, FullHeight);
    float2 Ndc = float2(Uv.x * 2.0f - 1.0f, 1.0f - Uv.y * 2.0f);
    float4 Clip = float4(Ndc, Depth, 1.0f);
    float4 WorldPos = mul(Clip, ViewProjectionInverse);
    WorldPos.xyz /= max(WorldPos.w, 1e-6f);
    return WorldPos.xyz;
}


bool RestirGINewTraceVisibility(float3 Origin, float3 Direction, float MaxDistance)
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

float3 RestirGINewSampleHistoryIndirect(uint2 FullPos, float3 HitWorldPos)
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

float3 RestirGINewEvaluateHitRadiance(uint InstanceID, float2 UV, float3 HitNormal, float3 HitAlbedo, float HitMetalness, float HitRoughness, float3 OutDirection, float3 HitWorldPos, uint2 FullPos)
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
        Visibility = RestirGINewTraceVisibility(HitWorldPos + HitNormal * 0.01f, Wi, max(0.1f, RayLength)) ? 1.0f : 0.0f;
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

    const float3 HistoryIndirect = RestirGINewSampleHistoryIndirect(FullPos, HitWorldPos);
    const float3 Emissive = max(SampleEmissive(InstanceID, UV), 0.0f.xxx);
    return max(Direct + HistoryIndirect + Emissive, 0.0f.xxx);
}

float3 RestirGINewSampleCandidate(float3 WorldPos, float3 Normal, uint2 FullPos, uint2 HalfPos, out float3 OutDirection, out float OutHitDistance, out bool bOutHit)
{
    const float2 Xi = RestirGINewRandom02(HalfPos, SequenceFrame * 1999u + 17u);
    const float3 Direction = SampleHemisphereCosine(Xi, Normal);
    OutDirection = Direction;
    OutHitDistance = max(0.1f, RayLength);
    bOutHit = false;

    RayDesc Ray;
    Ray.Origin = WorldPos + Normal * 0.01f;
    Ray.Direction = Direction;
    Ray.TMin = 1e-3f;
    Ray.TMax = max(0.1f, RayLength);

    RayQuery<RestirGINewRayFlags> Query;
    Query.TraceRayInline(Scene, RestirGINewRayFlags, 0xFF, Ray);

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
        Incoming = RestirGINewEvaluateHitRadiance(InstanceID, UV, HitNormal, HitAlbedo, HitMR.x, HitMR.y, -Direction, HitWorldPos, FullPos);
    }
    else
    {
        Incoming = EvaluateSky(Direction);
    }

    // NOTE: Keep any RestirGINewRandom01 test hook behavior unchanged in this pass;
    // restore to production path when running strict Legacy/New comparisons.
    return max(Incoming, 0.0f.xxx);
}

uint RestirGINewEncodeNormal16x2(float3 N)
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

float3 RestirGINewDecodeNormal16x2(uint Packed)
{
    float2 Enc = float2(Packed & 0xFFFFu, Packed >> 16u) / 65535.0f;
    Enc = Enc * 2.0f - 1.0f;

    float3 N = float3(Enc.xy, 1.0f - abs(Enc.x) - abs(Enc.y));
    float2 T = saturate(-N.zz);
    N.xy += lerp(T, -T, step(0.0f.xx, N.xy));
    return normalize(N);
}

FRestirGINewSample RestirGINewLoadSample(Texture2D<float4> RadianceTexture, Texture2D<uint> RayDirTexture, uint2 Pos)
{
    FRestirGINewSample S;
    S.Radiance = RadianceTexture[Pos].xyz;
    S.RayDirection = RayDirTexture[Pos];
    return S;
}

FRestirGINewReservoir RestirGINewLoadReservoir(Texture2D<float4> SampleRadianceTexture, Texture2D<uint> RayDirectionTexture, Texture2D<float2> MWTexture, uint2 Pos)
{
    FRestirGINewReservoir R;
    R.Sample.Radiance = SampleRadianceTexture[Pos].xyz;
    R.Sample.RayDirection = RayDirectionTexture[Pos];
    R.M = MWTexture[Pos].x;
    R.W = MWTexture[Pos].y;
    R.SumWeight = R.W * R.M * RestirGINewTarget(R.Sample.Radiance);
    return R;
}

void RestirGINewStoreReservoir(
    uint2 Pos,
    FRestirGINewReservoir Reservoir,
    float Depth,
    float3 Normal,
    RWTexture2D<uint2> OutDepthNormal,
    RWTexture2D<float4> OutSampleRadiance,
    RWTexture2D<uint> OutRayDirection,
    RWTexture2D<float2> OutMW)
{
    OutDepthNormal[Pos] = uint2(asuint(Depth), RestirGINewEncodeNormal16x2(Normal));
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

    const uint2 FullPos = RestirGINewHalfToFull(HalfPos);
    const float Depth = DepthTexture[FullPos];
    if (Depth <= 0.0f || Depth >= 1.0f)
    {
        InitialRadianceOut[HalfPos] = 0.0f.xxxx;
        InitialRayDirOut[HalfPos] = 0u;
        return;
    }

    const float3 Normal = normalize(GBufferA[FullPos].xyz * 2.0f - 1.0f);
    const float3 WorldPos = RestirGINewReconstructWorldPosition(FullPos, Depth);
    float3 SampleDirection = 0.0f.xxx;
    float DebugHitDistance = max(0.1f, RayLength);
    bool bDebugHit = false;
    const float3 Candidate = RestirGINewSampleCandidate(WorldPos, Normal, FullPos, HalfPos, SampleDirection, DebugHitDistance, bDebugHit);

    if (DebugRayEnabled != 0u && DebugLineBufferUavIndex != 0xFFFFFFFFu && all(HalfPos == uint2(DebugPixelX, DebugPixelY)))
    {
        const float TraceDistance = bDebugHit ? max(1e-3f, DebugHitDistance) : max(0.1f, RayLength);
        const uint DebugColor = RestirGINewPackDebugColor(Candidate);
        DebugDrawLine(DebugLineBufferUavIndex, WorldPos, WorldPos + SampleDirection * TraceDistance, DebugColor);
    }

    InitialRadianceOut[HalfPos] = float4(Candidate, 0.0f);
    InitialRayDirOut[HalfPos] = 0u;
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

    const uint2 FullPos = RestirGINewHalfToFull(HalfPos);
    const float Depth = DepthTexture[FullPos];
    const float3 Normal = normalize(GBufferA[FullPos].xyz * 2.0f - 1.0f);

    if (Enabled == 0u || Depth <= 0.0f || Depth >= 1.0f)
    {
        FRestirGINewReservoir Empty = (FRestirGINewReservoir)0;
        RestirGINewStoreReservoir(HalfPos, Empty, Depth, Normal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
        return;
    }

    FRestirGINewSample Current = RestirGINewLoadSample(InitialRadiance, InitialRayDir, HalfPos);

    FRestirGINewReservoir Reservoir = (FRestirGINewReservoir)0;
    Reservoir.Sample = Current;
    Reservoir.SumWeight = 0.0f;
    Reservoir.M = 0.0f;
    Reservoir.W = 0.0f;

    if (TemporalReuseEnabled > 0u && HistoryValid > 0u)
    {
        const float2 Uv = (float2(FullPos) + 0.5f) / float2(FullWidth, FullHeight);
        const float2 VelocityNdc = VelocityTexture[FullPos];
        const float2 PrevUv = float2(Uv.x - VelocityNdc.x * 0.5f, Uv.y + VelocityNdc.y * 0.5f);

        if (all(PrevUv > 0.0f.xx) && all(PrevUv < 1.0f.xx))
        {
            const uint2 PrevHalfPos = min(uint2(PrevUv * float2(HalfWidth, HalfHeight)), uint2(HalfWidth - 1u, HalfHeight - 1u));
            const uint2 PackedHistory = HistoryDepthNormal[PrevHalfPos];
            const float PrevDepth = asfloat(PackedHistory.x);
            const float3 PrevNormal = RestirGINewDecodeNormal16x2(PackedHistory.y);

            const float DepthDelta = abs(Depth - PrevDepth);
            const float NormalSimilarity = dot(Normal, PrevNormal);

            if (PrevDepth > 0.0f && PrevDepth < 1.0f && DepthDelta < 0.01f && NormalSimilarity > 0.8f)
            {
                FRestirGINewReservoir History = RestirGINewLoadReservoir(HistorySampleRadiance, HistoryRayDirection, HistoryMW, PrevHalfPos);
                const float Target = RestirGINewTarget(History.Sample.Radiance);
                if (History.M > 0.0f && History.W > 0.0f && Target > 0.0f)
                {
                    RestirGINewMerge(Reservoir, History, Target, RestirGINewRandom01(HalfPos, SequenceFrame * 1543u + 3u));
                }
            }
        }
    }

    const float CurrentWeight = max(1e-5f, RestirGINewTarget(Current.Radiance));
    RestirGINewUpdate(Reservoir, Current, CurrentWeight, RestirGINewRandom01(HalfPos, SequenceFrame * 1531u + 41u));

    const float SelectedTarget = max(1e-5f, RestirGINewTarget(Reservoir.Sample.Radiance));
    Reservoir.W = Reservoir.SumWeight / max(1e-5f, Reservoir.M * SelectedTarget);
    Reservoir.M = min(Reservoir.M, 30.0f);

    RestirGINewStoreReservoir(HalfPos, Reservoir, Depth, Normal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
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

    FRestirGINewReservoir Reservoir = RestirGINewLoadReservoir(InSampleRadiance, InRayDirection, InMW, HalfPos);
    const uint2 PackedCenter = InDepthNormal[HalfPos];
    const float CenterDepth = asfloat(PackedCenter.x);
    const float3 CenterNormal = RestirGINewDecodeNormal16x2(PackedCenter.y);

    if (Enabled == 0u || CenterDepth <= 0.0f || CenterDepth >= 1.0f)
    {
        FRestirGINewReservoir Empty = (FRestirGINewReservoir)0;
        RestirGINewStoreReservoir(HalfPos, Empty, CenterDepth, CenterNormal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
        return;
    }

    const uint MaxIterations = (SpatialPassIndex == 0u) ? 8u : 5u;
    const float SearchRadius = (SpatialPassIndex == 0u) ? 16.0f : 8.0f;

    [loop]
    for (uint Iteration = 0u; Iteration < MaxIterations; ++Iteration)
    {
        const float2 Jitter = RestirGINewRandom02(HalfPos, SequenceFrame * 2467u + Iteration * 17u) * 2.0f - 1.0f;
        const int2 CandidatePos = int2(HalfPos) + int2(round(Jitter * SearchRadius));
        if (CandidatePos.x < 0 || CandidatePos.y < 0 || CandidatePos.x >= int(HalfWidth) || CandidatePos.y >= int(HalfHeight))
        {
            continue;
        }

        const uint2 NeighborPos = uint2(CandidatePos);
        const uint2 PackedNeighbor = InDepthNormal[NeighborPos];
        const float NeighborDepth = asfloat(PackedNeighbor.x);
        const float3 NeighborNormal = RestirGINewDecodeNormal16x2(PackedNeighbor.y);
        if (NeighborDepth <= 0.0f || NeighborDepth >= 1.0f)
        {
            continue;
        }

        if (abs(CenterDepth - NeighborDepth) > 0.01f || dot(CenterNormal, NeighborNormal) < 0.9f)
        {
            continue;
        }

        FRestirGINewReservoir Neighbor = RestirGINewLoadReservoir(InSampleRadiance, InRayDirection, InMW, NeighborPos);
        const float Target = RestirGINewTarget(Neighbor.Sample.Radiance);
        if (Target <= 0.0f || Neighbor.M <= 0.0f || Neighbor.W <= 0.0f)
        {
            continue;
        }

        RestirGINewMerge(Reservoir, Neighbor, Target, RestirGINewRandom01(HalfPos, SequenceFrame * 4513u + Iteration * 53u));
    }

    const float SelectedTarget = max(1e-5f, RestirGINewTarget(Reservoir.Sample.Radiance));
    // Normalization factor W used in resolve: SampleRadiance * W.
    // Compensates for the selection bias of reservoir sampling (brighter samples are picked more often),
    // so that a single selected sample reproduces the average contribution of all candidates (¥ÒTarget / M).
    Reservoir.W = Reservoir.SumWeight / max(1e-5f, Reservoir.M * SelectedTarget);
    Reservoir.M = min(Reservoir.M, 30.0f);

    RestirGINewStoreReservoir(HalfPos, Reservoir, CenterDepth, CenterNormal, OutDepthNormal, OutSampleRadiance, OutRayDirection, OutMW);
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
    Texture2D<float4> GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D<float4> GBufferC = ResourceDescriptorHeap[GBufferCIndex];

    Texture2D<float4> ReservoirSampleRadiance = ResourceDescriptorHeap[InputSampleRadianceSrvIndex];
    Texture2D<float2> ReservoirMW = ResourceDescriptorHeap[InputMWSrvIndex];

    RWTexture2D<float4> OutputTexture = ResourceDescriptorHeap[OutputTextureUavIndex];
    RWTexture2D<float4> HistoryGeomAOut = ResourceDescriptorHeap[OutputHistoryGeomAUavIndex];
    RWTexture2D<float4> HistoryGeomBOut = ResourceDescriptorHeap[OutputHistoryGeomBUavIndex];

    const float Depth = DepthTexture[Pixel];
    const float3 PackedNormal = normalize(GBufferA[Pixel].xyz * 2.0f - 1.0f) * 0.5f + 0.5f;
    const float Roughness = saturate(GBufferB[Pixel].z);
    const float Metalness = saturate(GBufferB[Pixel].y);
    HistoryGeomAOut[Pixel] = float4(PackedNormal, Roughness);
    HistoryGeomBOut[Pixel] = float4(saturate(GBufferC[Pixel].rgb), Metalness);

    if (Enabled == 0u || Depth <= 0.0f || Depth >= 1.0f)
    {
        OutputTexture[Pixel] = float4(0.0f, 0.0f, 0.0f, saturate(Depth));
        return;
    }

    const uint2 HalfPos = min(Pixel / 2u, uint2(HalfWidth - 1u, HalfHeight - 1u));
    const float3 SampleRadiance = ReservoirSampleRadiance[HalfPos].xyz;
    const float2 MW = ReservoirMW[HalfPos];
    const float M = max(1.0f, MW.x);
    const float W = max(0.0f, MW.y);
    const float3 Resolved = min(max(SampleRadiance * W * max(0.0f, Intensity), 0.0f.xxx), ClampThreshold.xxx);
    OutputTexture[Pixel] = float4(Resolved, saturate(Depth));
}
