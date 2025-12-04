#pragma once

#include <string>

class FMesh;

class FGltfLoader
{
public:
    static bool LoadMeshFromFile(const std::wstring& FilePath, FMesh& OutMesh);
};
