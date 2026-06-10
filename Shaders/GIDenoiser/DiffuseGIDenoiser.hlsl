#include "../CommonSH.hlsli"
#include "../Common.hlsli"

cbuffer DiffuseGIDenoiserConstants : register(b0)
{
    uint Width;
    uint Height;
    uint HistoryValid;
    uint PassIndex;
    float DepthThresholdScale;
    float NormalThreshold;
    float BlendStrength;
    uint MipLevel;
    float DenoiserPadding1;
    float DenoiserPadding2;
};

cbuffer DiffuseGIDenoiserBindless : register(b1)
{
    uint InputSHIndex;
    uint VarianceIndex;
    uint VelocityIndex;
    uint CurrentLinearDepthIndex;
    uint PrevLinearDepthIndex;
    uint GBufferAIndex;
    uint PrevNormalIndex;
    uint HistorySHIndex;
    uint HistoryCountIndex;
    uint TemporalSHIndex;
    uint OutHistoryIrradianceIndex;
    uint OutHistorySHIndex;
    uint OutHistoryCountIndex;
    uint OutPrevLinearDepthIndex;
    uint OutPrevNormalIndex;
    uint AuxiliaryIndex;
};

cbuffer DiffuseGIDenoiserSceneConstants : register(b2)
{
    row_major float4x4 SceneWorld;
    row_major float4x4 SceneWorldInverseTranspose;
    row_major float4x4 SceneView;
    row_major float4x4 SceneViewInverse;
    row_major float4x4 SceneProjection;
};

float3 DecodeNormalFromGBufferA(float4 GBufferA)
{
    return normalize(GBufferA.xyz * 2.0f - 1.0f);
}

FPackedSh LoadPackedSh(Texture2D<uint4> Texture, uint2 Pixel)
{
    return UnpackSh(Texture[Pixel]);
}

FPackedSh LoadPackedSh(RWTexture2D<uint4> Texture, uint2 Pixel)
{
    return UnpackSh(Texture[Pixel]);
}

float ComputeGeometryWeight(float CenterDepth, float SampleDepth, float3 CenterNormal, float3 SampleNormal)
{
    const float DepthWeight = exp(-abs(CenterDepth - SampleDepth));
    const float NormalWeight = pow(saturate(dot(CenterNormal, SampleNormal)), 32.0f);
    return saturate(DepthWeight * NormalWeight);
}

float3 ReconstructWorldPositionFromLinearDepth(uint2 Pixel, float LinearDepth)
{
    const float2 Uv = (float2(Pixel) + 0.5f) / float2(Width, Height);
    const float3 ViewPosition = ReconstructViewPosition(Uv, LinearDepth, SceneProjection);
    return mul(float4(ViewPosition, 1.0f), SceneViewInverse).xyz;
}

float2 ProjectWorldPositionToUv(float3 WorldPosition)
{
    const float4 ViewPosition = mul(float4(WorldPosition, 1.0f), SceneView);
    const float4 ClipPosition = mul(ViewPosition, SceneProjection);
    if (ClipPosition.w <= 1e-6f)
    {
        return -1.0f.xx;
    }

    const float InvW = rcp(ClipPosition.w);
    const float2 Ndc = ClipPosition.xy * InvW;
    return float2(Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f);
}

float ComputeSurfaceGeometryWeight(float3 CenterWorldPos, float3 CenterNormal, float3 SampleWorldPos)
{
    const float3 Ray = SampleWorldPos - CenterWorldPos;
    const float DistanceToPlane = dot(CenterNormal, Ray);
    return saturate(1.0f - abs(DistanceToPlane));
}

float ComputeSurfaceNormalWeight(float3 CenterNormal, float3 SampleNormal)
{
    return pow(saturate(dot(CenterNormal, SampleNormal)), 128.0f);
}

static const uint RecurrentBlurSampleNum = 4u;
static const float2 RecurrentBlurPoisson[RecurrentBlurSampleNum] =
{
    float2(-0.4646624f, 0.2480316f),
    float2(0.9562537f, 0.1687815f),
    float2(0.1834577f, -0.8139205f),
    float2(0.1929236f, 0.6890683f)
};



