#include "SceneConstants.hlsl"

cbuffer ObjectIdPassConstants : register(b1)
{
    uint ObjectId;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float4 Tangent : TANGENT;
    float4 Color : COLOR;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
};

VSOutput ObjectIdVS(VSInput Input)
{
    VSOutput Output;
    float4 WorldPosition = mul(float4(Input.Position, 1.0f), World);
    float4 ViewPosition = mul(WorldPosition, View);
    Output.Position = mul(ViewPosition, Projection);
    return Output;
}

uint ObjectIdPS(VSOutput Input) : SV_Target0
{
    return ObjectId;
}
