#pragma once

#include "../Math/MathTypes.h"

#include <string>
#include <vector>

struct FSceneModelDesc
{
    std::wstring MeshPath;
    std::wstring BaseColorTexturePath;
    std::wstring MetallicRoughnessTexturePath;
    std::wstring NormalTexturePath;
    std::wstring EmissiveTexturePath;
    bool bVisible{true};
    FFloat3 Position{0.0f, 0.0f, 0.0f};
    FFloat3 RotationEuler{0.0f, 0.0f, 0.0f};
    FFloat3 Scale{1.0f, 1.0f, 1.0f};
};

struct FSceneLightDesc
{
    FFloat3 Direction{ -0.5f, -1.0f, 0.2f };
    float Intensity{ 1.0f };
    FFloat3 Color{ 1.0f, 1.0f, 1.0f };
};

class FSceneJsonLoader
{
public:
    static bool LoadScene(const std::wstring& FilePath, std::vector<FSceneModelDesc>& OutModels);
    static bool LoadSceneLighting(const std::wstring& FilePath, FSceneLightDesc& OutLight);
};

