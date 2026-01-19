#include "SceneConstants.hlsl"

#ifndef USE_SKINNING
#define USE_SKINNING 0
#endif

struct VSInput
{
    uint VertexId : SV_VertexID;
	uint StartVertexLocation : SV_StartVertexLocation;
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


cbuffer BasePassBindlessConstants : register(b1)
{
    uint AlbedoTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint NormalTextureIndex;
    uint EmissiveTextureIndex;
};
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
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.x];
    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.y];
    StructuredBuffer<float2> TexCoordBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.z];
    StructuredBuffer<float4> TangentBuffer = ResourceDescriptorHeap[VertexBufferBindlessIndices.w];
    StructuredBuffer<float4> ColorBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.x];
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[ExtraBindlessIndices.y];
#if USE_SKINNING
    StructuredBuffer<uint4> JointBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.x];
    StructuredBuffer<float4> WeightBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.y];
    StructuredBuffer<row_major float4x4> BoneMatrices = ResourceDescriptorHeap[SkinningBindlessIndices.z];
    StructuredBuffer<float3> SkinnedPositionBuffer = ResourceDescriptorHeap[SkinningBindlessIndices.w];
#endif

    uint vertexIndex = IndexBuffer[Input.VertexId + Input.StartVertexLocation];
    float3 position = PositionBuffer[vertexIndex];
    float3 normal = NormalBuffer[vertexIndex];
    float2 uv = TexCoordBuffer[vertexIndex];
    float4 tangent = TangentBuffer[vertexIndex];
    float4 color = ColorBuffer[vertexIndex];

#if USE_SKINNING
    uint4 joints = JointBuffer[vertexIndex];
    float4 weights = WeightBuffer[vertexIndex];
    row_major float4x4 skinMatrix =
        weights.x * BoneMatrices[joints.x] +
        weights.y * BoneMatrices[joints.y] +
        weights.z * BoneMatrices[joints.z] +
        weights.w * BoneMatrices[joints.w];
    position = SkinnedPositionBuffer[vertexIndex];
    normal = mul(normal, (float3x3)skinMatrix);
    tangent.xyz = mul(tangent.xyz, (float3x3)skinMatrix);
#endif

    float4 WorldPos = mul(float4(position, 1.0), World);
    float4 ViewPos = mul(WorldPos, View);
    Output.Position = mul(ViewPos, Projection);
    Output.Normal = mul(normal, (float3x3)World);
    Output.UV = uv;
    Output.WorldPos = WorldPos.xyz;
    Output.Tangent = float4(normalize(mul(tangent.xyz, (float3x3)World)), tangent.w);
    Output.Color = color;
    return Output;
}

struct PSOutput
{
    float4 GBufferA : SV_Target0; // Normal (encoded)
    float4 GBufferB : SV_Target1; // Specular/Metallic/Roughness
    float4 GBufferC : SV_Target2; // BaseColor
    float4 SceneColor : SV_Target3; // Emissive
};

float3 ComputeViewNormal(VSOutput Input, float2 normalUV)
{
    float3 vertexNormal = normalize(Input.Normal);

#if USE_NORMAL_MAP
    Texture2D NormalTexture = ResourceDescriptorHeap[NormalTextureIndex];
    float3 tangent = normalize(Input.Tangent.xyz - vertexNormal * dot(vertexNormal, Input.Tangent.xyz));
    float3 bitangent = normalize(cross(vertexNormal, tangent)) * Input.Tangent.w;

    float2 tangentNormalRG = NormalTexture.Sample(AlbedoSampler, normalUV).rg * 2.0f - 1.0f;
    float tangentNormalZ = sqrt(saturate(1.0f - dot(tangentNormalRG, tangentNormalRG)));
    float3 tangentNormal = float3(tangentNormalRG, tangentNormalZ);
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
    Texture2D AlbedoTexture = ResourceDescriptorHeap[AlbedoTextureIndex];
    Texture2D MetallicRoughnessTexture = ResourceDescriptorHeap[MetallicRoughnessTextureIndex];
    Texture2D EmissiveTexture = ResourceDescriptorHeap[EmissiveTextureIndex];

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

    Output.GBufferA = float4(viewNormal * 0.5f + 0.5f, 1.0f);

    const float specular = 0.04f;
    float metallic = MetallicFactor;
    float roughness = RoughnessFactor;
#if USE_METALLIC_ROUGHNESS_MAP
    float2 metallicRoughness = MetallicRoughnessTexture.Sample(AlbedoSampler, mrUV).bg;
    metallic *= metallicRoughness.x;
    roughness *= metallicRoughness.y;
#endif
    Output.GBufferB = float4(specular, metallic, roughness, 1.0f);

    Output.GBufferC = float4(albedo, 1.0);

    float3 emissive = EmissiveFactor;
#if USE_EMISSIVE_MAP
    emissive *= EmissiveTexture.Sample(AlbedoSampler, emissiveUV).rgb;
#endif
    Output.SceneColor = float4(emissive, 1.0);
    return Output;
}
