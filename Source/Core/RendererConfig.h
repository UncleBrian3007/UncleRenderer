#pragma once

#include <filesystem>
#include <string>

enum class ERendererType
{
    Deferred,
    Forward
};

struct FRendererConfig
{
    ERendererType RendererType = ERendererType::Deferred;
    std::wstring SceneFile = L"Assets/Scenes/Scene.json";
    bool bUseDepthPrepass = true;
};

class FRendererConfigLoader
{
public:
    static FRendererConfig LoadOrDefault(const std::filesystem::path& ConfigPath);

private:
    static void ApplyKeyValue(const std::string& Key, const std::string& Value, FRendererConfig& OutConfig);
    static std::string TrimCopy(const std::string& Input);
};
