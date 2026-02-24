#include "DeferredRenderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "RenderGraph.h"
#include "Deferred/DeferredPassContext.h"
#include "Deferred/DeferredFrameOrchestrator.h"
#include "Deferred/DeferredVisibilityPasses.h"
#include "Deferred/DeferredGeometryPasses.h"
#include "Deferred/DeferredLightingPasses.h"
#include "Deferred/DeferredRayTracingPasses.h"
#include "Deferred/DeferredPostProcessPasses.h"
#include "Deferred/DeferredResourceImporter.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/Camera.h"
#include "../Scene/Mesh.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12CommandQueue.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../Core/RendererConfig.h"
#include <d3dx12.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <array>
#include <sstream>

using Microsoft::WRL::ComPtr;

FDeferredRenderer::FDeferredRenderer()
    : FrameOrchestrator(std::make_unique<FDeferredFrameOrchestrator>())
    , VisibilityPasses(std::make_unique<FDeferredVisibilityPasses>())
    , GeometryPasses(std::make_unique<FDeferredGeometryPasses>())
    , LightingPasses(std::make_unique<FDeferredLightingPasses>())
    , RayTracingPasses(std::make_unique<FDeferredRayTracingPasses>())
    , PostProcessPasses(std::make_unique<FDeferredPostProcessPasses>())
    , ResourceImporter(std::make_unique<FDeferredResourceImporter>())
{
}

namespace
{
    constexpr DXGI_FORMAT GBufferFormats[4] =
    {
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };

    constexpr DXGI_FORMAT LightingBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT PathTracingBufferFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr uint32_t SsrRayItemStride = 48u;

    float HaltonSequence(uint32_t Index, uint32_t Base)
    {
        float Result = 0.0f;
        float Fraction = 1.0f / static_cast<float>(Base);
        uint32_t Current = Index;
        while (Current > 0)
        {
            Result += static_cast<float>(Current % Base) * Fraction;
            Current /= Base;
            Fraction /= static_cast<float>(Base);
        }
        return Result;
    }

    DirectX::XMFLOAT2 BuildTaaJitter(uint32_t SampleIndex)
    {
        const uint32_t Index = SampleIndex + 1;
        const float JitterX = HaltonSequence(Index, 2) - 0.5f;
        const float JitterY = HaltonSequence(Index, 3) - 0.5f;
        return DirectX::XMFLOAT2(JitterX, JitterY);
    }

    uint32_t HilbertIndex(uint32_t PosX, uint32_t PosY)
    {
        constexpr uint32_t HilbertLevel = 6u;
        constexpr uint32_t HilbertWidth = 1u << HilbertLevel;
        uint32_t Index = 0u;

        for (uint32_t CurLevel = HilbertWidth / 2u; CurLevel > 0u; CurLevel /= 2u)
        {
            const uint32_t RegionX = (PosX & CurLevel) > 0u;
            const uint32_t RegionY = (PosY & CurLevel) > 0u;
            Index += CurLevel * CurLevel * ((3u * RegionX) ^ RegionY);

            if (RegionY == 0u)
            {
                if (RegionX == 1u)
                {
                    PosX = (HilbertWidth - 1u) - PosX;
                    PosY = (HilbertWidth - 1u) - PosY;
                }

                std::swap(PosX, PosY);
            }
        }

        return Index;
    }
}

