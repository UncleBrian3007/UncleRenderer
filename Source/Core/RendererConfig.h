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
    float TonemapExposure = 1.0f;
    float TonemapGamma = 2.2f;
    bool bEnableCas = true;
    float CasSharpness = 0.5f;
    bool bEnableAutoExposure = true;
    float AutoExposureKey = 0.3f;
    float AutoExposureMin = 0.1f;
    float AutoExposureMax = 5.0f;
    float AutoExposureSpeedUp = 3.0f;
    float AutoExposureSpeedDown = 1.0f;
    bool bEnableTAA = true;
    float TaaHistoryWeight = 0.9f;
    bool bEnableTaskSystem = true;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bEnableIndirectDraw = true;
    bool bEnableGpuDebugPrint = true;
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
