struct VSInput
{
    float3 Position : POSITION;
};

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 View;
    row_major float4x4 ViewInverse;
    row_major float4x4 Projection;
    float3 BaseColor;
    float LightIntensity;
    float3 LightDirection;
    float Padding1;
    float3 CameraPosition;
    float Padding2;
    float3 LightColor;
    float Padding3;
    float3 EmissiveFactor;
    float Padding4;
    row_major float4x4 LightViewProjection;
    float ShadowStrength;
    float ShadowBias;
    float2 PaddingShadow;
    float4 BaseColorTransformOffsetScale;
    float4 BaseColorTransformRotation;
    float4 MetallicRoughnessTransformOffsetScale;
    float4 MetallicRoughnessTransformRotation;
    float4 NormalTransformOffsetScale;
    float4 NormalTransformRotation;
    float4 EmissiveTransformOffsetScale;
    float4 EmissiveTransformRotation;
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
