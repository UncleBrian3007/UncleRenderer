#include "DeferredRenderer.h"

#include "EnvironmentMap.h"
#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "SceneWorldBuilder.h"
#include "TextureLoader.h"
#include "RenderGraph.h"
#include "Deferred/DeferredPassContext.h"
#include "Deferred/DeferredFrameOrchestrator.h"
#include "Deferred/DeferredBasePass.h"
#include "Deferred/ClusterDagVisibilityPass.h"
#include "Deferred/ClusterDagStreamingManager.h"
#include "Deferred/DeferredLightingPass.h"
#include "Deferred/Gtao.h"
#include "Deferred/RayTracingShadow.h"
#include "Deferred/Ssr.h"
#include "Deferred/RestirGI.h"
#include "Deferred/SparseSdfGI.h"
#include "Deferred/AutoExposure.h"
#include "Deferred/Cas.h"
#include "Deferred/Taa.h"
#include "Deferred/Tonemap.h"
#include "Deferred/PathTracing.h"
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

namespace
{
    uint32_t PackClusterDagDebugColor(uint32_t ClusterIndex, uint32_t MipLevel, uint32_t MaxMipLevel)
    {
        uint32_t Hash = ClusterIndex * 747796405u + 2891336453u + MipLevel * 277803737u;
        Hash ^= Hash >> 16;
        Hash *= 2246822519u;
        Hash ^= Hash >> 13;
        Hash *= 3266489917u;
        Hash ^= Hash >> 16;

        const uint8_t R = static_cast<uint8_t>(96u + (Hash & 0x7fu));
        const uint8_t G = static_cast<uint8_t>(96u + ((Hash >> 8) & 0x7fu));
        const uint8_t B = static_cast<uint8_t>(96u + ((Hash >> 16) & 0x7fu));
        const uint32_t MipColorByte = MaxMipLevel > 0u
            ? (MipLevel * 255u + MaxMipLevel / 2u) / MaxMipLevel
            : 0u;
        const uint8_t A = static_cast<uint8_t>((std::min)(MipColorByte, 255u));
        return static_cast<uint32_t>(R)
            | (static_cast<uint32_t>(G) << 8)
            | (static_cast<uint32_t>(B) << 16)
            | (static_cast<uint32_t>(A) << 24);
    }

    std::vector<uint32_t> BuildClusterDagDebugColorTable(const FRuntimeClusterHierarchy& RuntimeHierarchy, uint32_t IndexCount)
    {
        uint32_t MaxMipLevel = 0u;
        for (const FRuntimeCluster& Cluster : RuntimeHierarchy.Clusters)
        {
            MaxMipLevel = (std::max)(MaxMipLevel, Cluster.MipLevel);
        }

        std::vector<uint32_t> DebugColors(IndexCount, 0xffffffffu);
        for (uint32_t ClusterIndex = 0; ClusterIndex < static_cast<uint32_t>(RuntimeHierarchy.Clusters.size()); ++ClusterIndex)
        {
            const FRuntimeCluster& Cluster = RuntimeHierarchy.Clusters[ClusterIndex];
            const uint32_t PackedColor = PackClusterDagDebugColor(ClusterIndex, Cluster.MipLevel, MaxMipLevel);
            for (uint32_t DrawDataOffset = 0; DrawDataOffset < Cluster.DrawDataCount; ++DrawDataOffset)
            {
                const uint32_t DrawDataIndex = Cluster.DrawDataStart + DrawDataOffset;
                if (DrawDataIndex >= RuntimeHierarchy.DrawDatas.size())
                {
                    continue;
                }

                const FRuntimeClusterDrawData& DrawData = RuntimeHierarchy.DrawDatas[DrawDataIndex];
                if (DrawData.IndexStart >= IndexCount)
                {
                    continue;
                }

                const uint32_t DrawDataEnd = (std::min)(DrawData.IndexStart + DrawData.IndexCount, IndexCount);
                for (uint32_t IndexOffset = DrawData.IndexStart; IndexOffset < DrawDataEnd; ++IndexOffset)
                {
                    DebugColors[IndexOffset] = PackedColor;
                }
            }
        }

        return DebugColors;
    }
}


FDeferredRenderer::FDeferredRenderer()
    : FrameOrchestrator(std::make_unique<FDeferredFrameOrchestrator>())
    , BasePass(std::make_unique<FDeferredBasePass>())
    , LightingPass(std::make_unique<FDeferredLightingPass>())
    , ClusterDagVisibilityPass(std::make_unique<FClusterDagVisibilityPass>())
    , ClusterDagStreamingManager(std::make_unique<FClusterDagStreamingManager>())
    , Gtao(std::make_unique<FGtao>())
    , Hzb(std::make_unique<FHzb>())
    , RayTracingShadow(std::make_unique<FRayTracingShadow>())
    , Ssr(std::make_unique<FSsr>())
    , SkyAtmosphere(std::make_unique<FSkyAtmosphere>())
    , ClusterDagRuntime(std::make_unique<FClusterDagRuntime>())
    , RestirGI(std::make_unique<FRestirGI>())
    , DiffuseGIDenoiser(std::make_unique<FDiffuseGIDenoiser>())
    , SparseSdfGI(std::make_unique<FSparseSdfGI>())
    , PathTracing(std::make_unique<FPathTracing>())
    , AutoExposure(std::make_unique<FAutoExposure>())
    , Cas(std::make_unique<FCas>())
    , Taa(std::make_unique<FTaa>())
    , Tonemap(std::make_unique<FTonemap>())
    , ResourceImporter(std::make_unique<FDeferredResourceImporter>())
{
}

const DXGI_FORMAT FDeferredRenderer::GBufferFormats[kDeferredGBufferCount] =
{
    DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
    DXGI_FORMAT_R16G16B16A16_FLOAT,
};

