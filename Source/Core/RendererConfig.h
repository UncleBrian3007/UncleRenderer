#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

enum class ERendererType
{
    Deferred,
    Forward
};

enum class ESSRMode
{
    PS,
    CS
};

struct FRendererConfig
{
    ERendererType RendererType = ERendererType::Deferred;
    std::wstring SceneFile = L"Assets/Scenes/Scene.json";
    bool bUseDepthPrepass = true;
    uint32_t FramesInFlight = 3;
    bool bEnableFrameOverlap = true;
    bool bEnableShadows = true;
    bool bEnableRayTracedShadows = false;
    bool bEnablePathTracing = false;
    bool bEnablePathTracingAccumulation = true;
    bool bEnablePathTracingVndf = true;
    uint32_t PathTracingMaxBounces = 8;
    float ShadowBias = 0.0f;
    bool bEnableTonemap = true;
    float TonemapExposure = 1.0f;
    float TonemapWhitePoint = 4.0f;
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
    bool bEnableHZB = true;
    bool bEnableGtao = true;
    bool bEnableGtaoJitter = true;
    bool bEnableSsrSw = true;
    bool bEnableSsrHw = true;
    bool bEnableSsrHzb = false;
    bool bEnableSsrRefine = false;
    bool bEnableSsrDenoise = true;
    bool bEnableRestirGI = false;
    uint32_t RestirGISamplesPerPixel = 2;
    float RestirGIIntensity = 1.0f;
    bool bEnableIndirectDraw = true;
    bool bEnableSkinningIndirectDraw = false;
    bool bEnablePbrResearch = false;
    bool bEnableGpuDebugPrint = true;
    float GtaoRadius = 0.75f;
    float GtaoIntensity = 1.0f;
    float GtaoPower = 1.5f;
    float GtaoThickness = 0.1f;
    uint32_t GtaoDirectionCount = 6;
    uint32_t GtaoStepCount = 4;
	uint32_t SsrMaxSteps = 32;
	float SsrMaxDistance = 50.0f;
	float SsrThickness = 1.00f;
    float SsrStride = 1.0f;
    float SsrRoughnessCutoff = 0.6f;
    float SsrIntensity = 0.3f;
    ESSRMode SsrMode = ESSRMode::CS;
    uint32_t SsrSamplesPerQuad = 1;
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
