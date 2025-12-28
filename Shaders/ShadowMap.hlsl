#include "SceneConstants.hlsl"

struct VSInput
{
    float3 Position : POSITION;
};


struct VSOutput
{
    float4 Position : SV_Position;
};

VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    float4 WorldPos = mul(float4(Input.Position, 1.0), World);
    Output.Position = mul(WorldPos, LightViewProjection);
    return Output;
}