bool FDeferredRenderer::Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config)
{
    if (Device == nullptr)
    {
        LogError("Deferred renderer initialization failed: device is null");
        return false;
    }

    this->Device = Device;

    LogInfo("Deferred renderer initialization started");

    this->BackBufferFormat = BackBufferFormat;

    bTonemapEnabled = Config.bEnableTonemap;
    TonemapExposure = Config.TonemapExposure;
    TonemapWhitePoint = Config.TonemapWhitePoint;
    TonemapGamma = Config.TonemapGamma;
    bCasEnabled = Config.bEnableCas;
    CasSharpness = Config.CasSharpness;
    bAutoExposureEnabled = Config.bEnableAutoExposure;
    AutoExposureKey = Config.AutoExposureKey;
    AutoExposureMin = Config.AutoExposureMin;
    AutoExposureMax = Config.AutoExposureMax;
    AutoExposureSpeedUp = Config.AutoExposureSpeedUp;
    AutoExposureSpeedDown = Config.AutoExposureSpeedDown;
    bTaaEnabled = Config.bEnableTAA;
    TaaHistoryWeight = Config.TaaHistoryWeight;
    TaaFrameCount = Config.FramesInFlight;
    TaaHistoryValid.clear();
    TaaHistoryValid.resize(TaaFrameCount, false);
    TaaSampleIndex = 0;
    bPathTracingAccumulationEnabled = Config.bEnablePathTracingAccumulation;
    PathTracingAccumulationFrameCount = Config.FramesInFlight;
    PathTracingAccumulationHistoryValid.clear();
    PathTracingAccumulationHistoryValid.resize(PathTracingAccumulationFrameCount, false);
    PathTracingAccumulatedFrames = 0;
    bHZBEnabled = Config.bEnableHZB;
    bHZBReady = false;
    bEnablePbrResearch = Config.bEnablePbrResearch;
    bSsrSwEnabled = Config.bEnableSsrSw;
    bSsrHwEnabled = Config.bEnableSsrHw;
    bSsrHzbEnabled = Config.bEnableSsrHzb;
    bSsrRefineEnabled = Config.bEnableSsrRefine;
    bSsrDenoiseEnabled = Config.bEnableSsrDenoise;
    bRestirGIEnabled = Config.bEnableRestirGI;
    SsrMaxSteps = Config.SsrMaxSteps;
    SsrMaxDistance = Config.SsrMaxDistance;
    SsrThickness = Config.SsrThickness;
    SsrStride = Config.SsrStride;
    SsrRoughnessCutoff = Config.SsrRoughnessCutoff;
    SsrIntensity = Config.SsrIntensity;
    RestirGISamplesPerPixel = std::clamp(Config.RestirGISamplesPerPixel, 1u, 32u);
    RestirGIIntensity = (std::max)(0.0f, Config.RestirGIIntensity);
    RestirGIRayLength = (std::max)(0.1f, Config.RestirGIRayLength);
    RestirGIClamp = (std::max)(0.1f, Config.RestirGIClamp);
    bRestirGITemporalReuse = Config.bEnableRestirGITemporalReuse;
    bRestirGISpatialReuse = Config.bEnableRestirGISpatialReuse;
    RestirGITemporalAdditionalScale = std::clamp(Config.RestirGITemporalAdditionalScale, 0.0f, 1.0f);
    RestirGISpatialAdditionalScale = std::clamp(Config.RestirGISpatialAdditionalScale, 0.0f, 1.0f);
    RestirGIResolveMinDenominator = (std::max)(Config.RestirGIResolveMinDenominator, 1e-6f);
    RestirGIResolveMaxNormalization = (std::max)(Config.RestirGIResolveMaxNormalization, 1.0f);
    RestirGIResolveLowSampleBoostGuard = std::clamp(Config.RestirGIResolveLowSampleBoostGuard, 0.0f, 1.0f);
    bRestirGIResolveUseConfidence = Config.bRestirGIResolveUseConfidence;
    RestirGIMaxHistoryFrames = std::clamp(Config.RestirGIMaxHistoryFrames, 1u, 16u);
    bRestirGIUseVisibility = Config.bRestirGIUseVisibility;
    bRestirGIUseBrdf = Config.bRestirGIUseBrdf;
    bRestirGIUseHistoryIndirect = Config.bRestirGIUseHistoryIndirect;
    SsrMode = Config.SsrMode;
    SsrSamplesPerQuad = Config.SsrSamplesPerQuad;

    InitializeCommonSettings(Width, Height, Config);

    if (!CreateRayTracingPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: ray tracing pipeline creation failed");
        return false;
    }
    if (!CreateSkinningPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: skinning pipeline creation failed");
        return false;
    }
    if (!CreateEnvironmentBuildPipelines(Device))
    {
        LogError("Deferred renderer initialization failed: environment build pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer base pass root signature...");
    if (!CreateBasePassRootSignature(Device))
    {
        LogError("Deferred renderer initialization failed: base pass root signature creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting root signature...");
    if (!CreateLightingRootSignature(Device))
    {
        LogError("Deferred renderer initialization failed: lighting root signature creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer base pass pipeline...");
    if (!CreateBasePassPipeline(Device, LightingBufferFormat))
    {
        LogError("Deferred renderer initialization failed: base pass pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer object ID pipeline...");
    if (!CreateObjectIdPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: object ID pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer depth prepass pipeline...");
    if (!CreateDepthPrepassPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: depth prepass pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer shadow pipeline...");
    const std::vector<std::wstring> ShadowDefines;
    if (!CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowDefines, ShadowPipelines[0], false))
    {
        LogError("Deferred renderer initialization failed: shadow pipeline creation failed");
        return false;
    }
    const std::vector<std::wstring> ShadowSkinnedDefines = { L"USE_SKINNING=1" };
    if (!CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowSkinnedDefines, ShadowPipelinesSkinned[0], false))
    {
        LogError("Deferred renderer initialization failed: shadow pipeline (skinned) creation failed");
        return false;
    }
    if (!CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowDefines, ShadowPipelines[1], true))
    {
        LogError("Deferred renderer initialization failed: shadow pipeline (double-sided) creation failed");
        return false;
    }
    if (!CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowSkinnedDefines, ShadowPipelinesSkinned[1], true))
    {
        LogError("Deferred renderer initialization failed: shadow pipeline (double-sided skinned) creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting pipeline...");
    if (!CreateLightingPipeline(Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: lighting pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer linear depth root signature and pipeline...");
    if (!CreateLinearDepthRootSignature(Device) || !CreateLinearDepthPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: linear depth pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer GTAO root signature and pipeline...");
    if (!CreateGtaoRootSignature(Device) || !CreateGtaoPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: GTAO pipeline creation failed");
        return false;
    }

    if (!Device->IsRayTracingSupported())
    {
        if (bRestirGIEnabled)
        {
            LogWarning("Deferred renderer: ReSTIR GI disabled because DXR is not supported on this device.");
        }
        bRestirGIEnabled = false;
        }
    else
    {
        LogInfo("Creating deferred renderer ReSTIR GI root signature and pipeline...");
        if (!CreateRestirGIRootSignature(Device) || !CreateRestirGIPipeline(Device))
        {
            LogWarning("Deferred renderer: ReSTIR GI pipeline creation failed.");
            RestirGIRootSignature.Reset();
            RestirGIInitialPipeline.Reset();
            RestirGITemporalPipeline.Reset();
            RestirGISpatialPipeline.Reset();
            RestirGIResolvePipeline.Reset();
        }

        if (!CreateRestirGiDenoiserPipelines(Device))
        {
            LogWarning("Deferred renderer: ReSTIR GI denoiser pipeline creation failed (passes will be skipped).");
        }
    }

    LogInfo("Creating deferred renderer velocity root signature and pipeline...");
    if (!CreateVelocityRootSignature(Device) || !CreateVelocityPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: velocity pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer SSR root signature and pipeline...");
    if (!CreateSsrRootSignature(Device) || !CreateSsrPipeline(Device) || !CreateSsrDenoiseRootSignature(Device) || !CreateSsrDenoisePipeline(Device)
        || !CreateSsrRayGatherRootSignature(Device) || !CreateSsrRayGatherPipeline(Device)
        || !CreateSsrSwTraceRootSignature(Device) || !CreateSsrSwTracePipeline(Device)
        || !CreateSsrBuildIndirectArgsRootSignature(Device) || !CreateSsrBuildIndirectArgsPipeline(Device)
        || !CreateSsrResolveRootSignature(Device) || !CreateSsrResolvePipeline(Device)
        || !CreateSsrDispatchCommandSignature(Device))
    {
        LogError("Deferred renderer initialization failed: SSR pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer hierarchical Z-buffer root signature and pipeline...");
    if (!CreateHZBRootSignature(Device) || !CreateHZBPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: HZB pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer auto exposure root signature and pipeline...");
    if (!CreateAutoExposureRootSignature(Device) || !CreateAutoExposurePipeline(Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer TAA root signature and pipeline...");
    if (!CreateTaaRootSignature(Device) || !CreateTaaPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: TAA pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer PathTracing accumulation root signature and pipeline...");
    if (!CreatePathTracingAccumulationRootSignature(Device) || !CreatePathTracingAccumulationPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: PathTracing accumulation pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer tonemap root signature and pipeline...");
    if (!CreateTonemapRootSignature(Device) || !CreateTonemapPipeline(Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: tonemap pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer CAS root signature and pipeline...");
    if (!CreateCasRootSignature(Device) || !CreateCasPipeline(Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: CAS pipeline creation failed");
        return false;
    }

    TextureLoader = std::make_unique<FTextureLoader>(Device);

    if (!TextureLoader->LoadOrSolidColor(L"", 0xffffffff, NullTexture))
    {
        LogError("Deferred renderer initialization failed: null texture creation failed");
        return false;
    }

    if (NullTexture)
    {
        NullTexture->SetName(L"NullTexture");
    }

    if (!CreateDepthResourcesPerFrame(Device, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT))
    {
        LogError("Deferred renderer initialization failed: depth resources creation failed");
        return false;
    }

    if (!CreateObjectIdResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: object ID resources creation failed");
        return false;
    }
    if (ObjectIdTexture)
    {
        ObjectIdTexture->SetName(L"ObjectIdTexture");
    }
    if (ObjectIdRtvHeap)
    {
        ObjectIdRtvHeap->SetName(L"ObjectIdRtvHeap");
    }
    if (ObjectIdReadback)
    {
        ObjectIdReadback->SetName(L"ObjectIdReadback");
    }

    if (!CreateShadowResources(Device, ShadowMapWidth, ShadowMapHeight, ShadowMap, ShadowDSVHeap, ShadowDSVHandle, ShadowMapState))
    {
        LogError("Deferred renderer initialization failed: shadow resources creation failed");
        return false;
    }

    if (!CreateGBufferResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: GBuffer resource creation failed");
        return false;
    }

    if (!CreateLinearDepthResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: linear depth resource creation failed");
        return false;
    }

    if (!CreateGtaoResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: GTAO resource creation failed");
        return false;
    }

    if (!CreateRestirGIResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI resource creation failed");
        return false;
    }

    if (!CreateRestirGiDenoiserResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI denoiser resource creation failed");
        return false;
    }

    if (!CreateVelocityResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: velocity resource creation failed");
        return false;
    }

    if (!CreateSsrResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: SSR resource creation failed");
        return false;
    }

    if (!CreateHilbertLutResources(Device))
    {
        LogError("Deferred renderer initialization failed: GTAO Hilbert LUT creation failed");
        return false;
    }

    if (!CreateLuminanceResources(Device))
    {
        LogError("Deferred renderer initialization failed: luminance resource creation failed");
        return false;
    }

    if (!CreateTaaResources(Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: TAA resource creation failed");
        return false;
    }

    if (!CreatePathTracingAccumulationResources(Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: PathTracing accumulation resource creation failed");
        return false;
    }

    if (!CreateHZBResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: HZB resource creation failed");
        return false;
    }

    const std::wstring SceneFilePath = Config.SceneFile.empty() ? L"Assets/Scenes/Scene.json" : Config.SceneFile;
    if (!RendererUtils::CreateSceneModelsFromJson(Device, SceneFilePath, SceneModels, SceneCenter, SceneRadius, &GltfScenes))
    {
        LogError("scene JSON could not be loaded.");
        return false;
    }

    if (!CreateSkinnedPositionBuffers())
    {
        LogError("Deferred renderer initialization failed: skinned position buffer creation failed");
        return false;
    }

    GltfScenePoses.resize(GltfScenes.size());
    GltfSceneTimes.assign(GltfScenes.size(), 0.0f);
    for (size_t Index = 0; Index < GltfScenes.size(); ++Index)
    {
        InitializeGltfAnimationPose(GltfScenes[Index], GltfScenePoses[Index]);
    }

    SceneWorldMatrix = SceneModels.front().WorldMatrix;
    for (FSceneModelResource& Model : SceneModels)
    {
        Model.PreviousWorldMatrix = Model.WorldMatrix;
        Model.bHasPreviousWorldMatrix = false;
    }

    SceneConstantBufferStride = (sizeof(FSceneConstants) + 255ULL) & ~255ULL;
    const uint64_t ConstantBufferSize = SceneConstantBufferStride * (std::max<uint64_t>(1, SceneModels.size()));

    if (!CreateSceneConstantBuffersPerFrame(Device, ConstantBufferSize))
    {
        LogError("Deferred renderer initialization failed: constant buffer creation failed");
        return false;
    }
    if (!CreateCullingConstantBuffersPerFrame(Device))
    {
        LogError("Deferred renderer initialization failed: culling constant buffer creation failed");
        return false;
    }

    if (!BuildEnvironmentFromEquirect(Config))
    {
        LogError("Deferred renderer initialization failed: environment build requires R11G11B10 typed UAV support");
        return false;
    }
    if (EnvironmentCubeTexture)
    {
        EnvironmentCubeTexture->SetName(L"EnvironmentCube");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/PreintegratedGF.dds", BrdfLutTexture))
    {
        LogError("Deferred renderer initialization failed: BRDF LUT texture loading failed");
        return false;
    }
    if (BrdfLutTexture)
    {
        BrdfLutTexture->SetName(L"BrdfLut");
    }

    if (EnvironmentCubeTexture)
    {
        const D3D12_RESOURCE_DESC EnvDesc = EnvironmentCubeTexture->GetDesc();
        EnvironmentMipCount = static_cast<float>((std::max)(1u, static_cast<uint32_t>(EnvDesc.MipLevels)));
    }

    if (!CreateSceneTextures(Device, SceneModels))
    {
        LogError("Deferred renderer initialization failed: scene texture creation failed");
        return false;
    }

    if (!CreateDescriptorHeap(Device))
    {
        LogError("Deferred renderer initialization failed: descriptor heap creation failed");
        return false;
    }

    if (!CreateGpuDrivenResources(Device))
    {
        LogWarning("Deferred renderer GPU-driven resources creation failed; fallback to CPU-driven draws.");
    }

    SkySphereRadius = (std::max)(SceneRadius * 5.0f, 100.0f);
    if (!RendererUtils::CreateSkyAtmosphereResources(Device, SkySphereRadius, SkyGeometry, SkyConstantBuffer, SkyConstantBufferMapped))
    {
        LogError("Deferred renderer initialization failed: sky resource creation failed");
        return false;
    }

    if (SkyConstantBuffer)
    {
        SkyConstantBuffer->SetName(L"SkyConstantBuffer");
    }

    FSkyPipelineConfig SkyPipelineConfig;
    SkyPipelineConfig.DepthEnable = true;
    SkyPipelineConfig.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    SkyPipelineConfig.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    SkyPipelineConfig.DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    if (!RendererUtils::CreateSkyAtmospherePipeline(Device, LightingBufferFormat, SkyPipelineConfig, SkyRootSignature, SkyPipelineState))
    {
        LogError("Deferred renderer initialization failed: sky pipeline state creation failed");
        return false;
    }

    if (bEnableGpuDebugPrint)
    {
        if (!CreateGpuDebugPrintResources(Device) || !CreateGpuDebugPrintPipeline(Device, BackBufferFormat) || !CreateGpuDebugLinePipeline(Device, BackBufferFormat) || !CreateGpuDebugPrintStatsPipeline(Device))
        {
            LogError("Deferred renderer initialization failed: GPU debug print setup failed");
            return false;
        }
    }

    LogInfo("Deferred renderer initialization completed");
    return true;
}

void FDeferredRenderer::RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime)
{
    if (HasRenderFatalError())
    {
        const float FatalClearColor[4] = { 0.05f, 0.0f, 0.1f, 1.0f };
        CmdContext.ClearRenderTarget(RtvHandle, FatalClearColor);
        return;
    }

    const bool bAnySkinningUpdated = RendererUtils::UpdateGltfSceneAnimation(SceneModels, GltfScenes, GltfScenePoses, GltfSceneTimes, DeltaTime);

    PrepareGpuDebugPrint(CmdContext);

    FDeferredFrameState FrameState;
    PrepareFrameState(Camera, bAnySkinningUpdated, FrameState);
    DispatchSkinning(CmdContext, FrameState.LightViewProjection);
    UpdateRayTracingBlasRefit(CmdContext);
    BuildRayTracingTlas(CmdContext);

    FRenderGraph Graph;
    ConfigureFrameGraph(Graph);

    const bool bUsePathTracing = bPathTracingEnabled && bRayTracingPipelineReady;

    FDeferredFrameResources Resources;
    FDeferredPassContext PassContext
    {
        *this,
        Graph,
        FrameState,
        Resources,
        Camera,
        GetFrameIndex() % GetFramesInFlight(),
        DeltaTime,
        bUsePathTracing,
        RtvHandle
    };
    ResourceImporter->ImportFrameResources(PassContext);

    ConfigureHZBOcclusion(FrameState.bUseHZBOcclusion, HZBSrvBindlessIndex, HZBWidth, HZBHeight, HZBMipCount);

    FrameOrchestrator->BuildFrameGraph(PassContext);

    Graph.Execute(CmdContext);

    FinalizeFrameState(FrameState);
}

void FDeferredRenderer::PrepareFrameState(const FCamera& Camera, bool bAnySkinningUpdated, FDeferredFrameState& OutState)
{
    UpdateCullingVisibility(Camera);

    bool bCameraMoved = false;
    if (!bFirstFrame)
    {
        const FFloat3 CurrentPosition = Camera.GetPosition();
        const DirectX::XMFLOAT3 CurrentPosXM(CurrentPosition.x, CurrentPosition.y, CurrentPosition.z);
        
        DirectX::XMFLOAT4X4 CurrentViewMatrix;
        DirectX::XMStoreFloat4x4(&CurrentViewMatrix, Camera.GetViewMatrix());
        
        const float PositionThreshold = 0.001f;
        const bool bPositionChanged = 
            std::abs(CurrentPosXM.x - PreviousCameraPosition.x) > PositionThreshold ||
            std::abs(CurrentPosXM.y - PreviousCameraPosition.y) > PositionThreshold ||
            std::abs(CurrentPosXM.z - PreviousCameraPosition.z) > PositionThreshold;
        
        bool bViewMatrixChanged = false;
        for (int i = 0; i < 16; ++i)
        {
            if (std::abs(reinterpret_cast<const float*>(&CurrentViewMatrix)[i] - 
                         reinterpret_cast<const float*>(&PreviousCameraViewMatrix)[i]) > 0.0001f)
            {
                bViewMatrixChanged = true;
                break;
            }
        }
        
        bCameraMoved = bPositionChanged || bViewMatrixChanged;
        
        PreviousCameraPosition = CurrentPosXM;
        PreviousCameraViewMatrix = CurrentViewMatrix;
    }
    else
    {
        const FFloat3 CurrentPosition = Camera.GetPosition();
        PreviousCameraPosition = DirectX::XMFLOAT3(CurrentPosition.x, CurrentPosition.y, CurrentPosition.z);
        DirectX::XMStoreFloat4x4(&PreviousCameraViewMatrix, Camera.GetViewMatrix());
        bFirstFrame = false;
    }

    OutState.bCameraMoved = bCameraMoved;
    OutState.bAnySkinningUpdated = bAnySkinningUpdated;

    if (bCameraMoved || !bHasPreviousViewProjection)
    {
        InvalidateRestirGiDenoiserHistory();
    }

    if (!bRestirGIEnabled)
    {
        InvalidateRestirGiDenoiserHistory();
    }

    OutState.bTaaActive = bTaaEnabled && TaaPipeline && TaaRootSignature && !TaaHistoryTextures.empty();
    OutState.TaaFrameIndex = GetFrameIndex();
    OutState.TaaReadIndex = TaaFrameCount > 0 ? (OutState.TaaFrameIndex + TaaFrameCount - 1u) % TaaFrameCount : 0u;
    OutState.TaaWriteIndex = TaaFrameCount > 0 ? OutState.TaaFrameIndex % TaaFrameCount : 0u;
    OutState.bTaaHistoryReady = OutState.bTaaActive && OutState.TaaReadIndex < TaaHistoryValid.size()
        ? TaaHistoryValid[OutState.TaaReadIndex]
        : false;

    OutState.bPathTracingAccumulationActive = bPathTracingAccumulationEnabled && PathTracingAccumulationPipeline && PathTracingAccumulationRootSignature && !PathTracingAccumulationTextures.empty();
    OutState.PathTracingAccumulationReadIndex = PathTracingAccumulationFrameCount > 0 ? (OutState.TaaFrameIndex + PathTracingAccumulationFrameCount - 1u) % PathTracingAccumulationFrameCount : 0u;
    OutState.PathTracingAccumulationWriteIndex = PathTracingAccumulationFrameCount > 0 ? OutState.TaaFrameIndex % PathTracingAccumulationFrameCount : 0u;
    
    if (bCameraMoved)
    {
        std::fill(PathTracingAccumulationHistoryValid.begin(), PathTracingAccumulationHistoryValid.end(), false);
        PathTracingAccumulatedFrames = 0;
    }
    
    OutState.bPathTracingAccumulationHistoryReady = OutState.bPathTracingAccumulationActive && OutState.PathTracingAccumulationReadIndex < PathTracingAccumulationHistoryValid.size()
        ? PathTracingAccumulationHistoryValid[OutState.PathTracingAccumulationReadIndex]
        : false;
    
    if (!OutState.bPathTracingAccumulationHistoryReady)
    {
        PathTracingAccumulatedFrames = 0;
    }

    bUseTaaJitter = OutState.bTaaActive && OutState.bTaaHistoryReady;
    OutState.bGtaoJitterActive = bGtaoEnabled && bGtaoJitterEnabled;
    const bool bUseGtaoJitter = OutState.bGtaoJitterActive;
    const bool bNeedJitter = bUseTaaJitter || bUseGtaoJitter;
    if (bNeedJitter)
    {
        TaaJitter = BuildTaaJitter(TaaSampleIndex);
    }
    else
    {
        TaaJitter = DirectX::XMFLOAT2(0.0f, 0.0f);
    }

    DirectX::XMFLOAT4X4 ProjectionMatrix = {};
    DirectX::XMStoreFloat4x4(&ProjectionMatrix, Camera.GetProjectionMatrix());
    if (bUseTaaJitter && Viewport.Width > 0.0f && Viewport.Height > 0.0f)
    {
        const float JitterX = (2.0f * TaaJitter.x) / Viewport.Width;
        const float JitterY = (2.0f * TaaJitter.y) / Viewport.Height;
        ProjectionMatrix._31 += JitterX;
        ProjectionMatrix._32 += JitterY;
    }
    TaaProjection = DirectX::XMLoadFloat4x4(&ProjectionMatrix);

    const DirectX::XMMATRIX CurrentProjection = bUseTaaJitter ? TaaProjection : Camera.GetProjectionMatrix();
    const DirectX::XMMATRIX CurrentViewProjection = Camera.GetViewMatrix() * CurrentProjection;
    DirectX::XMStoreFloat4x4(&CurrentViewProjectionMatrix, CurrentViewProjection);

    const DirectX::XMMATRIX CurrentUnjitteredProjection = Camera.GetProjectionMatrix();
    const DirectX::XMMATRIX CurrentUnjitteredViewProjection = Camera.GetViewMatrix() * CurrentUnjitteredProjection;
    DirectX::XMStoreFloat4x4(&CurrentUnjitteredViewProjectionMatrix, CurrentUnjitteredViewProjection);

    OutState.bRenderShadows = bShadowsEnabled && ShadowPipelines[0] && ShadowPipelines[1] && ShadowMap;
    OutState.bDoDepthPrepass = bDepthPrepassEnabled && DepthPrepassPipelines[0] && DepthPrepassPipelines[1];
    if (!bHZBEnabled)
    {
        bHZBReady = false;
    }

    const bool bPrevHZBReady = bHZBReady;
    OutState.bUseHZBOcclusion = bHZBEnabled && bPrevHZBReady && HZBSrvBindlessIndex != UINT32_MAX;
    OutState.bUseHzbTwoPass = bEnableIndirectDraw && OutState.bUseHZBOcclusion && bEnableHzbTwoPass;
    if (OutState.bUseHzbTwoPass)
    {
        OutState.bDoDepthPrepass = false;
    }
    OutState.bBuildHZB = bHZBEnabled;
    OutState.bCasActive = bCasEnabled && CasPipeline && CasRootSignature;
    OutState.LightViewProjection = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
}

void FDeferredRenderer::ConfigureFrameGraph(FRenderGraph& Graph) const
{
    Graph.SetDevice(Device);
    Graph.SetBarrierLoggingEnabled(bLogResourceBarriers);
    Graph.SetGraphDumpEnabled(bEnableGraphDump);
    Graph.SetGpuTimingEnabled(bEnableGpuTiming);
}

void FDeferredRenderer::ImportFrameResources(FRenderGraph& Graph, FDeferredFrameResources& OutResources)
{
    OutResources.ShadowHandle = Graph.ImportTexture(
        "ShadowMap",
        ShadowMap.Get(),
        &ShadowMapState,
        { 2048, 2048, DXGI_FORMAT_D32_FLOAT });

    const FRGTextureDesc DepthDesc =
    {
        static_cast<uint32>(Viewport.Width),
        static_cast<uint32>(Viewport.Height),
        DXGI_FORMAT_R24G8_TYPELESS
    };

    D3D12_RESOURCE_STATES& DepthState = GetDepthBufferState();
    OutResources.DepthHandle = Graph.ImportTexture("Depth", GetDepthBuffer(), &DepthState, DepthDesc);
    OutResources.ObjectIdHandle = Graph.ImportTexture(
        "ObjectId",
        ObjectIdTexture.Get(),
        &ObjectIdState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R32_UINT });
    OutResources.VelocityHandle = Graph.ImportTexture(
        "Velocity",
        VelocityTexture.Get(),
        &VelocityState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16_FLOAT });
    OutResources.GBufferHandles =
    {
        Graph.ImportTexture("GBufferA", GBufferA.Get(), &GBufferStates[0], { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), GBufferFormats[0] }),
        Graph.ImportTexture("GBufferB", GBufferB.Get(), &GBufferStates[1], { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), GBufferFormats[1] }),
        Graph.ImportTexture("GBufferC", GBufferC.Get(), &GBufferStates[2], { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), GBufferFormats[2] }),
        Graph.ImportTexture("GBufferD", GBufferD.Get(), &GBufferStates[3], { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), GBufferFormats[3] }),
    };

    OutResources.LinearDepthHandle = Graph.ImportTexture(
        "LinearDepth",
        LinearDepthTexture.Get(),
        &LinearDepthState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16_FLOAT });

    OutResources.GtaoHandle = Graph.ImportTexture(
        "GTAO",
        GtaoTexture.Get(),
        &GtaoState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R8_UNORM });

    OutResources.RestirGIHandle = Graph.ImportTexture(
        "ReSTIR GI",
        RestirGITexture.Get(),
        &RestirGIState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIHistoryHandle = Graph.ImportTexture(
        "ReSTIR GI History",
        RestirGIHistoryTexture.Get(),
        &RestirGIHistoryState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    FRGBufferDesc RestirReservoirDesc = {};
    RestirReservoirDesc.Size = static_cast<size_t>(Viewport.Width) * static_cast<size_t>(Viewport.Height) * sizeof(float) * 8u;
    RestirReservoirDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    OutResources.RestirGITemporalReservoirHandle = Graph.ImportBuffer(
        "ReSTIR GI Temporal Reservoir",
        RestirGITemporalReservoirBuffer.Get(),
        &RestirGITemporalReservoirState,
        RestirReservoirDesc);

    OutResources.RestirGISpatialReservoirHandle = Graph.ImportBuffer(
        "ReSTIR GI Spatial Reservoir",
        RestirGISpatialReservoirBuffer.Get(),
        &RestirGISpatialReservoirState,
        RestirReservoirDesc);

    OutResources.RestirGIReservoirHistoryHandle = Graph.ImportBuffer(
        "ReSTIR GI Reservoir History",
        RestirGIReservoirHistoryBuffer.Get(),
        &RestirGIReservoirHistoryState,
        RestirReservoirDesc);

    const uint32_t HalfWidth = (static_cast<uint32>(Viewport.Width) + 1u) / 2u;
    const uint32_t HalfHeight = (static_cast<uint32>(Viewport.Height) + 1u) / 2u;

    OutResources.RestirGIInitialRadianceHandle = Graph.ImportTexture(
        "ReSTIR GI Initial Radiance",
        RestirGIInitialRadianceTexture.Get(),
        &RestirGIInitialRadianceState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIInitialRayDirectionHandle = Graph.ImportTexture(
        "ReSTIR GI Initial RayDir",
        RestirGIInitialRayDirectionTexture.Get(),
        &RestirGIInitialRayDirectionState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    OutResources.RestirGIReservoirDepthNormalAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir DepthNormal A",
        RestirGIReservoirDepthNormalATexture.Get(),
        &RestirGIReservoirDepthNormalAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT });

    OutResources.RestirGIReservoirDepthNormalBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir DepthNormal B",
        RestirGIReservoirDepthNormalBTexture.Get(),
        &RestirGIReservoirDepthNormalBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT });

    OutResources.RestirGIReservoirSampleRadianceAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir SampleRadiance A",
        RestirGIReservoirSampleRadianceATexture.Get(),
        &RestirGIReservoirSampleRadianceAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIReservoirSampleRadianceBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir SampleRadiance B",
        RestirGIReservoirSampleRadianceBTexture.Get(),
        &RestirGIReservoirSampleRadianceBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIReservoirRayDirectionAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir RayDirection A",
        RestirGIReservoirRayDirectionATexture.Get(),
        &RestirGIReservoirRayDirectionAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    OutResources.RestirGIReservoirRayDirectionBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir RayDirection B",
        RestirGIReservoirRayDirectionBTexture.Get(),
        &RestirGIReservoirRayDirectionBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    OutResources.RestirGIReservoirMWAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir MW A",
        RestirGIReservoirMWATexture.Get(),
        &RestirGIReservoirMWAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_FLOAT });

    OutResources.RestirGIReservoirMWBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir MW B",
        RestirGIReservoirMWBTexture.Get(),
        &RestirGIReservoirMWBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_FLOAT });

    OutResources.RestirGiInputSHHandle = Graph.ImportTexture(
        "ReSTIR GI Input SH",
        RestirGiInputSHTexture.Get(),
        &RestirGiInputSHState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R32G32B32A32_UINT });

    OutResources.RestirGiVarianceHandle = Graph.ImportTexture(
        "ReSTIR GI Variance",
        RestirGiVarianceTexture.Get(),
        &RestirGiVarianceState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R8_UNORM });

    OutResources.RestirGiHistoryIrradianceHandle = Graph.ImportTexture(
        "ReSTIR GI Denoised Irradiance",
        RestirGiHistoryIrradianceTexture.Get(),
        &RestirGiHistoryIrradianceState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R11G11B10_FLOAT });

    OutResources.RestirGiTemporalSHHandle = Graph.ImportTexture(
        "ReSTIR GI Temporal SH",
        RestirGiTemporalSHTexture.Get(),
        &RestirGiTemporalSHState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R32G32B32A32_UINT });

    OutResources.RestirGiHistorySHHandle = Graph.ImportTexture(
        "ReSTIR GI History SH",
        RestirGiHistorySHTexture.Get(),
        &RestirGiHistorySHState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R32G32B32A32_UINT });

    OutResources.RestirGiHistoryCountAHandle = Graph.ImportTexture(
        "ReSTIR GI History Count A",
        RestirGiHistoryCountATexture.Get(),
        &RestirGiHistoryCountAState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R8_UINT });

    OutResources.RestirGiHistoryCountBHandle = Graph.ImportTexture(
        "ReSTIR GI History Count B",
        RestirGiHistoryCountBTexture.Get(),
        &RestirGiHistoryCountBState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R8_UINT });

    OutResources.RestirGiPrevLinearDepthHandle = Graph.ImportTexture(
        "ReSTIR GI Prev LinearDepth",
        RestirGiPrevLinearDepthTexture.Get(),
        &RestirGiPrevLinearDepthState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16_FLOAT });

    OutResources.RestirGiPrevNormalHandle = Graph.ImportTexture(
        "ReSTIR GI Prev Normal",
        RestirGiPrevNormalTexture.Get(),
        &RestirGiPrevNormalState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    uint32_t MipWidth = (static_cast<uint32>(Viewport.Width) + 1u) / 2u;
    uint32_t MipHeight = (static_cast<uint32>(Viewport.Height) + 1u) / 2u;
    for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
    {
        const std::string ShName = "ReSTIR GI SH Mip " + std::to_string(MipIndex);
        OutResources.RestirGiShMipHandles[MipIndex] = Graph.ImportTexture(
            ShName,
            RestirGiShMipTextures[MipIndex].Get(),
            &RestirGiShMipStates[MipIndex],
            { MipWidth, MipHeight, DXGI_FORMAT_R32G32B32A32_UINT });

        const std::string DepthName = "ReSTIR GI LinearDepth Mip " + std::to_string(MipIndex);
        OutResources.RestirGiLinearDepthMipHandles[MipIndex] = Graph.ImportTexture(
            DepthName,
            RestirGiLinearDepthMipTextures[MipIndex].Get(),
            &RestirGiLinearDepthMipStates[MipIndex],
            { MipWidth, MipHeight, DXGI_FORMAT_R16_FLOAT });

        MipWidth = (std::max)(1u, (MipWidth + 1u) / 2u);
        MipHeight = (std::max)(1u, (MipHeight + 1u) / 2u);
    }

    OutResources.SsrHandle = Graph.ImportTexture(
        "SSR",
        SsrTexture.Get(),
        &SsrState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrDenoiseHandle = Graph.ImportTexture(
        "SSR Denoise",
        SsrDenoiseTexture.Get(),
        &SsrDenoiseState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrFallbackHandle = Graph.ImportTexture(
        "SSR Fallback",
        SsrFallbackTexture.Get(),
        &SsrFallbackState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrResolveHandle = Graph.ImportTexture(
        "SSR Resolve",
        SsrResolveTexture.Get(),
        &SsrResolveState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.LightingHandle = Graph.ImportTexture(
        "Lighting",
        LightingBuffer.Get(),
        &LightingBufferState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), LightingBufferFormat });

    OutResources.TonemapOutputResource = Graph.ImportTexture(
        "TonemapOutput",
        TonemapOutput.Get(),
        &TonemapOutputState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), BackBufferFormat });

    OutResources.LuminanceHandles =
    {
        Graph.ImportTexture(
            "LuminanceA",
            LuminanceTextures[0].Get(),
            &LuminanceStates[0],
            { 1u, 1u, DXGI_FORMAT_R32_FLOAT }),
        Graph.ImportTexture(
            "LuminanceB",
            LuminanceTextures[1].Get(),
            &LuminanceStates[1],
            { 1u, 1u, DXGI_FORMAT_R32_FLOAT })
    };

    OutResources.TaaHandles.reserve(TaaHistoryTextures.size());
    for (size_t Index = 0; Index < TaaHistoryTextures.size(); ++Index)
    {
        const std::string HandleName = "TaaHistory_" + std::to_string(Index);
        OutResources.TaaHandles.push_back(Graph.ImportTexture(
            HandleName,
            TaaHistoryTextures[Index].Get(),
            &TaaStates[Index],
            { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), LightingBufferFormat }));
    }

    OutResources.PathTracingTempHandle = Graph.ImportTexture(
        "PathTracingTemp",
        PathTracingTempTexture.Get(),
        &PathTracingTempState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), PathTracingBufferFormat });

    OutResources.PathTracingAccumulationHandles.reserve(PathTracingAccumulationTextures.size());
    for (size_t Index = 0; Index < PathTracingAccumulationTextures.size(); ++Index)
    {
        const std::string HandleName = "PathTracingAccumulation_" + std::to_string(Index);
        OutResources.PathTracingAccumulationHandles.push_back(Graph.ImportTexture(
            HandleName,
            PathTracingAccumulationTextures[Index].Get(),
            &PathTracingAccumulationStates[Index],
            { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), PathTracingBufferFormat }));
    }

    OutResources.HZBHandle = Graph.ImportTexture(
        "HZB",
        HierarchicalZBuffer.Get(),
        &HZBState,
        { HZBWidth, HZBHeight, DXGI_FORMAT_R32G32_FLOAT });
}

