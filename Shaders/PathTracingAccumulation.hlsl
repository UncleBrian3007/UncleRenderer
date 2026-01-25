cbuffer PathTracingAccumulationConstants : register(b0)
{
    uint2 OutputSize;
    uint FrameIndex;
    uint UseHistory;
};

cbuffer PathTracingAccumulationBindlessConstants : register(b1)
{
    uint PathTracingTempTextureIndex;
    uint AccumulationHistoryTextureIndex;
    uint AccumulationOutputTextureIndex;
    uint LightingOutputTextureIndex;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= OutputSize.x || DispatchThreadId.y >= OutputSize.y)
    {
        return;
    }

    const int2 pixel = int2(DispatchThreadId.xy);
    Texture2D<float4> PathTracingTemp = ResourceDescriptorHeap[PathTracingTempTextureIndex];
    Texture2D<float4> AccumulationHistory = ResourceDescriptorHeap[AccumulationHistoryTextureIndex];
    RWTexture2D<float4> AccumulationOutput = ResourceDescriptorHeap[AccumulationOutputTextureIndex];
    RWTexture2D<float4> LightingOutput = ResourceDescriptorHeap[LightingOutputTextureIndex];
    
    float4 current = PathTracingTemp.Load(int3(pixel, 0));

    if (UseHistory == 0)
    {
        AccumulationOutput[pixel] = current;
        LightingOutput[pixel] = current;
        return;
    }

    float4 history = AccumulationHistory.Load(int3(pixel, 0));

	static const float MAX_ACCUMULATION_SAMPLES = 1000.0f;
	float SampleCount = min(float(FrameIndex), MAX_ACCUMULATION_SAMPLES);
    float weight = 1.0f / float(SampleCount + 1.0f);
    float4 accumulated = history + (current - history) * weight;
    
    AccumulationOutput[pixel] = accumulated;
    LightingOutput[pixel] = accumulated;
}
