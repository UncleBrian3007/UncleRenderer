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
void PathTracingAccumulationCS(uint3 DispatchThreadId : SV_DispatchThreadID)
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

    if (UseHistory == 0 || FrameIndex == 0)
    {
        AccumulationOutput[pixel] = current;
        LightingOutput[pixel] = current;
        return;
    }

    float4 history = AccumulationHistory.Load(int3(pixel, 0));

/*    
	float3 h = history.xyz;
	float3 c = current.xyz;
    
    bool badRange = any(h < 1e-3f) || any(c < 1e-3f);
    
    if (BadFloat4(history) || BadFloat4(current) || badRange)
    {
        AccumulationOutput[pixel] = current;
        LightingOutput[pixel] = current;
        return;
	}
*/
//	static const float MAX_ACCUMULATION_SAMPLES = 60*5.0f;
//	float sampleCount = min(float(FrameIndex) + 1.0f, MAX_ACCUMULATION_SAMPLES);
	float sampleCount = float(FrameIndex) + 1.0f;
    float weight = 1.0f / float(sampleCount);
    float4 accumulated = history + (current - history) * weight;
    
    AccumulationOutput[pixel] = accumulated;
    LightingOutput[pixel] = accumulated;
}
