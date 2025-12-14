#pragma once

#include <string>
#include <vector>

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

struct FGltfLoadedMesh
{
    FMesh Mesh;
    FGltfMaterialTextureSet Material;
};

class FGltfLoader
{
public:
    static bool LoadMeshFromFile(
        const std::wstring& FilePath,
        FMesh& OutMesh,
        FGltfMaterialTextures* OutMaterialTextures = nullptr,
        std::vector<FGltfLoadedMesh>* OutMeshes = nullptr);
};
