#include "PBRCommon.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float4 Tangent  : TEXCOORD2;
};

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 View;
    row_major float4x4 Projection;
    float3 BaseColor;
    float LightIntensity;
    float3 LightDirection;
    float Padding1;
    float3 CameraPosition;
    float Padding2;
};

Texture2D AlbedoTexture : register(t0);
SamplerState AlbedoSampler : register(s0);

float4 PSMain(VSOutput Input) : SV_Target
{
    float3 albedo = AlbedoTexture.Sample(AlbedoSampler, Input.UV).rgb * BaseColor;
    float3 n = normalize(Input.Normal);
    float3 v = normalize(CameraPosition - Input.WorldPos);
    float3 l = normalize(-LightDirection);

    float metallic = 0.0f;
    float roughness = 0.5f;
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    float3 lighting = EvaluatePBR(albedo, metallic, roughness, F0, n, v, l) * LightIntensity;

    float3 ambient = albedo * 0.03f;
    float3 color = lighting + ambient;
    return float4(color, 1.0);
}