void FDeferredRenderer::AddHZBPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle HZBHandle)
{
    struct FHZBPassData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipCount = 0;
        uint32_t SourceWidth = 0;
        uint32_t SourceHeight = 0;
        uint32_t DepthBindlessIndex = UINT32_MAX;
        std::vector<uint32_t> HZBSrvMips;
        std::vector<uint32_t> HZBUavs;
        uint32_t HZBNullUav = UINT32_MAX;
    };

    if (!bHZBEnabled || !FrameState.bBuildHZB)
    {
        return;
    }

    Graph.AddPass<FHZBPassData>("Build HZB", [&](FHZBPassData& Data, FRGPassBuilder& Builder)
    {
        Data.Width = HZBWidth;
        Data.Height = HZBHeight;
        Data.MipCount = HZBMipCount;
        ID3D12Resource* DepthBuffer = GetDepthBuffer();
        const D3D12_RESOURCE_DESC DepthDesc = DepthBuffer ? DepthBuffer->GetDesc() : D3D12_RESOURCE_DESC{};
        Data.SourceWidth = static_cast<uint32>(DepthDesc.Width);
        Data.SourceHeight = DepthDesc.Height;
        const uint32_t DepthIndex = GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
        Data.DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthIndex];
        Data.HZBSrvMips = HZBSrvMipBindlessIndices;
        Data.HZBUavs = HZBUavBindlessIndices;
        Data.HZBNullUav = HZBNullUavBindlessIndex;

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(HZBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this](const FHZBPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!HZBRootSignature || Data.MipCount == 0 || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent HZBEvent(LocalCommandList, L"BuildHZB");

        if (Data.DepthBindlessIndex == UINT32_MAX || Data.HZBNullUav == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(HZBRootSignature.Get());

        struct FHZBConstants
        {
            uint32_t SourceWidth;
            uint32_t SourceHeight;
            uint32_t DestWidth;
            uint32_t DestHeight;
            uint32_t DestWidth1;
            uint32_t DestHeight1;
            uint32_t DestWidth2;
            uint32_t DestHeight2;
            uint32_t DestWidth3;
            uint32_t DestHeight3;
            uint32_t SourceMip;
            uint32_t SourceIsDepth;
        };

        uint32_t CurrentWidth = Data.Width;
        uint32_t CurrentHeight = Data.Height;
        std::vector<D3D12_RESOURCE_STATES> MipStates(Data.MipCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        uint32_t MipIndex = 0;
        while (MipIndex < Data.MipCount)
        {
            const uint32_t RemainingMips = Data.MipCount - MipIndex;
            const uint32_t MipsThisDispatch = (std::min)(4u, RemainingMips);
            const bool bHasSecondMip = MipsThisDispatch > 1;
            const bool bHasThirdMip = MipsThisDispatch > 2;
            const bool bHasFourthMip = MipsThisDispatch > 3;

            const uint32_t SourceWidth = (MipIndex == 0) ? Data.SourceWidth : (std::max)(1u, CurrentWidth);
            const uint32_t SourceHeight = (MipIndex == 0) ? Data.SourceHeight : (std::max)(1u, CurrentHeight);

            const uint32_t DestWidth = (MipIndex == 0) ? CurrentWidth : (std::max)(1u, CurrentWidth / 2);
            const uint32_t DestHeight = (MipIndex == 0) ? CurrentHeight : (std::max)(1u, CurrentHeight / 2);
            const uint32_t DestWidth1 = bHasSecondMip ? (std::max)(1u, DestWidth / 2) : 0u;
            const uint32_t DestHeight1 = bHasSecondMip ? (std::max)(1u, DestHeight / 2) : 0u;
            const uint32_t DestWidth2 = bHasThirdMip ? (std::max)(1u, DestWidth1 / 2) : 0u;
            const uint32_t DestHeight2 = bHasThirdMip ? (std::max)(1u, DestHeight1 / 2) : 0u;
            const uint32_t DestWidth3 = bHasFourthMip ? (std::max)(1u, DestWidth2 / 2) : 0u;
            const uint32_t DestHeight3 = bHasFourthMip ? (std::max)(1u, DestHeight2 / 2) : 0u;

            FHZBConstants Constants = {};
            Constants.SourceWidth = SourceWidth;
            Constants.SourceHeight = SourceHeight;
            Constants.DestWidth = DestWidth;
            Constants.DestHeight = DestHeight;
            Constants.DestWidth1 = DestWidth1;
            Constants.DestHeight1 = DestHeight1;
            Constants.DestWidth2 = DestWidth2;
            Constants.DestHeight2 = DestHeight2;
            Constants.DestWidth3 = DestWidth3;
            Constants.DestHeight3 = DestHeight3;
            Constants.SourceMip = (MipIndex > 0) ? (MipIndex - 1) : 0u;
            Constants.SourceIsDepth = (MipIndex == 0) ? 1u : 0u;

            uint32_t SourceBindlessIndex = Data.DepthBindlessIndex;
            if (MipIndex > 0)
            {
                const uint32_t SourceMipIndex = MipIndex - 1;
                SourceBindlessIndex = HZBSrvBindlessIndex;

                if (SourceMipIndex < MipStates.size() && MipStates[SourceMipIndex] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                {
                    D3D12_RESOURCE_BARRIER ToSrvBarrier = {};
                    ToSrvBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    ToSrvBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    ToSrvBarrier.Transition.pResource = HierarchicalZBuffer.Get();
                    ToSrvBarrier.Transition.StateBefore = MipStates[SourceMipIndex];
                    ToSrvBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    ToSrvBarrier.Transition.Subresource = D3D12CalcSubresource(SourceMipIndex, 0, 0, Data.MipCount, 1);
                    if (bLogResourceBarriers)
                    {
						LogInfo("HZB Barrier: Mip " + std::to_string(SourceMipIndex) + " "
							+ RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateBefore) + " -> "
							+ RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateAfter));
                    }
                    LocalCommandList->ResourceBarrier(1, &ToSrvBarrier);
                    MipStates[SourceMipIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
            }
            const uint32_t DestIndex0 = (MipIndex < Data.HZBUavs.size()) ? Data.HZBUavs[MipIndex] : UINT32_MAX;
            const uint32_t DestIndex1 = (bHasSecondMip && (MipIndex + 1) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 1]
                : Data.HZBNullUav;
            const uint32_t DestIndex2 = (bHasThirdMip && (MipIndex + 2) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 2]
                : Data.HZBNullUav;
            const uint32_t DestIndex3 = (bHasFourthMip && (MipIndex + 3) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 3]
                : Data.HZBNullUav;

            if (SourceBindlessIndex == UINT32_MAX || DestIndex0 == UINT32_MAX || DestIndex1 == UINT32_MAX
                || DestIndex2 == UINT32_MAX || DestIndex3 == UINT32_MAX)
            {
                break;
            }

            ID3D12PipelineState* SelectedPipeline = HZBPipelines[MipsThisDispatch - 1].Get();
            if (!SelectedPipeline)
            {
                break;
            }

            LocalCommandList->SetPipelineState(SelectedPipeline);
            LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
            const uint32_t HZBBindlessIndices[] = { Data.DepthBindlessIndex, SourceBindlessIndex, DestIndex0, DestIndex1, DestIndex2, DestIndex3 };
            LocalCommandList->SetComputeRoot32BitConstants(1, _countof(HZBBindlessIndices), HZBBindlessIndices, 0);

            const uint32_t GroupX = (Constants.DestWidth + 7) / 8;
            const uint32_t GroupY = (Constants.DestHeight + 7) / 8;
            LocalCommandList->Dispatch(GroupX, GroupY, 1);

            if (bHasFourthMip)
            {
                CurrentWidth = DestWidth3;
                CurrentHeight = DestHeight3;
            }
            else if (bHasThirdMip)
            {
                CurrentWidth = DestWidth2;
                CurrentHeight = DestHeight2;
            }
            else if (bHasSecondMip)
            {
                CurrentWidth = DestWidth1;
                CurrentHeight = DestHeight1;
            }
            else
            {
                CurrentWidth = DestWidth;
                CurrentHeight = DestHeight;
            }

            std::vector<D3D12_RESOURCE_BARRIER> Barriers;
            Barriers.reserve(MipsThisDispatch + 1);

            D3D12_RESOURCE_BARRIER UavBarrier = {};
            UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UavBarrier.UAV.pResource = HierarchicalZBuffer.Get();
			if (bLogResourceBarriers)
			{
                    LogInfo("HZB Barrier: UAV sync");
            }
            Barriers.push_back(UavBarrier);

            for (uint32_t LocalMip = 0; LocalMip < MipsThisDispatch; ++LocalMip)
            {
                const uint32_t TargetMip = MipIndex + LocalMip;
                if (TargetMip >= Data.MipCount)
                {
                    break;
                }

                D3D12_RESOURCE_BARRIER Barrier = {};
                Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                Barrier.Transition.pResource = HierarchicalZBuffer.Get();
                Barrier.Transition.StateBefore = MipStates[TargetMip];
                Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                Barrier.Transition.Subresource = D3D12CalcSubresource(TargetMip, 0, 0, Data.MipCount, 1);
                if (bLogResourceBarriers)
                {
					LogInfo("HZB Barrier: Mip " + std::to_string(TargetMip) + " "
						+ RendererUtils::ResourceStateToString(Barrier.Transition.StateBefore) + " -> "
						+ RendererUtils::ResourceStateToString(Barrier.Transition.StateAfter));
                }
                Barriers.push_back(Barrier);
                MipStates[TargetMip] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }

            if (!Barriers.empty())
            {
                LocalCommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
            }

            MipIndex += MipsThisDispatch;
        }

        HZBState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bHZBReady = true;
    });
}

void FDeferredRenderer::FinalizeFrameState(const FDeferredFrameState& FrameState)
{
    if (FrameState.bTaaActive || FrameState.bGtaoJitterActive)
    {
        TaaSampleIndex = (TaaSampleIndex + 1u) % 8u;
    }
    else
    {
        std::fill(TaaHistoryValid.begin(), TaaHistoryValid.end(), false);
        TaaSampleIndex = 0;
    }

    if (bAutoExposureEnabled)
    {
        bLuminanceHistoryValid = true;
        LuminanceWriteIndex = 1u - LuminanceWriteIndex;
    }
    else
    {
        bLuminanceHistoryValid = false;
    }

    PreviousViewProjectionMatrix = CurrentViewProjectionMatrix;
    bHasPreviousViewProjection = true;
    PreviousUnjitteredViewProjectionMatrix = CurrentUnjitteredViewProjectionMatrix;
    bHasPreviousUnjitteredViewProjection = true;

    if (RestirGITexture && RestirGIHistoryTexture)
    {
        std::swap(RestirGITexture, RestirGIHistoryTexture);
        std::swap(RestirGIState, RestirGIHistoryState);

        const uint32_t CurrentSrv = RestirGIBindlessIndex;
        const uint32_t CurrentUav = RestirGIUavBindlessIndex;
        RestirGIBindlessIndex = RestirGIHistorySrvBindlessIndex;
        RestirGIUavBindlessIndex = RestirGIHistoryUavBindlessIndex;
        RestirGIHistorySrvBindlessIndex = CurrentSrv;
        RestirGIHistoryUavBindlessIndex = CurrentUav;
    }

    if (RestirGiTemporalSHTexture && RestirGiHistorySHTexture)
    {
        std::swap(RestirGiTemporalSHTexture, RestirGiHistorySHTexture);
        std::swap(RestirGiTemporalSHState, RestirGiHistorySHState);
        std::swap(RestirGiTemporalSHSrvBindlessIndex, RestirGiHistorySHSrvBindlessIndex);
        std::swap(RestirGiTemporalSHUavBindlessIndex, RestirGiHistorySHUavBindlessIndex);
    }

    if (RestirGiHistoryCountATexture && RestirGiHistoryCountBTexture)
    {
        std::swap(RestirGiHistoryCountATexture, RestirGiHistoryCountBTexture);
        std::swap(RestirGiHistoryCountAState, RestirGiHistoryCountBState);
        std::swap(RestirGiHistoryCountASrvBindlessIndex, RestirGiHistoryCountBSrvBindlessIndex);
        std::swap(RestirGiHistoryCountAUavBindlessIndex, RestirGiHistoryCountBUavBindlessIndex);
    }

    if (RestirGITemporalReservoirBuffer && RestirGIReservoirHistoryBuffer)
    {
        std::swap(RestirGITemporalReservoirBuffer, RestirGIReservoirHistoryBuffer);
        std::swap(RestirGITemporalReservoirState, RestirGIReservoirHistoryState);

        const uint32_t CurrentReservoirSrv = RestirGITemporalReservoirSrvBindlessIndex;
        const uint32_t CurrentReservoirUav = RestirGITemporalReservoirUavBindlessIndex;
        RestirGITemporalReservoirSrvBindlessIndex = RestirGIReservoirHistorySrvBindlessIndex;
        RestirGITemporalReservoirUavBindlessIndex = RestirGIReservoirHistoryUavBindlessIndex;
        RestirGIReservoirHistorySrvBindlessIndex = CurrentReservoirSrv;
        RestirGIReservoirHistoryUavBindlessIndex = CurrentReservoirUav;
    }

    if (RestirGIReservoirDepthNormalATexture && RestirGIReservoirDepthNormalBTexture)
    {
        std::swap(RestirGIReservoirDepthNormalATexture, RestirGIReservoirDepthNormalBTexture);
        std::swap(RestirGIReservoirDepthNormalAState, RestirGIReservoirDepthNormalBState);
        std::swap(RestirGIReservoirDepthNormalASrvBindlessIndex, RestirGIReservoirDepthNormalBSrvBindlessIndex);
        std::swap(RestirGIReservoirDepthNormalAUavBindlessIndex, RestirGIReservoirDepthNormalBUavBindlessIndex);
    }

    if (RestirGIReservoirSampleRadianceATexture && RestirGIReservoirSampleRadianceBTexture)
    {
        std::swap(RestirGIReservoirSampleRadianceATexture, RestirGIReservoirSampleRadianceBTexture);
        std::swap(RestirGIReservoirSampleRadianceAState, RestirGIReservoirSampleRadianceBState);
        std::swap(RestirGIReservoirSampleRadianceASrvBindlessIndex, RestirGIReservoirSampleRadianceBSrvBindlessIndex);
        std::swap(RestirGIReservoirSampleRadianceAUavBindlessIndex, RestirGIReservoirSampleRadianceBUavBindlessIndex);
    }

    if (RestirGIReservoirRayDirectionATexture && RestirGIReservoirRayDirectionBTexture)
    {
        std::swap(RestirGIReservoirRayDirectionATexture, RestirGIReservoirRayDirectionBTexture);
        std::swap(RestirGIReservoirRayDirectionAState, RestirGIReservoirRayDirectionBState);
        std::swap(RestirGIReservoirRayDirectionASrvBindlessIndex, RestirGIReservoirRayDirectionBSrvBindlessIndex);
        std::swap(RestirGIReservoirRayDirectionAUavBindlessIndex, RestirGIReservoirRayDirectionBUavBindlessIndex);
    }

    if (RestirGIReservoirMWATexture && RestirGIReservoirMWBTexture)
    {
        std::swap(RestirGIReservoirMWATexture, RestirGIReservoirMWBTexture);
        std::swap(RestirGIReservoirMWAState, RestirGIReservoirMWBState);
        std::swap(RestirGIReservoirMWASrvBindlessIndex, RestirGIReservoirMWBSrvBindlessIndex);
        std::swap(RestirGIReservoirMWAUavBindlessIndex, RestirGIReservoirMWBUavBindlessIndex);
    }

    if (bRestirGIEnabled && RestirGIHistoryTexture != nullptr)
    {
        const uint32_t MaxHistoryFrames = (std::max)(1u, RestirGIMaxHistoryFrames);
        RestirGIHistoryFrameCount = (std::min)(RestirGIHistoryFrameCount + 1u, MaxHistoryFrames);
        bRestirGIHistoryValid = RestirGIHistoryFrameCount > 0u;
    }
    else
    {
        RestirGIHistoryFrameCount = 0u;
        bRestirGIHistoryValid = false;
    }

    for (FSceneModelResource& Model : SceneModels)
    {
        Model.PreviousWorldMatrix = Model.WorldMatrix;
        Model.bHasPreviousWorldMatrix = true;
    }
}

void FDeferredRenderer::InvalidateRestirGiDenoiserHistory()
{
    bRestirGIHistoryValid = false;
    RestirGIHistoryFrameCount = 0;

    if (RestirGiHistoryCountASrvBindlessIndex != UINT32_MAX && RestirGiHistoryCountBSrvBindlessIndex != UINT32_MAX)
    {
        if (RestirGiHistoryCountASrvBindlessIndex > RestirGiHistoryCountBSrvBindlessIndex)
        {
            std::swap(RestirGiHistoryCountASrvBindlessIndex, RestirGiHistoryCountBSrvBindlessIndex);
            std::swap(RestirGiHistoryCountAUavBindlessIndex, RestirGiHistoryCountBUavBindlessIndex);
            std::swap(RestirGiHistoryCountATexture, RestirGiHistoryCountBTexture);
        }
    }
}

bool FDeferredRenderer::CreateBasePassRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Scene constant buffer (b0), used in Shaders/DeferredBasePass.hlsl VSMain and PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Base pass bindless indices (b1), used in Shaders/DeferredBasePass.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.Num32BitValues = 10;


    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.MipLODBias = 0.0f;
    SamplerDesc.MaxAnisotropy = 4;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(BasePassRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};
    // RootParams[0]: Lighting constants (b0), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Lighting bindless indices (b1), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.Num32BitValues = 13;

    // RootParams[2]: ReSTIR GI constants (b2), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[2].Constants.ShaderRegister = 2;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.Num32BitValues = 5;

    D3D12_STATIC_SAMPLER_DESC Samplers[3] = {};
    Samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    Samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    Samplers[0].MinLOD = 0.0f;
    Samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[0].ShaderRegister = 0;
    Samplers[0].RegisterSpace = 0;
    Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    Samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    Samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    Samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    Samplers[1].MinLOD = 0.0f;
    Samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[1].ShaderRegister = 1;
    Samplers[1].RegisterSpace = 0;
    Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    Samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    Samplers[2].MinLOD = 0.0f;
    Samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[2].ShaderRegister = 2;
    Samplers[2].RegisterSpace = 0;
    Samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = _countof(Samplers);
    RootSigDesc.Desc_1_1.pStaticSamplers = Samplers;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(LightingRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateRestirGIRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[4] = {};

    // RootParams[0]: TLAS SRV (t0), used in Shaders/RestirGI.hlsl
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Scene constants CBV (b0), used in Shaders/SceneConstants.hlsl
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Descriptor.ShaderRegister = 0;
    RootParams[1].Descriptor.RegisterSpace = 0;
    RootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[2]: ReSTIR GI constants (b1)
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].Constants.Num32BitValues = 19;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 1;

    // RootParams[3]: ReSTIR GI bindless indices (b2)
    RootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[3].Constants.Num32BitValues = 28;
    RootParams[3].Constants.RegisterSpace = 0;
    RootParams[3].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RestirGIRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateVelocityRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.Num32BitValues = 10;

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[2].Constants.ShaderRegister = 2;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.Num32BitValues = 33;


    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.MipLODBias = 0.0f;
    SamplerDesc.MaxAnisotropy = 4;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(VelocityRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateVelocityPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> VSByteCodeSkinned;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCodeSkinned, { L"USE_SKINNING=1" }))
    {
        return false;
    }

    std::array<std::vector<uint8_t>, 4> PSByteCodes;
    for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
    {
        for (uint32_t Permutation = 0; Permutation < 2; ++Permutation)
        {
            const uint32_t PipelineIndex = Permutation | (DoubleSidedVariant << 1);
            std::vector<std::wstring> Defines;
            Defines.push_back(Permutation != 0 ? L"USE_ALPHA_MASK=1" : L"USE_ALPHA_MASK=0");
            Defines.push_back(DoubleSidedVariant != 0 ? L"USE_DOUBLE_SIDED=1" : L"USE_DOUBLE_SIDED=0");
            if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMainVelocity", PSTarget, PSByteCodes[PipelineIndex], Defines))
            {
                return false;
            }
        }
    }

    auto InitializeVelocityDesc = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, const std::vector<uint8_t>& VertexShader)
    {
        Desc = {};
        Desc.pRootSignature = VelocityRootSignature.Get();
        Desc.InputLayout = { nullptr, 0 };
        Desc.VS = { VertexShader.data(), VertexShader.size() };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.SampleDesc.Count = 1;
        Desc.SampleMask = UINT_MAX;

        Desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        Desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        Desc.RasterizerState.FrontCounterClockwise = TRUE;

        Desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        Desc.BlendState.IndependentBlendEnable = FALSE;

        Desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        Desc.DepthStencilState.DepthEnable = TRUE;
        Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        Desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

        Desc.NumRenderTargets = 1;
        Desc.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
        Desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
    {
        for (uint32_t Permutation = 0; Permutation < 2; ++Permutation)
        {
            const uint32_t PipelineIndex = Permutation | (DoubleSidedVariant << 1);
            InitializeVelocityDesc(PsoDesc, VSByteCode);
            PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            PsoDesc.PS = { PSByteCodes[PipelineIndex].data(), PSByteCodes[PipelineIndex].size() };
            HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(VelocityPipelines[PipelineIndex].GetAddressOf())));

            InitializeVelocityDesc(PsoDesc, VSByteCodeSkinned);
            PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            PsoDesc.PS = { PSByteCodes[PipelineIndex].data(), PSByteCodes[PipelineIndex].size() };
            HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(VelocityPipelinesSkinned[PipelineIndex].GetAddressOf())));
        }
    }

    return true;
}

