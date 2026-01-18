#include "SceneConstants.hlsl"

#ifndef USE_SKINNING
#define USE_SKINNING 0
#endif

struct VSInput
{
    float3 Position : POSITION;
};


struct VSOutput
{
    float4 Position : SV_Position;
};

float4 GetWorldPosition(float3 position)
{
#if USE_SKINNING
    return mul(float4(position, 1.0), World);
#else
    return mul(float4(position, 1.0), World);
#endif
}

VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    float4 WorldPos = GetWorldPosition(Input.Position);
    Output.Position = mul(WorldPos, LightViewProjection);
    return Output;
}
