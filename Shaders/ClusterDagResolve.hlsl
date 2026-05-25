#include "Common.hlsli"
#include "ClusterDag/ClusterDagGeometryFetch.hlsl"
#include "../Source/Core/LightingVisualizationShared.h"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

cbuffer ClusterDagResolveBindlessConstants : register(b0)
{
    uint Visibility64TextureIndex;
    uint VisibleEntryBufferIndex;
    uint DrawDataBufferIndex;
    uint SceneDataBufferIndex;
    uint PageDataBufferIndex;
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

struct TextureUvGradients
{
    float2 UV;
    float2 Ddx;
    float2 Ddy;
};

struct TriangleBarycentrics
{
    float2 Value;
    float2 OffsetX;
    float2 OffsetY;
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
    const float2 ndc0 = clip0.xy * rcp(clip0.w);
    const float2 ndc1 = clip1.xy * rcp(clip1.w);
    const float2 ndc2 = clip2.xy * rcp(clip2.w);
    const float signedArea =
        (ndc1.x - ndc0.x) * (ndc2.y - ndc0.y) -
        (ndc1.y - ndc0.y) * (ndc2.x - ndc0.x);
    return signedArea >= 0.0f;
}

float Cross2D(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}

TriangleBarycentrics ComputePerspectiveBarycentrics(float2 pixelCenter, float4 clip0, float4 clip1, float4 clip2, float2 viewportSize)
{
    TriangleBarycentrics Result;

    const float2 invViewportSize = rcp(viewportSize);
    const float2 pixelNdc = pixelCenter * invViewportSize * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);

    const float3 signedInvW = rcp(float3(clip0.w, clip1.w, clip2.w));
    const float2 ndc0 = clip0.xy * signedInvW.x;
    const float2 ndc1 = clip1.xy * signedInvW.y;
    const float2 ndc2 = clip2.xy * signedInvW.z;

    const float2 edge12 = ndc2 - ndc1;
    const float2 edge20 = ndc0 - ndc2;
    const float2 edge01 = ndc1 - ndc0;

    const float3 edgeArea = float3(
        Cross2D(pixelNdc - ndc1, edge12),
        Cross2D(pixelNdc - ndc2, edge20),
        Cross2D(pixelNdc - ndc0, edge01));

    const float3 edgeAreaNdcDx = float3(edge12.y, edge20.y, edge01.y);
    const float3 edgeAreaNdcDy = -float3(edge12.x, edge20.x, edge01.x);

    const float3 weightedArea = edgeArea * signedInvW;
    const float weightedAreaSum = dot(edgeArea, signedInvW);
    const float invWeightedAreaSum = rcp(weightedAreaSum);
    const float3 barycentric3 = weightedArea * invWeightedAreaSum;

    const float3 weightedAreaNdcDx = edgeAreaNdcDx * signedInvW;
    const float3 weightedAreaNdcDy = edgeAreaNdcDy * signedInvW;
    const float weightedAreaSumNdcDx = dot(edgeAreaNdcDx, signedInvW);
    const float weightedAreaSumNdcDy = dot(edgeAreaNdcDy, signedInvW);
    const float invWeightedAreaSum2 = invWeightedAreaSum * invWeightedAreaSum;

    const float3 barycentricNdcDx = (weightedAreaNdcDx * weightedAreaSum - weightedArea * weightedAreaSumNdcDx) * invWeightedAreaSum2;
    const float3 barycentricNdcDy = (weightedAreaNdcDy * weightedAreaSum - weightedArea * weightedAreaSumNdcDy) * invWeightedAreaSum2;
    const float3 barycentricPixelDx = barycentricNdcDx * (2.0f * invViewportSize.x);
    const float3 barycentricPixelDy = barycentricNdcDy * (-2.0f * invViewportSize.y);

    Result.Value = barycentric3.yz;
    Result.OffsetX = barycentric3.yz + barycentricPixelDx.yz;
    Result.OffsetY = barycentric3.yz + barycentricPixelDy.yz;
    return Result;
}

float3 BuildPixelRayDirection(float2 pixelCenter, float2 viewportSize, row_major float4x4 viewProjectionInverse, float3 cameraPosition)
{
    const float2 uv = pixelCenter / viewportSize;
    const float3 nearWorld = ReconstructWorldPosition(uv, 1.0f, viewProjectionInverse);
    return normalize(nearWorld - cameraPosition);
}

float2 ComputeRayTriangleBarycentrics(float3 rayOrigin, float3 rayDirection, float3 p0, float3 p1, float3 p2)
{
    const float3 edge1 = p1 - p0;
    const float3 edge2 = p2 - p0;
    const float3 pvec = cross(rayDirection, edge2);
    const float det = dot(edge1, pvec);
    if (abs(det) < 1e-12f)
    {
        return float2(0.0f, 0.0f);
    }

    const float invDet = rcp(det);
    const float3 tvec = rayOrigin - p0;
    const float u = dot(tvec, pvec) * invDet;
    const float3 qvec = cross(tvec, edge1);
    const float v = dot(rayDirection, qvec) * invDet;
    return float2(u, v);
}

