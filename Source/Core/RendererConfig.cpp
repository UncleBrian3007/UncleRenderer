#include "RendererConfig.h"

#include "Logger.h"
#include "../../Shaders/SparseSdfGI/SparseSdfGIShared.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <string_view>

namespace
{
    std::wstring ToWide(const std::string& Input)
    {
        return std::wstring(Input.begin(), Input.end());
    }

    std::string ToLowerCopy(const std::string& Input)
    {
        std::string Result(Input);
        std::transform(Result.begin(), Result.end(), Result.begin(), [](unsigned char Ch)
        {
            return static_cast<char>(std::tolower(Ch));
        });
        return Result;
    }

    bool MatchesKey(const std::string& LowerKey, std::initializer_list<const char*> Keys)
    {
        return std::any_of(Keys.begin(), Keys.end(), [&](const char* Key)
        {
            return LowerKey == ToLowerCopy(Key);
        });
    }

    bool ParseBoolValue(const std::string& LowerValue)
    {
        return LowerValue == "1" || LowerValue == "true" || LowerValue == "yes";
    }

    void ApplyStringKey(
        const std::string& LowerKey,
        const std::string& Value,
        FRendererConfig& OutConfig,
        std::wstring FRendererConfig::* Member,
        std::initializer_list<const char*> Keys)
    {
        if (MatchesKey(LowerKey, Keys))
        {
            OutConfig.*Member = ToWide(Value);
        }
    }

    void ApplyBoolKey(
        const std::string& LowerKey,
        const std::string& LowerValue,
        FRendererConfig& OutConfig,
        bool FRendererConfig::* Member,
        std::initializer_list<const char*> Keys)
    {
        if (MatchesKey(LowerKey, Keys))
        {
            OutConfig.*Member = ParseBoolValue(LowerValue);
        }
    }

    void ApplyClampedUintKey(
        const std::string& LowerKey,
        const std::string& Value,
        FRendererConfig& OutConfig,
        uint32_t FRendererConfig::* Member,
        uint32_t MinValue,
        uint32_t MaxValue,
        std::initializer_list<const char*> Keys)
    {
        if (!MatchesKey(LowerKey, Keys))
        {
            return;
        }

        try
        {
            const int64_t ParsedValue = std::stoll(Value);
            const int64_t ClampedValue = std::clamp(
                ParsedValue,
                static_cast<int64_t>(MinValue),
                static_cast<int64_t>(MaxValue));
            OutConfig.*Member = static_cast<uint32_t>(ClampedValue);
        }
        catch (...)
        {
            LogWarning("Invalid renderer config value: " + Value);
        }
    }

    void ApplyFloatKey(
        const std::string& LowerKey,
        const std::string& Value,
        FRendererConfig& OutConfig,
        float FRendererConfig::* Member,
        std::initializer_list<const char*> Keys)
    {
        if (!MatchesKey(LowerKey, Keys))
        {
            return;
        }

        try
        {
            OutConfig.*Member = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid renderer config value: " + Value);
        }
    }

    void ApplyMinFloatKey(
        const std::string& LowerKey,
        const std::string& Value,
        FRendererConfig& OutConfig,
        float FRendererConfig::* Member,
        float MinValue,
        std::initializer_list<const char*> Keys)
    {
        if (!MatchesKey(LowerKey, Keys))
        {
            return;
        }

        try
        {
            OutConfig.*Member = (std::max)(MinValue, std::stof(Value));
        }
        catch (...)
        {
            LogWarning("Invalid renderer config value: " + Value);
        }
    }

    void ApplyClampedFloatKey(
        const std::string& LowerKey,
        const std::string& Value,
        FRendererConfig& OutConfig,
        float FRendererConfig::* Member,
        float MinValue,
        float MaxValue,
        std::initializer_list<const char*> Keys)
    {
        if (!MatchesKey(LowerKey, Keys))
        {
            return;
        }

        try
        {
            OutConfig.*Member = std::clamp(std::stof(Value), MinValue, MaxValue);
        }
        catch (...)
        {
            LogWarning("Invalid renderer config value: " + Value);
        }
    }
}

