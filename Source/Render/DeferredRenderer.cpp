#include "DeferredRenderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "TextureLoader.h"
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

const DXGI_FORMAT FDeferredRenderer::GBufferFormats[4] =
{
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
};

namespace
{
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

    ApplyRendererConfig(Config);

    InitializeCommonSettings(Width, Height, Config);

    if (!InitializePipelineDomains(Device, BackBufferFormat))
    {
        return false;
    }

    if (!InitializeFrameResources(Device, Width, Height, Config))
    {
        return false;
    }

    if (!InitializeSceneResources(Device, BackBufferFormat, Config))
    {
        return false;
    }

    LogInfo("Deferred renderer initialization completed");
    return true;
}

void FDeferredRenderer::ApplyRendererConfig(const FRendererConfig& Config)
{
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
    TaaHistoryValid.assign(TaaFrameCount, false);
    TaaSampleIndex = 0;
    bPathTracingAccumulationEnabled = Config.bEnablePathTracingAccumulation;
    PathTracingAccumulationFrameCount = Config.FramesInFlight;
    PathTracingAccumulationHistoryValid.assign(PathTracingAccumulationFrameCount, false);
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
    bRestirGIDenoiserEnabled = Config.bEnableRestirGIDenoiser;
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
    RestirGIRandomMode = Config.RestirGIRandomMode;
    SsrMode = Config.SsrMode;
    SsrSamplesPerQuad = Config.SsrSamplesPerQuad;
}

bool FDeferredRenderer::InitializePipelineDomains(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
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

    LogInfo("Creating deferred renderer geometry domain pipelines...");
    if (!GeometryPasses->InitializePipelines(*this, Device, LightingBufferFormat))
    {
        LogError("Deferred renderer initialization failed: geometry domain pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting domain pipelines...");
    if (!LightingPasses->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: lighting domain pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer ray tracing domain pipelines...");
    if (!RayTracingPasses->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ray tracing domain pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer post-process pipelines...");
    if (!PostProcessPasses->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: post-process pipeline creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeFrameResources(FDX12Device* Device, uint32_t Width, uint32_t Height, const FRendererConfig& Config)
{
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

    if (!GeometryPasses->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: geometry domain resource creation failed");
        return false;
    }

    if (!PostProcessPasses->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: post-process resource creation failed");
        return false;
    }

    if (!RayTracingPasses->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: ray tracing domain resource creation failed");
        return false;
    }

    if (!LightingPasses->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: lighting domain resource creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeSceneResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config)
{
    if (!InitializeSceneModelResources(Device, Config))
    {
        return false;
    }

    if (!InitializeEnvironmentAndDescriptorResources(Device, Config))
    {
        return false;
    }

    if (!VisibilityPasses->InitializeResources(*this, Device))
    {
        LogWarning("Deferred renderer GPU-driven resources creation failed; fallback to CPU-driven draws.");
    }

    if (!InitializeSkyResources(Device))
    {
        return false;
    }

    if (!InitializeGpuDebugResources(Device, BackBufferFormat))
    {
        return false;
    }

    return true;
}

bool FDeferredRenderer::InitializeSceneModelResources(FDX12Device* Device, const FRendererConfig& Config)
{
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

    return true;
}

bool FDeferredRenderer::InitializeEnvironmentAndDescriptorResources(FDX12Device* Device, const FRendererConfig& Config)
{
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

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/BlueNoise/sobol_256_4d.png", BlueNoiseSobolTexture))
    {
        LogError("Deferred renderer initialization failed: blue noise sobol texture loading failed");
        return false;
    }
    if (BlueNoiseSobolTexture)
    {
        BlueNoiseSobolTexture->SetName(L"BlueNoiseSobol");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/BlueNoise/scrambling_ranking_128x128_2d_1spp.png", BlueNoiseScramblingRanking1SPPTexture))
    {
        LogError("Deferred renderer initialization failed: blue noise scrambling/ranking texture loading failed");
        return false;
    }
    if (BlueNoiseScramblingRanking1SPPTexture)
    {
        BlueNoiseScramblingRanking1SPPTexture->SetName(L"BlueNoiseScramblingRanking1SPP");
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

    return true;
}

bool FDeferredRenderer::InitializeSkyResources(FDX12Device* Device)
{
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

    return true;
}

bool FDeferredRenderer::InitializeGpuDebugResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    if (!bEnableGpuDebugPrint)
    {
        return true;
    }

    if (!CreateGpuDebugPrintResources(Device) || !CreateGpuDebugPrintPipeline(Device, BackBufferFormat) || !CreateGpuDebugLinePipeline(Device, BackBufferFormat) || !CreateGpuDebugPrintStatsPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: GPU debug print setup failed");
        return false;
    }

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

    if (!bHasPreviousViewProjection)
    {
        InvalidateRestirGIReservoirHistory();
        InvalidateRestirGiDenoiserHistory();
    }

    if (!bRestirGIEnabled)
    {
        InvalidateRestirGIReservoirHistory();
        InvalidateRestirGiDenoiserHistory();
    }

    if (!bRestirGIDenoiserEnabled)
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

    if (RestirGiHistoryCountATexture && RestirGiHistoryCountBTexture)
    {
        std::swap(RestirGiHistoryCountATexture, RestirGiHistoryCountBTexture);
        std::swap(RestirGiHistoryCountAState, RestirGiHistoryCountBState);
        std::swap(RestirGiHistoryCountASrvBindlessIndex, RestirGiHistoryCountBSrvBindlessIndex);
        std::swap(RestirGiHistoryCountAUavBindlessIndex, RestirGiHistoryCountBUavBindlessIndex);
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
        RestirGIReservoirHistoryFrameCount = (std::min)(RestirGIReservoirHistoryFrameCount + 1u, MaxHistoryFrames);
        bRestirGIReservoirHistoryValid = RestirGIReservoirHistoryFrameCount > 0u;
    }
    else
    {
        RestirGIReservoirHistoryFrameCount = 0u;
        bRestirGIReservoirHistoryValid = false;
    }

    if (bRestirGIEnabled && bRestirGIDenoiserEnabled && RestirGiHistoryIrradianceTexture != nullptr)
    {
        bRestirGIDenoiserHistoryValid = true;
    }
    else
    {
        bRestirGIDenoiserHistoryValid = false;
    }

    for (FSceneModelResource& Model : SceneModels)
    {
        Model.PreviousWorldMatrix = Model.WorldMatrix;
        Model.bHasPreviousWorldMatrix = true;
    }
}

void FDeferredRenderer::InvalidateRestirGiDenoiserHistory()
{
    bRestirGIDenoiserHistoryValid = false;

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

void FDeferredRenderer::InvalidateRestirGIReservoirHistory()
{
    bRestirGIReservoirHistoryValid = false;
    RestirGIReservoirHistoryFrameCount = 0;
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
