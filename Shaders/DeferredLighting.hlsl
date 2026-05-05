#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"
#include "Common.hlsli"
#include "../Source/Core/LightingVisualizationShared.h"

#ifndef COMPOSITE_VISUALIZATION_OFF
#define COMPOSITE_VISUALIZATION_OFF 0
#endif

#ifndef COMPOSITE_VISUALIZATION_ON
#define COMPOSITE_VISUALIZATION_ON 1
#endif

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};


cbuffer LightingBindlessConstants : register(b1)
{
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint GBufferDIndex;
    uint ShadowMapIndex;
    uint ShadowMaskIndex;
    uint EnvironmentMapIndex;
    uint BrdfLutIndex;
    uint DepthBufferIndex;
    uint GtaoTextureIndex;
    uint RestirGITextureIndex;
    uint SsrTextureIndex;
    uint SsrFallbackTextureIndex;
};

cbuffer DeferredLightingConstants : register(b2)
{
    float RestirGIIntensity;
    uint RestirGIEnabled;
    uint RestirGISamplesPerPixel;
    uint DeferredLightingVisualizationModeOverride;
    uint DeferredLightingPadding;
};

SamplerState GBufferSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
SamplerState IblSampler : register(s2);

VSOutput DeferredLightingVS(uint VertexId : SV_VertexID)
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

float4 DeferredLightingPS(VSOutput Input) : SV_Target
{
    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D GBufferC = ResourceDescriptorHeap[GBufferCIndex];
    Texture2D GBufferD = ResourceDescriptorHeap[GBufferDIndex];
    Texture2D ShadowMap = ResourceDescriptorHeap[ShadowMapIndex];
    Texture2D ShadowMaskTexture = ResourceDescriptorHeap[ShadowMaskIndex];
    TextureCube EnvironmentMap = ResourceDescriptorHeap[EnvironmentMapIndex];
    Texture2D BrdfLut = ResourceDescriptorHeap[BrdfLutIndex];
    Texture2D DepthBuffer = ResourceDescriptorHeap[DepthBufferIndex];
    Texture2D GtaoTexture = ResourceDescriptorHeap[GtaoTextureIndex];
    Texture2D RestirGITexture = ResourceDescriptorHeap[RestirGITextureIndex];
    Texture2D SsrTexture = ResourceDescriptorHeap[SsrTextureIndex];
    Texture2D SsrFallbackTexture = ResourceDescriptorHeap[SsrFallbackTextureIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferSampler, Input.UV);
    float depth = DepthBuffer.Sample(GBufferSampler, Input.UV).r;
    float3 albedo = GBufferC.Sample(GBufferSampler, Input.UV).rgb;
    float4 customData = GBufferD.Sample(GBufferSampler, Input.UV);

    float roughness = smr.z;
    float metallic = smr.y;
	float3 F0 = lerp(smr.x.xxx, albedo, metallic); // Metallic ???Albedo ?????熬?留???쀫뼔塋???轝???????????쀫뼔筌앹뼇???ㅼ맗???????? ?????
    uint shadingModelId = (uint)round(smr.w);

    float3 viewPos = ReconstructViewPositionFromDepth(Input.UV, depth, Projection);
    float3 worldPos = mul(float4(viewPos, 1.0f), ViewInverse).xyz;

    float3 V = normalize(CameraPosition - worldPos);
    float3 L = normalize(LightDirection);
    float4 shadowPosition = mul(float4(worldPos, 1.0f), LightViewProjection);
    float3 shadowCoord = shadowPosition.xyz / shadowPosition.w;
    float2 shadowUV = shadowCoord.xy * float2(0.5f, -0.5f) + 0.5f;
    float shadowDepth = shadowCoord.z;
    float shadow = 1.0f;
#if USE_SHADOW_MASK
    if (ShadowStrength > 0.0f)
    {
        float shadowMask = ShadowMaskTexture.Sample(GBufferSampler, Input.UV).r;
        shadow = lerp(1.0f, shadowMask, ShadowStrength);
    }
#else
    if (ShadowStrength > 0.0f && all(shadowUV >= 0.0f) && all(shadowUV <= 1.0f))
    {
        float2 shadowTexel = 1.0f / ShadowMapSize;
        float shadowCompare = shadowDepth - ShadowBias;
        shadow = 0.25f * (
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV, shadowCompare) +
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV + float2(shadowTexel.x, 0.0f), shadowCompare) +
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV + float2(0.0f, shadowTexel.y), shadowCompare) +
            ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV + shadowTexel, shadowCompare));
        shadow = lerp(1.0f, shadow, ShadowStrength);
    }
