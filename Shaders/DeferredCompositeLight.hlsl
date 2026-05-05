#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"
#include "Common.hlsli"
#include "../Source/Core/LightingVisualizationShared.h"

#ifndef COMPOSITE_DIFFUSE_SOURCE_ENV
#define COMPOSITE_DIFFUSE_SOURCE_ENV 0
#endif

#ifndef COMPOSITE_DIFFUSE_SOURCE_RESTIR
#define COMPOSITE_DIFFUSE_SOURCE_RESTIR 0
#endif

#ifndef COMPOSITE_VISUALIZATION_OFF
#define COMPOSITE_VISUALIZATION_OFF 0
#endif

#ifndef COMPOSITE_VISUALIZATION_ON
#define COMPOSITE_VISUALIZATION_ON 0
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
    uint DirectLightingIndex;
};

cbuffer DeferredLightingConstants : register(b2)
{
    float RestirGIIntensity;
    uint RestirGIEnabled;
    float SsrRoughnessCutoff;
    uint DeferredLightingVisualizationModeOverride;
    uint DeferredLightingPadding0;
    uint DeferredLightingPadding1;
    uint DeferredLightingPadding2;
};

SamplerState GBufferSampler : register(s0);
SamplerState IblSampler : register(s2);

VSOutput DeferredCompositeLightVS(uint VertexId : SV_VertexID)
{
    float2 Positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0, 1.0);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

float3 EvaluateSpecularIBL(float3 radiance, float3 worldNormal, float3 worldView, float roughness, float3 F0, Texture2D BrdfLut)
{
    float NdotV = saturate(dot(worldNormal, worldView));
    float2 brdf = BrdfLut.Sample(IblSampler, float2(NdotV, roughness)).rg;
    return radiance * (F0 * brdf.x + brdf.y);
}

float3 EvaluateSpecularIBLFromEnvironment(TextureCube EnvironmentMap, Texture2D BrdfLut, float3 reflection, float3 worldNormal, float3 worldView, float roughness, float3 F0, float maxMip)
{
    float mipLevel = roughness * maxMip;
    float3 prefilteredColor = EnvironmentMap.SampleLevel(IblSampler, reflection, mipLevel).rgb;
    return EvaluateSpecularIBL(prefilteredColor, worldNormal, worldView, roughness, F0, BrdfLut);
}

float3 ClusterDagMipToColor(float mipNormalized)
{
    float t = saturate(mipNormalized);
    float3 low = float3(0.08f, 0.35f, 0.95f);
    float3 mid = float3(0.20f, 0.85f, 0.35f);
    float3 high = float3(0.98f, 0.85f, 0.10f);
    float3 maxv = float3(0.95f, 0.20f, 0.12f);
    if (t < 0.33f)
    {
        return lerp(low, mid, t / 0.33f);
    }
    if (t < 0.66f)
    {
        return lerp(mid, high, (t - 0.33f) / 0.33f);
    }
    return lerp(high, maxv, (t - 0.66f) / 0.34f);
}

float4 DeferredCompositeLightPS(VSOutput Input) : SV_Target
{
    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D GBufferC = ResourceDescriptorHeap[GBufferCIndex];
    Texture2D GBufferD = ResourceDescriptorHeap[GBufferDIndex];
    TextureCube EnvironmentMap = ResourceDescriptorHeap[EnvironmentMapIndex];
    Texture2D BrdfLut = ResourceDescriptorHeap[BrdfLutIndex];
    Texture2D DepthBuffer = ResourceDescriptorHeap[DepthBufferIndex];
    Texture2D GtaoTexture = ResourceDescriptorHeap[GtaoTextureIndex];
#if COMPOSITE_DIFFUSE_SOURCE_RESTIR
    Texture2D RestirGITexture = ResourceDescriptorHeap[RestirGITextureIndex];
#endif
    Texture2D SsrTexture = ResourceDescriptorHeap[SsrTextureIndex];
    Texture2D SsrFallbackTexture = ResourceDescriptorHeap[SsrFallbackTextureIndex];
    Texture2D DirectLightingTexture = ResourceDescriptorHeap[DirectLightingIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferSampler, Input.UV);
    float depth = DepthBuffer.Sample(GBufferSampler, Input.UV).r;
    float3 albedo = GBufferC.Sample(GBufferSampler, Input.UV).rgb;
    float4 customData = GBufferD.Sample(GBufferSampler, Input.UV);

#if COMPOSITE_VISUALIZATION_ON
    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_CLUSTER_DAG_CLUSTERS)
    {
        return float4(customData.rgb, 1.0f);
    }

    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_CLUSTER_DAG_MIP)
    {
        return float4(ClusterDagMipToColor(customData.a), 1.0f);
    }
