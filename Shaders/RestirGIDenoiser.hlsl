#include "SceneConstants.hlsl"
#include "RestirGISh.hlsli"

cbuffer RestirGiDenoiserConstants : register(b0)
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

cbuffer RestirGiDenoiserBindless : register(b1)
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
    uint Reserved0;
};

float3 DecodeNormalFromGBufferA(float4 GBufferA)
{
    return normalize(GBufferA.xyz * 2.0f - 1.0f);
}

FRestirGiPackedSh LoadPackedSh(Texture2D<uint4> Texture, uint2 Pixel)
{
    return RestirGiUnpackSh(Texture[Pixel]);
}

FRestirGiPackedSh LoadPackedSh(RWTexture2D<uint4> Texture, uint2 Pixel)
{
    return RestirGiUnpackSh(Texture[Pixel]);
}

uint4 PackSh(FRestirGiPackedSh Sh)
{
    return RestirGiPackSh(Sh);
}

float ComputeGeometryWeight(float CenterDepth, float SampleDepth, float3 CenterNormal, float3 SampleNormal)
{
    const float DepthWeight = exp(-abs(CenterDepth - SampleDepth) * 20.0f);
    const float NormalWeight = saturate(dot(CenterNormal, SampleNormal));
    return DepthWeight * NormalWeight;
}

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
    RWTexture2D<uint4> TemporalSH = ResourceDescriptorHeap[TemporalSHIndex];

    const FRestirGiPackedSh CenterSh = LoadPackedSh(InputSH, Pixel);
    const float3 CenterIrradiance = CenterSh.Irradiance;
    const float CenterDepth = LinearDepthTexture[Pixel];
    const float3 CenterNormal = DecodeNormalFromGBufferA(GBufferA[Pixel]);

    float3 Accum = CenterIrradiance;
    float TotalWeight = 1.0f;

    const float RadiusScale = 1.0f + VarianceTexture[Pixel] * 2.0f;
    const int Radius = (int)round(clamp(RadiusScale, 1.0f, 3.0f));

    [loop]
    for (int y = -Radius; y <= Radius; ++y)
    {
        [loop]
        for (int x = -Radius; x <= Radius; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            const int2 SamplePixelI = clamp(int2(Pixel) + int2(x, y), int2(0, 0), int2((int)Width - 1, (int)Height - 1));
            const uint2 SamplePixel = uint2(SamplePixelI);
            const float3 SampleIrradiance = LoadPackedSh(InputSH, SamplePixel).Irradiance;
            const float SampleDepth = LinearDepthTexture[SamplePixel];
            const float3 SampleNormal = DecodeNormalFromGBufferA(GBufferA[SamplePixel]);
            const float Weight = ComputeGeometryWeight(CenterDepth, SampleDepth, CenterNormal, SampleNormal);
            Accum += SampleIrradiance * Weight;
            TotalWeight += Weight;
        }
    }

    const float3 Filtered = Accum / max(1e-5f, TotalWeight);
    FRestirGiPackedSh OutSh = CenterSh;
    OutSh.Irradiance = Filtered;
    TemporalSH[Pixel] = PackSh(OutSh);
}