[numthreads(8, 8, 1)]
void CSPreBlur(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= Width || Pixel.y >= Height)
    {
        return;
    }

    Texture2D<uint4> InputSH = ResourceDescriptorHeap[InputSHIndex];
    Texture2D<float> VarianceTexture = ResourceDescriptorHeap[VarianceIndex];
    Texture2D<float> LinearDepthTexture = ResourceDescriptorHeap[CurrentLinearDepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    RWTexture2D<uint4> PreBlurSH = ResourceDescriptorHeap[AuxiliaryIndex];

    const FPackedSh CenterSh = LoadPackedSh(InputSH, Pixel);
    const float CenterDepth = LinearDepthTexture[Pixel];
    if (CenterDepth <= 0.0f)
    {
        PreBlurSH[Pixel] = uint4(0u, 0u, 0u, 0u);
        return;
    }

    const float3 CenterNormal = DecodeNormalFromGBufferA(GBufferA[Pixel]);
    const float3 CenterWorldPos = ReconstructWorldPositionFromLinearDepth(Pixel, CenterDepth);
    float3 CenterTangent;
    float3 CenterBitangent;
    BuildOrthonormalBasis(CenterNormal, CenterTangent, CenterBitangent);

    FPackedSh AccumSh = CenterSh;
    float TotalWeight = 1.0f;

    const float Variance = saturate(VarianceTexture[Pixel]);
    const float2 BlurRadius = lerp(2.0f.xx, 8.0f.xx, Variance.xx) / float2(Width, Height) * max(CenterDepth, 0.01f);
    const float2 CenterUv = (float2(Pixel) + 0.5f) / float2(Width, Height);

    [unroll]
    for (uint SampleIndex = 0u; SampleIndex < RecurrentBlurSampleNum; ++SampleIndex)
    {
        const float2 SurfaceOffset = RecurrentBlurPoisson[SampleIndex] * BlurRadius;
        float3 SampleWorldPos = CenterWorldPos + CenterTangent * SurfaceOffset.x + CenterBitangent * SurfaceOffset.y;
        const float2 SampleUv = ProjectWorldPositionToUv(SampleWorldPos);
        if (any(SampleUv < 0.0f.xx) || any(SampleUv > 1.0f.xx))
        {
            continue;
        }

        const uint2 SamplePixel = min(uint2(SampleUv * float2(Width, Height)), uint2(Width - 1u, Height - 1u));
        const float SampleDepth = LinearDepthTexture[SamplePixel];
        if (SampleDepth <= 0.0f)
        {
            continue;
        }

        SampleWorldPos = ReconstructWorldPositionFromLinearDepth(SamplePixel, SampleDepth);
        const float3 SampleNormal = DecodeNormalFromGBufferA(GBufferA[SamplePixel]);
        const float GeometryWeight = ComputeSurfaceGeometryWeight(CenterWorldPos, CenterNormal, SampleWorldPos);
        const float NormalWeight = ComputeSurfaceNormalWeight(CenterNormal, SampleNormal);
        const float Weight = smoothstep(0.0f, 1.0f, GeometryWeight * NormalWeight);
        AccumSh = AddSh(AccumSh, ScaleSh(LoadPackedSh(InputSH, SamplePixel), Weight));
        TotalWeight += Weight;
    }

    const FPackedSh OutSh = ScaleSh(AccumSh, rcp(max(1e-5f, TotalWeight)));
    PreBlurSH[Pixel] = PackSh(OutSh);
}