FRendererConfig FRendererConfigLoader::LoadOrDefault(const std::filesystem::path& ConfigPath)
{
    FRendererConfig Config = {};

    if (!std::filesystem::exists(ConfigPath))
    {
        LogWarning("Renderer config not found; using default values.");
        return Config;
    }

    std::ifstream File(ConfigPath);
    if (!File.is_open())
    {
        LogWarning("Failed to open renderer config; using default values.");
        return Config;
    }

    std::string Line;
    while (std::getline(File, Line))
    {
        const std::string Trimmed = TrimCopy(Line);
        if (Trimmed.empty() || Trimmed[0] == ';' || Trimmed[0] == '#')
        {
            continue;
        }

        const size_t DelimiterPos = Trimmed.find('=');
        if (DelimiterPos == std::string::npos)
        {
            continue;
        }

        std::string Key = TrimCopy(Trimmed.substr(0, DelimiterPos));
        std::string Value = TrimCopy(Trimmed.substr(DelimiterPos + 1));

        if (Key.empty())
        {
            continue;
        }

        ApplyKeyValue(Key, Value, Config);
    }

    return Config;
}

void FRendererConfigLoader::ApplyKeyValue(const std::string& Key, const std::string& Value, FRendererConfig& OutConfig)
{
    const std::string LowerKey = ToLowerCopy(Key);
    const std::string LowerValue = ToLowerCopy(Value);

    if (MatchesKey(LowerKey, { "Type", "Renderer" }))
    {
        if (LowerValue == "forward")
        {
            OutConfig.RendererType = ERendererType::Forward;
        }
        else
        {
            OutConfig.RendererType = ERendererType::Deferred;
        }
        return;
    }

    if (MatchesKey(LowerKey, { "Scene", "SceneFile" }))
    {
        OutConfig.SceneFile = ToWide(Value);
        return;
    }

    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bUseDepthPrepass, { "UseDepthPrepass", "DepthPrepass" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableFrameOverlap, { "FrameOverlap", "UseFrameOverlap" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableVSync, { "VSync", "EnableVSync" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::FramesInFlight, 1u, 8u, { "FramesInFlight", "SwapChainBufferCount" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableShadows, { "EnableShadows", "Shadows" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableRayTracedShadows, { "EnableRayTracedShadows", "RayTracedShadows" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnablePathTracing, { "EnablePathTracing", "PathTracing" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnablePathTracingAccumulation, { "EnablePathTracingAccumulation", "PathTracingAccumulation" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnablePathTracingVndf, { "EnablePathTracingVndf", "PathTracingVndf" });
    ApplyStringKey(LowerKey, Value, OutConfig, &FRendererConfig::EnvironmentEquirectPath, { "EnvironmentEquirect", "EnvironmentMap" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::EnvironmentCubeResolution, 16u, 2048u, { "EnvironmentCubeSize", "EnvironmentCubeResolution" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::EnvironmentSpecularSampleCount, 1u, 4096u, { "EnvironmentSpecSamples", "EnvironmentSpecularSamples" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::ShadowBias, { "ShadowBias" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableTonemap, { "EnableTonemap", "Tonemap" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::TonemapExposure, { "TonemapExposure" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::TonemapWhitePoint, { "TonemapWhitePoint" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::TonemapGamma, { "TonemapGamma" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableCas, { "EnableCas", "Cas" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::CasSharpness, { "CasSharpness" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableAutoExposure, { "EnableAutoExposure", "AutoExposure" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIDebugMode, SPARSE_SDF_GI_DEBUG_MODE_OFF, SPARSE_SDF_GI_DEBUG_MODE_MAX, { "SparseSdfGIDebugMode" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGICascadeCount, 1u, 4u, { "SparseSdfGICascadeCount" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGISdfAtlasFormat, 0u, 2u, { "SparseSdfGISdfAtlasFormat" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIMaxBrickTriangleReferences, 1024u * 1024u, 32u * 1024u * 1024u, { "SparseSdfGIMaxBrickTriangleReferences" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIMaxScatterBricks, 4096u, 64u * 64u * 64u, { "SparseSdfGIMaxScatterBricks" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIBaseVoxelSize, { "SparseSdfGIBaseVoxelSize" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGICascadeScale, 1.01f, { "SparseSdfGICascadeScale" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGITraceHalfResolution, { "SparseSdfGITraceHalfResolution" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIUseHierarchicalTrace, { "SparseSdfGIUseHierarchicalTrace" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIEikonalEnabled, { "SparseSdfGIEikonalEnabled" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIIntensity, 0.0f, { "SparseSdfGIIntensity" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIBounceStrength, 0.0f, { "SparseSdfGIBounceStrength" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIEnableRadianceTemporalReuse, { "SparseSdfGIRadianceTemporalReuse", "SparseSdfGIEnableRadianceTemporalReuse" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIUseScreenProbes, { "SparseSdfGIUseScreenProbes" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIProbeTileSize, 4u, 16u, { "SparseSdfGIProbeTileSize" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIProbeRaysPerProbe, 4u, 64u, { "SparseSdfGIProbeRaysPerProbe" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIProbeDebugMode, SPARSE_SDF_GI_PROBE_DEBUG_MODE_OFF, SPARSE_SDF_GI_PROBE_DEBUG_MODE_MAX, { "SparseSdfGIProbeDebugMode" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIProbeTemporalReuse, { "SparseSdfGIProbeTemporalReuse" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIProbeDirectionalSH, { "SparseSdfGIProbeDirectionalSH" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIProbeSpawnJitter, { "SparseSdfGIProbeSpawnJitter" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIProbeMotionReproject, { "SparseSdfGIProbeMotionReproject" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bSparseSdfGIMultiBounce, { "SparseSdfGIMultiBounce" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGIMultiBounceStrength, 0.0f, { "SparseSdfGIMultiBounceStrength" });
    ApplyClampedFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SparseSdfGISurfaceHitThresholdVoxels, 0.05f, 0.75f, { "SparseSdfGISurfaceHitThresholdVoxels" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::AutoExposureKey, { "AutoExposureKey" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::AutoExposureMin, { "AutoExposureMin" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::AutoExposureMax, { "AutoExposureMax" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::AutoExposureSpeedUp, { "AutoExposureSpeedUp" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::AutoExposureSpeedDown, { "AutoExposureSpeedDown" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableTAA, { "EnableTAA", "TAA" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::TaaHistoryWeight, { "TaaHistoryWeight", "TaaWeight" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableTaskSystem, { "EnableTaskSystem", "TaskSystem" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bLogResourceBarriers, { "LogResourceBarriers", "LogBarriers" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bLogMeshOptimizationStats, { "LogMeshOptimizationStats", "MeshOptimizationStats" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableGraphDump, { "EnableGraphDump", "DumpGraph" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableGpuTiming, { "EnableGpuTiming", "GpuTiming" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableHZB, { "HZB", "EnableHZB" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableHzbTwoPass, { "HzbTwoPass", "EnableHzbTwoPass" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableGtao, { "GTAO", "EnableGTAO" });
    if (MatchesKey(LowerKey, { "SSR", "EnableSSR" }))
    {
        const bool bEnable = ParseBoolValue(LowerValue);
        OutConfig.bEnableSsrSw = bEnable;
        OutConfig.bEnableSsrHw = bEnable;
    }
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableSsrSw, { "SsrSw", "EnableSsrSw" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableSsrHw, { "SsrHw", "EnableSsrHw" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableSsrHzb, { "SsrHzb", "EnableSsrHzb" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableSsrHzbFullResDepth, { "EnableSsrHzbFullResDepth", "SsrFullResDepth" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableSsrRefine, { "EnableSsrRefine", "SsrRefine" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableSsrDenoise, { "EnableSsrDenoise", "SsrDenoise" });
    if (MatchesKey(LowerKey, { "DeferredLightingVisualizationMode", "LightingDebugView" }))
    {
        if (LowerValue == "off")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::Off;
        }
        else if (LowerValue == "diffuseindirect")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::DiffuseIndirect;
        }
        else if (LowerValue == "ao")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::AO;
        }
        else if (LowerValue == "directlighting")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::DirectLighting;
        }
        else if (LowerValue == "specularindirect")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::SpecularIndirect;
        }
        else if (LowerValue == "clusterdagclusters")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::ClusterDagClusters;
        }
        else if (LowerValue == "clusterdagmip")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::ClusterDagMip;
        }
        else if (LowerValue == "indirectirradiance")
        {
            OutConfig.DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::IndirectIrradiance;
        }
    }

    if (MatchesKey(LowerKey, { "DiffuseGISource", "DiffuseGI" }))
    {
        if (LowerValue == "restirgi" || LowerValue == "restir")
            OutConfig.DiffuseGISource = EDiffuseGISource::RestirGI;
        else if (LowerValue == "sparsesdfgi" || LowerValue == "sparsesdf")
            OutConfig.DiffuseGISource = EDiffuseGISource::SparseSdfGI;
        else
            OutConfig.DiffuseGISource = EDiffuseGISource::Off;
    }
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableDiffuseGIDenoiser, { "DiffuseGIDenoiser", "EnableDiffuseGIDenoiser", "RestirGIDenoiser", "EnableRestirGIDenoiser" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGISamplesPerPixel, 1u, 32u, { "RestirGISamplesPerPixel" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGIIntensity, 0.0f, { "RestirGIIntensity" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGIRayLength, 0.1f, { "RestirGIRayLength" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGIClamp, 0.1f, { "RestirGIClamp" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableRestirGITemporalReuse, { "EnableRestirGITemporalReuse", "RestirGITemporalReuse" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableRestirGISpatialReuse, { "EnableRestirGISpatialReuse", "RestirGISpatialReuse" });
    ApplyClampedFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGITemporalAdditionalScale, 0.0f, 1.0f, { "RestirGITemporalAdditionalScale" });
    ApplyClampedFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGISpatialAdditionalScale, 0.0f, 1.0f, { "RestirGISpatialAdditionalScale" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGIResolveMinDenominator, 1e-6f, { "RestirGIResolveMinDenominator" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGIResolveMaxNormalization, 1.0f, { "RestirGIResolveMaxNormalization" });
    ApplyClampedFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGIResolveLowSampleBoostGuard, 0.0f, 1.0f, { "RestirGIResolveLowSampleBoostGuard" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bRestirGIResolveUseConfidence, { "RestirGIResolveUseConfidence" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bRestirGIUseVisibility, { "RestirGIUseVisibility" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bRestirGIUseBrdf, { "RestirGIUseBrdf" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bRestirGIUseHistoryIndirect, { "RestirGIUseHistoryIndirect" });
    if (MatchesKey(LowerKey, { "RestirGIRandomMode" }))
    {
        if (LowerValue == "bluenoisesobol")
        {
            OutConfig.RestirGIRandomMode = ERestirGIRandomMode::BlueNoiseSobol;
        }
        else
        {
            OutConfig.RestirGIRandomMode = ERestirGIRandomMode::Hash;
        }
    }
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::RestirGIMaxHistoryFrames, 1u, 16u, { "RestirGIMaxHistoryFrames" });
    if (MatchesKey(LowerKey, { "SsrMode" }))
    {
        if (LowerValue == "cs")
        {
            OutConfig.SsrMode = ESSRMode::CS;
        }
        else
        {
            OutConfig.SsrMode = ESSRMode::PS;
        }
    }
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SsrSamplesPerQuad, 1u, 4u, { "SsrSamplesPerQuad" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableGtaoJitter, { "EnableGtaoJitter", "GtaoJitter" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::GtaoRadius, { "GtaoRadius" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::GtaoIntensity, { "GtaoIntensity" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::GtaoPower, { "GtaoPower" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::GtaoThickness, { "GtaoThickness" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::SsrMaxSteps, 1u, std::numeric_limits<uint32_t>::max(), { "SsrMaxSteps" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SsrMaxDistance, { "SsrMaxDistance" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SsrThickness, { "SsrThickness" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SsrStride, { "SsrStride" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SsrRoughnessCutoff, { "SsrRoughnessCutoff" });
    ApplyFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::SsrIntensity, { "SsrIntensity" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::GtaoDirectionCount, 1u, 16u, { "GtaoDirectionCount" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::GtaoStepCount, 1u, 8u, { "GtaoStepCount" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableGpuDebugPrint, { "EnableGpuDebugPrint", "GpuDebugPrint" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableIndirectDraw, { "EnableIndirectDraw", "IndirectDraw" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableClusterDAGRuntime, { "EnableClusterDAGRuntime", "ClusterDAGRuntime" });

    if (MatchesKey(LowerKey, { "ClusterDAGTraversalMode" }))
    {
        if (LowerValue == "persistentqueue")
        {
            OutConfig.ClusterDAGTraversalMode = EClusterDAGTraversalMode::PersistentQueue;
        }
        else if (LowerValue == "levelsplitqueue")
        {
            OutConfig.ClusterDAGTraversalMode = EClusterDAGTraversalMode::LevelSplitQueue;
        }
    }

    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableClusterDAGFastShader, { "ClusterDAGFastShader", "EnableClusterDAGFastShader" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bForceRebuildClusterDAGCache, { "ForceRebuildClusterDAGCache", "ForceClusterDAGCacheBuild" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGTargetErrorPixels, 0.0f, { "ClusterDAGTargetErrorPixels" });
    ApplyMinFloatKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGSwRasterThresholdPixels, 0.0f, { "ClusterDAGSwRasterThresholdPixels", "ClusterDAGSoftwareRasterThresholdPixels" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableClusterDAGSwRasterHzbReject, { "ClusterDAGSwRasterHzbReject", "EnableClusterDAGSwRasterHzb" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableClusterDAGStreaming, { "ClusterDAGStreaming", "EnableClusterDAGStreaming" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGStreamingPoolMB, 1u, 4096u, { "ClusterDAGStreamingPoolMB" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGStreamingRequestBufferCapacity, 64u, 1048576u, { "ClusterDAGStreamingRequestBufferCapacity" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGStreamingMaxPendingPages, 1u, 65536u, { "ClusterDAGStreamingMaxPendingPages" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGStreamingMaxPageInstallsPerFrame, 1u, 1024u, { "ClusterDAGStreamingMaxPageInstallsPerFrame" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGStreamingPageSlotBytes, 4096u, 16u * 1024u * 1024u, { "ClusterDAGStreamingPageSlotBytes" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGStreamingMaxIoInFlight, 1u, 1024u, { "ClusterDAGStreamingMaxIoInFlight" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGStreamingMaxPageUploadBytesPerFrame, 4096u, 1024u * 1024u * 1024u, { "ClusterDAGStreamingMaxPageUploadBytesPerFrame" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableClusterDAGForceMip, { "ClusterDAGForceMip", "EnableClusterDAGForceMip" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::ClusterDAGForceMipLevel, 0u, std::numeric_limits<uint32_t>::max(), { "ClusterDAGForceMipLevel" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableClusterDAGForceSoftwareRaster, { "ClusterDAGForceSoftwareRaster", "EnableClusterDAGForceSoftwareRaster" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableClusterDAGDebug, { "EnableClusterDAGDebug", "ClusterDAGDebug" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnableSkinningIndirectDraw, { "SkinningIndirectDraw", "EnableSkinningIndirectDraw" });
    ApplyBoolKey(LowerKey, LowerValue, OutConfig, &FRendererConfig::bEnablePbrResearch, { "PbrResearch", "EnablePbrResearch" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::WindowWidth, 1u, std::numeric_limits<uint32_t>::max(), { "Width", "WindowWidth" });
    ApplyClampedUintKey(LowerKey, Value, OutConfig, &FRendererConfig::WindowHeight, 1u, std::numeric_limits<uint32_t>::max(), { "Height", "WindowHeight" });

    if (MatchesKey(LowerKey, { "Resolution" }))
    {
        const size_t Separator = Value.find_first_of("xX");
        if (Separator != std::string::npos)
        {
            try
            {
                const int32_t ParsedWidth = std::stoi(Value.substr(0, Separator));
                const int32_t ParsedHeight = std::stoi(Value.substr(Separator + 1));
                OutConfig.WindowWidth = static_cast<uint32_t>((std::max)(1, ParsedWidth));
                OutConfig.WindowHeight = static_cast<uint32_t>((std::max)(1, ParsedHeight));
            }
            catch (...)
            {
                LogWarning("Invalid resolution value in renderer config: " + Value);
            }
        }
    }
}

std::string FRendererConfigLoader::TrimCopy(const std::string& Input)
{
    constexpr std::string_view Whitespace = " \t\r\n";

    const size_t Start = Input.find_first_not_of(Whitespace);
    if (Start == std::string::npos)
    {
        return std::string();
    }

    const size_t End = Input.find_last_not_of(Whitespace);
    return Input.substr(Start, End - Start + 1);
}
