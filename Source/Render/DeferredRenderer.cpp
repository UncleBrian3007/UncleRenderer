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

namespace
{
    constexpr DXGI_FORMAT GBufferFormats[4] =
    {
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };

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