[numthreads(8, 8, 1)]
void CSTemporalAccumulation(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= Width || Pixel.y >= Height)
    {
        return;
    }

    Texture2D<uint4> PreBlurSH = ResourceDescriptorHeap[AuxiliaryIndex];
    Texture2D<float> DepthBuffer = ResourceDescriptorHeap[OutHistoryIrradianceIndex];
    Texture2D<float4> VelocityTexture = ResourceDescriptorHeap[VelocityIndex];
    Texture2D<float> CurrentLinearDepthTexture = ResourceDescriptorHeap[CurrentLinearDepthIndex];
    Texture2D<float> PrevLinearDepthTexture = ResourceDescriptorHeap[PrevLinearDepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> PrevNormalTexture = ResourceDescriptorHeap[PrevNormalIndex];
    Texture2D<uint4> HistorySH = ResourceDescriptorHeap[HistorySHIndex];
    Texture2D<uint> HistoryCount = ResourceDescriptorHeap[HistoryCountIndex];

    RWTexture2D<uint4> OutTemporalSH = ResourceDescriptorHeap[TemporalSHIndex];
    RWTexture2D<uint> OutHistoryCount = ResourceDescriptorHeap[OutHistoryCountIndex];
    RWTexture2D<float> OutPrevLinearDepth = ResourceDescriptorHeap[OutPrevLinearDepthIndex];
    RWTexture2D<float4> OutPrevNormal = ResourceDescriptorHeap[OutPrevNormalIndex];

    FPackedSh CurrentSh = LoadPackedSh(PreBlurSH, Pixel);
    const float2 Uv = (float2(Pixel) + 0.5f) / float2(Width, Height);
    const float3 VelocityNdc = VelocityTexture[Pixel].xyz;
    const float2 PrevUv = float2(Uv.x - VelocityNdc.x * 0.5f, Uv.y + VelocityNdc.y * 0.5f);

    const float3 CurrentNormal = DecodeNormalFromGBufferA(GBufferA[Pixel]);
    const float CurrentLinearDepth = CurrentLinearDepthTexture[Pixel];
    FPackedSh TemporalSh = CurrentSh;
    uint NextCount = 1u;

    bool bHistoryAccepted = (HistoryValid != 0u) && (CurrentLinearDepth > 0.0f);
    if (bHistoryAccepted && (any(PrevUv <= 0.0f.xx) || any(PrevUv >= 1.0f.xx)))
    {
        bHistoryAccepted = false;
    }

    if (bHistoryAccepted)
    {
        const float CurrentDeviceDepth = DepthBuffer[Pixel];
        const float2 PrevCoord = PrevUv * float2(Width, Height) - 0.5f;
        const int2 PrevBase = int2(floor(PrevCoord));
        const float2 PrevFrac = frac(PrevCoord);

        const int2 Tap00 = clamp(PrevBase + int2(0, 0), int2(0, 0), int2((int)Width - 1, (int)Height - 1));
        const int2 Tap10 = clamp(PrevBase + int2(1, 0), int2(0, 0), int2((int)Width - 1, (int)Height - 1));
        const int2 Tap01 = clamp(PrevBase + int2(0, 1), int2(0, 0), int2((int)Width - 1, (int)Height - 1));
        const int2 Tap11 = clamp(PrevBase + int2(1, 1), int2(0, 0), int2((int)Width - 1, (int)Height - 1));

        const uint2 P00 = uint2(Tap00);
        const uint2 P10 = uint2(Tap10);
        const uint2 P01 = uint2(Tap01);
        const uint2 P11 = uint2(Tap11);

        const float4 BilinearWeights = float4(
            (1.0f - PrevFrac.x) * (1.0f - PrevFrac.y),
            PrevFrac.x * (1.0f - PrevFrac.y),
            (1.0f - PrevFrac.x) * PrevFrac.y,
            PrevFrac.x * PrevFrac.y);

        const float D00 = PrevLinearDepthTexture[P00];
        const float D10 = PrevLinearDepthTexture[P10];
        const float D01 = PrevLinearDepthTexture[P01];
        const float D11 = PrevLinearDepthTexture[P11];
        const float3 N00 = normalize(PrevNormalTexture[P00].xyz * 2.0f - 1.0f);
        const float3 N10 = normalize(PrevNormalTexture[P10].xyz * 2.0f - 1.0f);
        const float3 N01 = normalize(PrevNormalTexture[P01].xyz * 2.0f - 1.0f);
        const float3 N11 = normalize(PrevNormalTexture[P11].xyz * 2.0f - 1.0f);

        const float PrevLinearDepthEstimate = ResolveLinearDepthFromDeviceDepth(CurrentDeviceDepth - VelocityNdc.z, SceneProjection);
        const float4 Occlusion = float4(
            ((PrevLinearDepthEstimate <= D00 * DepthThresholdScale) && (dot(CurrentNormal, N00) > NormalThreshold)) ? 1.0f : 0.0f,
            ((PrevLinearDepthEstimate <= D10 * DepthThresholdScale) && (dot(CurrentNormal, N10) > NormalThreshold)) ? 1.0f : 0.0f,
            ((PrevLinearDepthEstimate <= D01 * DepthThresholdScale) && (dot(CurrentNormal, N01) > NormalThreshold)) ? 1.0f : 0.0f,
            ((PrevLinearDepthEstimate <= D11 * DepthThresholdScale) && (dot(CurrentNormal, N11) > NormalThreshold)) ? 1.0f : 0.0f);

        const float4 FinalWeights = BilinearWeights * Occlusion;
        const float WeightSum = dot(FinalWeights, 1.0f.xxxx);
        if (WeightSum > 1e-5f)
        {
            const FPackedSh H00 = LoadPackedSh(HistorySH, P00);
            const FPackedSh H10 = LoadPackedSh(HistorySH, P10);
            const FPackedSh H01 = LoadPackedSh(HistorySH, P01);
            const FPackedSh H11 = LoadPackedSh(HistorySH, P11);

            const FPackedSh PrevSh = ScaleSh(
                AddSh(
                    AddSh(ScaleSh(H00, FinalWeights.x), ScaleSh(H10, FinalWeights.y)),
                    AddSh(ScaleSh(H01, FinalWeights.z), ScaleSh(H11, FinalWeights.w))),
                rcp(WeightSum));

            const float PrevCount =
                (float(HistoryCount[P00]) * FinalWeights.x +
                float(HistoryCount[P10]) * FinalWeights.y +
                float(HistoryCount[P01]) * FinalWeights.z +
                float(HistoryCount[P11]) * FinalWeights.w) / WeightSum;
			const float ClampedPrevCount = min(PrevCount, 31.0f);
            const float BlendFactor = saturate((1.0f / (1.0f + ClampedPrevCount)) * BlendStrength);

            TemporalSh = LerpSh(PrevSh, CurrentSh, BlendFactor);
            NextCount = min((uint)round(ClampedPrevCount) + 1u, 32u);
        }
        else
        {
            bHistoryAccepted = false;
        }
    }

    OutTemporalSH[Pixel] = PackSh(TemporalSh);
    OutHistoryCount[Pixel] = NextCount;
    OutPrevLinearDepth[Pixel] = CurrentLinearDepth;
    OutPrevNormal[Pixel] = float4(CurrentNormal * 0.5f + 0.5f, 1.0f);
}