TextureUvGradients BuildTextureUvGradients(float2 uvCenter, float2 uvOffsetX, float2 uvOffsetY, float4 offsetScale, float2 rotation)
{
    TextureUvGradients Gradients;
    Gradients.UV = ApplyTextureTransform(uvCenter, offsetScale, rotation);
    const float2 uvX = ApplyTextureTransform(uvOffsetX, offsetScale, rotation);
    const float2 uvY = ApplyTextureTransform(uvOffsetY, offsetScale, rotation);
    Gradients.Ddx = uvX - Gradients.UV;
    Gradients.Ddy = uvY - Gradients.UV;
    return Gradients;
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
    Texture2D<uint2> Visibility64Texture = ResourceDescriptorHeap[Visibility64TextureIndex];
    StructuredBuffer<ClusterDagVisibleEntry> VisibleEntries = ResourceDescriptorHeap[VisibleEntryBufferIndex];
    StructuredBuffer<ClusterDagDrawData> DrawDatas = ResourceDescriptorHeap[DrawDataBufferIndex];
    StructuredBuffer<ClusterDagResolveSceneData> SceneDatas = ResourceDescriptorHeap[SceneDataBufferIndex];
    ByteAddressBuffer PageData = ResourceDescriptorHeap[PageDataBufferIndex];

    const int2 pixelPosition = int2(Input.Position.xy);
    uint width = 0u;
    uint height = 0u;
    Visibility64Texture.GetDimensions(width, height);

    const uint2 visibility = Visibility64Texture.Load(int3(pixelPosition, 0));
    const uint pixelValue = visibility.x;
    if (pixelValue == 0u)
    {
        clip(-1.0f);
    }

    const uint visibleEntryIndex = (pixelValue >> 7u) - 1u;
    const uint primitiveId = pixelValue & 0x7fu;
    const ClusterDagVisibleEntry visibleEntry = VisibleEntries[visibleEntryIndex];
    ClusterDagVisibleEntry geometryEntry = visibleEntry;
    const uint drawDataIndex = visibleEntry.DrawDataIndex;
    ClusterDagDrawData drawData = DrawDatas[drawDataIndex];
    ClusterDagDrawData pagedDrawData;
    if (TryLoadClusterDagVisibleEntryDrawData(visibleEntry, drawDataIndex, PageData, pagedDrawData))
    {
        drawData = pagedDrawData;
    }
    else
    {
        geometryEntry.PageDataBase = 0xffffffffu;
    }
    const ClusterDagResolveSceneData sceneData = SceneDatas[drawData.ModelIndex];

    uint vertexIndex0 = 0u;
    uint vertexIndex1 = 0u;
    uint vertexIndex2 = 0u;
    if (!LoadClusterDagTriangleIndices(sceneData, geometryEntry, drawData, primitiveId, PageData, vertexIndex0, vertexIndex1, vertexIndex2))
    {
        clip(-1.0f);
    }

    const float3 localPosition0 = LoadClusterDagPosition(sceneData, geometryEntry, vertexIndex0, PageData);
    const float3 localPosition1 = LoadClusterDagPosition(sceneData, geometryEntry, vertexIndex1, PageData);
    const float3 localPosition2 = LoadClusterDagPosition(sceneData, geometryEntry, vertexIndex2, PageData);
    const float3 localNormal0 = LoadClusterDagNormal(sceneData, geometryEntry, vertexIndex0, PageData);
    const float3 localNormal1 = LoadClusterDagNormal(sceneData, geometryEntry, vertexIndex1, PageData);
    const float3 localNormal2 = LoadClusterDagNormal(sceneData, geometryEntry, vertexIndex2, PageData);
    const float2 uv0 = LoadClusterDagUv(sceneData, geometryEntry, vertexIndex0, PageData);
    const float2 uv1 = LoadClusterDagUv(sceneData, geometryEntry, vertexIndex1, PageData);
    const float2 uv2 = LoadClusterDagUv(sceneData, geometryEntry, vertexIndex2, PageData);
    const float4 tangent0 = LoadClusterDagTangent(sceneData, geometryEntry, vertexIndex0, localNormal0, PageData);
    const float4 tangent1 = LoadClusterDagTangent(sceneData, geometryEntry, vertexIndex1, localNormal1, PageData);
    const float4 tangent2 = LoadClusterDagTangent(sceneData, geometryEntry, vertexIndex2, localNormal2, PageData);
    const float4 color0 = LoadClusterDagColor(sceneData, geometryEntry, vertexIndex0, PageData);
    const float4 color1 = LoadClusterDagColor(sceneData, geometryEntry, vertexIndex1, PageData);
    const float4 color2 = LoadClusterDagColor(sceneData, geometryEntry, vertexIndex2, PageData);

    const float4 worldClip0 = mul(mul(float4(localPosition0, 1.0f), sceneData.World), sceneData.View);
    const float4 worldClip1 = mul(mul(float4(localPosition1, 1.0f), sceneData.World), sceneData.View);
    const float4 worldClip2 = mul(mul(float4(localPosition2, 1.0f), sceneData.World), sceneData.View);
    const float4 clip0 = mul(worldClip0, sceneData.Projection);
    const float4 clip1 = mul(worldClip1, sceneData.Projection);
    const float4 clip2 = mul(worldClip2, sceneData.Projection);
    const bool isFrontFace = ComputeFrontFace(clip0, clip1, clip2);

    const uint pipelineKey = sceneData.ClusterDagMaterialPipelineKey;
    const float2 viewportSize = float2(width, height);
    const float2 pixelCenter = float2(pixelPosition) + 0.5f;
    const TriangleBarycentrics triangleBarycentrics = ComputePerspectiveBarycentrics(pixelCenter, clip0, clip1, clip2, viewportSize);
    const float2 barycentrics = triangleBarycentrics.Value;
    const float2 barycentricsDx = triangleBarycentrics.OffsetX;
    const float2 barycentricsDy = triangleBarycentrics.OffsetY;
    const float3 localPosition = InterpolateFloat3(localPosition0, localPosition1, localPosition2, barycentrics);
    const float3 localNormal = normalize(InterpolateFloat3(localNormal0, localNormal1, localNormal2, barycentrics));
    const float2 uv = InterpolateFloat2(uv0, uv1, uv2, barycentrics);
    const float2 uvDx = InterpolateFloat2(uv0, uv1, uv2, barycentricsDx);
    const float2 uvDy = InterpolateFloat2(uv0, uv1, uv2, barycentricsDy);
    const float4 tangent = InterpolateFloat4(tangent0, tangent1, tangent2, barycentrics);
    const float4 color = InterpolateFloat4(color0, color1, color2, barycentrics);
    const float4 worldPosition = mul(float4(localPosition, 1.0f), sceneData.World);
    float3 worldNormal = normalize(mul(localNormal, (float3x3)sceneData.WorldInverseTranspose));
    float4 worldTangent = float4(normalize(mul(tangent.xyz, (float3x3)sceneData.WorldInverseTranspose)), tangent.w);

    const bool useNormalMap = IsFeatureEnabled(pipelineKey, 0u);
    const bool useMetallicRoughnessMap = IsFeatureEnabled(pipelineKey, 1u);
    const bool useBaseColorMap = IsFeatureEnabled(pipelineKey, 2u);
    const bool useEmissiveMap = IsFeatureEnabled(pipelineKey, 3u);
    const bool useSheen = IsFeatureEnabled(pipelineKey, 5u);
    const bool useClearcoat = IsFeatureEnabled(pipelineKey, 6u);
    const bool useAnisotropy = IsFeatureEnabled(pipelineKey, 7u);
    const bool useDoubleSided = IsFeatureEnabled(pipelineKey, kClusterDagPipelineKeyDoubleSidedBit);

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

    const TextureUvGradients baseUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.BaseColorTransformOffsetScale, sceneData.BaseColorTransformRotation);
    const TextureUvGradients mrUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.MetallicRoughnessTransformOffsetScale, sceneData.MetallicRoughnessTransformRotation);
    const TextureUvGradients normalUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.NormalTransformOffsetScale, sceneData.NormalTransformRotation);
    const TextureUvGradients emissiveUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.EmissiveTransformOffsetScale, sceneData.EmissiveTransformRotation);
    const TextureUvGradients sheenColorUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.SheenColorTransformOffsetScale, sceneData.SheenColorTransformRotation);
    const TextureUvGradients sheenRoughnessUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.SheenRoughnessTransformOffsetScale, sceneData.SheenRoughnessTransformRotation);
    const TextureUvGradients clearcoatUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.ClearcoatTransformOffsetScale, sceneData.ClearcoatTransformRotation);
    const TextureUvGradients clearcoatRoughnessUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.ClearcoatRoughnessTransformOffsetScale, sceneData.ClearcoatRoughnessTransformRotation);
    const TextureUvGradients clearcoatNormalUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.ClearcoatNormalTransformOffsetScale, sceneData.ClearcoatNormalTransformRotation);
    const TextureUvGradients anisotropyUvGradients = BuildTextureUvGradients(uv, uvDx, uvDy, sceneData.AnisotropyTransformOffsetScale, sceneData.AnisotropyTransformRotation);

    if (useNormalMap && normalTextureIndex != 0xffffffffu)
    {
        Texture2D NormalTexture = ResourceDescriptorHeap[normalTextureIndex];
        const float2 tangentNormalRG = NormalTexture.SampleGrad(MaterialSampler, normalUvGradients.UV, normalUvGradients.Ddx, normalUvGradients.Ddy).rg * 2.0f - 1.0f;
        const float3 tangentNormal = DecodeTangentNormalRG(tangentNormalRG);
        worldNormal = ComputeWorldNormal(worldNormal, worldTangent, tangentNormal);
    }
    if (useClearcoat && clearcoatNormalTextureIndex != 0xffffffffu)
    {
        Texture2D ClearcoatNormalTexture = ResourceDescriptorHeap[clearcoatNormalTextureIndex];
        const float2 clearcoatTangentNormalRG = ClearcoatNormalTexture.SampleGrad(MaterialSampler, clearcoatNormalUvGradients.UV, clearcoatNormalUvGradients.Ddx, clearcoatNormalUvGradients.Ddy).rg * 2.0f - 1.0f;
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
        const float4 albedoSample = AlbedoTexture.SampleGrad(MaterialSampler, baseUvGradients.UV, baseUvGradients.Ddx, baseUvGradients.Ddy);
        albedo *= albedoSample.rgb;
        alpha *= albedoSample.a;
    }

    float metallic = sceneData.MetallicFactor;
    float roughness = sceneData.RoughnessFactor;
    if (useMetallicRoughnessMap && metallicRoughnessTextureIndex != 0xffffffffu)
    {
        Texture2D MetallicRoughnessTexture = ResourceDescriptorHeap[metallicRoughnessTextureIndex];
        const float2 metallicRoughness = MetallicRoughnessTexture.SampleGrad(MaterialSampler, mrUvGradients.UV, mrUvGradients.Ddx, mrUvGradients.Ddy).bg;
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
            sheenColor *= SheenColorTexture.SampleGrad(MaterialSampler, sheenColorUvGradients.UV, sheenColorUvGradients.Ddx, sheenColorUvGradients.Ddy).rgb;
        }
        if (sheenRoughnessTextureIndex != 0xffffffffu)
        {
            Texture2D SheenRoughnessTexture = ResourceDescriptorHeap[sheenRoughnessTextureIndex];
            sheenRoughness *= SheenRoughnessTexture.SampleGrad(MaterialSampler, sheenRoughnessUvGradients.UV, sheenRoughnessUvGradients.Ddx, sheenRoughnessUvGradients.Ddy).a;
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
            clearcoat *= ClearcoatTexture.SampleGrad(MaterialSampler, clearcoatUvGradients.UV, clearcoatUvGradients.Ddx, clearcoatUvGradients.Ddy).r;
        }
        if (clearcoatRoughnessTextureIndex != 0xffffffffu)
        {
            Texture2D ClearcoatRoughnessTexture = ResourceDescriptorHeap[clearcoatRoughnessTextureIndex];
            clearcoatRoughness *= ClearcoatRoughnessTexture.SampleGrad(MaterialSampler, clearcoatRoughnessUvGradients.UV, clearcoatRoughnessUvGradients.Ddx, clearcoatRoughnessUvGradients.Ddy).g;
        }
        customData = float4(clearcoat, clearcoatRoughness, 0.0f, 0.0f);
    }
    else if (useAnisotropy)
    {
        float anisotropyValue = 1.0f;
        if (anisotropyTextureIndex != 0xffffffffu)
        {
            Texture2D AnisotropyTexture = ResourceDescriptorHeap[anisotropyTextureIndex];
            anisotropyValue = AnisotropyTexture.SampleGrad(MaterialSampler, anisotropyUvGradients.UV, anisotropyUvGradients.Ddx, anisotropyUvGradients.Ddy).r;
        }
        customData = float4(anisotropyValue, sceneData.AnisotropyStrength, 0.0f, 0.0f);
    }

    float3 emissive = sceneData.EmissiveFactor;
    if (useEmissiveMap && emissiveTextureIndex != 0xffffffffu)
    {
        Texture2D EmissiveTexture = ResourceDescriptorHeap[emissiveTextureIndex];
        emissive *= EmissiveTexture.SampleGrad(MaterialSampler, emissiveUvGradients.UV, emissiveUvGradients.Ddx, emissiveUvGradients.Ddy).rgb;
    }

    PSOutput Output;
    Output.GBufferA = float4(worldNormal * 0.5f + 0.5f, 1.0f);
    Output.GBufferB = float4(0.04f, metallic, roughness, (float)sceneData.ShadingModelId);
    Output.GBufferC = float4(albedo, 1.0f);
    Output.GBufferD = customData;
    Output.SceneColor = float4(emissive, 1.0f);
    return Output;
}
