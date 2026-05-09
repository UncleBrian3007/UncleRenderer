#include "Common.hlsli"
#include "OctahedralEncoding.hlsli"
#include "ClusterDag/ClusterDagCommon.hlsl"
#include "SceneConstantsFields.hlsli"
#include "../Source/Core/LightingVisualizationShared.h"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

struct ClusterDagResolveSceneData
{
    SCENE_CONSTANTS_FIELDS
};

struct ClusterDagPackedPosition
{
    uint XY;
    uint Z;
};

cbuffer ClusterDagResolveBindlessConstants : register(b0)
{
    uint VisibilityTextureIndex0;
    uint VisibilityTextureIndex1;
    uint DrawDataBufferIndex;
    uint SceneDataBufferIndex;
};

SamplerState MaterialSampler : register(s0);

struct PSOutput
{
    float4 GBufferA : SV_Target0;
    float4 GBufferB : SV_Target1;
    float4 GBufferC : SV_Target2;
    float4 GBufferD : SV_Target3;
    float4 SceneColor : SV_Target4;
};

float4 DecodeDebugColor(uint packedColor)
{
    const float inv255 = 1.0f / 255.0f;
    return float4(
        (packedColor & 0xffu) * inv255,
        ((packedColor >> 8) & 0xffu) * inv255,
        ((packedColor >> 16) & 0xffu) * inv255,
        ((packedColor >> 24) & 0xffu) * inv255);
}

int SignExtend10(uint value)
{
    return (value & 0x200u) != 0u
        ? int(value | 0xFFFFFC00u)
        : int(value & 0x3FFu);
}

float DecodeSnorm10(uint value)
{
    return max((float)SignExtend10(value) / 511.0f, -1.0f);
}

float3 DecodeClusterDagPackedPosition(
    ClusterDagPackedPosition packedPosition,
    ClusterDagResolveSceneData sceneData)
{
    const uint qx = packedPosition.XY & 0xFFFFu;
    const uint qy = packedPosition.XY >> 16u;
    const uint qz = packedPosition.Z & 0xFFFFu;
    return sceneData.ClusterDagPackedPositionOffset.xyz
        + float3(qx, qy, qz) * sceneData.ClusterDagPackedPositionScale.xyz;
}

float2 DecodeClusterDagPackedUV(uint packedUV)
{
    return float2(
        f16tof32(packedUV & 0xFFFFu),
        f16tof32(packedUV >> 16u));
}

float4 DecodeClusterDagPackedTangent(uint packedTangent)
{
    const float3 tangent = normalize(float3(
        DecodeSnorm10(packedTangent & 0x3FFu),
        DecodeSnorm10((packedTangent >> 10u) & 0x3FFu),
        DecodeSnorm10((packedTangent >> 20u) & 0x3FFu)));
    const float tangentSign = ((packedTangent >> 30u) & 0x1u) != 0u ? -1.0f : 1.0f;
    return float4(tangent, tangentSign);
}

float4 DecodeClusterDagPackedColor(uint packedColor)
{
    const float inv255 = 1.0f / 255.0f;
    return float4(
        (packedColor & 0xFFu) * inv255,
        ((packedColor >> 8u) & 0xFFu) * inv255,
        ((packedColor >> 16u) & 0xFFu) * inv255,
        ((packedColor >> 24u) & 0xFFu) * inv255);
}

float3 BuildClusterDagFallbackTangentDirection(float3 normal)
{
    const float3 up = abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    return normalize(cross(up, normal));
}

float4 BuildClusterDagFallbackTangent(float3 normal)
{
    return float4(BuildClusterDagFallbackTangentDirection(normal), 1.0f);
}

float3 InterpolateFloat3(float3 v0, float3 v1, float3 v2, float2 barycentrics)
{
    return v0 * (1.0f - barycentrics.x - barycentrics.y) + v1 * barycentrics.x + v2 * barycentrics.y;
}

float2 InterpolateFloat2(float2 v0, float2 v1, float2 v2, float2 barycentrics)
{
    return v0 * (1.0f - barycentrics.x - barycentrics.y) + v1 * barycentrics.x + v2 * barycentrics.y;
}

float4 InterpolateFloat4(float4 v0, float4 v1, float4 v2, float2 barycentrics)
{
    return v0 * (1.0f - barycentrics.x - barycentrics.y) + v1 * barycentrics.x + v2 * barycentrics.y;
}

