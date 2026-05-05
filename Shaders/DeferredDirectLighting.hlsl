#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"
#include "Common.hlsli"

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

SamplerState GBufferSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);

VSOutput DeferredDirectLightingVS(uint VertexId : SV_VertexID)
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

float4 DeferredDirectLightingPS(VSOutput Input) : SV_Target
{
    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D GBufferC = ResourceDescriptorHeap[GBufferCIndex];
    Texture2D GBufferD = ResourceDescriptorHeap[GBufferDIndex];
    Texture2D ShadowMap = ResourceDescriptorHeap[ShadowMapIndex];
    Texture2D ShadowMaskTexture = ResourceDescriptorHeap[ShadowMaskIndex];
    Texture2D DepthBuffer = ResourceDescriptorHeap[DepthBufferIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferSampler, Input.UV);
    float depth = DepthBuffer.Sample(GBufferSampler, Input.UV).r;
    float3 albedo = GBufferC.Sample(GBufferSampler, Input.UV).rgb;
    float4 customData = GBufferD.Sample(GBufferSampler, Input.UV);

    if (depth <= 0.0f)
    {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float roughness = smr.z;
    float metallic = smr.y;
    float3 F0 = lerp(smr.x.xxx, albedo, metallic);
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

    float3 directLighting = 0.0f;
    static const uint SHADINGMODEL_DEFAULT = 0u;
    static const uint SHADINGMODEL_SHEEN = 1u;
    static const uint SHADINGMODEL_CLEARCOAT = 2u;
    static const uint SHADINGMODEL_ANISOTROPY = 3u;
    if (shadingModelId == SHADINGMODEL_SHEEN)
    {
        float3 sheenColor = customData.rgb;
        float sheenRoughness = customData.a;
        directLighting = EvaluatePBRWithSheen(albedo, metallic, roughness, F0, worldNormal, V, L, sheenColor, sheenRoughness) * LightIntensity * LightColor * shadow;
    }
    else if (shadingModelId == SHADINGMODEL_CLEARCOAT)
    {
        float clearcoat = customData.x;
        float clearcoatRoughness = customData.y;
        directLighting = EvaluatePBRWithClearcoat(albedo, metallic, roughness, F0, worldNormal, V, L, clearcoat, clearcoatRoughness) * LightIntensity * LightColor * shadow;
    }
    else if (shadingModelId == SHADINGMODEL_ANISOTROPY)
    {
        float anisotropyValue = customData.x;
        float anisotropyStrength = customData.y;
        directLighting = EvaluatePBRWithAnisotropy(albedo, metallic, roughness, F0, worldNormal, V, L, anisotropyValue, anisotropyStrength) * LightIntensity * LightColor * shadow;
    }
    else
    {
#if USE_PBR_RESEARCH
        directLighting = EvaluatePBR_Research(albedo, metallic, roughness, F0, worldNormal, V, L) * LightIntensity * LightColor * shadow;
#else
        directLighting = EvaluatePBR(albedo, metallic, roughness, F0, worldNormal, V, L) * LightIntensity * LightColor * shadow;
#endif
    }

    return float4(directLighting, 1.0f);
}