[numthreads(8, 8, 1)]
void CSTemporalAccumulation(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= Width || Pixel.y >= Height)
    {
        return;
    }

    Texture2D<uint4> TemporalSH = ResourceDescriptorHeap[TemporalSHIndex];
    Texture2D<float2> VelocityTexture = ResourceDescriptorHeap[VelocityIndex];
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

    FRestirGiPackedSh CurrentSh = LoadPackedSh(TemporalSH, Pixel);
    const float3 CurrentIrradiance = CurrentSh.Irradiance;
    const float2 Uv = (float2(Pixel) + 0.5f) / float2(Width, Height);
    const float2 VelocityNdc = VelocityTexture[Pixel];
    const float2 PrevUv = float2(Uv.x - VelocityNdc.x * 0.5f, Uv.y + VelocityNdc.y * 0.5f);

    bool bHistoryAccepted = HistoryValid != 0u;
    uint2 PrevPixel = Pixel;
    if (any(PrevUv <= 0.0f.xx) || any(PrevUv >= 1.0f.xx))
    {
        bHistoryAccepted = false;
    }
    else
    {
        PrevPixel = min(uint2(PrevUv * float2(Width, Height)), uint2(Width - 1u, Height - 1u));
    }

    const float3 CurrentNormal = DecodeNormalFromGBufferA(GBufferA[Pixel]);
    const float CurrentLinearDepth = CurrentLinearDepthTexture[Pixel];
    if (bHistoryAccepted)
    {
        const float PrevLinearDepth = PrevLinearDepthTexture[PrevPixel];
        const float3 PrevNormal = normalize(PrevNormalTexture[PrevPixel].xyz * 2.0f - 1.0f);
        const bool bDepthAccepted = CurrentLinearDepth <= PrevLinearDepth * DepthThresholdScale;
        const bool bNormalAccepted = dot(CurrentNormal, PrevNormal) > NormalThreshold;
        bHistoryAccepted = bDepthAccepted && bNormalAccepted;
    }

    float3 TemporalIrradiance = CurrentIrradiance;
    uint NextCount = 1u;
    if (bHistoryAccepted)
    {
        const FRestirGiPackedSh PrevSh = LoadPackedSh(HistorySH, PrevPixel);
        const float3 PrevIrradiance = PrevSh.Irradiance;
        const uint PrevCount = min(HistoryCount[PrevPixel], 31u);
        const float HistoryWeight = PrevCount / max(1.0f, PrevCount + 1.0f);
        TemporalIrradiance = lerp(CurrentIrradiance, PrevIrradiance, saturate(HistoryWeight * BlendStrength));
        CurrentSh.DominantDirection = normalize(lerp(CurrentSh.DominantDirection, PrevSh.DominantDirection, saturate(HistoryWeight)));
        NextCount = min(PrevCount + 1u, 32u);
    }

    CurrentSh.Irradiance = TemporalIrradiance;
    OutTemporalSH[Pixel] = PackSh(CurrentSh);
    OutHistoryCount[Pixel] = NextCount;
    OutPrevLinearDepth[Pixel] = CurrentLinearDepth;
    OutPrevNormal[Pixel] = float4(CurrentNormal * 0.5f + 0.5f, 1.0f);
}

[numthreads(8, 8, 1)]
void CSGenerateShMips(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= Width || Pixel.y >= Height)
    {
        return;
    }

    Texture2D<uint4> SourceSH = ResourceDescriptorHeap[TemporalSHIndex];
    RWTexture2D<uint4> OutShMip = ResourceDescriptorHeap[Reserved0];

    uint OutWidth = 0;
    uint OutHeight = 0;
    OutShMip.GetDimensions(OutWidth, OutHeight);
    if (Pixel.x >= OutWidth || Pixel.y >= OutHeight)
    {
        return;
    }

    const uint Scale = 1u << (MipLevel + 1u);
    const uint2 Base = Pixel * Scale;
    const uint2 S0 = min(Base, uint2(Width - 1u, Height - 1u));
    const uint2 S1 = min(Base + uint2(Scale, 0u), uint2(Width - 1u, Height - 1u));
    const uint2 S2 = min(Base + uint2(0u, Scale), uint2(Width - 1u, Height - 1u));
    const uint2 S3 = min(Base + uint2(Scale, Scale), uint2(Width - 1u, Height - 1u));

    FRestirGiPackedSh Sh0 = LoadPackedSh(SourceSH, S0);
    FRestirGiPackedSh Sh1 = LoadPackedSh(SourceSH, S1);
    FRestirGiPackedSh Sh2 = LoadPackedSh(SourceSH, S2);
    FRestirGiPackedSh Sh3 = LoadPackedSh(SourceSH, S3);

    FRestirGiPackedSh OutSh;
    OutSh.Irradiance = (Sh0.Irradiance + Sh1.Irradiance + Sh2.Irradiance + Sh3.Irradiance) * 0.25f;
    OutSh.DominantDirection = normalize(Sh0.DominantDirection + Sh1.DominantDirection + Sh2.DominantDirection + Sh3.DominantDirection);
    OutShMip[Pixel] = PackSh(OutSh);
}