bool IsFeatureEnabled(uint pipelineKey, uint bitIndex)
{
    return ((pipelineKey >> bitIndex) & 1u) != 0u;
}

bool ComputeFrontFace(float4 clip0, float4 clip1, float4 clip2)
{
    const float2 ndc0 = clip0.xy / max(abs(clip0.w), 1e-6f);
    const float2 ndc1 = clip1.xy / max(abs(clip1.w), 1e-6f);
    const float2 ndc2 = clip2.xy / max(abs(clip2.w), 1e-6f);
    const float signedArea =
        (ndc1.x - ndc0.x) * (ndc2.y - ndc0.y) -
        (ndc1.y - ndc0.y) * (ndc2.x - ndc0.x);
    return signedArea >= 0.0f;
}

void LoadTriangleIndices(
    ClusterDagResolveSceneData sceneData,
    ClusterDagDrawData drawData,
    uint primitiveId,
    out uint index0,
    out uint index1,
    out uint index2)
{
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[sceneData.ExtraBindlessIndices.y];
    const uint baseIndex = drawData.StartIndex + primitiveId * 3u;
    index0 = IndexBuffer[baseIndex + 0u];
    index1 = IndexBuffer[baseIndex + 1u];
    index2 = IndexBuffer[baseIndex + 2u];
}

float3 LoadPosition(ClusterDagResolveSceneData sceneData, uint vertexIndex)
{
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.x];
    StructuredBuffer<ClusterDagPackedPosition> PackedPositionBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.x];
    return sceneData.ClusterDagVertexPackingMode != 0u
        ? DecodeClusterDagPackedPosition(PackedPositionBuffer[vertexIndex], sceneData)
        : PositionBuffer[vertexIndex];
}

float3 LoadNormal(ClusterDagResolveSceneData sceneData, uint vertexIndex)
{
    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.y];
    StructuredBuffer<uint> PackedNormalBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.y];
    return sceneData.ClusterDagVertexPackingMode != 0u
        ? DecodeOctahedral16x2(PackedNormalBuffer[vertexIndex])
        : NormalBuffer[vertexIndex];
}

float2 LoadUv(ClusterDagResolveSceneData sceneData, uint vertexIndex)
{
    if (sceneData.ClusterDagVertexPackingMode != 0u)
    {
        if (sceneData.VertexBufferBindlessIndices.z != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedTexCoordBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.z];
            return DecodeClusterDagPackedUV(PackedTexCoordBuffer[vertexIndex]);
        }

        return sceneData.ClusterDagPackedConstantUV.xy;
    }

    StructuredBuffer<float2> TexCoordBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.z];
    return TexCoordBuffer[vertexIndex];
}

float4 LoadTangent(ClusterDagResolveSceneData sceneData, uint vertexIndex, float3 normal)
{
    if (sceneData.ClusterDagVertexPackingMode != 0u)
    {
        if (sceneData.VertexBufferBindlessIndices.w != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedTangentBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.w];
            return DecodeClusterDagPackedTangent(PackedTangentBuffer[vertexIndex]);
        }

        return BuildClusterDagFallbackTangent(normal);
    }

    StructuredBuffer<float4> TangentBuffer = ResourceDescriptorHeap[sceneData.VertexBufferBindlessIndices.w];
    return TangentBuffer[vertexIndex];
}

float4 LoadColor(ClusterDagResolveSceneData sceneData, uint vertexIndex)
{
    if (sceneData.ClusterDagVertexPackingMode != 0u)
    {
        if (sceneData.ExtraBindlessIndices.x != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedColorBuffer = ResourceDescriptorHeap[sceneData.ExtraBindlessIndices.x];
            return DecodeClusterDagPackedColor(PackedColorBuffer[vertexIndex]);
        }

        return sceneData.ClusterDagPackedConstantColor;
    }

    StructuredBuffer<float4> ColorBuffer = ResourceDescriptorHeap[sceneData.ExtraBindlessIndices.x];
    return ColorBuffer[vertexIndex];
}

