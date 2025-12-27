struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float4 Tangent  : TANGENT;
    float4 Color    : COLOR0;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float4 Tangent  : TEXCOORD2;
    float4 Color    : COLOR0;
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
    float MetallicFactor;
    float RoughnessFactor;
    float2 PaddingMaterial;
    float4 BaseColorTransformOffsetScale;
    float4 BaseColorTransformRotation;
    float4 MetallicRoughnessTransformOffsetScale;
    float4 MetallicRoughnessTransformRotation;
    float4 NormalTransformOffsetScale;
    float4 NormalTransformRotation;
    float4 EmissiveTransformOffsetScale;
    float4 EmissiveTransformRotation;
    float EnvMapMipCount;
    float3 PaddingEnvMap;
};

VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    float4 WorldPos = mul(float4(Input.Position, 1.0), World);
    float4 ViewPos = mul(WorldPos, View);
    Output.Position = mul(ViewPos, Projection);
    Output.Normal = mul(Input.Normal, (float3x3)World);
    Output.UV = Input.UV;
    Output.WorldPos = WorldPos.xyz;
    Output.Tangent = float4(normalize(mul(Input.Tangent.xyz, (float3x3)World)), Input.Tangent.w);
    Output.Color = Input.Color;
    return Output;
}
