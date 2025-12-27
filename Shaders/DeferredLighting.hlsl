#include "PBRCommon.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
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

Texture2D GBufferA : register(t0);
Texture2D GBufferB : register(t1);
Texture2D GBufferC : register(t2);
Texture2D ShadowMap : register(t3);
TextureCube EnvironmentMap : register(t4);
Texture2D BrdfLut : register(t5);
SamplerState GBufferSampler : register(s0);
SamplerState ShadowSampler : register(s1);
SamplerState IblSampler : register(s2);

VSOutput VSMain(uint VertexId : SV_VertexID)
{
    float2 Positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0, 1.0);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float4 normalDepth = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 normal = normalize(normalDepth.xyz);
    float depth = normalDepth.w;
    float4 smr = GBufferB.Sample(GBufferSampler, Input.UV);
    float3 albedo = GBufferC.Sample(GBufferSampler, Input.UV).rgb;

    float roughness = smr.z;
    float metallic = smr.y;
    float3 F0 = lerp(smr.x.xxx, albedo, metallic);

    float2 ndc = float2(Input.UV * 2.0f - 1.0f);
    float viewZ = -depth;
    float viewX = ndc.x * viewZ / Projection._11;
    float viewY = -ndc.y * viewZ / Projection._22;
    float3 viewPos = float3(viewX, viewY, viewZ);

    float3 V = normalize(-viewPos);
    float3 L = normalize(mul(float4(LightDirection, 0.0f), View).xyz);

    float3 worldPos = mul(float4(viewPos, 1.0f), ViewInverse).xyz;
    float4 shadowPosition = mul(float4(worldPos, 1.0f), LightViewProjection);
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

    float3 lighting = EvaluatePBR(albedo, metallic, roughness, F0, normal, V, L) * LightIntensity * LightColor * shadow;

    float3 worldNormal = normalize(mul(normal, (float3x3)ViewInverse));
    float3 worldView = normalize(CameraPosition - worldPos);
    float3 reflection = reflect(-worldView, worldNormal);

    float maxMip = max(0.0f, EnvMapMipCount - 1.0f);
    float mipLevel = roughness * maxMip;
    float3 prefilteredColor = EnvironmentMap.SampleLevel(IblSampler, reflection, mipLevel).rgb;

    float NdotV = saturate(dot(worldNormal, worldView));
    float2 brdf = BrdfLut.Sample(IblSampler, float2(NdotV, roughness)).rg;
    float3 specularIbl = prefilteredColor * (F0 * brdf.x + brdf.y);

    float3 irradiance = EnvironmentMap.SampleLevel(IblSampler, worldNormal, maxMip).rgb;
    float3 diffuseIbl = irradiance * albedo * (1.0f - metallic);

    float3 ambient = diffuseIbl + specularIbl;
    float3 color = lighting + ambient;
    return float4(color, 1.0);
}