bool FDeferredRenderer::CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat)
{
    FShaderCompiler Compiler;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, DeferredBasePassVsBytecodes[0]))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, DeferredBasePassVsBytecodes[1], { L"USE_SKINNING=1" }))
    {
        return false;
    }

    DeferredBasePassLightingFormat = LightingFormat;
    for (uint32_t PipelineKey = 0; PipelineKey < 512; ++PipelineKey)
    {
        BasePassPipelines[PipelineKey].Reset();
        BasePassPipelinesSkinned[PipelineKey].Reset();
        DeferredBasePassPsBytecodes[PipelineKey].clear();
        DeferredBasePassPsCompiled[PipelineKey] = false;
        DeferredBasePassFailureLogged[PipelineKey] = false;
    }

    return true;
}


bool FDeferredRenderer::CompileDeferredBasePassPs(uint32_t PipelineKey, std::vector<uint8_t>& OutPs)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    const bool bUseNormal = (PipelineKey & 1u) != 0;
    const bool bUseMR = (PipelineKey & 2u) != 0;
    const bool bUseBaseColor = (PipelineKey & 4u) != 0;
    const bool bUseEmissive = (PipelineKey & 8u) != 0;
    const bool bUseAlphaMask = (PipelineKey & 16u) != 0;
    const bool bUseSheenModel = (PipelineKey & 32u) != 0;
    const bool bUseClearcoatModel = (PipelineKey & 64u) != 0;
    const bool bUseAnisotropyModel = (PipelineKey & 128u) != 0;
    const bool bUseDoubleSided = (PipelineKey & 256u) != 0;

    std::vector<std::wstring> Defines;
    Defines.push_back(bUseNormal ? L"USE_NORMAL_MAP=1" : L"USE_NORMAL_MAP=0");
    Defines.push_back(bUseMR ? L"USE_METALLIC_ROUGHNESS_MAP=1" : L"USE_METALLIC_ROUGHNESS_MAP=0");
    Defines.push_back(bUseBaseColor ? L"USE_BASE_COLOR_MAP=1" : L"USE_BASE_COLOR_MAP=0");
    Defines.push_back(bUseEmissive ? L"USE_EMISSIVE_MAP=1" : L"USE_EMISSIVE_MAP=0");
    Defines.push_back(bUseSheenModel ? L"SHADINGMODEL_SHEEN=1" : L"SHADINGMODEL_SHEEN=0");
    Defines.push_back(bUseClearcoatModel ? L"SHADINGMODEL_CLEARCOAT=1" : L"SHADINGMODEL_CLEARCOAT=0");
    Defines.push_back(bUseAnisotropyModel ? L"SHADINGMODEL_ANISOTROPY=1" : L"SHADINGMODEL_ANISOTROPY=0");
    Defines.push_back(bUseDoubleSided ? L"USE_DOUBLE_SIDED=1" : L"USE_DOUBLE_SIDED=0");
    if (bUseAlphaMask)
    {
        Defines.push_back(L"USE_ALPHA_MASK=1");
    }

    return Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, OutPs, Defines);
}

bool FDeferredRenderer::BuildDeferredBasePassPsoDesc(uint32_t PipelineKey, bool bUseSkinning, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const
{
    if (DeferredBasePassLightingFormat == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    OutDesc = {};
    OutDesc.pRootSignature = BasePassRootSignature.Get();
    OutDesc.InputLayout = { nullptr, 0 };
    const std::vector<uint8_t>& VsBytecode = DeferredBasePassVsBytecodes[bUseSkinning ? 1u : 0u];
    OutDesc.VS = { VsBytecode.data(), VsBytecode.size() };
    OutDesc.PS = { DeferredBasePassPsBytecodes[PipelineKey].data(), DeferredBasePassPsBytecodes[PipelineKey].size() };
    OutDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    OutDesc.SampleDesc.Count = 1;
    OutDesc.SampleMask = UINT_MAX;

    OutDesc.RasterizerState = {};
    OutDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    OutDesc.RasterizerState.CullMode = (PipelineKey & 256u) != 0 ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
    OutDesc.RasterizerState.FrontCounterClockwise = TRUE;
    OutDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    OutDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    OutDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    OutDesc.RasterizerState.DepthClipEnable = TRUE;
    OutDesc.RasterizerState.MultisampleEnable = FALSE;
    OutDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    OutDesc.RasterizerState.ForcedSampleCount = 0;
    OutDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    OutDesc.BlendState = {};
    OutDesc.BlendState.AlphaToCoverageEnable = FALSE;
    OutDesc.BlendState.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 5; ++i)
    {
        D3D12_RENDER_TARGET_BLEND_DESC RtBlend = {};
        RtBlend.BlendEnable = FALSE;
        RtBlend.LogicOpEnable = FALSE;
        RtBlend.SrcBlend = D3D12_BLEND_ONE;
        RtBlend.DestBlend = D3D12_BLEND_ZERO;
        RtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        RtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        RtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        RtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        RtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
        RtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        OutDesc.BlendState.RenderTarget[i] = RtBlend;
    }

    OutDesc.DepthStencilState = {};
    OutDesc.DepthStencilState.DepthEnable = TRUE;
    OutDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    OutDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    OutDesc.DepthStencilState.StencilEnable = FALSE;
    OutDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    OutDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    OutDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    OutDesc.DepthStencilState.BackFace = OutDesc.DepthStencilState.FrontFace;
    OutDesc.NumRenderTargets = 5;
    OutDesc.RTVFormats[0] = GBufferFormats[0];
    OutDesc.RTVFormats[1] = GBufferFormats[1];
    OutDesc.RTVFormats[2] = GBufferFormats[2];
    OutDesc.RTVFormats[3] = GBufferFormats[3];
    OutDesc.RTVFormats[4] = DeferredBasePassLightingFormat;
    OutDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    OutDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return true;
}

bool FDeferredRenderer::EnsureBasePassPipeline(uint32_t PipelineKey, bool bUseSkinning)
{
    auto& TargetPipeline = bUseSkinning ? BasePassPipelinesSkinned[PipelineKey] : BasePassPipelines[PipelineKey];
    if (TargetPipeline)
    {
        return true;
    }

    std::lock_guard<std::mutex> Lock(DeferredBasePassPipelineMutex);
    if (TargetPipeline)
    {
        return true;
    }

    if (!DeferredBasePassPsCompiled[PipelineKey])
    {
        if (!CompileDeferredBasePassPs(PipelineKey, DeferredBasePassPsBytecodes[PipelineKey]))
        {
            return false;
        }
        DeferredBasePassPsCompiled[PipelineKey] = true;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = {};
    if (!BuildDeferredBasePassPsoDesc(PipelineKey, bUseSkinning, Desc))
    {
        return false;
    }

    HRESULT Hr = Device->GetDevice()->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(TargetPipeline.GetAddressOf()));
    if (FAILED(Hr))
    {
        return false;
    }

    LogInfo(std::string("Deferred BasePass pipeline created. key=") + std::to_string(PipelineKey) + ", skinned=" + (bUseSkinning ? "1" : "0"));
    return true;
}

bool FDeferredRenderer::EnsureBasePassPipelineOrFail(uint32_t PipelineKey, bool bUseSkinning, const char* PassContext)
{
    if (EnsureBasePassPipeline(PipelineKey, bUseSkinning))
    {
        return true;
    }

    if (!DeferredBasePassFailureLogged[PipelineKey])
    {
        DeferredBasePassFailureLogged[PipelineKey] = true;
        LogError(std::string("Deferred BasePass pipeline creation failed. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", key=" + std::to_string(PipelineKey)
            + ", skinned=" + (bUseSkinning ? "1" : "0"));
    }

    SetRenderFatalError(std::string("Deferred BasePass fatal failure. context=")
        + (PassContext ? PassContext : "Unknown")
        + ", key=" + std::to_string(PipelineKey)
        + ", skinned=" + (bUseSkinning ? "1" : "0"));
    return false;
}

bool FDeferredRenderer::CreateDepthPrepassPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> VSByteCodeSkinned;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCodeSkinned, { L"USE_SKINNING=1" }))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = BasePassRootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Single-sided and double-sided depth prepass pipelines are created per variant
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PsoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;
    PsoDesc.RasterizerState.MultisampleEnable = FALSE;
    PsoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    PsoDesc.RasterizerState.ForcedSampleCount = 0;
    PsoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    PsoDesc.BlendState.IndependentBlendEnable = FALSE;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    PsoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    PsoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    PsoDesc.DepthStencilState.BackFace = PsoDesc.DepthStencilState.FrontFace;
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
    {
        PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipelines[DoubleSidedVariant].GetAddressOf())));
        PsoDesc.VS = { VSByteCodeSkinned.data(), VSByteCodeSkinned.size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipelinesSkinned[DoubleSidedVariant].GetAddressOf())));
    }
    return true;
}

bool FDeferredRenderer::CreateLinearDepthRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Linear depth constants (b0), used in Shaders/LinearDepth.hlsl VSMain and PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Depth bindless index (b1), used in Shaders/LinearDepth.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 1;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(LinearDepthRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLinearDepthPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/LinearDepth.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/LinearDepth.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LinearDepthRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(LinearDepthPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCodes[4];

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> DefaultDefines;
    const std::vector<std::wstring> ShadowMaskDefines = { L"USE_SHADOW_MASK=1" };
    const std::vector<std::wstring> ResearchDefines = { L"USE_PBR_RESEARCH=1" };
    const std::vector<std::wstring> ShadowMaskResearchDefines = { L"USE_SHADOW_MASK=1", L"USE_PBR_RESEARCH=1" };

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[0], DefaultDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[1], ShadowMaskDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[2], ResearchDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[3], ShadowMaskResearchDefines))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LightingRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCodes[0].data(), PSByteCodes[0].size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = LightingBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    for (size_t Index = 0; Index < LightingPipelines.size(); ++Index)
    {
        PsoDesc.PS = { PSByteCodes[Index].data(), PSByteCodes[Index].size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(LightingPipelines[Index].GetAddressOf())));
    }
    return true;
}

bool FDeferredRenderer::CreateRestirGIPipeline(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    if (!Device->IsRayTracingSupported())
    {
        LogWarning("Skipping ReSTIR GI pipeline creation: DXR is not supported.");
        return false;
    }

    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> InitialByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSInitialSampling", CSTarget, InitialByteCode))
    {
        return false;
    }

    std::vector<uint8_t> TemporalByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSTemporalResampling", CSTarget, TemporalByteCode))
    {
        return false;
    }

    std::vector<uint8_t> SpatialByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSSpatialResampling", CSTarget, SpatialByteCode))
    {
        return false;
    }

    std::vector<uint8_t> ResolveByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSResolve", CSTarget, ResolveByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RestirGIRootSignature.Get();

    auto CreateComputePso = [Device](const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipeline, const char* PipelineName)
    {
        const HRESULT Hr = Device->GetDevice()->CreateComputePipelineState(&Desc, IID_PPV_ARGS(OutPipeline.GetAddressOf()));
        if (FAILED(Hr))
        {
            std::ostringstream Oss;
            Oss << "ReSTIR GI pipeline creation failed for " << PipelineName << ", hr=0x" << std::hex << static_cast<uint32_t>(Hr);
            LogWarning(Oss.str());
            return false;
        }
        return true;
    };

    PsoDesc.CS = { InitialByteCode.data(), InitialByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIInitialPipeline, "CSInitialSampling"))
    {
        return false;
    }

    PsoDesc.CS = { TemporalByteCode.data(), TemporalByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGITemporalPipeline, "CSTemporalResampling"))
    {
        RestirGIInitialPipeline.Reset();
        return false;
    }

    PsoDesc.CS = { SpatialByteCode.data(), SpatialByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGISpatialPipeline, "CSSpatialResampling"))
    {
        RestirGIInitialPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        return false;
    }

    PsoDesc.CS = { ResolveByteCode.data(), ResolveByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIResolvePipeline, "CSResolve"))
    {
        RestirGIInitialPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        RestirGISpatialPipeline.Reset();
        return false;
    }

    return true;
}

