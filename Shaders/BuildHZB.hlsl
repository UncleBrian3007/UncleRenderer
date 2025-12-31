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
};

Texture2D<float> SourceTexture : register(t0);
RWTexture2D<float> DestTexture0 : register(u0);
RWTexture2D<float> DestTexture1 : register(u1);
RWTexture2D<float> DestTexture2 : register(u2);
RWTexture2D<float> DestTexture3 : register(u3);

groupshared float SharedDepth[8][8];
#if HZB_MIPS_PER_DISPATCH >= 2
groupshared float SharedDepth1[4][4];
#endif
#if HZB_MIPS_PER_DISPATCH >= 3
groupshared float SharedDepth2[2][2];
#endif

float SampleDepth(uint2 coord)
{
    const uint x = min(coord.x, SourceWidth - 1);
    const uint y = min(coord.y, SourceHeight - 1);
    return SourceTexture.Load(int3(x, y, SourceMip));
}

[numthreads(8, 8, 1)]
void BuildHZB(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupThreadId : SV_GroupThreadID,
    uint3 groupId : SV_GroupID)
{
    float minDepth = 1.0f;
    if (dispatchThreadId.x < DestWidth && dispatchThreadId.y < DestHeight)
    {
        const uint2 baseCoord = dispatchThreadId.xy * 2;

        const float d0 = SampleDepth(baseCoord);
        const float d1 = SampleDepth(baseCoord + uint2(1, 0));
        const float d2 = SampleDepth(baseCoord + uint2(0, 1));
        const float d3 = SampleDepth(baseCoord + uint2(1, 1));

        minDepth = min(min(d0, d1), min(d2, d3));
        DestTexture0[dispatchThreadId.xy] = minDepth;
    }

    SharedDepth[groupThreadId.y][groupThreadId.x] = minDepth;
    GroupMemoryBarrierWithGroupSync();

#if HZB_MIPS_PER_DISPATCH >= 2
    if (groupThreadId.x < 4 && groupThreadId.y < 4)
    {
        const uint2 destCoord1 = groupId.xy * 4 + groupThreadId.xy;
        if (destCoord1.x < DestWidth1 && destCoord1.y < DestHeight1)
        {
            const uint2 base = groupThreadId.xy * 2;
            const float s0 = SharedDepth[base.y][base.x];
            const float s1 = SharedDepth[base.y][base.x + 1];
            const float s2 = SharedDepth[base.y + 1][base.x];
            const float s3 = SharedDepth[base.y + 1][base.x + 1];
            const float minDepth1 = min(min(s0, s1), min(s2, s3));
            DestTexture1[destCoord1] = minDepth1;
            SharedDepth1[groupThreadId.y][groupThreadId.x] = minDepth1;
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
            const float s0 = SharedDepth1[base.y][base.x];
            const float s1 = SharedDepth1[base.y][base.x + 1];
            const float s2 = SharedDepth1[base.y + 1][base.x];
            const float s3 = SharedDepth1[base.y + 1][base.x + 1];
            const float minDepth2 = min(min(s0, s1), min(s2, s3));
            DestTexture2[destCoord2] = minDepth2;
            SharedDepth2[groupThreadId.y][groupThreadId.x] = minDepth2;
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
            const float s0 = SharedDepth2[0][0];
            const float s1 = SharedDepth2[0][1];
            const float s2 = SharedDepth2[1][0];
            const float s3 = SharedDepth2[1][1];
            DestTexture3[destCoord3] = min(min(s0, s1), min(s2, s3));
        }
    }
#endif
#endif
#endif
}
