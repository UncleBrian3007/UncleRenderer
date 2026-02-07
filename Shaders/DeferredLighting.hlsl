#include "PBRCommon.hlsl"
#include "SceneConstants.hlsl"

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
    uint ShadowMapIndex;
    uint ShadowMaskIndex;
    uint EnvironmentMapIndex;
    uint BrdfLutIndex;
    uint DepthBufferIndex;
    uint GtaoTextureIndex;
    uint SsrTextureIndex;
};
SamplerState GBufferSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
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

float ReconstructViewZ(float depth)
{
    return Projection._43 / max(depth, 1e-6f);
}

float3 ReconstructViewPosition(float2 uv, float depth)
{
    float2 ndc = float2(uv * 2.0f - 1.0f);
    float viewZ = ReconstructViewZ(depth);
    float viewX = ndc.x * viewZ / Projection._11;
    float viewY = -ndc.y * viewZ / Projection._22;
    return float3(viewX, viewY, viewZ);
}

float4 PSMain(VSOutput Input) : SV_Target
{
    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D GBufferC = ResourceDescriptorHeap[GBufferCIndex];
    Texture2D ShadowMap = ResourceDescriptorHeap[ShadowMapIndex];
    Texture2D ShadowMaskTexture = ResourceDescriptorHeap[ShadowMaskIndex];
    TextureCube EnvironmentMap = ResourceDescriptorHeap[EnvironmentMapIndex];
    Texture2D BrdfLut = ResourceDescriptorHeap[BrdfLutIndex];
    Texture2D DepthBuffer = ResourceDescriptorHeap[DepthBufferIndex];
    Texture2D GtaoTexture = ResourceDescriptorHeap[GtaoTextureIndex];
    Texture2D SsrTexture = ResourceDescriptorHeap[SsrTextureIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferSampler, Input.UV);
    float depth = DepthBuffer.Sample(GBufferSampler, Input.UV).r;
    float3 albedo = GBufferC.Sample(GBufferSampler, Input.UV).rgb;

    float roughness = smr.z;
    float metallic = smr.y;
	float3 F0 = lerp(smr.x.xxx, albedo, metallic); // Metallic Àº Albedo ¿¡ ¹Ý»çÀ²(»ö±ò) ÀúÀå

    float3 viewPos = ReconstructViewPosition(Input.UV, depth);
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
#if USE_PBR_RESEARCH
    lighting = EvaluatePBR_Research(albedo, metallic, roughness, F0, worldNormal, V, L) * LightIntensity * LightColor * shadow;
#else
    lighting = EvaluatePBR(albedo, metallic, roughness, F0, worldNormal, V, L) * LightIntensity * LightColor * shadow;
#endif

    float3 worldView = normalize(CameraPosition - worldPos);
    float3 reflection = reflect(-worldView, worldNormal);

    float maxMip = max(0.0f, EnvMapMipCount - 1.0f);
    float mipLevel = roughness * maxMip;
    float3 prefilteredColor = EnvironmentMap.SampleLevel(IblSampler, reflection, mipLevel).rgb;

    float NdotV = saturate(dot(worldNormal, worldView));
    float2 brdf = BrdfLut.Sample(IblSampler, float2(NdotV, roughness)).rg;
    float3 specularIbl = prefilteredColor * (F0 * brdf.x + brdf.y);

    float4 ssrSample = SsrTexture.Sample(GBufferSampler, Input.UV);
    float ssrWeight = saturate(ssrSample.a);
    specularIbl = lerp(specularIbl, ssrSample.rgb, ssrWeight);

    float3 irradiance = EnvironmentMap.SampleLevel(IblSampler, worldNormal, maxMip).rgb;
    float3 diffuseIbl = irradiance * albedo * (1.0f - metallic);

    float ao = (GtaoIntensity <= 0.0f) ? 1.0f : GtaoTexture.Sample(GBufferSampler, Input.UV).r;
    float3 ambient = (diffuseIbl + specularIbl) * ao;
    float3 color = lighting + ambient;
    return float4(color, 1.0);
}
