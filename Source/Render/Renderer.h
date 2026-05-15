#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "GpuDrivenCulling.h"
#include "GpuDebug.h"
#include "GpuResource.h"
#include "ObjectId.h"
#include "RayTracingRuntime.h"
#include "RendererUtils.h"
#include "../Core/RendererConfig.h"

struct FSceneModelResource;
class FEnvironmentMap;
class FHzb;
class FTextureLoader;
struct FRendererConfig;

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FRenderer
{
public:
    // Type Aliases
    using ECullingMode = FGpuDrivenCulling::ECullingMode;
    using FGpuDebugPrintEntry = GpuDebug::FGpuDebugPrintEntry;
    static constexpr uint32_t GpuDebugPrintMaxEntries = GpuDebug::GpuDebugPrintMaxEntries;
    static constexpr uint32_t GpuDebugPrintHeaderSize = GpuDebug::GpuDebugPrintHeaderSize;
    static constexpr uint32_t GpuDebugPrintEntryStride = GpuDebug::GpuDebugPrintEntryStride;
    static constexpr uint64_t GpuDebugPrintBufferSize = GpuDebug::GpuDebugPrintBufferSize;
    static constexpr uint32_t GpuDebugPrintStatsCount = GpuDebug::GpuDebugPrintStatsCount;

    using FGpuDebugLineEntry = GpuDebug::FGpuDebugLineEntry;
    static constexpr uint32_t GpuDebugLineMaxEntries = GpuDebug::GpuDebugLineMaxEntries;
    static constexpr uint32_t GpuDebugLineHeaderCount = GpuDebug::GpuDebugLineHeaderCount;
    static constexpr uint32_t GpuDebugLineHeaderSize = GpuDebug::GpuDebugLineHeaderSize;
    static constexpr uint32_t GpuDebugLineEntryStride = GpuDebug::GpuDebugLineEntryStride;
    static constexpr uint64_t GpuDebugLineBufferSize = GpuDebug::GpuDebugLineBufferSize;

    using FGpuDebugBoxEntry = GpuDebug::FGpuDebugBoxEntry;
    static constexpr uint32_t GpuDebugBoxMaxEntries = GpuDebug::GpuDebugBoxMaxEntries;
    static constexpr uint32_t GpuDebugBoxHeaderCount = GpuDebug::GpuDebugBoxHeaderCount;
    static constexpr uint32_t GpuDebugBoxHeaderSize = GpuDebug::GpuDebugBoxHeaderSize;
    static constexpr uint32_t GpuDebugBoxEntryStride = GpuDebug::GpuDebugBoxEntryStride;
    static constexpr uint64_t GpuDebugBoxBufferSize = GpuDebug::GpuDebugBoxBufferSize;

    using FMeshletVisibilityFrameData = FGpuDrivenCulling::FMeshletVisibilityFrameData;
    using FVisibilityListBuildIndices = FGpuDrivenCulling::FVisibilityListBuildIndices;
    using FEarlyRejectListIndices = FGpuDrivenCulling::FEarlyRejectListIndices;
    using FLateMergeVisibilityListIndices = FGpuDrivenCulling::FLateMergeVisibilityListIndices;
    using FVisibilityListFrameSrvIndices = FGpuDrivenCulling::FVisibilityListFrameSrvIndices;
    using FMappedConstantBuffer = FMappedUploadBuffer;

    struct FIndirectDrawRange
    {
        uint32_t Start = 0;
        uint32_t Count = 0;
        uint32_t PipelineKey = 0;
        std::array<uint32_t, 10> MaterialBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX } };
        std::wstring Name;

        static FIndirectDrawRange Make(uint32_t InStart, uint32_t InPipelineKey,
                                       const std::array<uint32_t, 10>& InMaterialIndices,
                                       const std::string& InName = {})
        {
            FIndirectDrawRange Range;
            Range.Start = InStart;
            Range.PipelineKey = InPipelineKey;
            Range.MaterialBindlessIndices = InMaterialIndices;
            Range.Name.assign(InName.begin(), InName.end());
            return Range;
        }
    };

    struct FGpuDrivenCullingProvider
    {
        ID3D12DescriptorHeap* BindlessDescriptorHeap = nullptr;
        ID3D12DescriptorHeap* BindlessCpuClearDescriptorHeap = nullptr;
        UINT BindlessDescriptorStride = 0;
        ID3D12RootSignature* CullingRootSignature = nullptr;
        ID3D12RootSignature* MeshletRunRootSignature = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress = 0;
        uint8_t* CullingConstantBufferMapped = nullptr;
        bool bClusterDagDebugEnabled = false;
        bool bClusterDagFastShaderEnabled = false;
        bool bClusterDagGpuDebugEnabled = false;
        float ClusterDagTargetErrorPixels = 0.0f;
        float ClusterDagSwRasterThresholdPixels = 16.0f;
        float ViewportHeightPixels = 0.0f;
        uint32_t ClusterDagVisibleRootCount = 0;
        uint32_t ClusterDagClusterCount = 0;
        bool bClusterDagForceMipEnabled = false;
        uint32_t ClusterDagForceMipLevel = 0;
        bool bClusterDagForceMipSkipFrustumCull = false;
        uint32_t GpuDebugPrintStatsUavBindlessIndex = UINT32_MAX;
        uint32_t GpuDebugLineBufferUavBindlessIndex = UINT32_MAX;

        D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessGpuHandle(uint32_t Index) const
        {
            D3D12_GPU_DESCRIPTOR_HANDLE Handle{};
            if (!BindlessDescriptorHeap || BindlessDescriptorStride == 0)
            {
                return Handle;
            }

            Handle = BindlessDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
            Handle.ptr += static_cast<UINT64>(Index) * BindlessDescriptorStride;
            return Handle;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE GetBindlessCpuClearHandle(uint32_t Index) const
        {
            D3D12_CPU_DESCRIPTOR_HANDLE Handle{};
            if (!BindlessCpuClearDescriptorHeap || BindlessDescriptorStride == 0)
            {
                return Handle;
            }

            Handle = BindlessCpuClearDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
            Handle.ptr += static_cast<SIZE_T>(Index) * BindlessDescriptorStride;
            return Handle;
        }
    };

    // Lifecycle
    virtual ~FRenderer();
    virtual bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config) = 0;
    virtual void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) = 0;
    virtual void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue) {}

    // Configuration
    virtual void ApplyGtaoConfig(const FRendererConfig& Config) {}
    virtual void ApplyPathTracingConfig(const FRendererConfig& Config);

    // Frame State
    void SetFrameIndex(uint32_t FrameIndex);
    uint32_t GetFrameIndex() const { return CurrentFrameIndex; }
    void SetFrameNumber(uint64_t InFrameNumber) { CurrentFrameNumber = InFrameNumber; }
    uint64_t GetFrameNumber() const { return CurrentFrameNumber; }
    uint32_t GetFramesInFlight() const { return FramesInFlight; }

    // Depth Prepass
    virtual void SetDepthPrepassEnabled(bool bEnabled) { bDepthPrepassEnabled = bEnabled; }
    virtual bool IsDepthPrepassEnabled() const { return bDepthPrepassEnabled; }

    // Shadows
    void SetShadowsEnabled(bool bEnabled) { bShadowsEnabled = bEnabled; }
    bool IsShadowsEnabled() const { return bShadowsEnabled; }
    void SetShadowBias(float Bias) { ShadowBias = Bias; }
    float GetShadowBias() const { return ShadowBias; }
    virtual void SetRayTracedShadowsEnabled(bool bEnabled) { bRayTracedShadowsEnabled = bEnabled; }
    virtual bool IsRayTracedShadowsEnabled() const { return bRayTracedShadowsEnabled; }

    // Lighting
    void SetLightDirection(const DirectX::XMFLOAT3& Direction) { LightDirection = Direction; }
    DirectX::XMFLOAT3 GetLightDirection() const { return LightDirection; }
    void SetLightIntensity(float Intensity) { LightIntensity = Intensity; }
    float GetLightIntensity() const { return LightIntensity; }
    void SetLightColor(const DirectX::XMFLOAT3& Color) { LightColor = Color; }
    DirectX::XMFLOAT3 GetLightColor() const { return LightColor; }

    // Scene Bounds
    DirectX::XMFLOAT3 GetSceneCenter() const { return SceneCenter; }
    float GetSceneRadius() const { return SceneRadius; }

    // Scene Models
    virtual const std::vector<FSceneModelResource>* GetSceneModels() const { return &SceneModels; }
    std::vector<FSceneModelResource>& GetSceneModelsMutable() { return SceneModels; }
    virtual bool GetSceneModelStats(size_t& OutTotal, size_t& OutCulled) const;

    // Object ID
    virtual void RequestObjectIdReadback(uint32_t X, uint32_t Y) { ObjectId->RequestReadback(X, Y); }
    virtual bool ConsumeObjectIdReadback(uint32_t& OutObjectId) { return ObjectId->ConsumeReadback(OutObjectId); }

    // Camera
    void SetCullingCameraOverride(const FCamera* Camera) { CullingCameraOverride = Camera; }
    const FCamera* GetCullingCameraOverride() const { return CullingCameraOverride; }

    // Indirect Draw
    void SetIndirectDrawEnabled(bool bEnabled) { bEnableIndirectDraw = bEnabled; }
    bool IsIndirectDrawEnabled() const { return bEnableIndirectDraw; }
    void SetSkinningIndirectDrawEnabled(bool bEnabled) { bEnableSkinningIndirectDraw = bEnabled; }
    bool IsSkinningIndirectDrawEnabled() const { return bEnableSkinningIndirectDraw; }
    bool CanDispatchGpuCulling() const { return bEnableIndirectDraw && bGpuDrivenCullingPersistentInputsValid; }
    uint32_t GetIndirectCommandCount() const { return IndirectCommandCount; }

    // Cluster DAG
    virtual bool IsClusterDagEnabled() const { return false; }
    virtual bool IsClusterDagFastShaderEnabled() const { return false; }
    virtual bool IsClusterDagDebugEnabled() const { return false; }
    virtual EClusterDAGTraversalMode GetClusterDagTraversalMode() const { return EClusterDAGTraversalMode::LevelSplitQueue; }
    virtual float GetClusterDagTargetErrorPixels() const { return 1.0f; }
    virtual float GetClusterDagSwRasterThresholdPixels() const { return 16.0f; }
    virtual bool IsClusterDagForceMipEnabled() const { return false; }
    virtual uint32_t GetClusterDagForceMipLevel() const { return 0; }
    virtual bool IsClusterDagForceMipSkipFrustumCull() const { return false; }
    virtual uint32_t GetClusterDagVisibleRootCount() const { return 0; }
    virtual uint32_t GetClusterDagClusterCount() const { return 0; }

    // Path Tracing
    virtual bool IsPathTracingPreferred() const { return false; }
    virtual bool IsPathTracingVndfEnabled() const { return false; }
    virtual void ForceDisablePathTracing() {}

    // GPU Debug
    FGpuDebug& GetGpuDebugState() { return GpuDebugState; }
    const FGpuDebug& GetGpuDebugState() const { return GpuDebugState; }

    // GPU Driven Culling
    FGpuDrivenCulling& GetGpuDrivenCullingState() { return GpuDrivenCullingState; }
    const FGpuDrivenCulling& GetGpuDrivenCullingState() const { return GpuDrivenCullingState; }
    FGpuDrivenCullingProvider GetGpuDrivenCullingProvider(bool bLatePass = false) const;

    // Scene Resources
    const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const;
    ID3D12Resource* GetDepthBuffer() const;
    D3D12_RESOURCE_STATES& GetDepthBufferState();
    uint32_t GetCurrentDepthSrvBindlessIndex() const;
    DXGI_FORMAT GetDepthTypelessFormat() const;
    ID3D12Resource* GetSceneConstantBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetSceneConstantBufferAddress() const;
    uint8_t* GetSceneConstantBufferMapped() const;
    uint64_t GetSceneConstantBufferStride() const { return SceneConstantBufferStride; }
    ID3D12Resource* GetIndirectCommandBuffer() const;
    D3D12_RESOURCE_STATES& GetIndirectCommandState();
    ID3D12Resource* GetMeshletRunCountBuffer() const;
    D3D12_RESOURCE_STATES& GetMeshletRunCountState();
    ID3D12Resource* GetEnvironmentCubeTexture() const;
    ID3D12Resource* GetBrdfLutTexture() const;
    uint32_t GetEnvironmentCubeSrvIndex() const;
    uint32_t GetBrdfLutSrvIndex() const;
    float GetEnvironmentMipCount() const;
    const FRayTracingRuntime& GetRayTracingRuntime() const;
    FRayTracingRuntime& GetRayTracingRuntime();

    // Device
    FDX12Device* GetDevice() const { return Device; }

    // Fatal Error
    void SetRenderFatalError(const std::string& Reason);
    bool HasRenderFatalError() const;

