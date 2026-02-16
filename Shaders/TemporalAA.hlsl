cbuffer TemporalAAConstants : register(b0)
{
    uint2 OutputSize;
    float HistoryWeight;
    uint UseHistory;
};

cbuffer TemporalAABindlessConstants : register(b1)
{
    uint CurrentTextureIndex;
    uint HistoryTextureIndex;
    uint OutputTextureIndex;
};

bool BadFloat3(float3 Value)
{
    return any(isnan(Value)) || any(isinf(Value));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= OutputSize.x || DispatchThreadId.y >= OutputSize.y)
    {
        return;
    }

    const int2 Pixel = int2(DispatchThreadId.xy);
    Texture2D<float4> CurrentTexture = ResourceDescriptorHeap[CurrentTextureIndex];
    Texture2D<float4> HistoryTexture = ResourceDescriptorHeap[HistoryTextureIndex];
    RWTexture2D<float4> OutputTexture = ResourceDescriptorHeap[OutputTextureIndex];
    float4 Current = CurrentTexture.Load(int3(Pixel, 0));
    if (BadFloat3(Current.rgb))
    {
		Current.rgb = float3(0, 0, 0);
	}
    
    if (UseHistory == 0)
    {
        OutputTexture[Pixel] = Current;
        return;
    }

    
    float3 MinColor = Current.rgb;
    float3 MaxColor = Current.rgb;

    [unroll]
    for (int OffsetY = -1; OffsetY <= 1; ++OffsetY)
    {
        [unroll]
        for (int OffsetX = -1; OffsetX <= 1; ++OffsetX)
        {
            int2 SampleCoord = clamp(Pixel + int2(OffsetX, OffsetY), int2(0, 0), int2(OutputSize) - 1);
            float3 SampleColor = CurrentTexture.Load(int3(SampleCoord, 0)).rgb;
            MinColor = min(MinColor, SampleColor);
            MaxColor = max(MaxColor, SampleColor);
        }
    }

    float3 History = HistoryTexture.Load(int3(Pixel, 0)).rgb;
    History = clamp(History, MinColor, MaxColor);
    if (BadFloat3(History))
	{
		OutputTexture[Pixel] = Current;
        return;
	}

    float3 Blended = lerp(Current.rgb, History, saturate(HistoryWeight));
	if (BadFloat3(Blended))
	{
		Blended = Current.rgb;
	}
    
    OutputTexture[Pixel] = float4(Blended, Current.a);
}