[numthreads(8, 8, 1)]
void CSHistoryReconstruction(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= Width || Pixel.y >= Height)
    {
        return;
    }

    Texture2D<uint4> HistorySH = ResourceDescriptorHeap[HistorySHIndex];
    Texture2D<uint4> ShMip = ResourceDescriptorHeap[AuxiliaryIndex];
    Texture2D<float> DepthMip = ResourceDescriptorHeap[OutPrevLinearDepthIndex];
    Texture2D<float> CurrentLinearDepthTexture = ResourceDescriptorHeap[CurrentLinearDepthIndex];
    Texture2D<uint> HistoryCount = ResourceDescriptorHeap[HistoryCountIndex];
    RWTexture2D<uint4> TemporalSH = ResourceDescriptorHeap[TemporalSHIndex];

    const uint Count = HistoryCount[Pixel];
    if (Count >= 10u)
    {
        return;
    }

    const float CountRatio = saturate((float)Count / 10.0f);
    const uint ComputedMip = min((uint)round(3.0f * (1.0f - CountRatio)), 3u);
    const uint Scale = 1u << (ComputedMip + 1u);
    const uint MipWidth = (Width + Scale - 1u) / Scale;
    const uint MipHeight = (Height + Scale - 1u) / Scale;

    const float2 FullUv = (float2(Pixel) + 0.5f) / float2(Width, Height);
    const float2 MipSize = float2(MipWidth, MipHeight);
    const float2 MipCoord = FullUv * MipSize - 0.5f;
    const int2 MipBase = int2(floor(MipCoord));
    const float2 Frac = frac(MipCoord);

    const int2 Tap00 = clamp(MipBase + int2(0, 0), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));
    const int2 Tap10 = clamp(MipBase + int2(1, 0), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));
    const int2 Tap01 = clamp(MipBase + int2(0, 1), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));
    const int2 Tap11 = clamp(MipBase + int2(1, 1), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));

    const float CurrentDepth = CurrentLinearDepthTexture[Pixel];

    const float W00 = (1.0f - Frac.x) * (1.0f - Frac.y);
    const float W10 = Frac.x * (1.0f - Frac.y);
    const float W01 = (1.0f - Frac.x) * Frac.y;
    const float W11 = Frac.x * Frac.y;

    FPackedSh AccumSh;
    AccumSh.ShY = 0.0f.xxxx;
    AccumSh.Co = 0.0f;
    AccumSh.Cg = 0.0f;
    float Total = 0.0f;

    const float D00 = DepthMip.Load(uint3(uint2(Tap00), ComputedMip));
    const float D10 = DepthMip.Load(uint3(uint2(Tap10), ComputedMip));
    const float D01 = DepthMip.Load(uint3(uint2(Tap01), ComputedMip));
    const float D11 = DepthMip.Load(uint3(uint2(Tap11), ComputedMip));

    const float DepthSigma = max(CurrentDepth * 0.1f, 1e-3f);
    const float4 BilinearWeights = float4(W00, W10, W01, W11);
    const float4 GeometryWeights = float4(
        exp(-abs(CurrentDepth - D00) / DepthSigma),
        exp(-abs(CurrentDepth - D10) / DepthSigma),
        exp(-abs(CurrentDepth - D01) / DepthSigma),
        exp(-abs(CurrentDepth - D11) / DepthSigma));

    const float4 FinalWeights = BilinearWeights * GeometryWeights;
    AccumSh = AddSh(AccumSh, ScaleSh(UnpackSh(ShMip.Load(uint3(uint2(Tap00), ComputedMip))), FinalWeights.x));
    AccumSh = AddSh(AccumSh, ScaleSh(UnpackSh(ShMip.Load(uint3(uint2(Tap10), ComputedMip))), FinalWeights.y));
    AccumSh = AddSh(AccumSh, ScaleSh(UnpackSh(ShMip.Load(uint3(uint2(Tap01), ComputedMip))), FinalWeights.z));
    AccumSh = AddSh(AccumSh, ScaleSh(UnpackSh(ShMip.Load(uint3(uint2(Tap11), ComputedMip))), FinalWeights.w));
    Total += FinalWeights.x + FinalWeights.y + FinalWeights.z + FinalWeights.w;

    if (Total <= 1e-4f)
    {
        return;
    }

    const FPackedSh Reconstructed = ScaleSh(AccumSh, rcp(Total));
    const FPackedSh CurrentTemporal = LoadPackedSh(TemporalSH, Pixel);
    const float ReconstructAmount = saturate(1.0f - CountRatio) * saturate(Total * 4.0f);
    TemporalSH[Pixel] = PackSh(LerpSh(CurrentTemporal, Reconstructed, ReconstructAmount));
}