bool FDeferredRenderer::CreateGtaoRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Scene constants (b0) used in Gtao.hlsl.
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: GTAO bindless indices (b1) used in Gtao.hlsl.
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 3;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(GtaoRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateGtaoPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::array<std::vector<uint8_t>, 8> PSByteCodes;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/Gtao.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> JitterOffDefines = { L"GTAO_USE_JITTER=0" };
    const std::vector<std::wstring> JitterOnDefines = { L"GTAO_USE_JITTER=1" };
    if (!Compiler.CompileFromFile(L"Shaders/Gtao.hlsl", L"PSMain", PSTarget, PSByteCodes[0], JitterOffDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/Gtao.hlsl", L"PSMain", PSTarget, PSByteCodes[1], JitterOnDefines))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = GtaoRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCodes[0].data(), PSByteCodes[0].size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(GtaoPipelines[0].GetAddressOf())));
    PsoDesc.PS = { PSByteCodes[1].data(), PSByteCodes[1].size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(GtaoPipelines[1].GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};

    // RootParams[0]: Scene constants (b0), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: SSR constants (b1), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 15;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    // RootParams[2]: SSR bindless indices (b2), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[2].Constants.Num32BitValues = 9;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/SsrSWTracePS.hlsl", L"VSMain", VSTarget, SsrGraphicsVsBytecode))
    {
        return false;
    }

    for (size_t PipelineIndex = 0; PipelineIndex < SsrPipelines.size(); ++PipelineIndex)
    {
        SsrPipelines[PipelineIndex].Reset();
        SsrGraphicsPsBytecodes[PipelineIndex].clear();
        SsrGraphicsPsCompiled[PipelineIndex] = false;
        SsrGraphicsFailureLogged[PipelineIndex] = false;
    }

    return true;
}


bool FDeferredRenderer::CompileSsrGraphicsPs(uint32_t PipelineIndex, std::vector<uint8_t>& OutPs)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    const bool bUseHzb = (PipelineIndex & 2u) != 0;
    const bool bUseRefine = (PipelineIndex & 1u) != 0;
    const bool bUseSwSsr = (PipelineIndex & 4u) == 0;

    const std::vector<std::wstring> Defines =
    {
        bUseHzb ? L"HZB_ENABLED=1" : L"HZB_ENABLED=0",
        bUseRefine ? L"SSR_REFINE_ENABLED=1" : L"SSR_REFINE_ENABLED=0",
        bUseSwSsr ? L"SW_SSR_ENABLED=1" : L"SW_SSR_ENABLED=0"
    };

    return Compiler.CompileFromFile(L"Shaders/SsrSWTracePS.hlsl", L"PSMain", PSTarget, OutPs, Defines);
}

bool FDeferredRenderer::BuildSsrGraphicsPsoDesc(uint32_t PipelineIndex, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const
{
    OutDesc = {};
    OutDesc.pRootSignature = SsrRootSignature.Get();
    OutDesc.VS = { SsrGraphicsVsBytecode.data(), SsrGraphicsVsBytecode.size() };
    OutDesc.PS = { SsrGraphicsPsBytecodes[PipelineIndex].data(), SsrGraphicsPsBytecodes[PipelineIndex].size() };
    OutDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    OutDesc.SampleDesc.Count = 1;
    OutDesc.SampleMask = UINT_MAX;

    OutDesc.RasterizerState = {};
    OutDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    OutDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    OutDesc.RasterizerState.FrontCounterClockwise = TRUE;
    OutDesc.RasterizerState.DepthClipEnable = TRUE;

    OutDesc.BlendState = {};
    OutDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    OutDesc.DepthStencilState = {};
    OutDesc.DepthStencilState.DepthEnable = FALSE;
    OutDesc.DepthStencilState.StencilEnable = FALSE;
    OutDesc.NumRenderTargets = 1;
    OutDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    OutDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    return true;
}

bool FDeferredRenderer::EnsureSsrGraphicsPipeline(uint32_t PipelineIndex)
{
    if (PipelineIndex >= SsrPipelines.size())
    {
        return false;
    }

    if (SsrPipelines[PipelineIndex])
    {
        return true;
    }

    std::lock_guard<std::mutex> Lock(SsrGraphicsPipelineMutex);
    if (SsrPipelines[PipelineIndex])
    {
        return true;
    }

    if (!SsrGraphicsPsCompiled[PipelineIndex])
    {
        if (!CompileSsrGraphicsPs(PipelineIndex, SsrGraphicsPsBytecodes[PipelineIndex]))
        {
            return false;
        }
        SsrGraphicsPsCompiled[PipelineIndex] = true;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = {};
    if (!BuildSsrGraphicsPsoDesc(PipelineIndex, Desc))
    {
        return false;
    }

    HRESULT Hr = Device->GetDevice()->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(SsrPipelines[PipelineIndex].GetAddressOf()));
    if (FAILED(Hr))
    {
        return false;
    }

    LogInfo("SSR graphics pipeline created. index=" + std::to_string(PipelineIndex));
    return true;
}

bool FDeferredRenderer::EnsureSsrGraphicsPipelineOrFail(uint32_t PipelineIndex, const char* PassContext)
{
    if (EnsureSsrGraphicsPipeline(PipelineIndex))
    {
        return true;
    }

    if (PipelineIndex < SsrGraphicsFailureLogged.size() && !SsrGraphicsFailureLogged[PipelineIndex])
    {
        SsrGraphicsFailureLogged[PipelineIndex] = true;
        LogError(std::string("SSR graphics pipeline creation failed. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", index=" + std::to_string(PipelineIndex));
    }

    SetRenderFatalError(std::string("SSR graphics fatal failure. context=")
        + (PassContext ? PassContext : "Unknown")
        + ", index=" + std::to_string(PipelineIndex));
    return false;
}

bool FDeferredRenderer::CreateSsrDenoiseRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 5;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrDenoiseRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrDenoisePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/ScreenSpaceReflectionsDenoise.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/ScreenSpaceReflectionsDenoise.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrDenoiseRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(SsrDenoisePipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrRayGatherRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 11;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].Constants.Num32BitValues = 6;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrRayGatherRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrRayGatherPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/SsrRayGather.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrRayGatherRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SsrRayGatherPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrSwTraceRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 13;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].Constants.Num32BitValues = 10;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrSwTraceRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrSwTracePipeline(FDX12Device* Device)
{
    (void)Device;

    for (size_t PipelineIndex = 0; PipelineIndex < SsrSwTracePipelines.size(); ++PipelineIndex)
    {
        SsrSwTracePipelines[PipelineIndex].Reset();
        SsrSwTraceCsBytecodes[PipelineIndex].clear();
        SsrSwTraceCsCompiled[PipelineIndex] = false;
        SsrSwTraceFailureLogged[PipelineIndex] = false;
    }

    return true;
}


bool FDeferredRenderer::CompileSsrSwTraceCs(uint32_t PipelineIndex, std::vector<uint8_t>& OutCs)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    const bool bUseHzb = (PipelineIndex & 2u) != 0;
    const bool bUseRefine = (PipelineIndex & 1u) != 0;
    const bool bUseSwSsr = (PipelineIndex & 4u) == 0;

    const std::vector<std::wstring> Defines =
    {
        bUseHzb ? L"HZB_ENABLED=1" : L"HZB_ENABLED=0",
        bUseRefine ? L"SSR_REFINE_ENABLED=1" : L"SSR_REFINE_ENABLED=0",
        bUseSwSsr ? L"SW_SSR_ENABLED=1" : L"SW_SSR_ENABLED=0"
    };

    return Compiler.CompileFromFile(L"Shaders/SsrSWTraceCS.hlsl", L"CSMain", CSTarget, OutCs, Defines);
}

bool FDeferredRenderer::EnsureSsrSwTracePipeline(uint32_t PipelineIndex)
{
    if (PipelineIndex >= SsrSwTracePipelines.size())
    {
        return false;
    }

    if (SsrSwTracePipelines[PipelineIndex])
    {
        return true;
    }

    std::lock_guard<std::mutex> Lock(SsrSwTracePipelineMutex);
    if (SsrSwTracePipelines[PipelineIndex])
    {
        return true;
    }

    if (!SsrSwTraceCsCompiled[PipelineIndex])
    {
        if (!CompileSsrSwTraceCs(PipelineIndex, SsrSwTraceCsBytecodes[PipelineIndex]))
        {
            return false;
        }
        SsrSwTraceCsCompiled[PipelineIndex] = true;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC Desc = {};
    Desc.pRootSignature = SsrSwTraceRootSignature.Get();
    Desc.CS = { SsrSwTraceCsBytecodes[PipelineIndex].data(), SsrSwTraceCsBytecodes[PipelineIndex].size() };
    HRESULT Hr = Device->GetDevice()->CreateComputePipelineState(&Desc, IID_PPV_ARGS(SsrSwTracePipelines[PipelineIndex].GetAddressOf()));
    if (FAILED(Hr))
    {
        return false;
    }

    LogInfo("SSR SW trace pipeline created. index=" + std::to_string(PipelineIndex));
    return true;
}

bool FDeferredRenderer::EnsureSsrSwTracePipelineOrFail(uint32_t PipelineIndex, const char* PassContext)
{
    if (EnsureSsrSwTracePipeline(PipelineIndex))
    {
        return true;
    }

    if (PipelineIndex < SsrSwTraceFailureLogged.size() && !SsrSwTraceFailureLogged[PipelineIndex])
    {
        SsrSwTraceFailureLogged[PipelineIndex] = true;
        LogError(std::string("SSR SW trace pipeline creation failed. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", index=" + std::to_string(PipelineIndex));
    }

    SetRenderFatalError(std::string("SSR SW trace fatal failure. context=")
        + (PassContext ? PassContext : "Unknown")
        + ", index=" + std::to_string(PipelineIndex));
    return false;
}

bool FDeferredRenderer::CreateSsrBuildIndirectArgsRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 2;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 2;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrBuildIndirectArgsRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrBuildIndirectArgsPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/SsrBuildIndirectArgs.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrBuildIndirectArgsRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SsrBuildIndirectArgsPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrResolveRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 5;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 1;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 6;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrResolveRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrResolvePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/SsrResolve.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrResolveRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SsrResolvePipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateSsrDispatchCommandSignature(FDX12Device* Device)
{
    if (Device == nullptr)
    {
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC ArgumentDesc = {};
    ArgumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    CommandDesc.NumArgumentDescs = 1;
    CommandDesc.pArgumentDescs = &ArgumentDesc;

    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, nullptr, IID_PPV_ARGS(SsrDispatchCommandSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateHZBRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    // RootParams[0]: HZB constants (mip counts, dimensions, source mip), used in Shaders/BuildHZB.hlsl BuildHZB
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 12;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: HZB bindless indices (b1), used in Shaders/BuildHZB.hlsl BuildHZB
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 6;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(HZBRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateHZBPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    for (size_t PipelineIndex = 0; PipelineIndex < HZBPipelines.size(); ++PipelineIndex)
    {
        std::vector<uint8_t> CSByteCode;
        const std::wstring Define = L"HZB_MIPS_PER_DISPATCH=" + std::to_wstring(PipelineIndex + 1);
        const std::vector<std::wstring> Defines = { Define };

        if (!Compiler.CompileFromFile(L"Shaders/BuildHZB.hlsl", L"BuildHZB", CSTarget, CSByteCode, Defines))
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
        PsoDesc.pRootSignature = HZBRootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };

        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(HZBPipelines[PipelineIndex].GetAddressOf())));
    }
    return true;
}

bool FDeferredRenderer::CreateAutoExposureRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Auto exposure constants (input size, delta time, adaptation), used in Shaders/AutoExposure.hlsl CSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 9;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: Auto exposure bindless indices (b1), used in Shaders/AutoExposure.hlsl CSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 3;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(AutoExposureRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateAutoExposurePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/AutoExposure.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = AutoExposureRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(AutoExposurePipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateTaaRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    // RootParams[0]: TAA constants (output size, history weight, history toggle), used in Shaders/TemporalAA.hlsl CSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: TAA bindless indices (b1), used in Shaders/TemporalAA.hlsl CSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 3;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(TaaRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateTaaPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/TemporalAA.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = TaaRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(TaaPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreatePathTracingAccumulationRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    // RootParams[0]: PathTracing accumulation constants (output size, accumulation weight, history toggle)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: PathTracing accumulation bindless indices (b1) - 4 indices now
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 4;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(PathTracingAccumulationRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreatePathTracingAccumulationPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/PathTracingAccumulation.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = PathTracingAccumulationRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(PathTracingAccumulationPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateTonemapRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    // RootParams[0]: Tonemap constants (exposure/gamma/auto-exposure), used in Shaders/Tonemap.hlsl PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: Tonemap bindless indices (b1), used in Shaders/Tonemap.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 2;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(TonemapRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateTonemapPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/Tonemap.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/Tonemap.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = TonemapRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(TonemapPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateCasRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(CasRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateCasPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/Cas.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/Cas.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = CasRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(CasPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    Microsoft::WRL::ComPtr<ID3D12Resource>* Targets[4] = { &GBufferA, &GBufferB, &GBufferC, &GBufferD };
    const wchar_t* GBufferNames[4] = { L"GBufferA", L"GBufferB", L"GBufferC", L"GBufferD" };

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    const UINT RtvDescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = {};
    RtvHandle.ptr = 0;

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 6;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(GBufferRTVHeap.GetAddressOf())));
    if (GBufferRTVHeap)
    {
        GBufferRTVHeap->SetName(L"GBufferRTVHeap");
    }

    RtvHandle = GBufferRTVHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < 4; ++i)
    {
        CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
            GBufferFormats[i],
            Width,
            Height,
            1,
            1,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        D3D12_CLEAR_VALUE ClearValue = {};
        ClearValue.Format = Desc.Format;
        ClearValue.Color[0] = 0.0f;
        ClearValue.Color[1] = 0.0f;
        ClearValue.Color[2] = 0.0f;
        ClearValue.Color[3] = 1.0f;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &ClearValue,
            IID_PPV_ARGS(Targets[i]->GetAddressOf())));

        Targets[i]->Get()->SetName(GBufferNames[i]);

        GBufferRTVHandles[i] = RtvHandle;
        D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
        RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        RtvDesc.Format = GBufferFormats[i];
        Device->GetDevice()->CreateRenderTargetView(Targets[i]->Get(), &RtvDesc, RtvHandle);
        RtvHandle.ptr += RtvDescriptorSize;

        GBufferStates[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        LightingBufferFormat,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_CLEAR_VALUE LightingClear = {};
    LightingClear.Format = Desc.Format;
    LightingClear.Color[0] = 0.0f;
    LightingClear.Color[1] = 0.0f;
    LightingClear.Color[2] = 0.0f;
    LightingClear.Color[3] = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        LightingBufferState,
        &LightingClear,
        IID_PPV_ARGS(LightingBuffer.GetAddressOf())));

    LightingBuffer->SetName(L"LightingBuffer");

    LightingRTVHandle = RtvHandle;
    D3D12_RENDER_TARGET_VIEW_DESC LightingRtvDesc = {};
    LightingRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    LightingRtvDesc.Format = LightingBufferFormat;
    Device->GetDevice()->CreateRenderTargetView(LightingBuffer.Get(), &LightingRtvDesc, RtvHandle);
    RtvHandle.ptr += RtvDescriptorSize;

	Desc = CD3DX12_RESOURCE_DESC::Tex2D(
		BackBufferFormat,
		Width,
		Height,
		1,
		1,
		1,
		0,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    D3D12_CLEAR_VALUE TonemapClear = {};
    TonemapClear.Format = Desc.Format;
    TonemapClear.Color[0] = 0.0f;
    TonemapClear.Color[1] = 0.0f;
    TonemapClear.Color[2] = 0.0f;
    TonemapClear.Color[3] = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        TonemapOutputState,
        &TonemapClear,
        IID_PPV_ARGS(TonemapOutput.GetAddressOf())));

    TonemapOutput->SetName(L"TonemapOutput");

    TonemapOutputRtvHandle = RtvHandle;
    D3D12_RENDER_TARGET_VIEW_DESC TonemapRtvDesc = {};
    TonemapRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    TonemapRtvDesc.Format = BackBufferFormat;
    Device->GetDevice()->CreateRenderTargetView(TonemapOutput.Get(), &TonemapRtvDesc, RtvHandle);

    return true;
}

bool FDeferredRenderer::CreateLinearDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = Desc.Format;
    ClearValue.Color[0] = 0.0f;
    ClearValue.Color[1] = 0.0f;
    ClearValue.Color[2] = 0.0f;
    ClearValue.Color[3] = 0.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(LinearDepthTexture.GetAddressOf())));

    if (LinearDepthTexture)
    {
        LinearDepthTexture->SetName(L"LinearDepth");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 1;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(LinearDepthRtvHeap.GetAddressOf())));
    if (LinearDepthRtvHeap)
    {
        LinearDepthRtvHeap->SetName(L"LinearDepthRTVHeap");
    }

    LinearDepthRtvHandle = LinearDepthRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    Device->GetDevice()->CreateRenderTargetView(LinearDepthTexture.Get(), &RtvDesc, LinearDepthRtvHandle);

    LinearDepthState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
}

bool FDeferredRenderer::CreateGtaoResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8_UNORM,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = Desc.Format;
    ClearValue.Color[0] = 1.0f;
    ClearValue.Color[1] = 1.0f;
    ClearValue.Color[2] = 1.0f;
    ClearValue.Color[3] = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(GtaoTexture.GetAddressOf())));

    if (GtaoTexture)
    {
        GtaoTexture->SetName(L"GTAO");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 1;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(GtaoRtvHeap.GetAddressOf())));
    if (GtaoRtvHeap)
    {
        GtaoRtvHeap->SetName(L"GTAO_RTVHeap");
    }

    GtaoRtvHandle = GtaoRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R8_UNORM;
    Device->GetDevice()->CreateRenderTargetView(GtaoTexture.Get(), &RtvDesc, GtaoRtvHandle);

    GtaoState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
}

