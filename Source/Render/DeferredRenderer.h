#pragma once

#include <array>
#include <algorithm>
#include <mutex>
#include <memory>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include "Renderer.h"
#include "RenderGraph.h"
#include "GpuResource.h"
#include "Deferred/Gtao.h"
#include "Deferred/Hzb.h"
#include "Deferred/RayTracingShadow.h"
#include "Deferred/Ssr.h"
#include "SkyAtmosphere.h"
#include "Deferred/RestirGI.h"
#include "Deferred/RestirGIDenoiser.h"
#include "Deferred/SparseSdfGI.h"
#include "Deferred/PathTracing.h"
#include "Deferred/AutoExposure.h"
#include "Deferred/Cas.h"
#include "Deferred/ClusterDagVisibilityPass.h"
#include "Deferred/ClusterDagRuntime.h"
#include "Deferred/ClusterDagStreamingManager.h"
#include "Deferred/GBufferLayout.h"
#include "Deferred/Taa.h"
#include "Deferred/Tonemap.h"
#include "../Core/RendererConfig.h"
#include "../Scene/GltfAnimation.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;
class FDeferredFrameOrchestrator;
class FDeferredBasePass;
class FDeferredLightingPass;
class FDeferredResourceImporter;

using FDeferredGBufferHandles = std::array<FRGResourceHandle, kDeferredGBufferCount>;


class FDeferredRenderer : public FRenderer
{
public:
    // Format constants
    static constexpr DXGI_FORMAT LightingBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT PathTracingBufferFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    static const DXGI_FORMAT GBufferFormats[kDeferredGBufferCount];

    // Transient debug state structs
    struct FRestirGITransientState
    {
        bool bFreezeFrame = false;
        bool bDebugRayEnabled = false;
        uint32_t DebugPixelX = 0u;
        uint32_t DebugPixelY = 0u;
    };

