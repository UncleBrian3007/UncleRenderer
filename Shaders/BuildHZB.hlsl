#ifndef HZB_MIPS_PER_DISPATCH
#define HZB_MIPS_PER_DISPATCH 1
#endif

cbuffer HZBConstants : register(b0)
{
    uint SourceWidth;
    uint SourceHeight;
    uint DestWidth;
    uint DestHeight;
    uint DestWidth1;
    uint DestHeight1;
    uint DestWidth2;
    uint DestHeight2;
    uint DestWidth3;
    uint DestHeight3;
    uint SourceMip;
    uint SourceIsDepth;
};

cbuffer HZBBindlessConstants : register(b1)
{
    uint DepthTextureIndex;
    uint HZBTextureIndex;
    uint DestTexture0Index;
    uint DestTexture1Index;
    uint DestTexture2Index;
    uint DestTexture3Index;
};

groupshared float2 SharedDepth[8][8];
#if HZB_MIPS_PER_DISPATCH >= 2
groupshared float2 SharedDepth1[4][4];
#endif
#if HZB_MIPS_PER_DISPATCH >= 3
groupshared float2 SharedDepth2[2][2];
#endif

float2 SampleDepth(uint2 coord, Texture2D<float> DepthTexture, Texture2D<float2> HZBTexture)
{
    const uint x = min(coord.x, SourceWidth - 1);
    const uint y = min(coord.y, SourceHeight - 1);
    if (SourceIsDepth != 0)
    {
        float depth = DepthTexture.Load(int3(x, y, 0));
        return float2(depth, depth);
    }

    return HZBTexture.Load(int3(x, y, SourceMip));
}

[numthreads(8, 8, 1)]
void BuildHZB(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthTextureIndex];
    Texture2D<float2> HZBTexture = ResourceDescriptorHeap[HZBTextureIndex];
    RWTexture2D<float2> DestTexture0 = ResourceDescriptorHeap[DestTexture0Index];
    RWTexture2D<float2> DestTexture1 = ResourceDescriptorHeap[DestTexture1Index];
    RWTexture2D<float2> DestTexture2 = ResourceDescriptorHeap[DestTexture2Index];
    RWTexture2D<float2> DestTexture3 = ResourceDescriptorHeap[DestTexture3Index];

    float2 minMaxDepth = float2(1.0f, 0.0f);
    if (dispatchThreadId.x < DestWidth && dispatchThreadId.y < DestHeight)
    {
        const uint2 baseCoord = dispatchThreadId.xy * 2;

        const float2 d0 = SampleDepth(baseCoord, DepthTexture, HZBTexture);
        const float2 d1 = SampleDepth(baseCoord + uint2(1, 0), DepthTexture, HZBTexture);
        const float2 d2 = SampleDepth(baseCoord + uint2(0, 1), DepthTexture, HZBTexture);
        const float2 d3 = SampleDepth(baseCoord + uint2(1, 1), DepthTexture, HZBTexture);

        const float minDepth = min(min(d0.x, d1.x), min(d2.x, d3.x));
        const float maxDepth = max(max(d0.y, d1.y), max(d2.y, d3.y));
        minMaxDepth = float2(minDepth, maxDepth);
        DestTexture0[dispatchThreadId.xy] = minMaxDepth;
    }

    SharedDepth[groupThreadId.y][groupThreadId.x] = minMaxDepth;
    GroupMemoryBarrierWithGroupSync();

#if HZB_MIPS_PER_DISPATCH >= 2
    if (groupThreadId.x < 4 && groupThreadId.y < 4)
    {
        const uint2 destCoord1 = groupId.xy * 4 + groupThreadId.xy;
        if (destCoord1.x < DestWidth1 && destCoord1.y < DestHeight1)
        {
            const uint2 base = groupThreadId.xy * 2;
            const float2 s0 = SharedDepth[base.y][base.x];
            const float2 s1 = SharedDepth[base.y][base.x + 1];
            const float2 s2 = SharedDepth[base.y + 1][base.x];
            const float2 s3 = SharedDepth[base.y + 1][base.x + 1];
            const float minDepth1 = min(min(s0.x, s1.x), min(s2.x, s3.x));
            const float maxDepth1 = max(max(s0.y, s1.y), max(s2.y, s3.y));
            const float2 minMaxDepth1 = float2(minDepth1, maxDepth1);
            DestTexture1[destCoord1] = minMaxDepth1;
            SharedDepth1[groupThreadId.y][groupThreadId.x] = minMaxDepth1;
        }
        else
        {
            SharedDepth1[groupThreadId.y][groupThreadId.x] = 0.0f;
        }
    }

#if HZB_MIPS_PER_DISPATCH >= 3
    GroupMemoryBarrierWithGroupSync();

    if (groupThreadId.x < 2 && groupThreadId.y < 2)
    {
        const uint2 destCoord2 = groupId.xy * 2 + groupThreadId.xy;
        if (destCoord2.x < DestWidth2 && destCoord2.y < DestHeight2)
        {
            const uint2 base = groupThreadId.xy * 2;
            const float2 s0 = SharedDepth1[base.y][base.x];
            const float2 s1 = SharedDepth1[base.y][base.x + 1];
            const float2 s2 = SharedDepth1[base.y + 1][base.x];
            const float2 s3 = SharedDepth1[base.y + 1][base.x + 1];
            const float minDepth2 = min(min(s0.x, s1.x), min(s2.x, s3.x));
            const float maxDepth2 = max(max(s0.y, s1.y), max(s2.y, s3.y));
            const float2 minMaxDepth2 = float2(minDepth2, maxDepth2);
            DestTexture2[destCoord2] = minMaxDepth2;
            SharedDepth2[groupThreadId.y][groupThreadId.x] = minMaxDepth2;
        }
        else
        {
            SharedDepth2[groupThreadId.y][groupThreadId.x] = 0.0f;
        }
    }

#if HZB_MIPS_PER_DISPATCH >= 4
    GroupMemoryBarrierWithGroupSync();

    if (groupThreadId.x == 0 && groupThreadId.y == 0)
    {
        const uint2 destCoord3 = groupId.xy;
        if (destCoord3.x < DestWidth3 && destCoord3.y < DestHeight3)
        {
            const float2 s0 = SharedDepth2[0][0];
            const float2 s1 = SharedDepth2[0][1];
            const float2 s2 = SharedDepth2[1][0];
            const float2 s3 = SharedDepth2[1][1];
            const float minDepth3 = min(min(s0.x, s1.x), min(s2.x, s3.x));
            const float maxDepth3 = max(max(s0.y, s1.y), max(s2.y, s3.y));
            DestTexture3[destCoord3] = float2(minDepth3, maxDepth3);
        }
    }
#endif
#endif
#endif
}
