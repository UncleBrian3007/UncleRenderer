#ifndef SCENE_CONSTANTS_FIELDS_HLSLI
#define SCENE_CONSTANTS_FIELDS_HLSLI

#define SCENE_CONSTANTS_FIELDS \
    row_major float4x4 World; \
    row_major float4x4 WorldInverseTranspose; \
    row_major float4x4 View; \
    row_major float4x4 ViewInverse; \
    row_major float4x4 Projection; \
    row_major float4x4 ViewProjectionInverse; \
    row_major float4x4 PreviousWorld; \
    uint PaddingPrevVP; \
    uint HasPreviousWorld; \
    uint HasPreviousSkinning; \
    uint PreviousSkinnedPositionBindlessIndex; \
    float3 BaseColor; \
    float LightIntensity; \
    float3 LightDirection; \
    float LightRadius; \
    float3 CameraPosition; \
    float Padding2; \
    float3 LightColor; \
    float Padding3; \
    float3 EmissiveFactor; \
    float Padding4; \
    row_major float4x4 LightViewProjection; \
    float ShadowStrength; \
    float ShadowBias; \
    float2 ShadowMapSize; \
    float MetallicFactor; \
    float RoughnessFactor; \
    float BaseColorAlpha; \
    float AlphaCutoff; \
    uint AlphaMode; \
    uint3 PaddingMaterial; \
    float3 SheenColorFactor; \
    float SheenRoughnessFactor; \
    uint ShadingModelId; \
    uint3 PaddingShadingModel; \
    float ClearcoatFactor; \
    float ClearcoatRoughnessFactor; \
    float2 PaddingClearcoat; \
    float AnisotropyStrength; \
    float AnisotropyRotation; \
    float2 PaddingAnisotropy; \
    float4 BaseColorTransformOffsetScale; \
    float4 MetallicRoughnessTransformOffsetScale; \
    float4 NormalTransformOffsetScale; \
    float4 EmissiveTransformOffsetScale; \
    float4 SheenColorTransformOffsetScale; \
    float4 SheenRoughnessTransformOffsetScale; \
    float4 ClearcoatTransformOffsetScale; \
    float4 ClearcoatRoughnessTransformOffsetScale; \
    float4 ClearcoatNormalTransformOffsetScale; \
    float4 AnisotropyTransformOffsetScale; \
    float2 BaseColorTransformRotation; \
    float2 MetallicRoughnessTransformRotation; \
    float2 NormalTransformRotation; \
    float2 EmissiveTransformRotation; \
    float2 SheenColorTransformRotation; \
    float2 SheenRoughnessTransformRotation; \
    float2 ClearcoatTransformRotation; \
    float2 ClearcoatRoughnessTransformRotation; \
    float2 ClearcoatNormalTransformRotation; \
    float2 AnisotropyTransformRotation; \
    uint4 VertexBufferBindlessIndices; \
    uint4 ExtraBindlessIndices; \
    uint4 SkinningBindlessIndices; \
    float4 ClusterDagPackedPositionOffset; \
    float4 ClusterDagPackedPositionScale; \
    float4 ClusterDagPackedConstantUV; \
    float4 ClusterDagPackedConstantColor; \
    uint ClusterDagVertexPackingMode; \
    uint3 PaddingClusterDagPacking; \
    uint4 MaterialTextureIndices0; \
    uint4 MaterialTextureIndices1; \
    uint4 MaterialTextureIndices2; \
    uint ClusterDagMaterialPipelineKey; \
    uint3 PaddingClusterDagResolve; \
    float EnvMapMipCount; \
    float3 PaddingEnvMap; \
    float GtaoIntensity; \
    uint DeferredLightingVisualizationMode; \
    float SceneConstantsPad; \
    uint CbvPad0; \
    uint4 CbvPad1; \
    uint4 CbvPad2; \
    uint4 CbvPad3; \
    uint4 CbvPad4; \
    uint4 CbvPad5; \
    uint4 CbvPad6;

#endif