[numthreads(8, 8, 1)]
void CSGenerateLinearDepthMips(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    const uint2 Pixel = DispatchThreadId.xy;
    if (Pixel.x >= Width || Pixel.y >= Height)
    {
        return;
    }

    Texture2D<float> CurrentLinearDepthTexture = ResourceDescriptorHeap[CurrentLinearDepthIndex];
    RWTexture2D<float> OutDepthMip = ResourceDescriptorHeap[Reserved0];

    uint OutWidth = 0;
    uint OutHeight = 0;
    OutDepthMip.GetDimensions(OutWidth, OutHeight);
    if (Pixel.x >= OutWidth || Pixel.y >= OutHeight)
    {
        return;
    }

    const uint Scale = 1u << (MipLevel + 1u);
    const uint2 Base = Pixel * Scale;
    const uint2 S0 = min(Base, uint2(Width - 1u, Height - 1u));
    const uint2 S1 = min(Base + uint2(Scale, 0u), uint2(Width - 1u, Height - 1u));
    const uint2 S2 = min(Base + uint2(0u, Scale), uint2(Width - 1u, Height - 1u));
    const uint2 S3 = min(Base + uint2(Scale, Scale), uint2(Width - 1u, Height - 1u));
    OutDepthMip[Pixel] = (CurrentLinearDepthTexture[S0] + CurrentLinearDepthTexture[S1] + CurrentLinearDepthTexture[S2] + CurrentLinearDepthTexture[S3]) * 0.25f;
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
    Texture2D<uint4> ShMip = ResourceDescriptorHeap[Reserved0];
    Texture2D<float> DepthMip = ResourceDescriptorHeap[OutPrevLinearDepthIndex];
    Texture2D<float> CurrentLinearDepthTexture = ResourceDescriptorHeap[CurrentLinearDepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<uint> HistoryCount = ResourceDescriptorHeap[HistoryCountIndex];
    RWTexture2D<uint4> TemporalSH = ResourceDescriptorHeap[TemporalSHIndex];

    const uint Count = HistoryCount[Pixel];
    if (Count >= 10u)
    {
        if (MipLevel == 0u)
        {
            TemporalSH[Pixel] = HistorySH[Pixel];
        }
        return;
    }

    const float CountRatio = saturate((float)Count / 10.0f);
    const uint ComputedMip = (uint)round(3.0f * (1.0f - CountRatio));
    if (ComputedMip != MipLevel)
    {
        return;
    }

    uint MipWidth = 0;
    uint MipHeight = 0;
    ShMip.GetDimensions(MipWidth, MipHeight);

    const float2 FullUv = (float2(Pixel) + 0.5f) / float2(Width, Height);
    const float2 MipCoord = FullUv * float2(MipWidth, MipHeight) - 0.5f;
    const int2 MipBase = int2(floor(MipCoord));
    const float2 Frac = frac(MipCoord);

    const int2 Tap00 = clamp(MipBase + int2(0, 0), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));
    const int2 Tap10 = clamp(MipBase + int2(1, 0), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));
    const int2 Tap01 = clamp(MipBase + int2(0, 1), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));
    const int2 Tap11 = clamp(MipBase + int2(1, 1), int2(0, 0), int2((int)MipWidth - 1, (int)MipHeight - 1));

    const float CurrentDepth = CurrentLinearDepthTexture[Pixel];
    const float3 CurrentNormal = DecodeNormalFromGBufferA(GBufferA[Pixel]);

    const float W00 = (1.0f - Frac.x) * (1.0f - Frac.y);
    const float W10 = Frac.x * (1.0f - Frac.y);
    const float W01 = (1.0f - Frac.x) * Frac.y;
    const float W11 = Frac.x * Frac.y;

    float3 Accum = 0.0f.xxx;
    float Total = 0.0f;

    const float D00 = DepthMip[uint2(Tap00)];
    const float D10 = DepthMip[uint2(Tap10)];
    const float D01 = DepthMip[uint2(Tap01)];
    const float D11 = DepthMip[uint2(Tap11)];

    const float3 N00 = normalize(LoadPackedSh(ShMip, uint2(Tap00)).DominantDirection);
    const float3 N10 = normalize(LoadPackedSh(ShMip, uint2(Tap10)).DominantDirection);
    const float3 N01 = normalize(LoadPackedSh(ShMip, uint2(Tap01)).DominantDirection);
    const float3 N11 = normalize(LoadPackedSh(ShMip, uint2(Tap11)).DominantDirection);

    const float4 BilinearWeights = float4(W00, W10, W01, W11);
    const float4 GeometryWeights = float4(
        ComputeGeometryWeight(CurrentDepth, D00, CurrentNormal, N00),
        ComputeGeometryWeight(CurrentDepth, D10, CurrentNormal, N10),
        ComputeGeometryWeight(CurrentDepth, D01, CurrentNormal, N01),
        ComputeGeometryWeight(CurrentDepth, D11, CurrentNormal, N11));

    const float4 FinalWeights = BilinearWeights * GeometryWeights;
    Accum += LoadPackedSh(ShMip, uint2(Tap00)).Irradiance * FinalWeights.x;
    Accum += LoadPackedSh(ShMip, uint2(Tap10)).Irradiance * FinalWeights.y;
    Accum += LoadPackedSh(ShMip, uint2(Tap01)).Irradiance * FinalWeights.z;
    Accum += LoadPackedSh(ShMip, uint2(Tap11)).Irradiance * FinalWeights.w;
    Total += FinalWeights.x + FinalWeights.y + FinalWeights.z + FinalWeights.w;

    FRestirGiPackedSh Reconstructed = LoadPackedSh(TemporalSH, Pixel);
    Reconstructed.Irradiance = Accum / max(1e-5f, Total);
    TemporalSH[Pixel] = PackSh(Reconstructed);
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
    Texture2D<uint> HistoryCount = ResourceDescriptorHeap[OutHistoryCountIndex];
    Texture2D<float> LinearDepthTexture = ResourceDescriptorHeap[CurrentLinearDepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];

    RWTexture2D<float3> OutHistoryIrradiance = ResourceDescriptorHeap[OutHistoryIrradianceIndex];
    RWTexture2D<uint4> OutHistorySH = ResourceDescriptorHeap[OutHistorySHIndex];

    const FRestirGiPackedSh CenterSh = LoadPackedSh(TemporalSH, Pixel);
    const float3 CenterIrradiance = CenterSh.Irradiance;
    const uint Count = HistoryCount[Pixel];
    const int Radius = (Count < 4u) ? 2 : 1;
    const float CenterDepth = LinearDepthTexture[Pixel];
    const float3 CenterNormal = DecodeNormalFromGBufferA(GBufferA[Pixel]);

    float3 Accum = CenterIrradiance;
    float Total = 1.0f;
    [loop]
    for (int y = -Radius; y <= Radius; ++y)
    {
        [loop]
        for (int x = -Radius; x <= Radius; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }

            const int2 SampleI = clamp(int2(Pixel) + int2(x, y), int2(0, 0), int2((int)Width - 1, (int)Height - 1));
            const uint2 SamplePixel = uint2(SampleI);
            const float3 SampleIrradiance = LoadPackedSh(TemporalSH, SamplePixel).Irradiance;
            const float SampleDepth = LinearDepthTexture[SamplePixel];
            const float3 SampleNormal = DecodeNormalFromGBufferA(GBufferA[SamplePixel]);
            const float Weight = ComputeGeometryWeight(CenterDepth, SampleDepth, CenterNormal, SampleNormal);
            Accum += SampleIrradiance * Weight;
            Total += Weight;
        }
    }

    FRestirGiPackedSh OutSh = CenterSh;
    OutSh.Irradiance = max(Accum / max(1e-5f, Total), 0.0f.xxx);
    OutHistoryIrradiance[Pixel] = RestirGiUnprojectIrradiance(OutSh, CenterNormal);
    OutHistorySH[Pixel] = PackSh(OutSh);
}
