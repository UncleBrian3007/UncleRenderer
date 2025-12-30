#pragma once

#include <cstdint>
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
    uint32_t FramesInFlight = 3;
    bool bEnableFrameOverlap = true;
    bool bEnableShadows = true;
    float ShadowBias = 0.0f;
    bool bEnableTonemap = true;
    float TonemapExposure = 0.5f;
    float TonemapWhitePoint = 4.0f;
    float TonemapGamma = 1.0f;
    bool bEnableTaskSystem = true;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bEnableIndirectDraw = true;
    uint32_t WindowWidth = 1280;
    uint32_t WindowHeight = 720;
};

class FRendererConfigLoader
{
public:
    static FRendererConfig LoadOrDefault(const std::filesystem::path& ConfigPath);

private:
    static void ApplyKeyValue(const std::string& Key, const std::string& Value, FRendererConfig& OutConfig);
    static std::string TrimCopy(const std::string& Input);
};
