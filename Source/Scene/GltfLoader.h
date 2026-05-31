#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Mesh.h"
#include "../Math/MathTypes.h"

struct FGltfTextureTransform
{
	FFloat2 Offset{ 0.0f, 0.0f };
	FFloat2 Scale{ 1.0f, 1.0f };
	float Rotation = 0.0f;
};

struct FGltfMaterialTextureSet
{
    std::wstring BaseColor;
    std::wstring MetallicRoughness;
    std::wstring Normal;
    std::wstring Emissive;
    std::wstring SheenColor;
    std::wstring SheenRoughness;
    std::wstring Clearcoat;
    std::wstring ClearcoatRoughness;
    std::wstring ClearcoatNormal;
    std::wstring Anisotropy;
    FFloat3 BaseColorFactor{ 1.0f, 1.0f, 1.0f };
    float BaseColorAlpha = 1.0f;
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    FFloat3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    FFloat3 SheenColorFactor{ 0.0f, 0.0f, 0.0f };
    float SheenRoughnessFactor = 0.0f;
    float ClearcoatFactor = 0.0f;
    float ClearcoatRoughnessFactor = 0.0f;
    float AnisotropyStrength = 0.0f;
    float AnisotropyRotation = 0.0f;
    float AlphaCutoff = 0.5f;
    bool bAlphaMask = false;
    bool bAlphaBlend = false;
    bool bDoubleSided = false;
    uint32_t ShadingModelId = 0;
    FGltfTextureTransform BaseColorTransform;
    FGltfTextureTransform MetallicRoughnessTransform;
    FGltfTextureTransform NormalTransform;
    FGltfTextureTransform EmissiveTransform;
    FGltfTextureTransform SheenColorTransform;
    FGltfTextureTransform SheenRoughnessTransform;
    FGltfTextureTransform ClearcoatTransform;
    FGltfTextureTransform ClearcoatRoughnessTransform;
    FGltfTextureTransform ClearcoatNormalTransform;
    FGltfTextureTransform AnisotropyTransform;
};

struct FGltfMaterialTextures
{
    std::vector<FGltfMaterialTextureSet> PerPrimitive;
};

struct FGltfPrimitiveSection
{
    uint32_t IndexStart = 0;
    uint32_t IndexCount = 0;
    FGltfMaterialTextureSet Material;
};

struct FGltfNode
{
    int MeshIndex = -1;
    int SkinIndex = -1;
    int NodeIndex = -1;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    std::string Name;
};

struct FGltfNodeTransform
{
    FFloat3 Translation{ 0.0f, 0.0f, 0.0f };
    FFloat4 Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    FFloat3 Scale{ 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4X4 LocalMatrix{};
    int ParentIndex = -1;
    bool bHasMatrix = false;
};

enum class EGltfAnimationPath
{
    Translation,
    Rotation,
    Scale
};

enum class EGltfAnimationInterpolation
{
    Linear,
    Step,
    CubicSpline
};

struct FGltfAnimationSampler
{
    EGltfAnimationInterpolation Interpolation = EGltfAnimationInterpolation::Linear;
    std::vector<float> InputTimes;
    std::vector<FFloat3> OutputVec3;
    std::vector<FFloat4> OutputVec4;
};

struct FGltfAnimationChannel
{
    int NodeIndex = -1;
    int SamplerIndex = -1;
    EGltfAnimationPath Path = EGltfAnimationPath::Translation;
};

struct FGltfAnimation
{
    std::string Name;
    std::vector<FGltfAnimationSampler> Samplers;
    std::vector<FGltfAnimationChannel> Channels;
};

struct FGltfSkin
{
    std::string Name;
    int SkeletonNodeIndex = -1;
    std::vector<int> Joints;
    std::vector<DirectX::XMFLOAT4X4> InverseBindMatrices;
};

struct FGltfAnimationPose
{
    std::vector<DirectX::XMFLOAT4X4> LocalMatrices;
    std::vector<DirectX::XMFLOAT4X4> WorldMatrices;
    std::vector<std::vector<DirectX::XMFLOAT4X4>> SkinMatrices;
};

struct FGltfScene
{
    std::vector<FMesh> Meshes;
    std::vector<std::vector<FGltfPrimitiveSection>> MeshPrimitiveSections;
    std::vector<FGltfNode> Nodes;
    std::vector<FGltfNodeTransform> NodeTransforms;
    std::vector<FGltfSkin> Skins;
    std::vector<FGltfAnimation> Animations;
    FGltfAnimationPose Pose;
    float AnimationTime = 0.0f;
};

struct FGltfAnimationRuntime
{
    std::vector<FGltfNode> Nodes;
    std::vector<FGltfNodeTransform> NodeTransforms;
    std::vector<FGltfSkin> Skins;
    std::vector<FGltfAnimation> Animations;
    FGltfAnimationPose Pose;
    float AnimationTime = 0.0f;
};

class FGltfLoader
{
public:
    static bool LoadSceneFromFile(const std::wstring& FilePath, FGltfScene& OutScene);
};