#endif

    float3 lighting = 0.0f;
    static const uint SHADINGMODEL_DEFAULT = 0u;
    static const uint SHADINGMODEL_SHEEN = 1u;
    static const uint SHADINGMODEL_CLEARCOAT = 2u;
    static const uint SHADINGMODEL_ANISOTROPY = 3u;
    if (shadingModelId == SHADINGMODEL_SHEEN)
    {
        float3 sheenColor = customData.rgb;
        float sheenRoughness = customData.a;
        lighting = EvaluatePBRWithSheen(albedo, metallic, roughness, F0, worldNormal, V, L, sheenColor, sheenRoughness) * LightIntensity * LightColor * shadow;
    }
    else if (shadingModelId == SHADINGMODEL_CLEARCOAT)
    {
        float clearcoat = customData.x;
        float clearcoatRoughness = customData.y;
        lighting = EvaluatePBRWithClearcoat(albedo, metallic, roughness, F0, worldNormal, V, L, clearcoat, clearcoatRoughness) * LightIntensity * LightColor * shadow;
    }
    else if (shadingModelId == SHADINGMODEL_ANISOTROPY)
    {
        float anisotropyValue = customData.x;
        float anisotropyStrength = customData.y;
        lighting = EvaluatePBRWithAnisotropy(albedo, metallic, roughness, F0, worldNormal, V, L, anisotropyValue, anisotropyStrength) * LightIntensity * LightColor * shadow;
    }
    else
    {
#if USE_PBR_RESEARCH
        lighting = EvaluatePBR_Research(albedo, metallic, roughness, F0, worldNormal, V, L) * LightIntensity * LightColor * shadow;
#else
        lighting = EvaluatePBR(albedo, metallic, roughness, F0, worldNormal, V, L) * LightIntensity * LightColor * shadow;
#endif
    }

    float3 worldView = normalize(CameraPosition - worldPos);
    float3 reflection = reflect(-worldView, worldNormal);

    float maxMip = max(0.0f, EnvMapMipCount - 1.0f);
    float specularRoughness = roughness;
    if (shadingModelId == SHADINGMODEL_ANISOTROPY)
    {
        float anisotropyValue = customData.x;
        float anisotropyStrength = customData.y;
        float anisotropy = saturate(anisotropyValue * anisotropyStrength);
        specularRoughness = lerp(roughness, max(0.03f, roughness * 0.5f), anisotropy);
    }
    float mipLevel = specularRoughness * maxMip;
    float3 prefilteredColor = EnvironmentMap.SampleLevel(IblSampler, reflection, mipLevel).rgb;

    float NdotV = saturate(dot(worldNormal, worldView));
    float2 brdf = BrdfLut.Sample(IblSampler, float2(NdotV, specularRoughness)).rg;
    float3 specularIbl = prefilteredColor * (F0 * brdf.x + brdf.y);
    if (shadingModelId == SHADINGMODEL_SHEEN)
    {
        float sheenRoughness = customData.a;
        float3 sheenColor = customData.rgb;
        float sheenMip = sheenRoughness * maxMip;
        float3 sheenPrefiltered = EnvironmentMap.SampleLevel(IblSampler, reflection, sheenMip).rgb;
        float2 sheenBrdf = BrdfLut.Sample(IblSampler, float2(NdotV, sheenRoughness)).rg;
        float3 sheenSpecIbl = sheenPrefiltered * (sheenColor * sheenBrdf.x + sheenBrdf.y);
        specularIbl += sheenSpecIbl;
    }
    else if (shadingModelId == SHADINGMODEL_CLEARCOAT)
    {
        float clearcoat = customData.x;
        float clearcoatRoughness = customData.y;
        float clearcoatMip = clearcoatRoughness * maxMip;
        float3 clearcoatPrefiltered = EnvironmentMap.SampleLevel(IblSampler, reflection, clearcoatMip).rgb;
        float2 clearcoatBrdf = BrdfLut.Sample(IblSampler, float2(NdotV, clearcoatRoughness)).rg;
        float3 clearcoatSpecIbl = clearcoatPrefiltered * (0.04.xxx * clearcoatBrdf.x + clearcoatBrdf.y);
        specularIbl += clearcoatSpecIbl * clearcoat;
    }

    float4 ssrSample = SsrTexture.Sample(GBufferSampler, Input.UV);
    float ssrWeight = saturate(ssrSample.a);
    if (ssrWeight > 0.0f)
    {
        specularIbl = lerp(specularIbl, ssrSample.rgb, ssrWeight);
    }
    else
    {
        float4 fallbackSample = SsrFallbackTexture.Sample(GBufferSampler, Input.UV);
        if (fallbackSample.a > 0.0f)
        {
            specularIbl = lerp(specularIbl, fallbackSample.rgb, saturate(fallbackSample.a));
        }
    }

    float3 irradiance = EnvironmentMap.SampleLevel(IblSampler, worldNormal, maxMip).rgb;
    float3 diffuseIbl = irradiance * albedo * (1.0f - metallic);

    float ao = (GtaoIntensity <= 0.0f) ? 1.0f : GtaoTexture.Sample(GBufferSampler, Input.UV).r;
    float3 restirIrradiance = 0.0f.xxx;
    if (RestirGIEnabled > 0 && RestirGIIntensity > 0.0f)
    {
        restirIrradiance = RestirGITexture.Sample(GBufferSampler, Input.UV).rgb;
    }

    float3 restirDiffuse = restirIrradiance * albedo * (1.0f - metallic);
    float3 ambient = (diffuseIbl + specularIbl) * ao;
    ambient += restirDiffuse;
    float3 color = lighting + ambient;

#if COMPOSITE_VISUALIZATION_ON
    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_DIFFUSE_INDIRECT)
    {
        return float4(diffuseIbl + restirDiffuse, 1.0f);
    }

    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_AO)
    {
        return float4(ao.xxx, 1.0f);
    }

    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_DIRECT_LIGHTING)
    {
        return float4(lighting, 1.0f);
    }

    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_SPECULAR_INDIRECT)
    {
        return float4(specularIbl, 1.0f);
    }
#endif

    return float4(color, 1.0);
}