bool FDeferredRenderer::CreateRestirGIResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGITexture.GetAddressOf())));

    if (RestirGITexture)
    {
        RestirGITexture->SetName(L"ReSTIR_GI");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGIHistoryTexture.GetAddressOf())));

    if (RestirGIHistoryTexture)
    {
        RestirGIHistoryTexture->SetName(L"ReSTIR_GI_History");
    }

    const uint64_t ReservoirStride = sizeof(float) * 8u;
    const uint64_t ReservoirElementCount = static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height);
    const uint64_t ReservoirBufferSize = ReservoirStride * ReservoirElementCount;
    CD3DX12_RESOURCE_DESC ReservoirDesc = CD3DX12_RESOURCE_DESC::Buffer(ReservoirBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ReservoirDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGITemporalReservoirBuffer.GetAddressOf())));

    if (RestirGITemporalReservoirBuffer)
    {
        RestirGITemporalReservoirBuffer->SetName(L"ReSTIR_GI_TemporalReservoir");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ReservoirDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGISpatialReservoirBuffer.GetAddressOf())));

    if (RestirGISpatialReservoirBuffer)
    {
        RestirGISpatialReservoirBuffer->SetName(L"ReSTIR_GI_SpatialReservoir");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ReservoirDesc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(RestirGIReservoirHistoryBuffer.GetAddressOf())));

    if (RestirGIReservoirHistoryBuffer)
    {
        RestirGIReservoirHistoryBuffer->SetName(L"ReSTIR_GI_Reservoir_History");
    }

    const uint32_t HalfWidth = (Width + 1u) / 2u;
    const uint32_t HalfHeight = (Height + 1u) / 2u;

    auto CreateRestirGITexture = [&](DXGI_FORMAT Format, const wchar_t* Name, Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource)
    {
        CD3DX12_RESOURCE_DESC TextureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            Format,
            HalfWidth,
            HalfHeight,
            1,
            1,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &TextureDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(OutResource.ReleaseAndGetAddressOf())));

        if (OutResource)
        {
            OutResource->SetName(Name);
        }
    };

    CreateRestirGITexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"ReSTIR_GI_InitialRadiance", RestirGIInitialRadianceTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_InitialRayDirection", RestirGIInitialRayDirectionTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalA", RestirGIReservoirDepthNormalATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalB", RestirGIReservoirDepthNormalBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"ReSTIR_GI_ReservoirSampleRadianceA", RestirGIReservoirSampleRadianceATexture);
    CreateRestirGITexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"ReSTIR_GI_ReservoirSampleRadianceB", RestirGIReservoirSampleRadianceBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_ReservoirRayDirectionA", RestirGIReservoirRayDirectionATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_ReservoirRayDirectionB", RestirGIReservoirRayDirectionBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_FLOAT, L"ReSTIR_GI_ReservoirMWA", RestirGIReservoirMWATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_FLOAT, L"ReSTIR_GI_ReservoirMWB", RestirGIReservoirMWBTexture);

    RestirGIState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIHistoryState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGITemporalReservoirState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGISpatialReservoirState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirHistoryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    RestirGIInitialRadianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIInitialRayDirectionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirDepthNormalAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirDepthNormalBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirSampleRadianceAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirSampleRadianceBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirRayDirectionAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirRayDirectionBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirMWAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirMWBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bRestirGIHistoryValid = false;
    return true;
}

bool FDeferredRenderer::CreateRestirGiDenoiserResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto CreateTexture = [&](DXGI_FORMAT Format, uint32_t InWidth, uint32_t InHeight, const wchar_t* Name, Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource)
    {
        CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
            Format,
            InWidth,
            InHeight,
            1,
            1,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(OutResource.ReleaseAndGetAddressOf())));

        if (OutResource)
        {
            OutResource->SetName(Name);
        }
    };

    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, L"ReSTIR_GI_InputSH", RestirGiInputSHTexture);
    CreateTexture(DXGI_FORMAT_R8_UNORM, Width, Height, L"ReSTIR_GI_Variance", RestirGiVarianceTexture);
    CreateTexture(DXGI_FORMAT_R11G11B10_FLOAT, Width, Height, L"ReSTIR_GI_HistoryIrradiance", RestirGiHistoryIrradianceTexture);
    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, L"ReSTIR_GI_TemporalSH", RestirGiTemporalSHTexture);
    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, L"ReSTIR_GI_HistorySH", RestirGiHistorySHTexture);
    CreateTexture(DXGI_FORMAT_R8_UINT, Width, Height, L"ReSTIR_GI_HistoryCountA", RestirGiHistoryCountATexture);
    CreateTexture(DXGI_FORMAT_R8_UINT, Width, Height, L"ReSTIR_GI_HistoryCountB", RestirGiHistoryCountBTexture);
    CreateTexture(DXGI_FORMAT_R16_FLOAT, Width, Height, L"ReSTIR_GI_PrevLinearDepth", RestirGiPrevLinearDepthTexture);
    CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, Width, Height, L"ReSTIR_GI_PrevNormal", RestirGiPrevNormalTexture);

    uint32_t MipWidth = (Width + 1u) / 2u;
    uint32_t MipHeight = (Height + 1u) / 2u;
    for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
    {
        const std::wstring ShName = L"ReSTIR_GI_SH_Mip_" + std::to_wstring(MipIndex);
        const std::wstring DepthName = L"ReSTIR_GI_LinearDepth_Mip_" + std::to_wstring(MipIndex);
        CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, MipWidth, MipHeight, ShName.c_str(), RestirGiShMipTextures[MipIndex]);
        CreateTexture(DXGI_FORMAT_R16_FLOAT, MipWidth, MipHeight, DepthName.c_str(), RestirGiLinearDepthMipTextures[MipIndex]);
        MipWidth = (std::max)(1u, (MipWidth + 1u) / 2u);
        MipHeight = (std::max)(1u, (MipHeight + 1u) / 2u);
    }

    RestirGiInputSHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiVarianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistoryIrradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiTemporalSHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistorySHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistoryCountAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistoryCountBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiPrevLinearDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiPrevNormalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
    {
        RestirGiShMipStates[MipIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        RestirGiLinearDepthMipStates[MipIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    return true;
}

bool FDeferredRenderer::CreateRestirGiDenoiserPipelines(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 10;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 16;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RestirGiDenoiserRootSignature.ReleaseAndGetAddressOf())));

    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);
    auto CreateDenoiserPso = [&](const wchar_t* EntryPoint, Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipeline) -> bool
    {
        std::vector<uint8_t> CSByteCode;
        if (!Compiler.CompileFromFile(L"Shaders/RestirGIDenoiser.hlsl", EntryPoint, CSTarget, CSByteCode))
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
        PsoDesc.pRootSignature = RestirGiDenoiserRootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(OutPipeline.ReleaseAndGetAddressOf())));
        return true;
    };

    if (!CreateDenoiserPso(L"CSPreBlur", RestirGiPreBlurPipeline)
        || !CreateDenoiserPso(L"CSTemporalAccumulation", RestirGiTemporalAccumulationPipeline)
        || !CreateDenoiserPso(L"CSGenerateShMips", RestirGiGenerateShMipsPipeline)
        || !CreateDenoiserPso(L"CSGenerateLinearDepthMips", RestirGiGenerateLinearDepthMipsPipeline)
        || !CreateDenoiserPso(L"CSHistoryReconstruction", RestirGiHistoryReconstructionPipeline)
        || !CreateDenoiserPso(L"CSFinalBlur", RestirGiFinalBlurPipeline))
    {
        return false;
    }
    return true;
}

bool FDeferredRenderer::CreateVelocityResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = DXGI_FORMAT_R16G16_FLOAT;
    ClearValue.Color[0] = 0.0f;
    ClearValue.Color[1] = 0.0f;
    ClearValue.Color[2] = 0.0f;
    ClearValue.Color[3] = 0.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(VelocityTexture.GetAddressOf())));

    if (VelocityTexture)
    {
        VelocityTexture->SetName(L"Velocity");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 1;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(VelocityRtvHeap.GetAddressOf())));

    if (VelocityRtvHeap)
    {
        VelocityRtvHeap->SetName(L"VelocityRtvHeap");
    }

    VelocityRtvHandle = VelocityRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    Device->GetDevice()->CreateRenderTargetView(VelocityTexture.Get(), &RtvDesc, VelocityRtvHandle);

    VelocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bHasPreviousViewProjection = false;
    bHasPreviousUnjitteredViewProjection = false;
    return true;
}

bool FDeferredRenderer::CreateSsrResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = Desc.Format;
    ClearValue.Color[0] = 0.0f;
    ClearValue.Color[1] = 0.0f;
    ClearValue.Color[2] = 0.0f;
    ClearValue.Color[3] = 0.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(SsrTexture.GetAddressOf())));

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(SsrDenoiseTexture.GetAddressOf())));

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        &ClearValue,
        IID_PPV_ARGS(SsrResolveTexture.GetAddressOf())));

    CD3DX12_RESOURCE_DESC FallbackDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &FallbackDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(SsrFallbackTexture.GetAddressOf())));

    if (SsrTexture)
    {
        SsrTexture->SetName(L"SSR");
    }

    if (SsrDenoiseTexture)
    {
        SsrDenoiseTexture->SetName(L"SSR_Denoise");
    }

    if (SsrResolveTexture)
    {
        SsrResolveTexture->SetName(L"SSR_Resolve");
    }

    if (SsrFallbackTexture)
    {
        SsrFallbackTexture->SetName(L"SSR_Fallback");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 2;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(SsrRtvHeap.GetAddressOf())));
    if (SsrRtvHeap)
    {
        SsrRtvHeap->SetName(L"SSR_RTVHeap");
    }

    SsrRtvHandle = SsrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT RtvDescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    SsrDenoiseRtvHandle = SsrRtvHandle;
    SsrDenoiseRtvHandle.ptr += RtvDescriptorSize;
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Device->GetDevice()->CreateRenderTargetView(SsrTexture.Get(), &RtvDesc, SsrRtvHandle);
    Device->GetDevice()->CreateRenderTargetView(SsrDenoiseTexture.Get(), &RtvDesc, SsrDenoiseRtvHandle);

    SsrState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    SsrDenoiseState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    SsrFallbackState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    SsrResolveState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    const uint32_t Frames = GetFramesInFlight();
    SsrMaxRayCount = Width * Height;
    SsrRayListBuffers.clear();
    SsrRayListBuffers.resize(Frames);
    SsrRayCounterBuffers.clear();
    SsrRayCounterBuffers.resize(Frames);
    SsrRayListSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayCounterStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayListPrimaryBuffers.clear();
    SsrRayListPrimaryBuffers.resize(Frames);
    SsrRayCounterPrimaryBuffers.clear();
    SsrRayCounterPrimaryBuffers.resize(Frames);
    SsrRayListHwMissBuffers.clear();
    SsrRayListHwMissBuffers.resize(Frames);
    SsrRayCounterHwMissBuffers.clear();
    SsrRayCounterHwMissBuffers.resize(Frames);
    SsrRayListPrimarySrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListPrimaryUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterPrimarySrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterPrimaryUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListHwMissSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListHwMissUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterHwMissSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterHwMissUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListPrimaryStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayCounterPrimaryStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayListHwMissStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayCounterHwMissStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrIndirectArgsPrimaryBuffers.clear();
    SsrIndirectArgsPrimaryBuffers.resize(Frames);
    SsrIndirectArgsHwMissBuffers.clear();
    SsrIndirectArgsHwMissBuffers.resize(Frames);
    SsrIndirectArgsPrimaryUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrIndirectArgsHwMissUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrIndirectArgsPrimaryStates.assign(Frames, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SsrIndirectArgsHwMissStates.assign(Frames, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const uint64_t RayListSize = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
    CD3DX12_HEAP_PROPERTIES DefaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC RayListDesc = CD3DX12_RESOURCE_DESC::Buffer(RayListSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_RESOURCE_DESC CounterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_RESOURCE_DESC IndirectArgsDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RayListDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayListBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayListBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayList_Frame" + std::to_wstring(FrameIndex);
            SsrRayListBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &CounterDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayCounterBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayCounterBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayCounter_Frame" + std::to_wstring(FrameIndex);
            SsrRayCounterBuffers[FrameIndex]->SetName(Name.c_str());
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC RayListSrvDesc = {};
        RayListSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        RayListSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        RayListSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        RayListSrvDesc.Buffer.FirstElement = 0;
        RayListSrvDesc.Buffer.NumElements = SsrMaxRayCount;
        RayListSrvDesc.Buffer.StructureByteStride = SsrRayItemStride;
        RayListSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        SsrRayListSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayListBuffers[FrameIndex].Get(), RayListSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC RayListUavDesc = {};
        RayListUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        RayListUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        RayListUavDesc.Buffer.FirstElement = 0;
        RayListUavDesc.Buffer.NumElements = SsrMaxRayCount;
        RayListUavDesc.Buffer.StructureByteStride = SsrRayItemStride;
        RayListUavDesc.Buffer.CounterOffsetInBytes = 0;
        RayListUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        SsrRayListUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayListBuffers[FrameIndex].Get(), nullptr, RayListUavDesc);

        D3D12_SHADER_RESOURCE_VIEW_DESC CounterSrvDesc = {};
        CounterSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        CounterSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        CounterSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        CounterSrvDesc.Buffer.FirstElement = 0;
        CounterSrvDesc.Buffer.NumElements = 1;
        CounterSrvDesc.Buffer.StructureByteStride = 0;
        CounterSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        SsrRayCounterSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayCounterBuffers[FrameIndex].Get(), CounterSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC CounterUavDesc = {};
        CounterUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        CounterUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        CounterUavDesc.Buffer.FirstElement = 0;
        CounterUavDesc.Buffer.NumElements = 1;
        CounterUavDesc.Buffer.StructureByteStride = 0;
        CounterUavDesc.Buffer.CounterOffsetInBytes = 0;
        CounterUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        SsrRayCounterUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayCounterBuffers[FrameIndex].Get(), nullptr, CounterUavDesc);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RayListDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayListPrimaryBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayListPrimaryBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayListPrimary_Frame" + std::to_wstring(FrameIndex);
            SsrRayListPrimaryBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &CounterDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayCounterPrimaryBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayCounterPrimaryBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayCounterPrimary_Frame" + std::to_wstring(FrameIndex);
            SsrRayCounterPrimaryBuffers[FrameIndex]->SetName(Name.c_str());
        }

        SsrRayListPrimarySrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayListPrimaryBuffers[FrameIndex].Get(), RayListSrvDesc);
        SsrRayListPrimaryUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayListPrimaryBuffers[FrameIndex].Get(), nullptr, RayListUavDesc);
        SsrRayCounterPrimarySrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayCounterPrimaryBuffers[FrameIndex].Get(), CounterSrvDesc);
        SsrRayCounterPrimaryUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayCounterPrimaryBuffers[FrameIndex].Get(), nullptr, CounterUavDesc);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RayListDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayListHwMissBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayListHwMissBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayListHwMiss_Frame" + std::to_wstring(FrameIndex);
            SsrRayListHwMissBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &CounterDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayCounterHwMissBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayCounterHwMissBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayCounterHwMiss_Frame" + std::to_wstring(FrameIndex);
            SsrRayCounterHwMissBuffers[FrameIndex]->SetName(Name.c_str());
        }

        SsrRayListHwMissSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayListHwMissBuffers[FrameIndex].Get(), RayListSrvDesc);
        SsrRayListHwMissUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayListHwMissBuffers[FrameIndex].Get(), nullptr, RayListUavDesc);
        SsrRayCounterHwMissSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayCounterHwMissBuffers[FrameIndex].Get(), CounterSrvDesc);
        SsrRayCounterHwMissUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayCounterHwMissBuffers[FrameIndex].Get(), nullptr, CounterUavDesc);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &IndirectArgsDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(SsrIndirectArgsPrimaryBuffers[FrameIndex].GetAddressOf())));
        if (SsrIndirectArgsPrimaryBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_IndirectArgsPrimary_Frame" + std::to_wstring(FrameIndex);
            SsrIndirectArgsPrimaryBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &IndirectArgsDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(SsrIndirectArgsHwMissBuffers[FrameIndex].GetAddressOf())));
        if (SsrIndirectArgsHwMissBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_IndirectArgsHwMiss_Frame" + std::to_wstring(FrameIndex);
            SsrIndirectArgsHwMissBuffers[FrameIndex]->SetName(Name.c_str());
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC IndirectArgsUavDesc = {};
        IndirectArgsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        IndirectArgsUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        IndirectArgsUavDesc.Buffer.FirstElement = 0;
        IndirectArgsUavDesc.Buffer.NumElements = sizeof(D3D12_DISPATCH_ARGUMENTS) / sizeof(uint32_t);
        IndirectArgsUavDesc.Buffer.StructureByteStride = 0;
        IndirectArgsUavDesc.Buffer.CounterOffsetInBytes = 0;
        IndirectArgsUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        SsrIndirectArgsPrimaryUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(), nullptr, IndirectArgsUavDesc);
        SsrIndirectArgsHwMissUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrIndirectArgsHwMissBuffers[FrameIndex].Get(), nullptr, IndirectArgsUavDesc);
    }

    return true;
}

bool FDeferredRenderer::CreateHilbertLutResources(FDX12Device* Device)
{
    if (Device == nullptr)
    {
        return false;
    }

    constexpr uint32_t HilbertWidth = 64u;
    std::array<uint16_t, HilbertWidth * HilbertWidth> Data = {};
    for (uint32_t Y = 0; Y < HilbertWidth; ++Y)
    {
        for (uint32_t X = 0; X < HilbertWidth; ++X)
        {
            const uint32_t Index = HilbertIndex(X, Y);
            Data[X + HilbertWidth * Y] = static_cast<uint16_t>(Index);
        }
    }

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16_UINT,
        HilbertWidth,
        HilbertWidth,
        1,
        1);

    CD3DX12_HEAP_PROPERTIES DefaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(HilbertLutTexture.GetAddressOf())));

    if (HilbertLutTexture)
    {
        HilbertLutTexture->SetName(L"GTAO_HilbertLUT");
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&Desc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    CD3DX12_HEAP_PROPERTIES UploadHeap(D3D12_HEAP_TYPE_UPLOAD);

    CD3DX12_RESOURCE_DESC UploadDesc = CD3DX12_RESOURCE_DESC::Buffer(UploadBufferSize);

    ComPtr<ID3D12Resource> UploadResource;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(UploadResource.GetAddressOf())));

    uint8_t* UploadData = nullptr;
    const D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&UploadData)));
    const size_t RowBytes = HilbertWidth * sizeof(uint16_t);
    for (uint32_t Row = 0; Row < NumRows; ++Row)
    {
        std::memcpy(UploadData + Row * Layout.Footprint.RowPitch, &Data[Row * HilbertWidth], RowBytes);
    }
    UploadResource->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"DeferredRenderer_HilbertUpload_CL");

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = HilbertLutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadResource.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = HilbertLutTexture.Get();
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    return true;
}