    // Lifecycle
    FDeferredRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;
    void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue) override;

    // Config application
    void ApplyPostProcessConfig(const FRendererConfig& Config);
    void ApplyLightingPassConfig(const FRendererConfig& Config);
    void ApplyClusterDAGConfig(const FRendererConfig& Config);
    void ApplySsrConfig(const FRendererConfig& Config);
    void ApplyRestirGIConfig(const FRendererConfig& Config);
    void ApplySparseSdfGIConfig(const FRendererConfig& Config);
    void ApplyRestirGITransientState(const FRestirGITransientState& State);
        void ApplyGtaoConfig(const FRendererConfig& Config) override;
    void ApplyPathTracingConfig(const FRendererConfig& Config) override;

    // HZB interface
    void SetHZBEnabled(bool bEnabled) { Hzb->SetEnabled(bEnabled); }
    bool IsHZBEnabled() const { return Hzb->IsEnabled(); }
    void SetHzbTwoPassEnabled(bool bEnabled) { Hzb->SetTwoPassEnabled(bEnabled); }
    bool IsHzbTwoPassEnabled() const { return Hzb->IsTwoPassEnabled(); }

    // Misc setters / getters
    EDeferredLightingVisualizationMode GetDeferredLightingVisualizationMode() const;

    // ClusterDAG queries
    bool IsClusterDagEnabled() const override;
    bool IsClusterDagFastShaderEnabled() const override;
    bool IsClusterDagDebugEnabled() const override;
    EClusterDAGTraversalMode GetClusterDagTraversalMode() const override;
    bool ShouldUseClusterDagRuntimePath(const FMeshSection& Section) const override;
    bool IsClusterDagRuntimePathReady() const;
    bool IsClusterDagVisibilityPathReady() const;
    FClusterDagStreamingManager* GetClusterDagStreamingManager() const { return ClusterDagStreamingManager.get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetClusterDagSceneConstantBufferAddress() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetClusterDagSceneConstantBufferAddress(uint32_t FrameIndex) const;

    // Path tracing queries
    bool IsPathTracingPreferred() const override;
    bool IsPathTracingVndfEnabled() const override;
    void ForceDisablePathTracing() override;

    // Submodule accessors
    FRestirGI* GetRestirGI() const { return RestirGI.get(); }
    FSparseSdfGI* GetSparseSdfGI() const { return SparseSdfGI.get(); }
    FPathTracing* GetPathTracing() const { return PathTracing.get(); }

public:
    // Per-frame state (used by render passes and orchestrator)
    struct FDeferredFrameState
    {
        bool bTaaActive = false;
        bool bTaaHistoryReady = false;
        uint32_t TaaFrameIndex = 0;
        uint32_t TaaReadIndex = 0;
        uint32_t TaaWriteIndex = 0;
        bool bRenderShadows = false;
        bool bDoDepthPrepass = false;
        bool bUseHZBOcclusion = false;
        bool bUseHzbTwoPass = false;
        bool bBuildHZB = false;
        bool bCasActive = false;
        bool bGtaoJitterActive = false;
        bool bPathTracingAccumulationActive = false;
        bool bPathTracingAccumulationHistoryReady = false;
        bool bCameraMoved = false;
        bool bAnySkinningUpdated = false;
        uint32_t PathTracingAccumulationReadIndex = 0;
        uint32_t PathTracingAccumulationWriteIndex = 0;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    struct FDeferredFrameResources
    {
        FRGResourceHandle ShadowHandle{};
        FRayTracingShadowFrameResources RayTracingShadow;
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle ObjectIdHandle{};
        FRGResourceHandle VelocityHandle{};
        FDeferredGBufferHandles GBufferHandles{};
        FClusterDagVisibilityFrameResources ClusterDagVisibility;
        FRGResourceHandle LinearDepthHandle{};
        FGtaoFrameResources Gtao;
        FSparseSdfGIFrameResources SparseSdfGI;
        FRestirGIFrameResources RestirGI;
        FRestirGIDenoiserFrameResources RestirGIDenoiser;
        FSsrFrameResources Ssr;
        FRGResourceHandle LightingHandle{};
        FAutoExposureFrameResources AutoExposure;
        FCasFrameResources Cas;
        FTonemapFrameResources Tonemap;
        FTaaFrameResources Taa;
        FHzbFrameResources Hzb;
        FPathTracingFrameResources PathTracing;
    };

private:
    // Initialization
    bool InitializePipelineDomains(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool InitializeFrameResources(FDX12Device* Device, uint32_t Width, uint32_t Height, const FRendererConfig& Config);
    bool InitializeSceneResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config);
    bool InitializeSceneModelResources(FDX12Device* Device, const FRendererConfig& Config);
    bool InitializeEnvironmentAndDescriptorResources(FDX12Device* Device, const FRendererConfig& Config);
    bool InitializeGpuDebugResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateClusterDagSceneConstantBuffersPerFrame(FDX12Device* Device, uint32_t ModelCount);
    bool CreateSceneTextures(FDX12Device* Device, FWorld& World);
    bool CreateGpuDrivenResources(FDX12Device* Device);

    // Frame rendering
    void PrepareFrameState(const FCamera& Camera, bool bAnySkinningUpdated, FDeferredFrameState& OutState);
    void ConfigureFrameGraph(FRenderGraph& Graph) const;
    void FinalizeFrameState(const FDeferredFrameState& FrameState);
    void UpdateCullingVisibility(const FCamera& Camera);

    // Scene constants
    void WriteSceneConstants(const FCamera& Camera, const FMeshSection& Section, uint64_t ConstantBufferOffset, uint8_t* ConstantBufferMapped, bool bUseClusterDagIndexBuffer);
    void UpdateSceneConstants(const FCamera& Camera, const FMeshSection& Section, size_t DrawSectionIndex, uint64_t ConstantBufferOffset, bool bUseClusterDagIndexBuffer = false);
    void UpdateClusterDagSceneConstants(const FCamera& Camera, const FMeshSection& Section, size_t DrawSectionIndex, uint64_t ConstantBufferOffset);
    void EnsureClusterDagSceneConstantsPrepared(const FCamera& Camera);
    void EnsureClusterDagDebugColorBuffers();
    uint8_t* GetClusterDagSceneConstantBufferMapped() const;

    // Config / state helpers
    void ApplyRendererConfig(const FRendererConfig& Config);
    DXGI_FORMAT ResolveRestirGiRadianceFormat(FDX12Device* Device) const;

private:
    // Friend render passes
    friend class FDeferredFrameOrchestrator;
    friend class FDeferredBasePass;
    friend class FDeferredLightingPass;
    friend class FClusterDagVisibilityPass;
    friend class FClusterDagStreamingManager;
    friend class FDeferredResourceImporter;
    friend class FGpuDebug;
    friend class FObjectId;
    friend class FGtao;
    friend class FHzb;
    friend class FRayTracingShadow;
    friend class FSsr;
    friend class FSkyAtmosphere;
    friend class FRestirGI;
    friend class FRestirGIDenoiser;
    friend class FSparseSdfGI;
    friend class FPathTracing;
    friend class FAutoExposure;
    friend class FCas;
    friend class FTaa;
    friend class FTonemap;

    // Render passes and submodules
    std::unique_ptr<FDeferredFrameOrchestrator> FrameOrchestrator;
    std::unique_ptr<FDeferredBasePass>          BasePass;
    std::unique_ptr<FDeferredLightingPass>      LightingPass;
    std::unique_ptr<FClusterDagVisibilityPass>  ClusterDagVisibilityPass;
    std::unique_ptr<FClusterDagStreamingManager> ClusterDagStreamingManager;
    std::unique_ptr<FGtao>                      Gtao;
    std::unique_ptr<FRayTracingShadow>          RayTracingShadow;
    std::unique_ptr<FSsr>                       Ssr;
    std::unique_ptr<FSkyAtmosphere>             SkyAtmosphere;
    std::unique_ptr<FClusterDagRuntime>         ClusterDagRuntime;
    std::unique_ptr<FRestirGI>                  RestirGI;
    std::unique_ptr<FRestirGIDenoiser>          RestirGIDenoiser;
    std::unique_ptr<FSparseSdfGI>               SparseSdfGI;
    std::unique_ptr<FPathTracing>               PathTracing;
    std::unique_ptr<FHzb>                       Hzb;
    std::unique_ptr<FAutoExposure>              AutoExposure;
    std::unique_ptr<FCas>                       Cas;
    std::unique_ptr<FTaa>                       Taa;
    std::unique_ptr<FTonemap>                   Tonemap;
    std::unique_ptr<FDeferredResourceImporter>  ResourceImporter;

    // Pipeline state
    Microsoft::WRL::ComPtr<ID3D12RootSignature>                  LightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>                  LinearDepthRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>                  LinearDepthPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>                  ExtractHalfDepthNormalRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>                  ExtractHalfDepthNormalPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4>   DirectLightingPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 6>   CompositeLightingPipelines;

    // Scene textures and bindless resources
    FBindlessTexture SceneTexture;
    FBindlessTexture LightingBuffer;
    FBindlessTexture VelocityTexture;
    FBindlessTexture LinearDepthTexture;
    FBindlessTexture BlueNoiseSobolTexture;
    FBindlessTexture BlueNoiseScramblingRanking1SPPTexture;
    FBindlessTexture TonemapOutput;
    FBindlessTexture GBufferA;
    FBindlessTexture GBufferB;
    FBindlessTexture GBufferC;
    FBindlessTexture GBufferD;

    // Descriptor heaps and RTV handles
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> LinearDepthRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> VelocityRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE GBufferRTVHandles[kDeferredGBufferCount]{};
    D3D12_CPU_DESCRIPTOR_HANDLE LightingRTVHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE VelocityRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE LinearDepthRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE TonemapOutputRtvHandle{};
    DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_UNKNOWN;

    // Bindless indices
    uint32_t DirectLightingBindlessIndex = UINT32_MAX;
    ID3D12Resource* DirectLightingResource = nullptr;

    // ClusterDAG constant buffers
    std::vector<FBindlessBuffer>                        ClusterDagSceneConstantBuffers;
    std::vector<uint8_t*>                               ClusterDagSceneConstantBufferMapped;
    uint32_t                                            ClusterDagSceneConstantsPreparedFrame = UINT32_MAX;

    // Scene and animation data
    std::vector<FGltfScene> GltfScenes;
    DirectX::XMFLOAT4X4 SceneWorldMatrix{};

    // Camera and view matrices
    DirectX::XMFLOAT3   PreviousCameraPosition{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4X4 PreviousCameraViewMatrix{};
    DirectX::XMFLOAT4X4 PreviousUnjitteredViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 CurrentUnjitteredViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 LightViewProjection{};

    // Per-frame flags
    bool bHasPreviousViewProjection = false;
    bool bHasPreviousUnjitteredViewProjection = false;
    bool bFirstFrame = true;

};
