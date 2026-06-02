#pragma once

struct FBlueNoiseSobolSampler
{
    uint2 Pixel;
    uint SampleIndex;
};

uint BlueNoiseSobolGetByte(float4 Value, uint Channel)
{
    const float Component = (Channel == 0u) ? Value.x : ((Channel == 1u) ? Value.y : ((Channel == 2u) ? Value.z : Value.w));
    return (uint)clamp(Component * 255.0f, 0.0f, 255.0f);
}

FBlueNoiseSobolSampler BlueNoiseSobolSamplerCreate(uint2 Pixel, uint2 ScreenSize, uint FrameNumber)
{
    FBlueNoiseSobolSampler Sampler;
    const uint2 Offset = (uint2)(float2(0.754877669f, 0.569840296f) * (float)FrameNumber * float2(ScreenSize));
    const uint2 OffsetPixel = Pixel + Offset;
    Sampler.Pixel = uint2(
        ScreenSize.x > 0u ? (OffsetPixel.x % ScreenSize.x) : 0u,
        ScreenSize.y > 0u ? (OffsetPixel.y % ScreenSize.y) : 0u);
    Sampler.SampleIndex = 0u;
    return Sampler;
}

float BlueNoiseSobolSamplerSample(in FBlueNoiseSobolSampler Sampler, uint SampleIndex, uint SampleDimension, uint SobolTextureIndex, uint ScramblingRankingTextureIndex)
{
    Texture2D<float4> SobolTexture = ResourceDescriptorHeap[SobolTextureIndex];
    Texture2D<float4> ScramblingRankingTexture = ResourceDescriptorHeap[ScramblingRankingTextureIndex];

    const uint PixelX = Sampler.Pixel.x % 128u;
    const uint PixelY = Sampler.Pixel.y % 128u;
    const uint WrappedSampleIndex = SampleIndex % 256u;
    const uint WrappedDimension = SampleDimension % 4u;

    const float4 RankingTexel = ScramblingRankingTexture[uint2(PixelX, PixelY)];
    const uint RankedSampleIndex = WrappedSampleIndex ^ BlueNoiseSobolGetByte(RankingTexel, 2u);

    const float4 SobolTexel = SobolTexture[uint2(RankedSampleIndex, 0u)];
    uint Value = BlueNoiseSobolGetByte(SobolTexel, WrappedDimension);
    Value ^= BlueNoiseSobolGetByte(RankingTexel, WrappedDimension % 2u);
    return (0.5f + Value) / 256.0f;
}

float BlueNoiseSobolSamplerRandomFloat(inout FBlueNoiseSobolSampler Sampler, uint SobolTextureIndex, uint ScramblingRankingTextureIndex)
{
    const float Result = BlueNoiseSobolSamplerSample(Sampler, Sampler.SampleIndex, 0u, SobolTextureIndex, ScramblingRankingTextureIndex);
    Sampler.SampleIndex++;
    return Result;
}

float2 BlueNoiseSobolSamplerRandomFloat2(inout FBlueNoiseSobolSampler Sampler, uint SobolTextureIndex, uint ScramblingRankingTextureIndex)
{
    const float2 Result = float2(
        BlueNoiseSobolSamplerSample(Sampler, Sampler.SampleIndex, 0u, SobolTextureIndex, ScramblingRankingTextureIndex),
        BlueNoiseSobolSamplerSample(Sampler, Sampler.SampleIndex, 1u, SobolTextureIndex, ScramblingRankingTextureIndex));
    Sampler.SampleIndex++;
    return Result;
}
