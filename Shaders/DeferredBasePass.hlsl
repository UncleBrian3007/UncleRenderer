#include "SceneConstants.hlsl"

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

#ifndef USE_NORMAL_MAP
#define USE_NORMAL_MAP 1
#endif
#ifndef USE_METALLIC_ROUGHNESS_MAP
#define USE_METALLIC_ROUGHNESS_MAP 1
#endif
#ifndef USE_BASE_COLOR_MAP
#define USE_BASE_COLOR_MAP 1
#endif
#ifndef USE_EMISSIVE_MAP
#define USE_EMISSIVE_MAP 1
#endif
#ifndef USE_ALPHA_MASK
#define USE_ALPHA_MASK 0
#endif


Texture2D AlbedoTexture : register(t0);
#if USE_METALLIC_ROUGHNESS_MAP
Texture2D MetallicRoughnessTexture : register(t1);
#endif
Texture2D NormalTexture : register(t2);
#if USE_EMISSIVE_MAP
Texture2D EmissiveTexture : register(t3);
#endif
SamplerState AlbedoSampler : register(s0);

float2 ApplyTextureTransform(float2 uv, float4 offsetScale, float4 rotation)
{
    float2 scaled = uv * offsetScale.zw;
    float2 rotated = float2(
        scaled.x * rotation.x - scaled.y * rotation.y,
        scaled.x * rotation.y + scaled.y * rotation.x);
    return rotated + offsetScale.xy;
}

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

struct PSOutput
{
    float4 GBufferA : SV_Target0; // Normal
    float4 GBufferB : SV_Target1; // Specular/Metallic/Roughness
    float4 GBufferC : SV_Target2; // BaseColor
    float4 SceneColor : SV_Target3; // Emissive
};

float3 ComputeViewNormal(VSOutput Input, float2 normalUV)
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

    return normalize(mul(normalize(worldNormal), (float3x3)View));
#else
    return normalize(mul(vertexNormal, (float3x3)View));
#endif
}

PSOutput PSMain(VSOutput Input)
{
    PSOutput Output;

    float2 baseUV = ApplyTextureTransform(Input.UV, BaseColorTransformOffsetScale, BaseColorTransformRotation);
    float2 mrUV = ApplyTextureTransform(Input.UV, MetallicRoughnessTransformOffsetScale, MetallicRoughnessTransformRotation);
    float2 normalUV = ApplyTextureTransform(Input.UV, NormalTransformOffsetScale, NormalTransformRotation);
    float2 emissiveUV = ApplyTextureTransform(Input.UV, EmissiveTransformOffsetScale, EmissiveTransformRotation);

    float3 viewNormal = ComputeViewNormal(Input, normalUV);

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

    float viewDepth = -mul(float4(Input.WorldPos, 1.0), View).z;
    Output.GBufferA = float4(viewNormal, viewDepth);

    const float specular = 0.04f;
    float metallic = MetallicFactor;
    float roughness = RoughnessFactor;
#if USE_METALLIC_ROUGHNESS_MAP
    float2 metallicRoughness = MetallicRoughnessTexture.Sample(AlbedoSampler, mrUV).bg;
    metallic *= metallicRoughness.x;
    roughness *= metallicRoughness.y;
#endif
    Output.GBufferB = float4(specular, metallic, roughness, 1.0);

    Output.GBufferC = float4(albedo, 1.0);

    float3 emissive = EmissiveFactor;
#if USE_EMISSIVE_MAP
    emissive *= EmissiveTexture.Sample(AlbedoSampler, emissiveUV).rgb;
#endif
    Output.SceneColor = float4(emissive, 1.0);
    return Output;
}