#endif

    float roughness = smr.z;
    float metallic = smr.y;
    float3 F0 = lerp(smr.x.xxx, albedo, metallic);
    uint shadingModelId = (uint)round(smr.w);

    float3 viewPos = ReconstructViewPositionFromDepth(Input.UV, depth, Projection);
    float3 worldPos = mul(float4(viewPos, 1.0f), ViewInverse).xyz;

    float3 directLighting = DirectLightingTexture.Sample(GBufferSampler, Input.UV).rgb;

    float3 worldView = normalize(CameraPosition - worldPos);
    float3 reflection = reflect(-worldView, worldNormal);

    float maxMip = max(0.0f, EnvMapMipCount - 1.0f);
    float specularRoughness = roughness;
    static const uint SHADINGMODEL_SHEEN = 1u;
    static const uint SHADINGMODEL_CLEARCOAT = 2u;
    static const uint SHADINGMODEL_ANISOTROPY = 3u;
    if (shadingModelId == SHADINGMODEL_ANISOTROPY)
    {
        float anisotropyValue = customData.x;
        float anisotropyStrength = customData.y;
        float anisotropy = saturate(anisotropyValue * anisotropyStrength);
        specularRoughness = lerp(roughness, max(0.03f, roughness * 0.5f), anisotropy);
    }
    float3 specularIbl = EvaluateSpecularIBLFromEnvironment(EnvironmentMap, BrdfLut, reflection, worldNormal, worldView, specularRoughness, F0, maxMip);
    float NdotV = saturate(dot(worldNormal, worldView));
    if (shadingModelId == SHADINGMODEL_SHEEN)
    {
        float sheenRoughness = customData.a;
        float3 sheenColor = customData.rgb;
        float3 sheenSpecIbl = EvaluateSpecularIBLFromEnvironment(EnvironmentMap, BrdfLut, reflection, worldNormal, worldView, sheenRoughness, sheenColor, maxMip);
        specularIbl += sheenSpecIbl;
    }
    else if (shadingModelId == SHADINGMODEL_CLEARCOAT)
    {
        float clearcoat = customData.x;
        float clearcoatRoughness = customData.y;
        float3 clearcoatSpecIbl = EvaluateSpecularIBLFromEnvironment(EnvironmentMap, BrdfLut, reflection, worldNormal, worldView, clearcoatRoughness, 0.04.xxx, maxMip);
        specularIbl += clearcoatSpecIbl * clearcoat;
    }

    float ao = (GtaoIntensity <= 0.0f) ? 1.0f : GtaoTexture.Sample(GBufferSampler, Input.UV).r;
    float3 indirectDiffuse = 0.0f.xxx;
#if COMPOSITE_DIFFUSE_SOURCE_ENV
    {
        const float3 irradiance = EnvironmentMap.SampleLevel(IblSampler, worldNormal, maxMip).rgb;
        indirectDiffuse = irradiance * albedo * (1.0f - metallic);
    }
#elif COMPOSITE_DIFFUSE_SOURCE_RESTIR
    {
        const float3 restirIrradiance = RestirGITexture.Sample(GBufferSampler, Input.UV).rgb;
        indirectDiffuse = restirIrradiance * albedo * (1.0f - metallic);
    }
#endif

    if (roughness < SsrRoughnessCutoff)
    {
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
    }
    else
    {
        specularIbl = EvaluateSpecularIBL(indirectDiffuse, worldNormal, worldView, specularRoughness, F0, BrdfLut);
    }

    float3 ambient = (indirectDiffuse + specularIbl) * ao;

    float3 color = directLighting + ambient;

#if COMPOSITE_VISUALIZATION_ON
    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_DIFFUSE_INDIRECT)
    {
        return float4(indirectDiffuse, 1.0f);
    }

    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_AO)
    {
        return float4(ao.xxx, 1.0f);
    }

    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_DIRECT_LIGHTING)
    {
        return float4(directLighting, 1.0f);
    }

    if (DeferredLightingVisualizationModeOverride == LIGHTING_VISUALIZATION_SPECULAR_INDIRECT)
    {
        return float4(specularIbl, 1.0f);
    }
#endif

    return float4(color, 1.0f);
}