EDeferredLightingVisualizationMode FDeferredRenderer::GetDeferredLightingVisualizationMode() const
{
    return LightingPass->GetDeferredLightingVisualizationMode();
}

void FDeferredRenderer::ApplyGtaoConfig(const FRendererConfig& Config)
{
    Gtao->ApplyConfig(Config);
}

bool FDeferredRenderer::IsClusterDagEnabled() const
{
    return ClusterDagRuntime->IsEnabled();
}

bool FDeferredRenderer::IsClusterDagFastShaderEnabled() const
{
#ifdef _DEBUG
    return ClusterDagRuntime->IsFastShaderEnabled();
#else
    return false;
#endif   
}

bool FDeferredRenderer::IsClusterDagDebugEnabled() const
{
#ifdef _DEBUG
    return ClusterDagRuntime->IsDebugEnabled();
#else
    return false;
#endif
}

EClusterDAGTraversalMode FDeferredRenderer::GetClusterDagTraversalMode() const
{
    return ClusterDagRuntime->GetTraversalMode();
}
bool FDeferredRenderer::ShouldUseClusterDagRuntimePath(const FMeshSection& Section) const
{
    return IsClusterDagRuntimePathReady() && ClusterDagRuntime->UsesRuntimeSection(Section);
}
bool FDeferredRenderer::IsClusterDagRuntimePathReady() const
{
    return ClusterDagRuntime
        && IsClusterDagEnabled()
        && ClusterDagRuntime->HasResources()
        && IsClusterDagVisibilityPathReady();
}
bool FDeferredRenderer::IsClusterDagVisibilityPathReady() const
{
    return ClusterDagVisibilityPass && ClusterDagVisibilityPass->IsReady();
}
bool FDeferredRenderer::IsPathTracingPreferred() const
{
    return PathTracing->IsPreferred();
}
bool FDeferredRenderer::IsPathTracingVndfEnabled() const
{
    return PathTracing->IsVndfEnabled();
}
void FDeferredRenderer::ForceDisablePathTracing()
{
    PathTracing->SetEnabled(false);
}

void FDeferredRenderer::ApplyPathTracingConfig(const FRendererConfig& Config)
{
    PathTracing->ApplyConfig(Config);
}

