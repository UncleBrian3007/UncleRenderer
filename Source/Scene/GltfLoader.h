#pragma once

#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Mesh.h"
#include "../Math/MathTypes.h"

struct FGltfMaterialTextureSet
{
    std::wstring BaseColor;
    std::wstring MetallicRoughness;
    std::wstring Normal;
    FFloat3 BaseColorFactor{ 1.0f, 1.0f, 1.0f };
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
};

struct FGltfMaterialTextures
{
    std::vector<FGltfMaterialTextureSet> PerMesh;
};

struct FGltfNode
{
    int MeshIndex = -1;
    DirectX::XMFLOAT4X4 WorldMatrix{};
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
