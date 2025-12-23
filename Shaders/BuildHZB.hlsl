cbuffer HZBConstants : register(b0)
{
    uint SourceWidth;
    uint SourceHeight;
    uint DestWidth;
    uint DestHeight;
    uint SourceMip;
};

Texture2D<float> SourceTexture : register(t0);
RWTexture2D<float> DestTexture : register(u0);

float SampleDepth(uint2 coord)
{
    const uint x = min(coord.x, SourceWidth - 1);
    const uint y = min(coord.y, SourceHeight - 1);
    return SourceTexture.Load(int3(x, y, SourceMip));
}

[numthreads(8, 8, 1)]
void BuildHZB(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DestWidth || dispatchThreadId.y >= DestHeight)
    {
        return;
    }

    const uint2 baseCoord = dispatchThreadId.xy * 2;

    const float d0 = SampleDepth(baseCoord);
    const float d1 = SampleDepth(baseCoord + uint2(1, 0));
    const float d2 = SampleDepth(baseCoord + uint2(0, 1));
    const float d3 = SampleDepth(baseCoord + uint2(1, 1));

    const float maxDepth = max(max(d0, d1), max(d2, d3));
    DestTexture[dispatchThreadId.xy] = maxDepth;
}
