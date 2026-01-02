#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float4 Tangent  : TEXCOORD2;
    float4 Color    : COLOR0;
};

#ifndef USE_BASE_COLOR_MAP
#define USE_BASE_COLOR_MAP 1
#endif
#ifndef USE_METALLIC_ROUGHNESS_MAP
#define USE_METALLIC_ROUGHNESS_MAP 1
#endif
#ifndef USE_EMISSIVE_MAP
#define USE_EMISSIVE_MAP 1
#endif
#ifndef USE_NORMAL_MAP
#define USE_NORMAL_MAP 1
#endif
#ifndef USE_ALPHA_MASK
#define USE_ALPHA_MASK 0
#endif


Texture2D AlbedoTexture : register(t0);
Texture2D MetallicRoughnessTexture : register(t1);
Texture2D NormalTexture : register(t2);
Texture2D EmissiveTexture : register(t3);
Texture2D ShadowMap : register(t4);
TextureCube EnvironmentMap : register(t5);
Texture2D BrdfLut : register(t6);
SamplerState AlbedoSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
SamplerState IblSampler : register(s2);

float2 ApplyTextureTransform(float2 uv, float4 offsetScale, float4 rotation)
{
    float2 scaled = uv * offsetScale.zw;
    float2 rotated = float2(
        scaled.x * rotation.x - scaled.y * rotation.y,
        scaled.x * rotation.y + scaled.y * rotation.x);
    return rotated + offsetScale.xy;
}

float3 ComputeWorldNormal(VSOutput Input, float2 normalUV)
{
    float3 vertexNormal = normalize(Input.Normal);

#if USE_NORMAL_MAP
    float3 tangent = normalize(Input.Tangent.xyz - vertexNormal * dot(vertexNormal, Input.Tangent.xyz));
    float3 bitangent = normalize(cross(vertexNormal, tangent)) * Input.Tangent.w;

    float3 tangentNormal = NormalTexture.Sample(AlbedoSampler, normalUV).rgb * 2.0f - 1.0f;
    const float tangentEpsilon = 1e-5f;
    float tangentNormalLength = length(tangentNormal);
    tangentNormal = tangentNormalLength < tangentEpsilon ? float3(0.0f, 0.0f, 1.0f) : tangentNormal;

    float3x3 TBN = float3x3(tangent, bitangent, vertexNormal);
    float3 worldNormal = mul(tangentNormal, TBN);

    return normalize(worldNormal);
#else
    return vertexNormal;
#endif
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float2 baseUV = ApplyTextureTransform(Input.UV, BaseColorTransformOffsetScale, BaseColorTransformRotation);
    float2 normalUV = ApplyTextureTransform(Input.UV, NormalTransformOffsetScale, NormalTransformRotation);
    float2 emissiveUV = ApplyTextureTransform(Input.UV, EmissiveTransformOffsetScale, EmissiveTransformRotation);

    float3 albedo = BaseColor * Input.Color.rgb;
    float alpha = BaseColorAlpha * Input.Color.a;
#if USE_BASE_COLOR_MAP
    float4 albedoSample = AlbedoTexture.Sample(AlbedoSampler, baseUV);
    albedo *= albedoSample.rgb;
    alpha *= albedoSample.a;
#endif
#if USE_ALPHA_MASK
    if (alpha < AlphaCutoff)
    {
        clip(alpha - AlphaCutoff);
    }
#endif
    float3 emissive = EmissiveFactor;
#if USE_EMISSIVE_MAP
    emissive *= EmissiveTexture.Sample(AlbedoSampler, emissiveUV).rgb;
#endif
    float3 n = ComputeWorldNormal(Input, normalUV);
    float3 v = normalize(CameraPosition - Input.WorldPos);
    float3 l = normalize(LightDirection);

    float metallic = MetallicFactor;
    float roughness = RoughnessFactor;
#if USE_METALLIC_ROUGHNESS_MAP
    float2 metallicRoughness = MetallicRoughnessTexture.Sample(AlbedoSampler, baseUV).bg;
    metallic *= metallicRoughness.x;
    roughness *= metallicRoughness.y;
#endif
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    float4 shadowPosition = mul(float4(Input.WorldPos, 1.0f), LightViewProjection);
    float3 shadowCoord = shadowPosition.xyz / shadowPosition.w;
    float2 shadowUV = shadowCoord.xy * float2(0.5f, -0.5f) + 0.5f;
    float shadowDepth = shadowCoord.z;
    float shadow = 1.0f;
    if (ShadowStrength > 0.0f && all(shadowUV >= 0.0f) && all(shadowUV <= 1.0f))
    {
        float2 halfTexel = 0.5f / ShadowMapSize;
        float shadowCompare = shadowDepth - ShadowBias;
        shadow = 0.25f * (
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV + float2(halfTexel.x, halfTexel.y), shadowCompare) +
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV + float2(-halfTexel.x, halfTexel.y), shadowCompare) +
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV + float2(halfTexel.x, -halfTexel.y), shadowCompare) +
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV + float2(-halfTexel.x, -halfTexel.y), shadowCompare));
        shadow = lerp(1.0f, shadow, ShadowStrength);
    }

    float3 lighting = EvaluatePBR(albedo, metallic, roughness, F0, n, v, l) * LightIntensity * LightColor * shadow;

    float3 reflection = reflect(-v, n);
    float maxMip = max(0.0f, EnvMapMipCount - 1.0f);
    float mipLevel = roughness * maxMip;
    float3 prefilteredColor = EnvironmentMap.SampleLevel(IblSampler, reflection, mipLevel).rgb;

    float NdotV = saturate(dot(n, v));
    float2 brdf = BrdfLut.Sample(IblSampler, float2(NdotV, roughness)).rg;
    float3 specularIbl = prefilteredColor * (F0 * brdf.x + brdf.y);

    float3 irradiance = EnvironmentMap.SampleLevel(IblSampler, n, maxMip).rgb;
    float3 diffuseIbl = irradiance * albedo * (1.0f - metallic);

    float3 ambient = diffuseIbl + specularIbl;
    float3 color = lighting + ambient + emissive;
    return float4(color, 1.0);
}
