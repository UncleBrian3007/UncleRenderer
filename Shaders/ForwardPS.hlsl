#include "PBRCommon.hlsl"

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
    float4 BaseColorTransformOffsetScale;
    float4 BaseColorTransformRotation;
    float4 MetallicRoughnessTransformOffsetScale;
    float4 MetallicRoughnessTransformRotation;
    float4 NormalTransformOffsetScale;
    float4 NormalTransformRotation;
    float4 EmissiveTransformOffsetScale;
    float4 EmissiveTransformRotation;
};

Texture2D AlbedoTexture : register(t0);
Texture2D EmissiveTexture : register(t1);
Texture2D ShadowMap : register(t2);
SamplerState AlbedoSampler : register(s0);
SamplerState ShadowSampler : register(s1);

float2 ApplyTextureTransform(float2 uv, float4 offsetScale, float4 rotation)
{
    float2 scaled = uv * offsetScale.zw;
    float2 rotated = float2(
        scaled.x * rotation.x - scaled.y * rotation.y,
        scaled.x * rotation.y + scaled.y * rotation.x);
    return rotated + offsetScale.xy;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float2 baseUV = ApplyTextureTransform(Input.UV, BaseColorTransformOffsetScale, BaseColorTransformRotation);
    float2 emissiveUV = ApplyTextureTransform(Input.UV, EmissiveTransformOffsetScale, EmissiveTransformRotation);

    float3 albedo = AlbedoTexture.Sample(AlbedoSampler, baseUV).rgb * BaseColor * Input.Color.rgb;
    float3 emissive = EmissiveTexture.Sample(AlbedoSampler, emissiveUV).rgb * EmissiveFactor;
    float3 n = normalize(Input.Normal);
    float3 v = normalize(CameraPosition - Input.WorldPos);
    float3 l = normalize(LightDirection);

    float metallic = 0.0f;
    float roughness = 0.5f;
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    float4 shadowPosition = mul(float4(Input.WorldPos, 1.0f), LightViewProjection);
    float3 shadowCoord = shadowPosition.xyz / shadowPosition.w;
    float2 shadowUV = shadowCoord.xy * float2(0.5f, -0.5f) + 0.5f;
    float shadowDepth = shadowCoord.z;
    float shadow = 1.0f;
    if (ShadowStrength > 0.0f && all(shadowUV >= 0.0f) && all(shadowUV <= 1.0f))
    {
        float shadowMapDepth = ShadowMap.Sample(ShadowSampler, shadowUV).r;
        shadow = shadowMapDepth + ShadowBias >= shadowDepth ? 1.0f : 0.0f;
        shadow = lerp(1.0f, shadow, ShadowStrength);
    }

    float3 lighting = EvaluatePBR(albedo, metallic, roughness, F0, n, v, l) * LightIntensity * LightColor * shadow;

    float3 ambient = albedo * 0.03f;
    float3 color = lighting + ambient + emissive;
    return float4(color, 1.0);
}
