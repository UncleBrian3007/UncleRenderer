#pragma once

#include <string>

class FMesh;

struct FGltfMaterialTextures
{
    std::wstring BaseColor;
    std::wstring MetallicRoughness;
    std::wstring Normal;
};

class FGltfLoader
{
public:
    static bool LoadMeshFromFile(const std::wstring& FilePath, FMesh& OutMesh, FGltfMaterialTextures* OutMaterialTextures = nullptr);
};
