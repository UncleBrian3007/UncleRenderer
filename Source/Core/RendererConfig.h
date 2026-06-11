#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <DirectXMath.h>

#include "LightingVisualizationShared.h"

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

enum class ERestirGIRandomMode : uint32_t
{
    Hash = 0,
    BlueNoiseSobol = 1
};

enum class EDiffuseGISource : uint32_t
{
    Off = 0,
    SparseSdfGI = 1,
    RestirGI = 2
};

enum class EDeferredLightingVisualizationMode : uint32_t
{
    Off = LIGHTING_VISUALIZATION_OFF,
    DiffuseIndirect = LIGHTING_VISUALIZATION_DIFFUSE_INDIRECT,
    AO = LIGHTING_VISUALIZATION_AO,
    DirectLighting = LIGHTING_VISUALIZATION_DIRECT_LIGHTING,
    SpecularIndirect = LIGHTING_VISUALIZATION_SPECULAR_INDIRECT,
    ClusterDagClusters = LIGHTING_VISUALIZATION_CLUSTER_DAG_CLUSTERS,
    ClusterDagMip = LIGHTING_VISUALIZATION_CLUSTER_DAG_MIP,
    IndirectIrradiance = LIGHTING_VISUALIZATION_INDIRECT_IRRADIANCE
};

enum class EClusterDAGTraversalMode : uint32_t
{
    PersistentQueue = 1,
    LevelSplitQueue = 3
};

struct FRendererConfig
{
    ERendererType RendererType = ERendererType::Deferred;
    std::wstring SceneFile = L"Assets/Scenes/Scene.json";
    bool bUseDepthPrepass = true;
    uint32_t FramesInFlight = 3;
    bool bEnableFrameOverlap = true;
    bool bEnableVSync = false;
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
    bool bLogMeshOptimizationStats = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bEnableSectionPixEvents = true;
    float LightYaw = -1.19028997f;
    float LightPitch = -1.07681236f;
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    bool bEnableHZB = true;
    bool bEnableHzbTwoPass = false;
    bool bEnableGtao = true;
    bool bEnableGtaoJitter = true;
    bool bEnableSsrSw = true;
    bool bEnableSsrHw = true;
    bool bEnableSsrHzb = false;
    bool bEnableSsrHzbFullResDepth = true;
    bool bEnableSsrRefine = false;
    bool bEnableSsrDenoise = true;
    EDeferredLightingVisualizationMode DeferredLightingVisualizationMode = EDeferredLightingVisualizationMode::Off;
    EDiffuseGISource DiffuseGISource = EDiffuseGISource::Off;
    bool bEnableDiffuseGIDenoiser = true;
    bool bDiffuseGIDenoiserSeparableBlur = true;
    uint32_t RestirGISamplesPerPixel = 2;
    float RestirGIIntensity = 1.0f;
    float RestirGIRayLength = 1000.0f;
    float RestirGIClamp = 10.0f;
    bool bEnableRestirGITemporalReuse = true;
    bool bEnableRestirGISpatialReuse = true;
    float RestirGITemporalAdditionalScale = 0.2f;
    float RestirGISpatialAdditionalScale = 0.15f;
    float RestirGIResolveMinDenominator = 1e-5f;
    float RestirGIResolveMaxNormalization = 32.0f;
    float RestirGIResolveLowSampleBoostGuard = 0.6f;
    bool bRestirGIResolveUseConfidence = true;
    uint32_t RestirGIMaxHistoryFrames = 1;
    bool bRestirGIUseVisibility = true;
    bool bRestirGIUseBrdf = true;
    bool bRestirGIUseHistoryIndirect = true;
    ERestirGIRandomMode RestirGIRandomMode = ERestirGIRandomMode::BlueNoiseSobol;
    uint32_t SparseSdfGIDebugMode = 0;
    uint32_t SparseSdfGICascadeCount = 1;
    uint32_t SparseSdfGISdfAtlasFormat = 0;
    uint32_t SparseSdfGIMaxBrickTriangleReferences = 8u * 1024u * 1024u;
    uint32_t SparseSdfGIMaxScatterBricks = 64u * 1024u;
    float SparseSdfGIBaseVoxelSize = 0.0f;
    float SparseSdfGICascadeScale = 2.0f;
    bool bSparseSdfGITraceHalfResolution = false;
    bool bSparseSdfGIUseHierarchicalTrace = true;
    bool bSparseSdfGIEikonalEnabled = true;
    bool bSparseSdfGIPropagateBrickSH = true;
    float SparseSdfGIIntensity = 1.0f;
    float SparseSdfGIBounceStrength = 1.0f;
    bool bSparseSdfGIEnableRadianceTemporalReuse = true;
    bool bSparseSdfGIUseScreenProbes = true;
    uint32_t SparseSdfGIProbeTileSize = 8;
    uint32_t SparseSdfGIProbeRaysPerProbe = 16;
    uint32_t SparseSdfGIProbeDebugMode = 0;
    bool bSparseSdfGIProbeTemporalReuse = true;
    bool bSparseSdfGIProbeDirectionalSH = true;
    bool bSparseSdfGIProbeSpawnJitter = false;
    bool bSparseSdfGIProbeMotionReproject = true;
    bool bSparseSdfGIMultiBounce = true;
    float SparseSdfGIMultiBounceStrength = 1.0f;
    float SparseSdfGISurfaceHitThresholdVoxels = 0.75f;
    bool bEnableIndirectDraw = true;
    bool bEnableClusterDAGRuntime = true;
    bool bForceRebuildClusterDAGCache = false;
    bool bEnableSkinningIndirectDraw = false;
    bool bEnableClusterDAGFastShader = false;
    EClusterDAGTraversalMode ClusterDAGTraversalMode = EClusterDAGTraversalMode::LevelSplitQueue;
    float ClusterDAGTargetErrorPixels = 1.0f;
    float ClusterDAGSwRasterThresholdPixels = 16.0f;
    bool bEnableClusterDAGSwRasterHzbReject = true;
    bool bEnableClusterDAGStreaming = false;
    uint32_t ClusterDAGStreamingPoolMB = 256;
    uint32_t ClusterDAGStreamingRequestBufferCapacity = 65536;
    uint32_t ClusterDAGStreamingMaxPendingPages = 64;
    uint32_t ClusterDAGStreamingMaxPageInstallsPerFrame = 16;
    uint32_t ClusterDAGStreamingPageSlotBytes = 131072;
    uint32_t ClusterDAGStreamingMaxIoInFlight = 8;
    uint32_t ClusterDAGStreamingMaxPageUploadBytesPerFrame = 8388608;
    bool bEnableClusterDAGForceMip = false;
    uint32_t ClusterDAGForceMipLevel = 0;
    bool bEnableClusterDAGForceSoftwareRaster = false;
    bool bEnableClusterDAGDebug = true;
    bool bEnablePbrResearch = false;
    bool bEnableGpuDebugPrint = true;
    std::wstring EnvironmentEquirectPath = L"Assets/Textures/hdri/rural_landscape_1k.hdr";
    uint32_t EnvironmentCubeResolution = 128;
    uint32_t EnvironmentSpecularSampleCount = 64;
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
