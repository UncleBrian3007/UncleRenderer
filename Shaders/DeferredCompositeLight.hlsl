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

cbuffer RestirGIConstants : register(b2)
{
    float RestirGIIntensity;
    uint RestirGIEnabled;
    uint RestirGISamplesPerPixel;
    uint RestirGIShowOnly;
    uint RestirGIPadding;
};

SamplerState GBufferSampler : register(s0);
SamplerState IblSampler : register(s2);

VSOutput VSMain(uint VertexId : SV_VertexID)
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
    Texture2D GBufferD = ResourceDescriptorHeap[GBufferDIndex];
    TextureCube EnvironmentMap = ResourceDescriptorHeap[EnvironmentMapIndex];
    Texture2D BrdfLut = ResourceDescriptorHeap[BrdfLutIndex];
    Texture2D DepthBuffer = ResourceDescriptorHeap[DepthBufferIndex];
    Texture2D GtaoTexture = ResourceDescriptorHeap[GtaoTextureIndex];
    Texture2D RestirGITexture = ResourceDescriptorHeap[RestirGITextureIndex];
    Texture2D SsrTexture = ResourceDescriptorHeap[SsrTextureIndex];
    Texture2D SsrFallbackTexture = ResourceDescriptorHeap[SsrFallbackTextureIndex];
    Texture2D DirectLightingTexture = ResourceDescriptorHeap[DirectLightingIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferSampler, Input.UV);
    float depth = DepthBuffer.Sample(GBufferSampler, Input.UV).r;
    float3 albedo = GBufferC.Sample(GBufferSampler, Input.UV).rgb;
    float4 customData = GBufferD.Sample(GBufferSampler, Input.UV);

    float roughness = smr.z;
    float metallic = smr.y;
    float3 F0 = lerp(smr.x.xxx, albedo, metallic);
    uint shadingModelId = (uint)round(smr.w);

    float3 viewPos = ReconstructViewPosition(Input.UV, depth);
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

    if (RestirGIShowOnly > 0u)
    {
        return float4(restirIrradiance, 1.0f);
    }

    float3 indirectDiffuse = diffuseIbl;
    if (RestirGIEnabled > 0 && RestirGIIntensity > 0.0f)
    {
        indirectDiffuse = restirIrradiance * albedo * (1.0f - metallic);
    }

    float3 ambient = (indirectDiffuse + specularIbl) * ao;

    float3 color = directLighting + ambient;
    return float4(color, 1.0f);
}