protected:
    // Friends
    friend class FEnvironmentMap;
    friend class FGpuDebug;
    friend class FGpuDrivenCulling;
    friend class FHzb;
    friend class FObjectId;
    friend class FRayTracingRuntime;

    // Initialization
    void InitializeCommonSettings(uint32_t Width, uint32_t Height, const FRendererConfig& Config);

    // Shadow Pipeline
    bool CreateShadowPipeline(
        FDX12Device* Device,
        ID3D12RootSignature* RootSignature,
        const std::vector<std::wstring>& Defines,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState,
        bool bDoubleSided = false);
    bool CreateShadowResources(
        FDX12Device* Device,
        uint32_t& InOutWidth,
        uint32_t& InOutHeight,
        FBindlessTexture& OutShadowMap,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& OutShadowDsvHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE& OutShadowDsvHandle);

    // GPU Driven Culling
    void DispatchGpuCulling(
        FDX12CommandContext& CmdContext,
        const FCamera& Camera,
        const char* PassName,
        ECullingMode Mode,
        uint32_t VisibilityInputIndex,
        uint32_t VisibilityInputFrameIndex,
        uint32_t CullingListIndex,
        uint32_t CullingListCountIndex,
        bool bUseLateVisibility);
    void ConfigureHZBOcclusion(bool bEnabled, uint32_t HZBBindlessIndex, uint32_t Width, uint32_t Height, uint32_t MipCount);
    void RefreshGpuDrivenPersistentValidation();
    D3D12_RESOURCE_STATES* GetMeshletVisibilityState(uint32_t FrameIndex, bool bLatePass = false) { return GpuDrivenCullingState.GetMeshletVisibilityState(FrameIndex, bLatePass); }

    // Skinning
    void DispatchSkinning(FDX12CommandContext& CmdContext, const DirectX::XMMATRIX& LightViewProjection);
    bool CreateSkinnedPositionBuffers();
    bool CreateSkinningPipeline(FDX12Device* Device);

    // Descriptor Handles
    D3D12_CPU_DESCRIPTOR_HANDLE GetBindlessCpuHandle(uint32_t Index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetBindlessCpuClearHandle(uint32_t Index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessGpuHandle(uint32_t Index) const;

    // Per-Frame Resource Creation
    bool CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format, FDepthStencilTarget& OutDepthResources);
    bool CreateDepthResourcesPerFrame(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format);
    bool CreateSceneConstantBuffersPerFrame(FDX12Device* Device, uint64_t BufferSize);
    bool CreateCullingConstantBuffersPerFrame(FDX12Device* Device);

    // GPU Driven Data Preparation
    // Number of shared buffers transitioned during GPU-driven resource upload
    // (DrawData, RangeOffset, Bounds, ConeAxis, ConeApex, DebugPrint, DebugPrintStats, DebugLine, DebugBox)
    static constexpr uint32_t GpuDrivenSharedBufferCount = 9;

    // Container for GPU-driven rendering data prepared for upload to GPU.
    // Holds indirect draw commands, meshlet data, bounds, and culling cone information.
    struct FGpuDrivenPreparedData
    {
        std::vector<FIndirectDrawCommand> Commands;
        std::vector<FMeshletDrawData> MeshletDrawData;
        std::vector<DirectX::XMFLOAT4> Bounds;
        std::vector<DirectX::XMFLOAT4> ConeAxisCutoff;
        std::vector<DirectX::XMFLOAT4> ConeApex;
        std::vector<uint32_t> RangeOffsets;

        void Reserve(size_t Count)
        {
            Commands.clear();       Commands.reserve(Count);
            MeshletDrawData.clear(); MeshletDrawData.reserve(Count);
            Bounds.clear();          Bounds.reserve(Count);
            ConeAxisCutoff.clear();  ConeAxisCutoff.reserve(Count);
            ConeApex.clear();        ConeApex.reserve(Count);
        }

        void PushDraw(const FIndirectDrawCommand& Cmd,
                      const FMeshletDrawData& Draw,
                      DirectX::XMFLOAT4 BoundingSphere,
                      DirectX::XMFLOAT4 Cone,
                      DirectX::XMFLOAT4 Apex)
        {
            Commands.push_back(Cmd);
            MeshletDrawData.push_back(Draw);
            Bounds.push_back(BoundingSphere);
            ConeAxisCutoff.push_back(Cone);
            ConeApex.push_back(Apex);
        }
    };

    bool PrepareGpuDrivenDrawData(FGpuDrivenPreparedData& OutData);
    bool CreatePerFrameIndirectBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data);
    bool CreateSharedGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data);
    bool UploadGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data);
    std::vector<FUploadBuffer> PrepareIndirectCommandUploads(FDX12Device* Device, const std::vector<FIndirectDrawCommand>& Commands);
    bool CreateCullingPipelines(FDX12Device* Device);
    bool CreateIndirectCommandSignature(FDX12Device* Device, ID3D12RootSignature* RootSignature);

    // Depth Resources
    std::vector<FDepthStencilTarget> DepthResourcesPerFrame;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
    DXGI_FORMAT SceneDepthFormat = DXGI_FORMAT_UNKNOWN;

    // Viewport
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    D3D12_VIEWPORT ShadowViewport{};
    D3D12_RECT ShadowScissor{};

    // Scene Constants
    std::vector<FMappedConstantBuffer> SceneConstantBuffers;
    uint64_t SceneConstantBufferStride = 0;

    // Scene Bounds
    DirectX::XMFLOAT3 SceneCenter{ 0.0f, 0.0f, 0.0f };
    float SceneRadius = 1.0f;

    // Lighting
    DirectX::XMFLOAT3 LightDirection{ -0.5f, -1.0f, 0.2f };
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };

    // Scene Models
    std::vector<FSceneModelResource> SceneModels;
    std::vector<bool> SceneModelVisibility;
    std::vector<bool> SceneModelSkinningVisibility;

    // Shadows
    FBindlessTexture ShadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShadowDSVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDSVHandle{};
    float ShadowBias = 0.0f;
    float ShadowStrength = 1.0f;
    uint32_t ShadowMapWidth = 0;
    uint32_t ShadowMapHeight = 0;
    bool bShadowsEnabled = true;
    bool bRayTracedShadowsEnabled = false;

    // Ray Tracing
    std::unique_ptr<FRayTracingRuntime> RayTracingRuntime;
    uint32_t ShadowMaskBindlessIndex = UINT32_MAX;
    ID3D12Resource* ShadowMaskResource = nullptr;

    // Environment
    std::unique_ptr<FEnvironmentMap> EnvironmentMap;
    Microsoft::WRL::ComPtr<ID3D12Resource> NullTexture;

    // Object ID / Texture Loading
    std::unique_ptr<FObjectId> ObjectId{ std::make_unique<FObjectId>() };
    std::unique_ptr<FTextureLoader> TextureLoader;

    // Indirect Draw
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> IndirectCommandSignature;
    uint32_t IndirectCommandCount = 0;
    std::vector<FIndirectDrawRange> IndirectDrawRanges;
    bool bEnableIndirectDraw = false;

    // Skinning
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkinningRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkinningPipeline;
    bool bEnableSkinningIndirectDraw = false;

    // GPU Driven Culling
    FGpuDrivenCulling GpuDrivenCullingState;
    bool bGpuDrivenCullingPersistentInputsValid = false;

    // GPU Debug
    FGpuDebug GpuDebugState;

    // Render State
    bool bDepthPrepassEnabled = false;
    const FCamera* CullingCameraOverride = nullptr;

    // Debug / Timing
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;

    // Fatal Error
    bool bRenderFatalError = false;
    std::string RenderFatalReason;

    // Device / Frame State
    FDX12Device* Device = nullptr;
    uint32_t FramesInFlight = 1;
    uint32_t CurrentFrameIndex = 0;
    uint64_t CurrentFrameNumber = 0;
};