[numthreads(8, 8, 1)]
void CSFinalBlur(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= Width || Pixel.y >= Height)
    {
        return;
    }

    Texture2D<uint4> TemporalSH = ResourceDescriptorHeap[TemporalSHIndex];
    Texture2D<uint> HistoryCount = ResourceDescriptorHeap[HistoryCountIndex];
    Texture2D<float> LinearDepthTexture = ResourceDescriptorHeap[CurrentLinearDepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];

    RWTexture2D<float3> OutHistoryIrradiance = ResourceDescriptorHeap[OutHistoryIrradianceIndex];
    RWTexture2D<uint4> OutHistorySH = ResourceDescriptorHeap[OutHistorySHIndex];

    const FPackedSh CenterSh = LoadPackedSh(TemporalSH, Pixel);
    const uint Count = HistoryCount[Pixel];
    const float CenterDepth = LinearDepthTexture[Pixel];
    if (CenterDepth <= 0.0f)
    {
        OutHistorySH[Pixel] = uint4(0u, 0u, 0u, 0u);
        OutHistoryIrradiance[Pixel] = 0.0f.xxx;
        return;
    }

    const float3 CenterNormal = DecodeNormalFromGBufferA(GBufferA[Pixel]);
    const float3 CenterWorldPos = ReconstructWorldPositionFromLinearDepth(Pixel, CenterDepth);
    float3 CenterTangent;
    float3 CenterBitangent;
    BuildOrthonormalBasis(CenterNormal, CenterTangent, CenterBitangent);
    const float CountRatio = saturate((float)Count / 32.0f);
    const float RadiusScale = lerp(2.0f, 8.0f, smoothstep(0.0f, 1.0f, 1.0f - CountRatio));
    const float2 BlurRadius = RadiusScale.xx / float2(Width, Height) * max(CenterDepth, 0.01f);
    const float Angle = Random01(Pixel, 617u) * 6.2831853f;
    const float S = sin(Angle);
    const float C = cos(Angle);
    const float2x2 Rotation = float2x2(C, -S, S, C);

    FPackedSh AccumSh = CenterSh;
    float Total = 1.0f;
    [unroll]
    for (uint SampleIndex = 0u; SampleIndex < RecurrentBlurSampleNum; ++SampleIndex)
    {
        const float2 Rotated = mul(Rotation, RecurrentBlurPoisson[SampleIndex]);
        const float2 SurfaceOffset = Rotated * BlurRadius;
        float3 SampleWorldPos = CenterWorldPos + CenterTangent * SurfaceOffset.x + CenterBitangent * SurfaceOffset.y;
        const float2 SampleUv = ProjectWorldPositionToUv(SampleWorldPos);
        if (any(SampleUv < 0.0f.xx) || any(SampleUv > 1.0f.xx))
        {
            continue;
        }

        const uint2 SamplePixel = min(uint2(SampleUv * float2(Width, Height)), uint2(Width - 1u, Height - 1u));
        const float SampleDepth = LinearDepthTexture[SamplePixel];
        if (SampleDepth <= 0.0f)
        {
            continue;
        }

        SampleWorldPos = ReconstructWorldPositionFromLinearDepth(SamplePixel, SampleDepth);
        const float3 SampleNormal = DecodeNormalFromGBufferA(GBufferA[SamplePixel]);
        const float GeometryWeight = ComputeSurfaceGeometryWeight(CenterWorldPos, CenterNormal, SampleWorldPos);
        const float NormalWeight = ComputeSurfaceNormalWeight(CenterNormal, SampleNormal);
        const float Weight = smoothstep(0.0f, 1.0f, GeometryWeight * NormalWeight);
        AccumSh = AddSh(AccumSh, ScaleSh(LoadPackedSh(TemporalSH, SamplePixel), Weight));
        Total += Weight;
    }

    FPackedSh OutSh = ScaleSh(AccumSh, rcp(max(1e-5f, Total)));
    OutHistoryIrradiance[Pixel] = UnprojectIrradiance(OutSh, CenterNormal);
    OutHistorySH[Pixel] = PackSh(OutSh);
}
