#pragma once

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
    FFloat3 BaseColorFactor{ 1.0f, 1.0f, 1.0f };
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    FFloat3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    FGltfTextureTransform BaseColorTransform;
    FGltfTextureTransform MetallicRoughnessTransform;
    FGltfTextureTransform NormalTransform;
    FGltfTextureTransform EmissiveTransform;
};

struct FGltfMaterialTextures
{
    std::vector<FGltfMaterialTextureSet> PerMesh;
};

struct FGltfNode
{
    int MeshIndex = -1;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    std::string Name;
};

struct FGltfScene
{
    std::vector<FMesh> Meshes;
    std::vector<FGltfMaterialTextureSet> MeshMaterials;
    std::vector<FGltfNode> Nodes;
};

class FGltfLoader
{
public:
    static bool LoadSceneFromFile(const std::wstring& FilePath, FGltfScene& OutScene);
};