bool FDeferredRenderer::CreateLuminanceResources(FDX12Device* Device)
{
    if (Device == nullptr)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = 1;
    Desc.Height = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = DXGI_FORMAT_R32_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(LuminanceTextures[0].GetAddressOf())));

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(LuminanceTextures[1].GetAddressOf())));

    if (LuminanceTextures[0])
    {
        LuminanceTextures[0]->SetName(L"LogAverageLuminanceA");
    }
    if (LuminanceTextures[1])
    {
        LuminanceTextures[1]->SetName(L"LogAverageLuminanceB");
    }
    LuminanceStates = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    return true;
}

bool FDeferredRenderer::CreateTaaResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t EffectiveFrameCount = (std::max)(1u, FrameCount);

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = Width;
    Desc.Height = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = LightingBufferFormat;
    Desc.SampleDesc.Count = 1;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    TaaHistoryTextures.clear();
    TaaHistoryTextures.resize(EffectiveFrameCount);
    for (uint32_t Index = 0; Index < EffectiveFrameCount; ++Index)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(TaaHistoryTextures[Index].GetAddressOf())));

        if (TaaHistoryTextures[Index])
        {
            const std::wstring ResourceName = L"TaaHistory_" + std::to_wstring(Index);
            TaaHistoryTextures[Index]->SetName(ResourceName.c_str());
        }
    }

    TaaFrameCount = EffectiveFrameCount;
    TaaStates.assign(EffectiveFrameCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TaaHistoryValid.assign(EffectiveFrameCount, false);
    return true;
}

bool FDeferredRenderer::CreatePathTracingAccumulationResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t EffectiveFrameCount = (std::max)(1u, FrameCount);

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = Width;
    Desc.Height = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = PathTracingBufferFormat;
    Desc.SampleDesc.Count = 1;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Create temporary texture for path tracing output
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(PathTracingTempTexture.GetAddressOf())));

    if (PathTracingTempTexture)
    {
        PathTracingTempTexture->SetName(L"PathTracingTemp");
    }

    // Create accumulation history textures
    PathTracingAccumulationTextures.clear();
    PathTracingAccumulationTextures.resize(EffectiveFrameCount);
    for (uint32_t Index = 0; Index < EffectiveFrameCount; ++Index)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(PathTracingAccumulationTextures[Index].GetAddressOf())));

        if (PathTracingAccumulationTextures[Index])
        {
            const std::wstring ResourceName = L"PathTracingAccumulation_" + std::to_wstring(Index);
            PathTracingAccumulationTextures[Index]->SetName(ResourceName.c_str());
        }
    }

    PathTracingAccumulationFrameCount = EffectiveFrameCount;
    PathTracingAccumulationStates.assign(EffectiveFrameCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PathTracingAccumulationHistoryValid.assign(EffectiveFrameCount, false);
    PathTracingTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

void FDeferredRenderer::OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue)
{
    if (bTaaEnabled && TaaFrameCount > 0)
    {
        const uint32_t TaaWriteIndex = FrameIndex % static_cast<uint32_t>(TaaHistoryValid.size());
        if (TaaWriteIndex < TaaHistoryValid.size())
        {
            TaaHistoryValid[TaaWriteIndex] = true;
        }
    }

    if (bPathTracingAccumulationEnabled && PathTracingAccumulationFrameCount > 0)
    {
        const uint32_t AccumWriteIndex = FrameIndex % static_cast<uint32_t>(PathTracingAccumulationHistoryValid.size());
        if (AccumWriteIndex < PathTracingAccumulationHistoryValid.size())
        {
            PathTracingAccumulationHistoryValid[AccumWriteIndex] = true;
        }
    }
}

bool FDeferredRenderer::CreateHZBResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t BaseWidth = (std::max)(1u, (Width + 1) / 2);
    const uint32_t BaseHeight = (std::max)(1u, (Height + 1) / 2);

    HZBWidth = BaseWidth;
    HZBHeight = BaseHeight;
    HZBMipCount = 1;

    uint32_t MipWidth = BaseWidth;
    uint32_t MipHeight = BaseHeight;
    while (MipWidth > 1 || MipHeight > 1)
    {
        MipWidth = (std::max)(1u, MipWidth / 2);
        MipHeight = (std::max)(1u, MipHeight / 2);
        ++HZBMipCount;
    }

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32G32_FLOAT,
        BaseWidth,
        BaseHeight,
        1,
        static_cast<UINT16>(HZBMipCount),
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        HZBState,
        nullptr,
        IID_PPV_ARGS(HierarchicalZBuffer.GetAddressOf())));

    HierarchicalZBuffer->SetName(L"HierarchicalZBuffer");

    {
        D3D12_RESOURCE_DESC NullDesc = {};
        NullDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        NullDesc.Alignment = 0;
        NullDesc.Width = 1;
        NullDesc.Height = 1;
        NullDesc.DepthOrArraySize = 1;
        NullDesc.MipLevels = 1;
        NullDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        NullDesc.SampleDesc.Count = 1;
        NullDesc.SampleDesc.Quality = 0;
        NullDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        NullDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &NullDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(HZBNullUavResource.GetAddressOf())));

        HZBNullUavResource->SetName(L"HZBNullUavResource");
    }

    return true;
}