DXGI_FORMAT FDeferredRenderer::ResolveRestirGiRadianceFormat(FDX12Device* Device) const
{
    return RestirGI->ResolveRadianceFormat(Device);
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
    FMesh::SetOptimizationStatsLoggingEnabled(Config.bLogMeshOptimizationStats);

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

void FDeferredRenderer::ApplyPostProcessConfig(const FRendererConfig& Config)
{
    Tonemap->SetTonemapEnabled(Config.bEnableTonemap);
    Tonemap->SetTonemapExposure(Config.TonemapExposure);
    Tonemap->SetTonemapWhitePoint(Config.TonemapWhitePoint);
    Tonemap->SetTonemapGamma(Config.TonemapGamma);

    Cas->SetEnabled(Config.bEnableCas);
    Cas->SetSharpness(Config.CasSharpness);

    AutoExposure->SetEnabled(Config.bEnableAutoExposure);
    AutoExposure->SetKey(Config.AutoExposureKey);
    AutoExposure->SetMinExposure(Config.AutoExposureMin);
    AutoExposure->SetMaxExposure(Config.AutoExposureMax);
    AutoExposure->SetSpeedUp(Config.AutoExposureSpeedUp);
    AutoExposure->SetSpeedDown(Config.AutoExposureSpeedDown);

    Taa->SetEnabled(Config.bEnableTAA);
    Taa->SetHistoryWeight(Config.TaaHistoryWeight);
}

void FDeferredRenderer::ApplyLightingPassConfig(const FRendererConfig& Config)
{
    LightingPass->ApplyLightingPassConfig(Config);
}

void FDeferredRenderer::ApplySsrConfig(const FRendererConfig& Config)
{
    Ssr->SetSwEnabled(Config.bEnableSsrSw);
    Ssr->SetHwEnabled(Config.bEnableSsrHw);
    Ssr->SetHzbEnabled(Config.bEnableSsrHzb);
    Ssr->SetHzbFullResDepthEnabled(Config.bEnableSsrHzbFullResDepth);
    Ssr->SetRefineEnabled(Config.bEnableSsrRefine);
    Ssr->SetDenoiseEnabled(Config.bEnableSsrDenoise);
    Ssr->SetMode(Config.SsrMode);
    Ssr->SetSamplesPerQuad(Config.SsrSamplesPerQuad);
    Ssr->SetMaxSteps(Config.SsrMaxSteps);
    Ssr->SetMaxDistance(Config.SsrMaxDistance);
    Ssr->SetThickness(Config.SsrThickness);
    Ssr->SetStride(Config.SsrStride);
    Ssr->SetRoughnessCutoff(Config.SsrRoughnessCutoff);
    Ssr->SetIntensity(Config.SsrIntensity);
}

void FDeferredRenderer::ApplyClusterDAGConfig(const FRendererConfig& Config)
{
    ClusterDagRuntime->ApplyConfig(Config);
    ClusterDagStreamingManager->ApplyConfig(Config);
    ClusterDagVisibilityPass->SetSoftwareRasterHzbRejectEnabled(Config.bEnableClusterDAGSwRasterHzbReject);
}

void FDeferredRenderer::ApplyRestirGIConfig(const FRendererConfig& Config)
{
    const bool bCurrentDenoiserEnabled = DiffuseGIDenoiser->IsEnabled();
    if (bCurrentDenoiserEnabled != Config.bEnableDiffuseGIDenoiser)
    {
        DiffuseGIDenoiser->SetEnabled(Config.bEnableDiffuseGIDenoiser);
        DiffuseGIDenoiser->InvalidateHistory();
    }

    RestirGI->SetEnabled(Config.DiffuseGISource == EDiffuseGISource::RestirGI);
    RestirGI->SetSamplesPerPixel(std::clamp(Config.RestirGISamplesPerPixel, 1u, 32u));
    RestirGI->SetIntensity((std::max)(0.0f, Config.RestirGIIntensity));
    RestirGI->SetRayLength((std::max)(0.1f, Config.RestirGIRayLength));
    RestirGI->SetClampThreshold((std::max)(0.1f, Config.RestirGIClamp));
    RestirGI->SetTemporalReuseEnabled(Config.bEnableRestirGITemporalReuse);
    RestirGI->SetSpatialReuseEnabled(Config.bEnableRestirGISpatialReuse);
    RestirGI->SetTemporalAdditionalScale(std::clamp(Config.RestirGITemporalAdditionalScale, 0.0f, 1.0f));
    RestirGI->SetSpatialAdditionalScale(std::clamp(Config.RestirGISpatialAdditionalScale, 0.0f, 1.0f));
    RestirGI->SetResolveMinDenominator((std::max)(Config.RestirGIResolveMinDenominator, 1e-6f));
    RestirGI->SetResolveMaxNormalization((std::max)(Config.RestirGIResolveMaxNormalization, 1.0f));
    RestirGI->SetResolveLowSampleBoostGuard(std::clamp(Config.RestirGIResolveLowSampleBoostGuard, 0.0f, 1.0f));
    RestirGI->SetResolveUseConfidence(Config.bRestirGIResolveUseConfidence);
    RestirGI->SetMaxHistoryFrames(std::clamp(Config.RestirGIMaxHistoryFrames, 1u, 16u));
    RestirGI->SetUseVisibility(Config.bRestirGIUseVisibility);
    RestirGI->SetUseBrdf(Config.bRestirGIUseBrdf);
    RestirGI->SetUseHistoryIndirect(Config.bRestirGIUseHistoryIndirect);

    if (RestirGI->GetRandomMode() != Config.RestirGIRandomMode)
    {
        RestirGI->SetRandomMode(Config.RestirGIRandomMode);
        RestirGI->InvalidateReservoirHistory();
        DiffuseGIDenoiser->InvalidateHistory();
    }
}

void FDeferredRenderer::ApplySparseSdfGIConfig(const FRendererConfig& Config)
{
    SparseSdfGI->ApplyConfig(Config);
}

void FDeferredRenderer::ApplyRestirGITransientState(const FRestirGITransientState& State)
{
    RestirGI->SetDebugRayEnabled(State.bDebugRayEnabled);
    RestirGI->SetDebugPixel(State.DebugPixelX, State.DebugPixelY);
    RestirGI->SetFreezeFrame(State.bFreezeFrame, GetFrameNumber());
}

void FDeferredRenderer::ApplyRendererConfig(const FRendererConfig& Config)
{
    ApplyPostProcessConfig(Config);
    ApplyLightingPassConfig(Config);
    ApplyClusterDAGConfig(Config);
    Hzb->SetEnabled(Config.bEnableHZB);
    Hzb->SetReady(false);
    ApplyGtaoConfig(Config);
    ApplyPathTracingConfig(Config);
    ApplySsrConfig(Config);
    ApplyRestirGIConfig(Config);
    ApplySparseSdfGIConfig(Config);
}

bool FDeferredRenderer::InitializePipelineDomains(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    if (!GetRayTracingRuntime().CreatePipeline(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ray tracing pipeline creation failed");
        return false;
    }
    if (!CreateSkinningPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: skinning pipeline creation failed");
        return false;
    }
    if (!EnvironmentMap->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: environment build pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer geometry domain pipelines...");
    if (!BasePass->InitializePipelines(*this, Device, LightingBufferFormat))
    {
        LogError("Deferred renderer initialization failed: geometry domain pipeline creation failed");
        return false;
    }

    if (!ClusterDagVisibilityPass->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ClusterDag visibility pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting domain pipelines...");
    if (!LightingPass->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: lighting domain pipeline creation failed");
        return false;
    }

    if (!Gtao->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: GTAO pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer path tracing pipelines...");
    if (!PathTracing->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: path tracing pipeline creation failed");
        return false;
    }

    if (!RestirGI->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI pipeline creation failed");
        return false;
    }

    if (!DiffuseGIDenoiser->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI denoiser pipeline creation failed");
        return false;
    }

    if (!SparseSdfGI->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: SparseSdfGI pipeline creation failed");
        return false;
    }

    if (!Ssr->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: SSR pipeline creation failed");
        return false;
    }

    if (!AutoExposure->InitializePipelines(*this, Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure pipeline creation failed");
        return false;
    }

    if (!Cas->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: CAS pipeline creation failed");
        return false;
    }

    if (!Tonemap->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: tonemap pipeline creation failed");
        return false;
    }

    if (!Taa->InitializePipelines(*this, Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: TAA pipeline creation failed");
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

    NullTexture->SetName(L"NullTexture");

    if (!BasePass->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: geometry domain resource creation failed");
        return false;
    }

    if (!ClusterDagVisibilityPass->InitializeResources(*this, Device, Width, Height))
    {
        LogWarning("Deferred renderer ClusterDag visibility resource creation failed; visibility pass will stay disabled.");
    }

    if (!Taa->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: TAA resource creation failed");
        return false;
    }

    if (!AutoExposure->InitializeResources(*this, Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure resource creation failed");
        return false;
    }

    if (!Cas->InitializeResources(*this, Device))
    {
        LogError("Deferred renderer initialization failed: CAS resource creation failed");
        return false;
    }

    if (!Tonemap->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: tonemap resource creation failed");
        return false;
    }

    if (!PathTracing->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: path tracing resource creation failed");
        return false;
    }

    if (!RestirGI->InitializeResources(*this, Device, Width, Height, Config.FramesInFlight))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI resource creation failed");
        return false;
    }

    if (!DiffuseGIDenoiser->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI denoiser resource creation failed");
        return false;
    }

    if (!SparseSdfGI->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: SparseSdfGI resource creation failed");
        return false;
    }

    if (!Ssr->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: SSR resource creation failed");
        return false;
    }

    if (!LightingPass->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: lighting domain resource creation failed");
        return false;
    }

    if (!Gtao->InitializeResources(*this, Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: GTAO resource creation failed");
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

    const bool bGpuDrivenCullingPipelinesReady = CreateCullingPipelines(Device);
    if (!bGpuDrivenCullingPipelinesReady)
    {
        LogWarning("Deferred renderer GPU-driven culling pipeline creation failed before ClusterDag runtime initialization; fallback paths may be incomplete.");
    }

    if (ClusterDagRuntime->IsEnabled())
    {
        bool bClusterDagRuntimeReady = false;
        if (!bGpuDrivenCullingPipelinesReady)
        {
            LogWarning("Deferred renderer ClusterDag runtime pipeline creation skipped because GPU-driven culling pipelines are unavailable; falling back to legacy path.");
        }
        else if (!ClusterDagRuntime->InitializePipelines(*this, Device))
        {
            LogWarning("Deferred renderer ClusterDag runtime pipeline creation failed; falling back to legacy path.");
        }
        else
        {
            bClusterDagRuntimeReady = true;
        }

        if (bClusterDagRuntimeReady && !ClusterDagRuntime->InitializeResources(*this, Device))
        {
            LogWarning("Deferred renderer ClusterDag runtime resource creation failed; falling back to legacy path.");
        }
        else if (bClusterDagRuntimeReady && ClusterDagRuntime->HasResources())
        {
            const uint32_t PageCount = ClusterDagRuntime->GetStreamingPageCount();
            if (!ClusterDagStreamingManager->InitializeResources(
                *this,
                Device,
                PageCount,
                GetFramesInFlight(),
                ClusterDagRuntime->GetStreamingPageSources()))
            {
                LogWarning("Deferred renderer ClusterDag streaming resource creation failed; streaming feedback will stay disabled.");
            }
        }
    }

    if (!CreateGpuDrivenResources(Device))
    {
        LogWarning("Deferred renderer GPU-driven resources creation failed; fallback to CPU-driven draws.");
    }

    FSkyPipelineConfig SkyPipelineConfig;
    SkyPipelineConfig.DepthEnable = true;
    SkyPipelineConfig.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    SkyPipelineConfig.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    SkyPipelineConfig.DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    const float SkySphereRadius = (std::max)(SceneRadius * 5.0f, 100.0f);
    if (!SkyAtmosphere->Initialize(Device, SkySphereRadius, LightingBufferFormat, SkyPipelineConfig))
    {
        LogError("Deferred renderer initialization failed: sky pipeline state creation failed");
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
    if (!SceneWorldBuilder::LoadWorldFromSceneFile(Device, SceneFilePath, World, SceneCenter, SceneRadius))
    {
        LogError("scene JSON could not be loaded.");
        return false;
    }

    if (!CreateSkinnedPositionBuffers())
    {
        LogError("Deferred renderer initialization failed: skinned position buffer creation failed");
        return false;
    }

    const std::vector<FDrawSectionView>& DrawSections = GetWorld().GetDrawSectionViews();
    SceneWorldMatrix = DrawSections.empty() ? DirectX::XMFLOAT4X4{} : DrawSections.front().Object->GetWorldMatrix();
    for (const std::unique_ptr<FObject>& Object : GetWorld().GetObjects())
    {
        if (Object)
        {
            Object->ResetPreviousWorldMatrix();
        }
    }

    SceneConstantBufferStride = (sizeof(FSceneConstants) + 255ULL) & ~255ULL;
    const uint64_t ConstantBufferSize = SceneConstantBufferStride * (std::max<uint64_t>(1, DrawSections.size()));

    if (!CreateSceneConstantBuffersPerFrame(Device, ConstantBufferSize))
    {
        LogError("Deferred renderer initialization failed: constant buffer creation failed");
        return false;
    }
    if (!CreateClusterDagSceneConstantBuffersPerFrame(Device, static_cast<uint32_t>((std::max<size_t>)(1, DrawSections.size()))))
    {
        LogError("Deferred renderer initialization failed: Cluster DAG constant buffer creation failed");
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
    if (!EnvironmentMap->InitializeResources(*this, Device, Config, "Deferred"))
    {
        return false;
    }

    bool bSobolFreshlyLoaded = false;
    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/BlueNoise/sobol_256_4d.png", BlueNoiseSobolTexture.Resource, nullptr, false, &bSobolFreshlyLoaded))
    {
        LogError("Deferred renderer initialization failed: blue noise sobol texture loading failed");
        return false;
    }
    BlueNoiseSobolTexture->SetName(L"BlueNoiseSobol");
    InitializeBindlessTexture(BlueNoiseSobolTexture, BuildTextureDescFromResource(BlueNoiseSobolTexture.Get()), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    WriteOrCreateBindlessTextureSrv(Device, BlueNoiseSobolTexture);

    bool bScramblingFreshlyLoaded = false;
    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/BlueNoise/scrambling_ranking_128x128_2d_1spp.png", BlueNoiseScramblingRanking1SPPTexture.Resource, nullptr, false, &bScramblingFreshlyLoaded))
    {
        LogError("Deferred renderer initialization failed: blue noise scrambling/ranking texture loading failed");
        return false;
    }
    BlueNoiseScramblingRanking1SPPTexture->SetName(L"BlueNoiseScramblingRanking1SPP");
    InitializeBindlessTexture(BlueNoiseScramblingRanking1SPPTexture, BuildTextureDescFromResource(BlueNoiseScramblingRanking1SPPTexture.Get()), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    WriteOrCreateBindlessTextureSrv(Device, BlueNoiseScramblingRanking1SPPTexture);

    {
        constexpr D3D12_RESOURCE_STATES BlueNoiseReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        FDX12CommandContext BlueNoiseBarrierContext;
        BlueNoiseBarrierContext.Initialize(Device, Device->GetGraphicsQueue(), 1);
        BlueNoiseBarrierContext.BeginFrame(0);
        if (bSobolFreshlyLoaded)
        {
            BlueNoiseBarrierContext.TransitionResource(BlueNoiseSobolTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BlueNoiseReadState);
        }
        if (bScramblingFreshlyLoaded)
        {
            BlueNoiseBarrierContext.TransitionResource(BlueNoiseScramblingRanking1SPPTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, BlueNoiseReadState);
        }
        BlueNoiseBarrierContext.CloseAndExecute();
        BlueNoiseBarrierContext.GetQueue()->Flush();
    }

    if (!CreateSceneTextures(Device, World))
    {
        LogError("Deferred renderer initialization failed: scene texture creation failed");
        return false;
    }

    if (!PathTracing->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: path tracing descriptor creation failed");
        return false;
    }

    if (!DiffuseGIDenoiser->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI denoiser descriptor creation failed");
        return false;
    }

    if (!RestirGI->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: ReSTIR GI descriptor creation failed");
        return false;
    }

    if (!SparseSdfGI->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: SparseSdfGI descriptor creation failed");
        return false;
    }

    if (!AutoExposure->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure descriptor creation failed");
        return false;
    }

    if (!Cas->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: CAS descriptor creation failed");
        return false;
    }

    if (!Taa->CreatePersistentDescriptors(*this, Device))
    {
        LogError("Deferred renderer initialization failed: TAA descriptor creation failed");
        return false;
    }

    return true;
}

bool FDeferredRenderer::CreateClusterDagSceneConstantBuffersPerFrame(FDX12Device* Device, uint32_t ModelCount)
{
    ClusterDagSceneConstantBuffers.clear();
    ClusterDagSceneConstantBufferMapped.clear();
    ClusterDagSceneConstantBuffers.resize(GetFramesInFlight());
    ClusterDagSceneConstantBufferMapped.resize(GetFramesInFlight(), nullptr);

    const FRGBufferDesc Desc = CreateStructuredBufferDesc<FSceneConstants>((std::max)(1u, ModelCount));
    for (uint32_t Index = 0; Index < GetFramesInFlight(); ++Index)
    {
        void* MappedData = nullptr;
        if (!CreateMappedBindlessBuffer(
            Device,
            L"ClusterDagSceneConstantBuffer_Frame" + std::to_wstring(Index),
            Desc,
            ClusterDagSceneConstantBuffers[Index],
            MappedData))
        {
            return false;
        }

        CreateBindlessBufferSrv(Device, ClusterDagSceneConstantBuffers[Index]);
        ClusterDagSceneConstantBufferMapped[Index] = static_cast<uint8_t*>(MappedData);
    }

    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS FDeferredRenderer::GetClusterDagSceneConstantBufferAddress() const
{
    return GetClusterDagSceneConstantBufferAddress(GetFrameIndex());
}

D3D12_GPU_VIRTUAL_ADDRESS FDeferredRenderer::GetClusterDagSceneConstantBufferAddress(uint32_t FrameIndex) const
{
    if (FrameIndex >= ClusterDagSceneConstantBuffers.size())
    {
        return 0;
    }

    return ClusterDagSceneConstantBuffers[FrameIndex].GetGPUVirtualAddress();
}

uint8_t* FDeferredRenderer::GetClusterDagSceneConstantBufferMapped() const
{
    const uint32_t FrameIndex = GetFrameIndex();
    if (FrameIndex >= ClusterDagSceneConstantBufferMapped.size())
    {
        return nullptr;
    }

    return ClusterDagSceneConstantBufferMapped[FrameIndex];
}


bool FDeferredRenderer::InitializeGpuDebugResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    if (!GpuDebugState.CreateResources(Device)
        || !GpuDebugState.CreateLinePipeline(Device, BackBufferFormat, SceneDepthFormat)
        || !GpuDebugState.CreateBoxPipeline(Device, BackBufferFormat, SceneDepthFormat))
    {
        LogError("Deferred renderer initialization failed: GPU debug debug-draw setup failed");
        return false;
    }

    if (GpuDebugState.IsPrintEnabled() && (!GpuDebugState.CreatePrintPipeline(Device, BackBufferFormat) || !GpuDebugState.CreatePrintStatsPipeline(Device)))
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

    GetWorld().Tick(DeltaTime);
    ClusterDagSceneConstantsPreparedFrame = UINT32_MAX;

    const bool bAnySkinningUpdated = World.WasAnySkinningUpdatedLastTick();

    GpuDebugState.PreparePrint(CmdContext);
    GpuDebugState.PrepareLine(CmdContext);
    GpuDebugState.PrepareBox(CmdContext);

    FDeferredFrameState FrameState;
    PrepareFrameState(Camera, bAnySkinningUpdated, FrameState);
    DispatchSkinning(CmdContext, FrameState.LightViewProjection);
    GetRayTracingRuntime().UpdateBlasRefit(*this, CmdContext);
    GetRayTracingRuntime().BuildTlas(*this, CmdContext);

    FRenderGraph Graph;
    ConfigureFrameGraph(Graph);

    const bool bUsePathTracing = PathTracing->IsPreferred() && GetRayTracingRuntime().bRayTracingPipelineReady;

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

    ConfigureHZBOcclusion(FrameState.bUseHZBOcclusion, Hzb->GetSrvBindlessIndex(), Hzb->GetWidth(), Hzb->GetHeight(), Hzb->GetMipCount());

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
        RestirGI->InvalidateReservoirHistory();
        DiffuseGIDenoiser->InvalidateHistory();
    }

    if (!RestirGI->IsEnabled())
    {
        RestirGI->InvalidateReservoirHistory();
        DiffuseGIDenoiser->InvalidateHistory();
    }

    if (!DiffuseGIDenoiser->IsEnabled())
    {
        DiffuseGIDenoiser->InvalidateHistory();
    }

    OutState.bGtaoJitterActive = Gtao->IsEnabled() && Gtao->IsJitterEnabled();
    Taa->PrepareFrameState(
        *this,
        Camera,
        OutState.bGtaoJitterActive,
        OutState.bTaaActive,
        OutState.bTaaHistoryReady,
        OutState.TaaFrameIndex,
        OutState.TaaReadIndex,
        OutState.TaaWriteIndex);

    PathTracing->PrepareFrameState(
        OutState.TaaFrameIndex,
        bCameraMoved,
        OutState.bPathTracingAccumulationActive,
        OutState.bPathTracingAccumulationHistoryReady,
        OutState.PathTracingAccumulationReadIndex,
        OutState.PathTracingAccumulationWriteIndex);

    const DirectX::XMMATRIX CurrentUnjitteredProjection = Camera.GetProjectionMatrix();
    const DirectX::XMMATRIX CurrentUnjitteredViewProjection = Camera.GetViewMatrix() * CurrentUnjitteredProjection;
    DirectX::XMStoreFloat4x4(&CurrentUnjitteredViewProjectionMatrix, CurrentUnjitteredViewProjection);

    OutState.bRenderShadows = bShadowsEnabled && BasePass->HasShadowPipelines() && ShadowMap;
    OutState.bDoDepthPrepass = bDepthPrepassEnabled && BasePass->HasDepthPrepassPipelines();
    const bool bPrevHZBReady = Hzb->IsReady();
    OutState.bUseHZBOcclusion = Hzb->IsEnabled() && bPrevHZBReady && IsValidBindlessIndex(Hzb->GetSrvBindlessIndex());
    OutState.bUseHzbTwoPass = bEnableIndirectDraw && OutState.bUseHZBOcclusion && Hzb->IsTwoPassEnabled();
    if (OutState.bUseHzbTwoPass)
    {
        OutState.bDoDepthPrepass = false;
    }
    OutState.bBuildHZB = Hzb->IsEnabled();
    OutState.bCasActive = Cas->IsReady();
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
    Taa->FinalizeFrameState(FrameState.bTaaActive, FrameState.bGtaoJitterActive);
    AutoExposure->FinalizeFrame();

    bHasPreviousViewProjection = true;
    PreviousUnjitteredViewProjectionMatrix = CurrentUnjitteredViewProjectionMatrix;
    bHasPreviousUnjitteredViewProjection = true;

    RestirGI->FinalizeFrame(*this);
    DiffuseGIDenoiser->FinalizeFrame(*this);

    for (const std::unique_ptr<FObject>& Object : GetWorld().GetObjects())
    {
        if (Object)
        {
            Object->SetPreviousWorldMatrix(Object->GetWorldMatrix());
        }
    }
}


void FDeferredRenderer::OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue)
{
    Taa->OnFrameFenceSignaled(FrameIndex);
    PathTracing->OnFrameFenceSignaled(FrameIndex);
    DiffuseGIDenoiser->OnFrameFenceSignaled(FrameIndex);
    SparseSdfGI->OnFrameFenceSignaled(FrameIndex);
    ClusterDagStreamingManager->OnFrameFenceSignaled(FrameIndex, FenceValue);
}

bool FDeferredRenderer::CreateSceneTextures(FDX12Device* Device, FWorld& World)
{
    if (!TextureLoader)
    {
        return false;
    }

    const std::vector<FDrawSectionView>& DrawSections = World.GetDrawSectionViews();
    std::vector<FTextureLoadRequest> Requests;
    Requests.reserve(DrawSections.size() * 10);

    for (const FDrawSectionView& DrawSection : DrawSections)
    {
        DrawSection.Section->Material.AppendTextureLoadRequests(Requests);
    }

    LogInfo("Loading " + std::to_string(Requests.size()) + " textures in parallel for " + std::to_string(DrawSections.size()) + " sections");
    if (!TextureLoader->LoadTexturesParallel(Requests))
    {
        LogError("Failed to load scene textures");
        return false;
    }

    for (const FDrawSectionView& DrawSection : DrawSections)
    {
        DrawSection.Section->Material.CreateTextureSrvs(Device);
    }

    return true;
}


void FDeferredRenderer::WriteSceneConstants(const FCamera& Camera, const FObject& Object, const FMeshSection& Section, uint64_t ConstantBufferOffset, uint8_t* ConstantBufferMapped, bool bUseClusterDagIndexBuffer)
{
    if (ConstantBufferMapped == nullptr)
    {
        return;
    }

    const DirectX::XMVECTOR LightDir = DirectX::XMLoadFloat3(&LightDirection);
    const DirectX::XMMATRIX LightVP = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
    DirectX::XMStoreFloat4x4(&LightViewProjection, LightVP);
    const bool bUseTaaJitter = Taa->UsesJitter();
    const DirectX::XMMATRIX Projection = bUseTaaJitter ? Taa->GetProjection() : Camera.GetProjectionMatrix();
    const DirectX::XMFLOAT2 Jitter = bUseTaaJitter ? Taa->GetJitter() : DirectX::XMFLOAT2(0.0f, 0.0f);
    const uint32_t TaaSampleIndex = Taa->GetSampleIndex();
    const uint32_t GtaoTemporalIndex = (Gtao->IsEnabled() && Gtao->IsJitterEnabled()) ? TaaSampleIndex : 0u;

    const DirectX::XMMATRIX PreviousWorld = Object.HasPreviousWorldMatrix()
        ? DirectX::XMLoadFloat4x4(&Object.GetPreviousWorldMatrix())
        : DirectX::XMMatrixIdentity();
    const bool bHasPreviousWorld = Object.HasPreviousWorldMatrix();

    uint32_t PreviousSkinnedPositionBindlessIndex = UINT32_MAX;
    bool bHasPreviousSkinning = false;
    const uint32_t FrameCount = GetFramesInFlight();
    const uint32_t PrevFrameIndex = FrameCount > 0 ? (GetFrameIndex() + FrameCount - 1u) % FrameCount : 0u;
    if (IsValidBindlessIndex(Section.BoneMatrixBuffer.SrvBindlessIndex)
        && Section.BoneMatrixCount > 0
        && PrevFrameIndex < Section.SkinnedPositionBuffers.size())
    {
        PreviousSkinnedPositionBindlessIndex = Section.SkinnedPositionBuffers[PrevFrameIndex].SrvBindlessIndex;
        bHasPreviousSkinning = IsValidBindlessIndex(PreviousSkinnedPositionBindlessIndex);
    }

    const EDeferredLightingVisualizationMode VisualizationMode = GetDeferredLightingVisualizationMode();
    const bool bUseClusterDagDebugColor =
        IsClusterDagDebugEnabled()
        && (VisualizationMode == EDeferredLightingVisualizationMode::ClusterDagClusters
            || VisualizationMode == EDeferredLightingVisualizationMode::ClusterDagMip)
        && Section.ClusterDagDebugColorBuffer.HasSrv();

    RendererUtils::FUpdateSceneConstantsParams Params;
    Params.Camera = &Camera;
    Params.Object = &Object;
    Params.Section = &Section;
    Params.LightIntensity = LightIntensity;
    Params.LightDirection = LightDir;
    Params.LightColor = LightColor;
    Params.LightViewProjection = LightVP;
    Params.Projection = Projection;
    Params.ShadowStrength = bShadowsEnabled ? ShadowStrength : 0.0f;
    Params.ShadowBias = ShadowBias;
    Params.ShadowMapWidth = static_cast<float>(ShadowMapWidth);
    Params.ShadowMapHeight = static_cast<float>(ShadowMapHeight);
    Params.EnvMapMipCount = GetEnvironmentMipCount();
    Gtao->SetTemporalIndex(GtaoTemporalIndex);
    Params.bGtaoEnabled = Gtao->IsEnabled();
    Params.GtaoIntensity = Gtao->GetIntensity();
    Params.ConstantBufferMapped = ConstantBufferMapped;
    Params.ConstantBufferOffset = ConstantBufferOffset;
    Params.PreviousWorld = PreviousWorld;
    Params.bHasPreviousWorld = bHasPreviousWorld;
    Params.PreviousSkinnedPositionBindlessIndex = PreviousSkinnedPositionBindlessIndex;
    Params.bHasPreviousSkinning = bHasPreviousSkinning;
    Params.DeferredLightingVisualizationMode = static_cast<uint32_t>(VisualizationMode);
    Params.bUseClusterDagIndexBuffer = bUseClusterDagIndexBuffer;
    Params.bUseClusterDagDebugColor = bUseClusterDagDebugColor;
    RendererUtils::UpdateSceneConstants(Params);
}

void FDeferredRenderer::EnsureClusterDagDebugColorBuffers()
{
    const EDeferredLightingVisualizationMode VisualizationMode = GetDeferredLightingVisualizationMode();
    const bool bNeedsDebugColors =
        IsClusterDagDebugEnabled()
        && (VisualizationMode == EDeferredLightingVisualizationMode::ClusterDagClusters
            || VisualizationMode == EDeferredLightingVisualizationMode::ClusterDagMip);
    if (!bNeedsDebugColors || Device == nullptr)
    {
        return;
    }

    const std::vector<FDrawSectionView>& DrawSections = GetWorld().GetDrawSectionViews();
    for (const FDrawSectionView& DrawSection : DrawSections)
    {
        FMeshSection& Section = *DrawSection.Section;
        if ((!ClusterDagRuntime->UsesRuntimeSection(Section) && !Section.bCoveredByClusterDagRuntime)
            || Section.ClusterDagDebugColorBuffer.HasSrv()
            || !Section.ClusterDagRuntimeHierarchy.IsValid()
            || Section.ClusterDagIndexCount == 0)
        {
            continue;
        }

        const std::vector<uint32_t> DebugColors = BuildClusterDagDebugColorTable(Section.ClusterDagRuntimeHierarchy, Section.ClusterDagIndexCount);
        if (DebugColors.empty())
        {
            continue;
        }

        FBindlessBuffer DebugColorBuffer;
        FUploadBuffer UploadBuffer;
        CreateBindlessBufferWithUpload(
            Device,
            L"ClusterDagDebugColorBuffer",
            CreateStructuredBufferDesc<uint32_t>(DebugColors.size()),
            D3D12_RESOURCE_STATE_COPY_DEST,
            DebugColorBuffer,
            UploadBuffer,
            DebugColors.data(),
            true,
            false);

        FDX12CommandContext UploadContext;
        UploadContext.Initialize(Device, Device->GetGraphicsQueue(), 1);
        UploadContext.BeginFrame(0);
        ID3D12GraphicsCommandList* UploadCommandList = UploadContext.GetCommandList();
        UploadCommandList->CopyBufferRegion(DebugColorBuffer.Get(), 0, UploadBuffer.Get(), 0, UploadBuffer.Size);
        UploadContext.TransitionResource(DebugColorBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
        UploadContext.CloseAndExecute();
        UploadContext.GetQueue()->Flush();

        if (!DebugColorBuffer.HasSrv())
        {
            continue;
        }

        if (DrawSection.Object && Section.bUseClusterDagRuntime)
        {
            std::vector<FMeshSection>& ObjectSections = DrawSection.Object->GetSections();
            for (FMeshSection& ObjectSection : ObjectSections)
            {
                ObjectSection.ClusterDagDebugColorBuffer = DebugColorBuffer;
            }
        }
        else
        {
            Section.ClusterDagDebugColorBuffer = DebugColorBuffer;
        }
    }
}

void FDeferredRenderer::UpdateSceneConstants(const FCamera& Camera, const FObject& Object, const FMeshSection& Section, size_t DrawSectionIndex, uint64_t ConstantBufferOffset, bool bUseClusterDagIndexBuffer)
{
    (void)DrawSectionIndex;

    WriteSceneConstants(
        Camera,
        Object,
        Section,
        ConstantBufferOffset,
        GetSceneConstantBufferMapped(),
        bUseClusterDagIndexBuffer);
}

void FDeferredRenderer::UpdateClusterDagSceneConstants(const FCamera& Camera, const FObject& Object, const FMeshSection& Section, size_t DrawSectionIndex, uint64_t ConstantBufferOffset)
{
    (void)DrawSectionIndex;

    WriteSceneConstants(
        Camera,
        Object,
        Section,
        ConstantBufferOffset,
        GetClusterDagSceneConstantBufferMapped(),
        true);
}

void FDeferredRenderer::EnsureClusterDagSceneConstantsPrepared(const FCamera& Camera)
{
    if (ClusterDagSceneConstantsPreparedFrame == GetFrameIndex())
    {
        return;
    }

    if (!IsClusterDagRuntimePathReady()
        || GetClusterDagSceneConstantBufferMapped() == nullptr)
    {
        return;
    }

    EnsureClusterDagDebugColorBuffers();

    const std::vector<FDrawSectionView>& DrawSections = GetWorld().GetDrawSectionViews();
    for (const FDrawSectionView& DrawSection : DrawSections)
    {
        const FMeshSection& Section = *DrawSection.Section;
        if (!ClusterDagRuntime->UsesRuntimeSection(Section) && !Section.bCoveredByClusterDagRuntime)
        {
            continue;
        }

        const uint64_t ConstantBufferOffset = SceneConstantBufferStride * DrawSection.DrawSectionIndex;
        UpdateClusterDagSceneConstants(Camera, *DrawSection.Object, Section, DrawSection.DrawSectionIndex, ConstantBufferOffset);
    }

    ClusterDagSceneConstantsPreparedFrame = GetFrameIndex();
}

void FDeferredRenderer::UpdateCullingVisibility(const FCamera& Camera)
{
    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    const bool bGpuCullingActive = CanDispatchGpuCulling();
    RendererUtils::UpdateCullingVisibility(*CullingCamera, World, !bGpuCullingActive);
}

bool FDeferredRenderer::CreateGpuDrivenResources(FDX12Device* Device)
{
    if (GetWorld().GetDrawSectionCount() == 0 || !GetSceneConstantBuffer())
    {
        return false;
    }

    bool bCreatedAnyResource = false;
    bool bPreparedMeshletGpuDrivenData = false;
    bool bHasMeshletIndirectCommands = false;

    // Step 1: Prepare indirect draw data
    FGpuDrivenPreparedData PreparedData;
    if (!PrepareGpuDrivenDrawData(PreparedData))
    {
        LogWarning("Failed to prepare meshlet GPU-driven draw data");
    }
    else
    {
        bPreparedMeshletGpuDrivenData = true;
        bHasMeshletIndirectCommands = !PreparedData.Commands.empty();

        if (bHasMeshletIndirectCommands && !CreatePerFrameIndirectBuffers(Device, PreparedData))
        {
            LogWarning("Failed to create meshlet GPU-driven per-frame indirect buffers");
        }
        else if (!CreateSharedGpuDrivenBuffers(Device, PreparedData))
        {
            LogWarning("Failed to create meshlet GPU-driven shared buffers");
        }
        else if (!UploadGpuDrivenBuffers(Device, PreparedData))
        {
            LogWarning("Failed to upload meshlet GPU-driven buffers");
        }
        else
        {
            bCreatedAnyResource = bHasMeshletIndirectCommands || bPreparedMeshletGpuDrivenData;
        }
    }

    if (!bCreatedAnyResource && !ClusterDagRuntime->IsEnabled())
    {
        return false;
    }

    if (!CreateCullingPipelines(Device))
    {
        LogError("Failed to create culling pipelines");
        return false;
    }

    if (!GpuDrivenCullingState.CreateVisibilityListPipelines(Device))
    {
        LogError("Failed to create visibility list pipelines");
        return false;
    }
    RefreshGpuDrivenPersistentValidation();

    if (!CreateIndirectCommandSignature(Device, BasePass->GetBasePassRootSignature()))
    {
        LogError("Failed to create indirect command signature");
        return false;
    }

    return true;
}
