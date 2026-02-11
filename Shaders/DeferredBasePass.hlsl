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
    float3 PrevWorldPos : TEXCOORD2;
    float4 Tangent  : TEXCOORD3;
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
#ifndef SHADINGMODEL_SHEEN
#define SHADINGMODEL_SHEEN 0
#endif
#ifndef SHADINGMODEL_CLEARCOAT
#define SHADINGMODEL_CLEARCOAT 0
#endif
#ifndef SHADINGMODEL_ANISOTROPY
#define SHADINGMODEL_ANISOTROPY 0
#endif
#ifndef VELOCITY_PASS
#define VELOCITY_PASS 0
#endif


cbuffer BasePassBindlessConstants : register(b1)
{
    uint AlbedoTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint NormalTextureIndex;
    uint EmissiveTextureIndex;
    uint SheenColorTextureIndex;
    uint SheenRoughnessTextureIndex;
    uint ClearcoatTextureIndex;
    uint ClearcoatRoughnessTextureIndex;
    uint ClearcoatNormalTextureIndex;
    uint AnisotropyTextureIndex;
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
    float3 basePosition = PositionBuffer[vertexIndex];
    float3 currentPosition = basePosition;
    float3 previousPosition = basePosition;
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
    currentPosition = SkinnedPositionBuffer[vertexIndex];
    normal = mul(normal, (float3x3)skinMatrix);
    tangent.xyz = mul(tangent.xyz, (float3x3)skinMatrix);

    if (HasPreviousSkinning != 0u && PreviousSkinnedPositionBindlessIndex != 0xFFFFFFFFu)
    {
        StructuredBuffer<float3> PreviousSkinnedPositionBuffer = ResourceDescriptorHeap[PreviousSkinnedPositionBindlessIndex];
        previousPosition = PreviousSkinnedPositionBuffer[vertexIndex];
    }
    else
    {
        previousPosition = currentPosition;
    }
#endif

    float4 WorldPos = mul(float4(currentPosition, 1.0), World);
    float4 PreviousWorldPos = (HasPreviousWorld != 0u)
        ? mul(float4(previousPosition, 1.0), PreviousWorld)
        : WorldPos;
    float4 ViewPos = mul(WorldPos, View);
    Output.Position = mul(ViewPos, Projection);
    Output.Normal = mul(normal, (float3x3)World);
    Output.UV = uv;
    Output.WorldPos = WorldPos.xyz;
    Output.PrevWorldPos = PreviousWorldPos.xyz;
    Output.Tangent = float4(normalize(mul(tangent.xyz, (float3x3)World)), tangent.w);
    Output.Color = color;
    return Output;
}

struct PSOutput
{
    float4 GBufferA : SV_Target0; // Normal (world, encoded)
    float4 GBufferB : SV_Target1; // Specular/Metallic/Roughness/ShadingModelId
    float4 GBufferC : SV_Target2; // BaseColor
    float4 GBufferD : SV_Target3; // CustomData
    float4 SceneColor : SV_Target4; // Emissive
};

float3 ComputeWorldNormal(VSOutput Input, float2 normalUV)
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

    return normalize(worldNormal);
#else
    return normalize(vertexNormal);
#endif
}

float3 ComputeWorldNormalFromTexture(VSOutput Input, Texture2D NormalTexture, float2 normalUV)
{
    float3 vertexNormal = normalize(Input.Normal);

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
    return normalize(worldNormal);
}


struct PSOutputVelocity
{
    float2 Velocity : SV_Target0;
};

