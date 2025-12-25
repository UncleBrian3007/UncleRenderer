cbuffer HZBConstants : register(b0)
{
    uint SourceWidth;
    uint SourceHeight;
    uint DestWidth;
    uint DestHeight;
    uint DestWidth1;
    uint DestHeight1;
    uint SourceMip;
    uint HasSecondMip;
};

Texture2D<float> SourceTexture : register(t0);
RWTexture2D<float> DestTexture0 : register(u0);
RWTexture2D<float> DestTexture1 : register(u1);

groupshared float SharedDepth[8][8];

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
    float maxDepth = 0.0f;
    if (dispatchThreadId.x < DestWidth && dispatchThreadId.y < DestHeight)
    {
        const uint2 baseCoord = dispatchThreadId.xy * 2;

        const float d0 = SampleDepth(baseCoord);
        const float d1 = SampleDepth(baseCoord + uint2(1, 0));
        const float d2 = SampleDepth(baseCoord + uint2(0, 1));
        const float d3 = SampleDepth(baseCoord + uint2(1, 1));

        maxDepth = max(max(d0, d1), max(d2, d3));
        DestTexture0[dispatchThreadId.xy] = maxDepth;
    }

    SharedDepth[groupThreadId.y][groupThreadId.x] = maxDepth;
    GroupMemoryBarrierWithGroupSync();

    if (HasSecondMip == 0)
    {
        return;
    }

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
            DestTexture1[destCoord1] = max(max(s0, s1), max(s2, s3));
        }
    }
}