bool FDeferredRenderer::CreateDescriptorHeap(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    const auto CreateSceneTextureSrv = [&](ID3D12Resource* Texture) -> uint32_t
    {
        if (!Texture)
        {
            return UINT32_MAX;
        }

        const D3D12_RESOURCE_DESC TextureDesc = Texture->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC SceneSrvDesc = {};
        SceneSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SceneSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SceneSrvDesc.Format = TextureDesc.Format;
        SceneSrvDesc.Texture2D.MipLevels = TextureDesc.MipLevels;
        SceneSrvDesc.Texture2D.MostDetailedMip = 0;
        SceneSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        return Device->CreateBindlessSrv(Texture, SceneSrvDesc);
    };

    for (size_t Index = 0; Index < SceneTextures.size(); ++Index)
    {
        SceneModels[Index].BaseColorBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].BaseColor.Get());
        SceneModels[Index].MetallicRoughnessBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].MetallicRoughness.Get());
        SceneModels[Index].NormalBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Normal.Get());
        SceneModels[Index].EmissiveBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Emissive.Get());
        SceneModels[Index].SheenColorBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].SheenColor.Get());
        SceneModels[Index].SheenRoughnessBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].SheenRoughness.Get());
        SceneModels[Index].ClearcoatBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Clearcoat.Get());
        SceneModels[Index].ClearcoatRoughnessBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].ClearcoatRoughness.Get());
        SceneModels[Index].ClearcoatNormalBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].ClearcoatNormal.Get());
        SceneModels[Index].AnisotropyBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Anisotropy.Get());
    }

    ID3D12Resource* Buffers[4] = { GBufferA.Get(), GBufferB.Get(), GBufferC.Get(), GBufferD.Get() };
    for (int i = 0; i < 4; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
        SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Format = GBufferFormats[i];
        SrvDesc.Texture2D.MipLevels = 1;
        GBufferBindlessIndices[i] = Device->CreateBindlessSrv(Buffers[i], SrvDesc);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC ShadowSrvDesc = {};
    ShadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ShadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ShadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ShadowSrvDesc.Texture2D.MipLevels = 1;
    ShadowMapBindlessIndex = Device->CreateBindlessSrv(ShadowMap.Get(), ShadowSrvDesc);

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC EnvSrvDesc = {};
        EnvSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        EnvSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        EnvSrvDesc.Format = EnvironmentCubeTexture->GetDesc().Format;
        EnvSrvDesc.TextureCube.MipLevels = EnvironmentCubeTexture->GetDesc().MipLevels;
        EnvSrvDesc.TextureCube.MostDetailedMip = 0;
        EnvSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        EnvironmentCubeBindlessIndex = Device->CreateBindlessSrv(EnvironmentCubeTexture.Get(), EnvSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC BrdfSrvDesc = {};
        BrdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        BrdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        BrdfSrvDesc.Format = BrdfLutTexture->GetDesc().Format;
        BrdfSrvDesc.Texture2D.MipLevels = BrdfLutTexture->GetDesc().MipLevels;
        BrdfSrvDesc.Texture2D.MostDetailedMip = 0;
        BrdfSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        BrdfLutBindlessIndex = Device->CreateBindlessSrv(BrdfLutTexture.Get(), BrdfSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LinearDepthSrvDesc = {};
        LinearDepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LinearDepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LinearDepthSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        LinearDepthSrvDesc.Texture2D.MipLevels = 1;
        LinearDepthBindlessIndex = Device->CreateBindlessSrv(LinearDepthTexture.Get(), LinearDepthSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC VelocitySrvDesc = {};
        VelocitySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        VelocitySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        VelocitySrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        VelocitySrvDesc.Texture2D.MipLevels = 1;
        VelocityBindlessIndex = Device->CreateBindlessSrv(VelocityTexture.Get(), VelocitySrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC HilbertSrvDesc = {};
        HilbertSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        HilbertSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        HilbertSrvDesc.Format = DXGI_FORMAT_R16_UINT;
        HilbertSrvDesc.Texture2D.MipLevels = 1;
        HilbertLutBindlessIndex = Device->CreateBindlessSrv(HilbertLutTexture.Get(), HilbertSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC GtaoSrvDesc = {};
        GtaoSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GtaoSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GtaoSrvDesc.Format = DXGI_FORMAT_R8_UNORM;
        GtaoSrvDesc.Texture2D.MipLevels = 1;
        GtaoBindlessIndex = Device->CreateBindlessSrv(GtaoTexture.Get(), GtaoSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC RestirSrvDesc = {};
        RestirSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        RestirSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        RestirSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirSrvDesc.Texture2D.MipLevels = 1;
        RestirGIBindlessIndex = Device->CreateBindlessSrv(RestirGITexture.Get(), RestirSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC RestirUavDesc = {};
        RestirUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        RestirUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirUavDesc.Texture2D.MipSlice = 0;
        RestirGIUavBindlessIndex = Device->CreateBindlessUav(RestirGITexture.Get(), nullptr, RestirUavDesc);

        RestirGIHistorySrvBindlessIndex = Device->CreateBindlessSrv(RestirGIHistoryTexture.Get(), RestirSrvDesc);
        RestirGIHistoryUavBindlessIndex = Device->CreateBindlessUav(RestirGIHistoryTexture.Get(), nullptr, RestirUavDesc);

        D3D12_SHADER_RESOURCE_VIEW_DESC ReservoirSrvDesc = {};
        ReservoirSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ReservoirSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ReservoirSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        ReservoirSrvDesc.Buffer.FirstElement = 0;
        ReservoirSrvDesc.Buffer.NumElements = static_cast<uint32_t>(Viewport.Width * Viewport.Height);
        ReservoirSrvDesc.Buffer.StructureByteStride = sizeof(float) * 8u;
        ReservoirSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_UNORDERED_ACCESS_VIEW_DESC ReservoirUavDesc = {};
        ReservoirUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ReservoirUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        ReservoirUavDesc.Buffer.FirstElement = 0;
        ReservoirUavDesc.Buffer.NumElements = static_cast<uint32_t>(Viewport.Width * Viewport.Height);
        ReservoirUavDesc.Buffer.StructureByteStride = sizeof(float) * 8u;
        ReservoirUavDesc.Buffer.CounterOffsetInBytes = 0;
        ReservoirUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        RestirGITemporalReservoirSrvBindlessIndex = Device->CreateBindlessSrv(RestirGITemporalReservoirBuffer.Get(), ReservoirSrvDesc);
        RestirGITemporalReservoirUavBindlessIndex = Device->CreateBindlessUav(RestirGITemporalReservoirBuffer.Get(), nullptr, ReservoirUavDesc);
        RestirGISpatialReservoirSrvBindlessIndex = Device->CreateBindlessSrv(RestirGISpatialReservoirBuffer.Get(), ReservoirSrvDesc);
        RestirGISpatialReservoirUavBindlessIndex = Device->CreateBindlessUav(RestirGISpatialReservoirBuffer.Get(), nullptr, ReservoirUavDesc);
        RestirGIReservoirHistorySrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirHistoryBuffer.Get(), ReservoirSrvDesc);
        RestirGIReservoirHistoryUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirHistoryBuffer.Get(), nullptr, ReservoirUavDesc);

        D3D12_SHADER_RESOURCE_VIEW_DESC RestirGISrvDesc = {};
        RestirGISrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        RestirGISrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        RestirGISrvDesc.Texture2D.MipLevels = 1;

        D3D12_UNORDERED_ACCESS_VIEW_DESC RestirGIUavDesc = {};
        RestirGIUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        RestirGIUavDesc.Texture2D.MipSlice = 0;

        RestirGISrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGIInitialRadianceSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIInitialRadianceTexture.Get(), RestirGISrvDesc);
        RestirGIInitialRadianceUavBindlessIndex = Device->CreateBindlessUav(RestirGIInitialRadianceTexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirSampleRadianceASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirSampleRadianceATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirSampleRadianceAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirSampleRadianceATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirSampleRadianceBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirSampleRadianceBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirSampleRadianceBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirSampleRadianceBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32_UINT;
        RestirGIInitialRayDirectionSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIInitialRayDirectionTexture.Get(), RestirGISrvDesc);
        RestirGIInitialRayDirectionUavBindlessIndex = Device->CreateBindlessUav(RestirGIInitialRayDirectionTexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirRayDirectionASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirRayDirectionATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirRayDirectionAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirRayDirectionATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirRayDirectionBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirRayDirectionBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirRayDirectionBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirRayDirectionBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32_UINT;
        RestirGIReservoirDepthNormalASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirDepthNormalATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirDepthNormalAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirDepthNormalATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirDepthNormalBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirDepthNormalBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirDepthNormalBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirDepthNormalBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        RestirGIReservoirMWASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirMWATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirMWAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirMWATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirMWBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirMWBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirMWBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirMWBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGiInputSHSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiInputSHTexture.Get(), RestirGISrvDesc);
        RestirGiInputSHUavBindlessIndex = Device->CreateBindlessUav(RestirGiInputSHTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R8_UNORM;
        RestirGIUavDesc.Format = DXGI_FORMAT_R8_UNORM;
        RestirGiVarianceSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiVarianceTexture.Get(), RestirGISrvDesc);
        RestirGiVarianceUavBindlessIndex = Device->CreateBindlessUav(RestirGiVarianceTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        RestirGiHistoryIrradianceSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistoryIrradianceTexture.Get(), RestirGISrvDesc);
        RestirGiHistoryIrradianceUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistoryIrradianceTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGiTemporalSHSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiTemporalSHTexture.Get(), RestirGISrvDesc);
        RestirGiTemporalSHUavBindlessIndex = Device->CreateBindlessUav(RestirGiTemporalSHTexture.Get(), nullptr, RestirGIUavDesc);
        RestirGiHistorySHSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistorySHTexture.Get(), RestirGISrvDesc);
        RestirGiHistorySHUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistorySHTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R8_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R8_UINT;
        RestirGiHistoryCountASrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistoryCountATexture.Get(), RestirGISrvDesc);
        RestirGiHistoryCountAUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistoryCountATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGiHistoryCountBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistoryCountBTexture.Get(), RestirGISrvDesc);
        RestirGiHistoryCountBUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistoryCountBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        RestirGiPrevLinearDepthSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiPrevLinearDepthTexture.Get(), RestirGISrvDesc);
        RestirGiPrevLinearDepthUavBindlessIndex = Device->CreateBindlessUav(RestirGiPrevLinearDepthTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGiPrevNormalSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiPrevNormalTexture.Get(), RestirGISrvDesc);
        RestirGiPrevNormalUavBindlessIndex = Device->CreateBindlessUav(RestirGiPrevNormalTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
        {
            RestirGiShMipSrvBindlessIndices[MipIndex] = Device->CreateBindlessSrv(RestirGiShMipTextures[MipIndex].Get(), RestirGISrvDesc);
            RestirGiShMipUavBindlessIndices[MipIndex] = Device->CreateBindlessUav(RestirGiShMipTextures[MipIndex].Get(), nullptr, RestirGIUavDesc);
        }

        RestirGISrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
        {
            RestirGiLinearDepthMipSrvBindlessIndices[MipIndex] = Device->CreateBindlessSrv(RestirGiLinearDepthMipTextures[MipIndex].Get(), RestirGISrvDesc);
            RestirGiLinearDepthMipUavBindlessIndices[MipIndex] = Device->CreateBindlessUav(RestirGiLinearDepthMipTextures[MipIndex].Get(), nullptr, RestirGIUavDesc);
        }
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC SsrSrvDesc = {};
        SsrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SsrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SsrSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrSrvDesc.Texture2D.MipLevels = 1;
        SsrBindlessIndex = Device->CreateBindlessSrv(SsrTexture.Get(), SsrSrvDesc);
        SsrDenoiseBindlessIndex = Device->CreateBindlessSrv(SsrDenoiseTexture.Get(), SsrSrvDesc);
        SsrFallbackBindlessIndex = Device->CreateBindlessSrv(SsrFallbackTexture.Get(), SsrSrvDesc);
        SsrResolveBindlessIndex = Device->CreateBindlessSrv(SsrResolveTexture.Get(), SsrSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC SsrUavDesc = {};
        SsrUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        SsrUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrUavDesc.Texture2D.MipSlice = 0;
        SsrUavBindlessIndex = Device->CreateBindlessUav(SsrTexture.Get(), nullptr, SsrUavDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC SsrResolveUavDesc = {};
        SsrResolveUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        SsrResolveUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrResolveUavDesc.Texture2D.MipSlice = 0;
        SsrResolveUavBindlessIndex = Device->CreateBindlessUav(SsrResolveTexture.Get(), nullptr, SsrResolveUavDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC SsrFallbackUavDesc = {};
        SsrFallbackUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        SsrFallbackUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrFallbackUavDesc.Texture2D.MipSlice = 0;
        SsrFallbackUavBindlessIndex = Device->CreateBindlessUav(SsrFallbackTexture.Get(), nullptr, SsrFallbackUavDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LightingSrvDesc = {};
        LightingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LightingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LightingSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        LightingSrvDesc.Texture2D.MipLevels = 1;
        LightingBufferBindlessIndex = Device->CreateBindlessSrv(LightingBuffer.Get(), LightingSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC TonemapSrvDesc = {};
        TonemapSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        TonemapSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        TonemapSrvDesc.Format = BackBufferFormat;
        TonemapSrvDesc.Texture2D.MipLevels = 1;
        TonemapOutputBindlessIndex = Device->CreateBindlessSrv(TonemapOutput.Get(), TonemapSrvDesc);
    }

    for (uint32_t Index = 0; Index < LuminanceTextures.size(); ++Index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LuminanceSrvDesc = {};
        LuminanceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LuminanceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LuminanceSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        LuminanceSrvDesc.Texture2D.MipLevels = 1;
        LuminanceSrvBindlessIndices[Index] = Device->CreateBindlessSrv(LuminanceTextures[Index].Get(), LuminanceSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC LuminanceUavDesc = {};
        LuminanceUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        LuminanceUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        LuminanceUavDesc.Texture2D.MipSlice = 0;
        LuminanceUavDesc.Texture2D.PlaneSlice = 0;
        LuminanceUavBindlessIndices[Index] = Device->CreateBindlessUav(LuminanceTextures[Index].Get(), nullptr, LuminanceUavDesc);
    }

    TaaSrvBindlessIndices.clear();
    TaaUavBindlessIndices.clear();
    TaaSrvBindlessIndices.resize(TaaHistoryTextures.size(), UINT32_MAX);
    TaaUavBindlessIndices.resize(TaaHistoryTextures.size(), UINT32_MAX);

    for (uint32_t Index = 0; Index < TaaHistoryTextures.size(); ++Index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC TaaSrvDesc = {};
        TaaSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        TaaSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        TaaSrvDesc.Format = LightingBufferFormat;
        TaaSrvDesc.Texture2D.MipLevels = 1;
        TaaSrvBindlessIndices[Index] = Device->CreateBindlessSrv(TaaHistoryTextures[Index].Get(), TaaSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC TaaUavDesc = {};
        TaaUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        TaaUavDesc.Format = LightingBufferFormat;
        TaaUavDesc.Texture2D.MipSlice = 0;
        TaaUavDesc.Texture2D.PlaneSlice = 0;
        TaaUavBindlessIndices[Index] = Device->CreateBindlessUav(TaaHistoryTextures[Index].Get(), nullptr, TaaUavDesc);
    }

    // Create bindless descriptors for PathTracing temp texture
    if (PathTracingTempTexture)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC PathTracingTempUavDesc = {};
        PathTracingTempUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        PathTracingTempUavDesc.Format = PathTracingBufferFormat;
        PathTracingTempUavDesc.Texture2D.MipSlice = 0;
        PathTracingTempBindlessIndex = Device->CreateBindlessUav(PathTracingTempTexture.Get(), nullptr, PathTracingTempUavDesc);
    }

    // Create bindless descriptors for PathTracing accumulation textures
    PathTracingAccumulationSrvBindlessIndices.clear();
    PathTracingAccumulationUavBindlessIndices.clear();
    PathTracingAccumulationSrvBindlessIndices.resize(PathTracingAccumulationTextures.size(), UINT32_MAX);
    PathTracingAccumulationUavBindlessIndices.resize(PathTracingAccumulationTextures.size(), UINT32_MAX);

    for (uint32_t Index = 0; Index < PathTracingAccumulationTextures.size(); ++Index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC AccumSrvDesc = {};
        AccumSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        AccumSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        AccumSrvDesc.Format = PathTracingBufferFormat;
        AccumSrvDesc.Texture2D.MipLevels = 1;
        PathTracingAccumulationSrvBindlessIndices[Index] = Device->CreateBindlessSrv(PathTracingAccumulationTextures[Index].Get(), AccumSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC AccumUavDesc = {};
        AccumUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        AccumUavDesc.Format = PathTracingBufferFormat;
        AccumUavDesc.Texture2D.MipSlice = 0;
        AccumUavDesc.Texture2D.PlaneSlice = 0;
        PathTracingAccumulationUavBindlessIndices[Index] = Device->CreateBindlessUav(PathTracingAccumulationTextures[Index].Get(), nullptr, AccumUavDesc);
    }

    DepthBindlessIndices.clear();
    DepthBindlessIndices.resize(GetFramesInFlight(), UINT32_MAX);
    for (uint32_t Index = 0; Index < DepthBindlessIndices.size(); ++Index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC DepthSrvDesc = {};
        DepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        DepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DepthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        DepthSrvDesc.Texture2D.MipLevels = 1;
        DepthSrvDesc.Texture2D.MostDetailedMip = 0;
        DepthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        ID3D12Resource* DepthBuffer = DepthResourcesPerFrame.empty() ? nullptr : DepthResourcesPerFrame[Index].DepthBuffer.Get();
        DepthBindlessIndices[Index] = Device->CreateBindlessSrv(DepthBuffer, DepthSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC HZBSrvDesc = {};
        HZBSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        HZBSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        HZBSrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        HZBSrvDesc.Texture2D.MipLevels = HZBMipCount;
        HZBSrvDesc.Texture2D.MostDetailedMip = 0;
        HZBSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        HZBSrvBindlessIndex = Device->CreateBindlessSrv(HierarchicalZBuffer.Get(), HZBSrvDesc);
    }

    HZBSrvMipBindlessIndices.clear();
    HZBSrvMipBindlessIndices.reserve(HZBMipCount);
    for (uint32_t Mip = 0; Mip < HZBMipCount; ++Mip)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC HZBMipSrvDesc = {};
        HZBMipSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        HZBMipSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        HZBMipSrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        HZBMipSrvDesc.Texture2D.MipLevels = 1;
        HZBMipSrvDesc.Texture2D.MostDetailedMip = Mip;
        HZBMipSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        HZBSrvMipBindlessIndices.push_back(Device->CreateBindlessSrv(HierarchicalZBuffer.Get(), HZBMipSrvDesc));
    }

    HZBUavBindlessIndices.clear();
    HZBUavBindlessIndices.reserve(HZBMipCount);
    for (uint32_t Mip = 0; Mip < HZBMipCount; ++Mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
        UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        UavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        UavDesc.Texture2D.MipSlice = Mip;
        UavDesc.Texture2D.PlaneSlice = 0;

        HZBUavBindlessIndices.push_back(Device->CreateBindlessUav(HierarchicalZBuffer.Get(), nullptr, UavDesc));
    }

    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC NullUavDesc = {};
        NullUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        NullUavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        NullUavDesc.Texture2D.MipSlice = 0;
        NullUavDesc.Texture2D.PlaneSlice = 0;

        HZBNullUavBindlessIndex = Device->CreateBindlessUav(HZBNullUavResource.Get(), nullptr, NullUavDesc);
    }

    return true;
}

bool FDeferredRenderer::CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    const bool bCreated = RendererUtils::CreateObjectIdResources(
        Device,
        Width,
        Height,
        ObjectIdTexture,
        ObjectIdRtvHeap,
        ObjectIdRtvHandle,
        ObjectIdReadback,
        ObjectIdFootprint,
        ObjectIdRowPitch);
    if (bCreated)
    {
        ObjectIdState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return bCreated;
}

bool FDeferredRenderer::CreateObjectIdPipeline(FDX12Device* Device)
{
    return RendererUtils::CreateObjectIdPipeline(Device, BasePassRootSignature.Get(), ObjectIdPipeline);
}

namespace
{
    uint32_t ClampToByte(float Value)
    {
        const float Clamped = (std::max)(0.0f, (std::min)(1.0f, Value));
        return static_cast<uint32_t>(std::round(Clamped * 255.0f));
    }

    uint32_t PackColor(const DirectX::XMFLOAT3& Color)
    {
        const uint32_t R = ClampToByte(Color.x);
        const uint32_t G = ClampToByte(Color.y);
        const uint32_t B = ClampToByte(Color.z);
        return 0xff000000 | (B << 16) | (G << 8) | R;
    }

}

bool FDeferredRenderer::CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models)
{
    if (!TextureLoader)
    {
        return false;
    }

    SceneTextures.clear();
    SceneTextures.reserve(Models.size());

    // Prepare all texture load requests
    std::vector<FTextureLoadRequest> Requests;
    Requests.reserve(Models.size() * 10); // 10 textures per model

    // Pre-allocate texture sets
    for (const FSceneModelResource& Model : Models)
    {
        FModelTextureSet TextureSet;
        SceneTextures.push_back(TextureSet);
    }

    // Build load requests for all textures
    for (size_t i = 0; i < Models.size(); ++i)
    {
        const FSceneModelResource& Model = Models[i];
        FModelTextureSet& TextureSet = SceneTextures[i];

        // Base color texture - skip when missing
        if (!Model.BaseColorTexturePath.empty())
        {
            FTextureLoadRequest BaseColorRequest;
            BaseColorRequest.Path = Model.BaseColorTexturePath;
            BaseColorRequest.bUseSolidColor = false;
            BaseColorRequest.bUseSRGB = true;
            BaseColorRequest.OutTexture = &TextureSet.BaseColor;
            Requests.push_back(BaseColorRequest);
        }

        // Metallic roughness texture - skip when missing
        if (!Model.MetallicRoughnessTexturePath.empty())
        {
            FTextureLoadRequest MetallicRoughnessRequest;
            MetallicRoughnessRequest.Path = Model.MetallicRoughnessTexturePath;
            MetallicRoughnessRequest.bUseSolidColor = false;
            MetallicRoughnessRequest.OutTexture = &TextureSet.MetallicRoughness;
            Requests.push_back(MetallicRoughnessRequest);
        }

		if (!Model.NormalTexturePath.empty())
		{
			FTextureLoadRequest NormalRequest;
            NormalRequest.Path = Model.NormalTexturePath;
            NormalRequest.bUseSolidColor = false;
            NormalRequest.OutTexture = &TextureSet.Normal;
			Requests.push_back(NormalRequest);
		}

        // Emissive texture - skip when missing
        if (!Model.EmissiveTexturePath.empty())
        {
            FTextureLoadRequest EmissiveRequest;
            EmissiveRequest.Path = Model.EmissiveTexturePath;
            EmissiveRequest.bUseSolidColor = false;
            EmissiveRequest.bUseSRGB = true;
            EmissiveRequest.OutTexture = &TextureSet.Emissive;
            Requests.push_back(EmissiveRequest);
        }

        if (!Model.SheenColorTexturePath.empty())
        {
            FTextureLoadRequest SheenColorRequest;
            SheenColorRequest.Path = Model.SheenColorTexturePath;
            SheenColorRequest.bUseSolidColor = false;
            SheenColorRequest.bUseSRGB = true;
            SheenColorRequest.OutTexture = &TextureSet.SheenColor;
            Requests.push_back(SheenColorRequest);
        }

        if (!Model.SheenRoughnessTexturePath.empty())
        {
            FTextureLoadRequest SheenRoughnessRequest;
            SheenRoughnessRequest.Path = Model.SheenRoughnessTexturePath;
            SheenRoughnessRequest.bUseSolidColor = false;
            SheenRoughnessRequest.OutTexture = &TextureSet.SheenRoughness;
            Requests.push_back(SheenRoughnessRequest);
        }

        if (!Model.ClearcoatTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatRequest;
            ClearcoatRequest.Path = Model.ClearcoatTexturePath;
            ClearcoatRequest.bUseSolidColor = false;
            ClearcoatRequest.OutTexture = &TextureSet.Clearcoat;
            Requests.push_back(ClearcoatRequest);
        }

        if (!Model.ClearcoatRoughnessTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatRoughnessRequest;
            ClearcoatRoughnessRequest.Path = Model.ClearcoatRoughnessTexturePath;
            ClearcoatRoughnessRequest.bUseSolidColor = false;
            ClearcoatRoughnessRequest.OutTexture = &TextureSet.ClearcoatRoughness;
            Requests.push_back(ClearcoatRoughnessRequest);
        }

        if (!Model.ClearcoatNormalTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatNormalRequest;
            ClearcoatNormalRequest.Path = Model.ClearcoatNormalTexturePath;
            ClearcoatNormalRequest.bUseSolidColor = false;
            ClearcoatNormalRequest.OutTexture = &TextureSet.ClearcoatNormal;
            Requests.push_back(ClearcoatNormalRequest);
        }

        if (!Model.AnisotropyTexturePath.empty())
        {
            FTextureLoadRequest AnisotropyRequest;
            AnisotropyRequest.Path = Model.AnisotropyTexturePath;
            AnisotropyRequest.bUseSolidColor = false;
            AnisotropyRequest.OutTexture = &TextureSet.Anisotropy;
            Requests.push_back(AnisotropyRequest);
        }
    }

    // Load all textures in parallel
    LogInfo("Loading " + std::to_string(Requests.size()) + " textures in parallel for " + std::to_string(Models.size()) + " models");
    const bool bSuccess = TextureLoader->LoadTexturesParallel(Requests);

    if (!bSuccess)
    {
        LogError("Failed to load scene textures");
    }

    return bSuccess;
}


bool FDeferredRenderer::CreateGpuDrivenResources(FDX12Device* Device)
{
    if (!Device || SceneModels.empty() || !GetSceneConstantBuffer())
    {
        return false;
    }

    // Step 1: Prepare indirect draw data
    FGpuDrivenPreparedData PreparedData;
    if (!PrepareGpuDrivenDrawData(PreparedData))
    {
        LogError("Failed to prepare GPU-driven draw data");
        return false;
    }

    // Step 2: Create per-frame indirect buffers
    if (!CreatePerFrameIndirectBuffers(Device, PreparedData))
    {
        LogError("Failed to create per-frame indirect buffers");
        return false;
    }

    // Step 3: Create shared GPU-driven buffers
    if (!CreateSharedGpuDrivenBuffers(Device, PreparedData))
    {
        LogError("Failed to create shared GPU-driven buffers");
        return false;
    }

    // Step 4: Upload buffers to GPU
    if (!UploadGpuDrivenBuffers(Device, PreparedData))
    {
        LogError("Failed to upload GPU-driven buffers");
        return false;
    }

    // Step 5: Create culling pipelines
    if (!CreateCullingPipelines(Device))
    {
        LogError("Failed to create culling pipelines");
        return false;
    }

    if (!CreateVisibilityListPipelines(Device))
    {
        LogError("Failed to create visibility list pipelines");
        return false;
    }

    // Step 6: Create indirect command signature
    if (!CreateIndirectCommandSignature(Device, BasePassRootSignature.Get()))
    {
        LogError("Failed to create indirect command signature");
        return false;
    }

    return true;
}


void FDeferredRenderer::UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, size_t ModelIndex, uint64_t ConstantBufferOffset)
{
    (void)ModelIndex;

    const DirectX::XMVECTOR LightDir = DirectX::XMLoadFloat3(&LightDirection);
    const DirectX::XMMATRIX LightVP = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
    DirectX::XMStoreFloat4x4(&LightViewProjection, LightVP);
    const DirectX::XMMATRIX Projection = bUseTaaJitter ? TaaProjection : Camera.GetProjectionMatrix();
    const DirectX::XMFLOAT2 Jitter = bUseTaaJitter ? TaaJitter : DirectX::XMFLOAT2(0.0f, 0.0f);
    const uint32_t GtaoTemporalIndex = (bGtaoEnabled && bGtaoJitterEnabled) ? TaaSampleIndex : 0u;

    const DirectX::XMMATRIX PreviousWorld = Model.bHasPreviousWorldMatrix
        ? DirectX::XMLoadFloat4x4(&Model.PreviousWorldMatrix)
        : DirectX::XMMatrixIdentity();
    const bool bHasPreviousWorld = Model.bHasPreviousWorldMatrix;

    uint32_t PreviousSkinnedPositionBindlessIndex = UINT32_MAX;
    bool bHasPreviousSkinning = false;
    const uint32_t FrameCount = GetFramesInFlight();
    const uint32_t PrevFrameIndex = FrameCount > 0 ? (GetFrameIndex() + FrameCount - 1u) % FrameCount : 0u;
    if (Model.BoneMatrixBindlessIndex != UINT32_MAX
        && Model.BoneMatrixCount > 0
        && PrevFrameIndex < Model.SkinnedPositionSrvBindlessIndices.size())
    {
        PreviousSkinnedPositionBindlessIndex = Model.SkinnedPositionSrvBindlessIndices[PrevFrameIndex];
        bHasPreviousSkinning = PreviousSkinnedPositionBindlessIndex != UINT32_MAX;
    }

    RendererUtils::UpdateSceneConstants(
        Camera,
        Model,
        LightIntensity,
        LightDir,
        LightColor,
        LightVP,
        Projection,
        bShadowsEnabled ? ShadowStrength : 0.0f,
        ShadowBias,
        static_cast<float>(ShadowMapWidth),
        static_cast<float>(ShadowMapHeight),
        EnvironmentMipCount,
        Jitter,
        GtaoTemporalIndex,
        bGtaoEnabled,
        GtaoRadius,
        GtaoIntensity,
        GtaoPower,
        GtaoThickness,
        GtaoDirectionCount,
        GtaoStepCount,
        GetSceneConstantBufferMapped(),
        ConstantBufferOffset,
        DirectX::XMLoadFloat4x4(&PreviousViewProjectionMatrix),
        bHasPreviousViewProjection,
        PreviousWorld,
        bHasPreviousWorld,
        PreviousSkinnedPositionBindlessIndex,
        bHasPreviousSkinning);
}

void FDeferredRenderer::UpdateSkyConstants(const FCamera& Camera)
{
    using namespace DirectX;

    const FFloat3 CameraPosition = Camera.GetPosition();
    const XMMATRIX Scale = XMMatrixScaling(SkySphereRadius, SkySphereRadius, SkySphereRadius);
    const XMMATRIX Translation = XMMatrixTranslation(CameraPosition.x, CameraPosition.y, CameraPosition.z);
    const XMMATRIX World = Scale * Translation;

    const XMVECTOR LightDir = XMLoadFloat3(&LightDirection);
    const DirectX::XMMATRIX Projection = bUseTaaJitter ? TaaProjection : Camera.GetProjectionMatrix();
    RendererUtils::UpdateSkyConstants(Camera, World, Projection, LightDir, LightColor, SkyConstantBufferMapped);
}

void FDeferredRenderer::UpdateCullingVisibility(const FCamera& Camera)
{
    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    const bool bGpuCullingActive = bEnableIndirectDraw && CullingPipeline && CullingRootSignature && GetIndirectCommandBuffer()
        && ModelBoundsBuffer && MeshletConeAxisBuffer && MeshletConeApexBuffer;
    RendererUtils::UpdateCullingVisibility(*CullingCamera, SceneModels, SceneModelVisibility, !bGpuCullingActive);
}