PSOutputVelocity PSMainVelocity(VSOutput Input)
{
    PSOutputVelocity Output;
#if USE_ALPHA_MASK
    Texture2D AlbedoTexture = ResourceDescriptorHeap[AlbedoTextureIndex];
    float2 baseUV = ApplyTextureTransform(Input.UV, BaseColorTransformOffsetScale, BaseColorTransformRotation);
    float alpha = BaseColorAlpha * Input.Color.a;
    float4 albedoSample = AlbedoTexture.Sample(AlbedoSampler, baseUV);
    alpha *= albedoSample.a;
    if (alpha < AlphaCutoff)
    {
        clip(alpha - AlphaCutoff);
    }
#endif

    if (HasPreviousViewProjection == 0u)
    {
        Output.Velocity = float2(0.0f, 0.0f);
        return Output;
    }

    const float Epsilon = 1e-6f;
    float CurrentW = max(abs(Input.Position.w), Epsilon);
    float2 CurrentNdc = Input.Position.xy / CurrentW;

    float4 PreviousClip = mul(float4(Input.PrevWorldPos, 1.0f), PreviousViewProjection);
    float PreviousW = max(abs(PreviousClip.w), Epsilon);
    float2 PreviousNdc = PreviousClip.xy / PreviousW;

    Output.Velocity = CurrentNdc - PreviousNdc;
    return Output;
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
    float2 sheenColorUV = ApplyTextureTransform(Input.UV, SheenColorTransformOffsetScale, SheenColorTransformRotation);
    float2 sheenRoughnessUV = ApplyTextureTransform(Input.UV, SheenRoughnessTransformOffsetScale, SheenRoughnessTransformRotation);
    float2 clearcoatUV = ApplyTextureTransform(Input.UV, ClearcoatTransformOffsetScale, ClearcoatTransformRotation);
    float2 clearcoatRoughnessUV = ApplyTextureTransform(Input.UV, ClearcoatRoughnessTransformOffsetScale, ClearcoatRoughnessTransformRotation);
    float2 clearcoatNormalUV = ApplyTextureTransform(Input.UV, ClearcoatNormalTransformOffsetScale, ClearcoatNormalTransformRotation);
#if SHADINGMODEL_ANISOTROPY
    float2 anisotropyUV = ApplyTextureTransform(Input.UV, AnisotropyTransformOffsetScale, AnisotropyTransformRotation);
#endif

    float3 worldNormal = ComputeWorldNormal(Input, normalUV);
#if SHADINGMODEL_CLEARCOAT
    if (ClearcoatNormalTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D ClearcoatNormalTexture = ResourceDescriptorHeap[ClearcoatNormalTextureIndex];
        worldNormal = ComputeWorldNormalFromTexture(Input, ClearcoatNormalTexture, clearcoatNormalUV);
    }
#endif

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

    Output.GBufferA = float4(worldNormal * 0.5f + 0.5f, 1.0f);

    const float specular = 0.04f;
    float metallic = MetallicFactor;
    float roughness = RoughnessFactor;
#if USE_METALLIC_ROUGHNESS_MAP
    float2 metallicRoughness = MetallicRoughnessTexture.Sample(AlbedoSampler, mrUV).bg;
    metallic *= metallicRoughness.x;
    roughness *= metallicRoughness.y;
#endif
    Output.GBufferB = float4(specular, metallic, roughness, (float)ShadingModelId);

    Output.GBufferC = float4(albedo, 1.0);

    float4 customData = 0.0f;
#if SHADINGMODEL_SHEEN
    float3 sheenColor = SheenColorFactor;
    float sheenRoughness = SheenRoughnessFactor;
    if (SheenColorTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D SheenColorTexture = ResourceDescriptorHeap[SheenColorTextureIndex];
        sheenColor *= SheenColorTexture.Sample(AlbedoSampler, sheenColorUV).rgb;
    }
    if (SheenRoughnessTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D SheenRoughnessTexture = ResourceDescriptorHeap[SheenRoughnessTextureIndex];
        sheenRoughness *= SheenRoughnessTexture.Sample(AlbedoSampler, sheenRoughnessUV).a;
    }
    customData = float4(sheenColor, sheenRoughness);
#endif
#if SHADINGMODEL_CLEARCOAT
    float clearcoat = ClearcoatFactor;
    float clearcoatRoughness = ClearcoatRoughnessFactor;
    if (ClearcoatTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D ClearcoatTexture = ResourceDescriptorHeap[ClearcoatTextureIndex];
        clearcoat *= ClearcoatTexture.Sample(AlbedoSampler, clearcoatUV).r;
    }
    if (ClearcoatRoughnessTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D ClearcoatRoughnessTexture = ResourceDescriptorHeap[ClearcoatRoughnessTextureIndex];
        clearcoatRoughness *= ClearcoatRoughnessTexture.Sample(AlbedoSampler, clearcoatRoughnessUV).g;
    }
    customData = float4(clearcoat, clearcoatRoughness, 0.0f, 0.0f);
#endif
#if SHADINGMODEL_ANISOTROPY
    float anisotropyValue = 1.0f;
    if (AnisotropyTextureIndex != 0xFFFFFFFFu)
    {
        Texture2D AnisotropyTexture = ResourceDescriptorHeap[AnisotropyTextureIndex];
        anisotropyValue = AnisotropyTexture.Sample(AlbedoSampler, anisotropyUV).r;
    }
    customData = float4(anisotropyValue, AnisotropyStrength, 0.0f, 0.0f);
#endif
    Output.GBufferD = customData;

    float3 emissive = EmissiveFactor;
#if USE_EMISSIVE_MAP
    emissive *= EmissiveTexture.Sample(AlbedoSampler, emissiveUV).rgb;
#endif
    Output.SceneColor = float4(emissive, 1.0);
    return Output;
}
