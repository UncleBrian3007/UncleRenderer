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
    float4 BaseColorTransformOffsetScale;
    float4 BaseColorTransformRotation;
    float4 MetallicRoughnessTransformOffsetScale;
    float4 MetallicRoughnessTransformRotation;
    float4 NormalTransformOffsetScale;
    float4 NormalTransformRotation;
    float4 EmissiveTransformOffsetScale;
    float4 EmissiveTransformRotation;
};

Texture2D GBufferA : register(t0);
Texture2D GBufferB : register(t1);
Texture2D GBufferC : register(t2);
Texture2D ShadowMap : register(t3);
SamplerState GBufferSampler : register(s0);
SamplerState ShadowSampler : register(s1);

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

    float3 ambient = albedo * 0.03f;
    float3 color = lighting + ambient + 0.1;
    return float4(color, 1.0);
}