VSOutput ClusterDagResolveVS(uint vertexId : SV_VertexID)
{
    VSOutput Output;
    Output.UV = float2((vertexId << 1) & 2, vertexId & 2);
    Output.Position = float4(Output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return Output;
}

PSOutput ClusterDagResolvePS(VSOutput Input)
{
    Texture2D<uint2> VisibilityTexture0 = ResourceDescriptorHeap[VisibilityTextureIndex0];
    Texture2D<float2> VisibilityTexture1 = ResourceDescriptorHeap[VisibilityTextureIndex1];
    StructuredBuffer<ClusterDagDrawData> DrawDatas = ResourceDescriptorHeap[DrawDataBufferIndex];
    StructuredBuffer<ClusterDagResolveSceneData> SceneDatas = ResourceDescriptorHeap[SceneDataBufferIndex];

    const int2 pixelPosition = int2(Input.Position.xy);
    const uint2 visibility = VisibilityTexture0.Load(int3(pixelPosition, 0));
    if (visibility.x == 0u)
    {
        clip(-1.0f);
    }

    const uint drawDataIndex = visibility.x - 1u;
    const uint primitiveId = visibility.y;
    const float2 barycentrics = VisibilityTexture1.Load(int3(pixelPosition, 0));
    const ClusterDagDrawData drawData = DrawDatas[drawDataIndex];
    const ClusterDagResolveSceneData sceneData = SceneDatas[drawData.ModelIndex];

    uint vertexIndex0 = 0u;
    uint vertexIndex1 = 0u;
    uint vertexIndex2 = 0u;
    LoadTriangleIndices(sceneData, drawData, primitiveId, vertexIndex0, vertexIndex1, vertexIndex2);

    const float3 localPosition0 = LoadPosition(sceneData, vertexIndex0);
    const float3 localPosition1 = LoadPosition(sceneData, vertexIndex1);
    const float3 localPosition2 = LoadPosition(sceneData, vertexIndex2);
    const float3 localNormal0 = LoadNormal(sceneData, vertexIndex0);
    const float3 localNormal1 = LoadNormal(sceneData, vertexIndex1);
    const float3 localNormal2 = LoadNormal(sceneData, vertexIndex2);
    const float2 uv0 = LoadUv(sceneData, vertexIndex0);
    const float2 uv1 = LoadUv(sceneData, vertexIndex1);
    const float2 uv2 = LoadUv(sceneData, vertexIndex2);
    const float4 tangent0 = LoadTangent(sceneData, vertexIndex0, localNormal0);
    const float4 tangent1 = LoadTangent(sceneData, vertexIndex1, localNormal1);
    const float4 tangent2 = LoadTangent(sceneData, vertexIndex2, localNormal2);
    const float4 color0 = LoadColor(sceneData, vertexIndex0);
    const float4 color1 = LoadColor(sceneData, vertexIndex1);
    const float4 color2 = LoadColor(sceneData, vertexIndex2);

    const float3 localPosition = InterpolateFloat3(localPosition0, localPosition1, localPosition2, barycentrics);
    const float3 localNormal = normalize(InterpolateFloat3(localNormal0, localNormal1, localNormal2, barycentrics));
    const float2 uv = InterpolateFloat2(uv0, uv1, uv2, barycentrics);
    const float4 tangent = InterpolateFloat4(tangent0, tangent1, tangent2, barycentrics);
    const float4 color = InterpolateFloat4(color0, color1, color2, barycentrics);

    const float4 worldPosition = mul(float4(localPosition, 1.0f), sceneData.World);
    float3 worldNormal = normalize(mul(localNormal, (float3x3)sceneData.WorldInverseTranspose));
    const float4 worldClip0 = mul(mul(float4(localPosition0, 1.0f), sceneData.World), sceneData.View);
    const float4 worldClip1 = mul(mul(float4(localPosition1, 1.0f), sceneData.World), sceneData.View);
    const float4 worldClip2 = mul(mul(float4(localPosition2, 1.0f), sceneData.World), sceneData.View);
    const float4 clip0 = mul(worldClip0, sceneData.Projection);
    const float4 clip1 = mul(worldClip1, sceneData.Projection);
    const float4 clip2 = mul(worldClip2, sceneData.Projection);
    const bool isFrontFace = ComputeFrontFace(clip0, clip1, clip2);

    float4 worldTangent = float4(normalize(mul(tangent.xyz, (float3x3)sceneData.WorldInverseTranspose)), tangent.w);
    const uint pipelineKey = sceneData.ClusterDagMaterialPipelineKey;
    const bool useNormalMap = IsFeatureEnabled(pipelineKey, 0u);
    const bool useMetallicRoughnessMap = IsFeatureEnabled(pipelineKey, 1u);
    const bool useBaseColorMap = IsFeatureEnabled(pipelineKey, 2u);
    const bool useEmissiveMap = IsFeatureEnabled(pipelineKey, 3u);
    const bool useSheen = IsFeatureEnabled(pipelineKey, 5u);
    const bool useClearcoat = IsFeatureEnabled(pipelineKey, 6u);
    const bool useAnisotropy = IsFeatureEnabled(pipelineKey, 7u);
    const bool useDoubleSided = IsFeatureEnabled(pipelineKey, 9u);

    const uint albedoTextureIndex = sceneData.MaterialTextureIndices0.x;
    const uint metallicRoughnessTextureIndex = sceneData.MaterialTextureIndices0.y;
    const uint normalTextureIndex = sceneData.MaterialTextureIndices0.z;
    const uint emissiveTextureIndex = sceneData.MaterialTextureIndices0.w;
    const uint sheenColorTextureIndex = sceneData.MaterialTextureIndices1.x;
    const uint sheenRoughnessTextureIndex = sceneData.MaterialTextureIndices1.y;
    const uint clearcoatTextureIndex = sceneData.MaterialTextureIndices1.z;
    const uint clearcoatRoughnessTextureIndex = sceneData.MaterialTextureIndices1.w;
    const uint clearcoatNormalTextureIndex = sceneData.MaterialTextureIndices2.x;
    const uint anisotropyTextureIndex = sceneData.MaterialTextureIndices2.y;

    const float2 baseUV = ApplyTextureTransform(uv, sceneData.BaseColorTransformOffsetScale, sceneData.BaseColorTransformRotation);
    const float2 mrUV = ApplyTextureTransform(uv, sceneData.MetallicRoughnessTransformOffsetScale, sceneData.MetallicRoughnessTransformRotation);
    const float2 normalUV = ApplyTextureTransform(uv, sceneData.NormalTransformOffsetScale, sceneData.NormalTransformRotation);
    const float2 emissiveUV = ApplyTextureTransform(uv, sceneData.EmissiveTransformOffsetScale, sceneData.EmissiveTransformRotation);
    const float2 sheenColorUV = ApplyTextureTransform(uv, sceneData.SheenColorTransformOffsetScale, sceneData.SheenColorTransformRotation);
    const float2 sheenRoughnessUV = ApplyTextureTransform(uv, sceneData.SheenRoughnessTransformOffsetScale, sceneData.SheenRoughnessTransformRotation);
    const float2 clearcoatUV = ApplyTextureTransform(uv, sceneData.ClearcoatTransformOffsetScale, sceneData.ClearcoatTransformRotation);
    const float2 clearcoatRoughnessUV = ApplyTextureTransform(uv, sceneData.ClearcoatRoughnessTransformOffsetScale, sceneData.ClearcoatRoughnessTransformRotation);
    const float2 clearcoatNormalUV = ApplyTextureTransform(uv, sceneData.ClearcoatNormalTransformOffsetScale, sceneData.ClearcoatNormalTransformRotation);
    const float2 anisotropyUV = ApplyTextureTransform(uv, sceneData.AnisotropyTransformOffsetScale, sceneData.AnisotropyTransformRotation);

    if (useNormalMap && normalTextureIndex != 0xffffffffu)
    {
        Texture2D NormalTexture = ResourceDescriptorHeap[normalTextureIndex];
        const float2 tangentNormalRG = NormalTexture.Sample(MaterialSampler, normalUV).rg * 2.0f - 1.0f;
        const float3 tangentNormal = DecodeTangentNormalRG(tangentNormalRG);
        worldNormal = ComputeWorldNormal(worldNormal, worldTangent, tangentNormal);
    }
    if (useClearcoat && clearcoatNormalTextureIndex != 0xffffffffu)
    {
        Texture2D ClearcoatNormalTexture = ResourceDescriptorHeap[clearcoatNormalTextureIndex];
        const float2 clearcoatTangentNormalRG = ClearcoatNormalTexture.Sample(MaterialSampler, clearcoatNormalUV).rg * 2.0f - 1.0f;
        const float3 clearcoatTangentNormal = DecodeTangentNormalRG(clearcoatTangentNormalRG);
        worldNormal = ComputeWorldNormal(worldNormal, worldTangent, clearcoatTangentNormal);
    }
    if (useDoubleSided && !isFrontFace)
    {
        worldNormal = -worldNormal;
    }

    float3 albedo = sceneData.BaseColor * color.rgb;
    float alpha = sceneData.BaseColorAlpha * color.a;
    if (useBaseColorMap && albedoTextureIndex != 0xffffffffu)
    {
        Texture2D AlbedoTexture = ResourceDescriptorHeap[albedoTextureIndex];
        const float4 albedoSample = AlbedoTexture.Sample(MaterialSampler, baseUV);
        albedo *= albedoSample.rgb;
        alpha *= albedoSample.a;
    }

    float metallic = sceneData.MetallicFactor;
    float roughness = sceneData.RoughnessFactor;
    if (useMetallicRoughnessMap && metallicRoughnessTextureIndex != 0xffffffffu)
    {
        Texture2D MetallicRoughnessTexture = ResourceDescriptorHeap[metallicRoughnessTextureIndex];
        const float2 metallicRoughness = MetallicRoughnessTexture.Sample(MaterialSampler, mrUV).bg;
        metallic *= metallicRoughness.x;
        roughness *= metallicRoughness.y;
    }

    float4 customData = 0.0f.xxxx;
    const bool clusterDagDebugView =
        sceneData.DeferredLightingVisualizationMode == LIGHTING_VISUALIZATION_CLUSTER_DAG_CLUSTERS
        || sceneData.DeferredLightingVisualizationMode == LIGHTING_VISUALIZATION_CLUSTER_DAG_MIP;
    if (clusterDagDebugView)
    {
        if (sceneData.ExtraBindlessIndices.z != 0xffffffffu)
        {
            StructuredBuffer<uint> ClusterDebugColorBuffer = ResourceDescriptorHeap[sceneData.ExtraBindlessIndices.z];
            customData = DecodeDebugColor(ClusterDebugColorBuffer[drawData.StartIndex]);
        }
    }
    else if (useSheen)
    {
        float3 sheenColor = sceneData.SheenColorFactor;
        float sheenRoughness = sceneData.SheenRoughnessFactor;
        if (sheenColorTextureIndex != 0xffffffffu)
        {
            Texture2D SheenColorTexture = ResourceDescriptorHeap[sheenColorTextureIndex];
            sheenColor *= SheenColorTexture.Sample(MaterialSampler, sheenColorUV).rgb;
        }
        if (sheenRoughnessTextureIndex != 0xffffffffu)
        {
            Texture2D SheenRoughnessTexture = ResourceDescriptorHeap[sheenRoughnessTextureIndex];
            sheenRoughness *= SheenRoughnessTexture.Sample(MaterialSampler, sheenRoughnessUV).a;
        }
        customData = float4(sheenColor, sheenRoughness);
    }
    else if (useClearcoat)
    {
        float clearcoat = sceneData.ClearcoatFactor;
        float clearcoatRoughness = sceneData.ClearcoatRoughnessFactor;
        if (clearcoatTextureIndex != 0xffffffffu)
        {
            Texture2D ClearcoatTexture = ResourceDescriptorHeap[clearcoatTextureIndex];
            clearcoat *= ClearcoatTexture.Sample(MaterialSampler, clearcoatUV).r;
        }
        if (clearcoatRoughnessTextureIndex != 0xffffffffu)
        {
            Texture2D ClearcoatRoughnessTexture = ResourceDescriptorHeap[clearcoatRoughnessTextureIndex];
            clearcoatRoughness *= ClearcoatRoughnessTexture.Sample(MaterialSampler, clearcoatRoughnessUV).g;
        }
        customData = float4(clearcoat, clearcoatRoughness, 0.0f, 0.0f);
    }
    else if (useAnisotropy)
    {
        float anisotropyValue = 1.0f;
        if (anisotropyTextureIndex != 0xffffffffu)
        {
            Texture2D AnisotropyTexture = ResourceDescriptorHeap[anisotropyTextureIndex];
            anisotropyValue = AnisotropyTexture.Sample(MaterialSampler, anisotropyUV).r;
        }
        customData = float4(anisotropyValue, sceneData.AnisotropyStrength, 0.0f, 0.0f);
    }

    float3 emissive = sceneData.EmissiveFactor;
    if (useEmissiveMap && emissiveTextureIndex != 0xffffffffu)
    {
        Texture2D EmissiveTexture = ResourceDescriptorHeap[emissiveTextureIndex];
        emissive *= EmissiveTexture.Sample(MaterialSampler, emissiveUV).rgb;
    }

    PSOutput Output;
    Output.GBufferA = float4(worldNormal * 0.5f + 0.5f, 1.0f);
    Output.GBufferB = float4(0.04f, metallic, roughness, (float)sceneData.ShadingModelId);
    Output.GBufferC = float4(albedo, 1.0f);
    Output.GBufferD = customData;
    Output.SceneColor = float4(emissive, 1.0f);
    return Output;
}
