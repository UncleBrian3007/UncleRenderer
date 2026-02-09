#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RendererUtils.h"
#include "../RHI/RayTracing.h"

struct FSceneModelResource;
class FTextureLoader;
struct FRendererConfig;

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FRenderer
{
public:
    enum class ECullingMode : uint32_t
    {
        All = 0,
        EarlyVisible = 1,
        LateAfterEarly = 2
    };
    struct FGpuDebugPrintEntry
    {
        uint32_t X = 0;
        uint32_t Y = 0;
        uint32_t Code = 0;
        uint32_t Color = 0;
    };

    static constexpr uint32_t GpuDebugPrintMaxEntries = 4096;
    static constexpr uint32_t GpuDebugPrintHeaderSize = sizeof(uint32_t);
    static constexpr uint32_t GpuDebugPrintEntryStride = sizeof(FGpuDebugPrintEntry);
    static constexpr uint64_t GpuDebugPrintBufferSize = GpuDebugPrintHeaderSize + static_cast<uint64_t>(GpuDebugPrintMaxEntries) * GpuDebugPrintEntryStride;
    static constexpr uint32_t GpuDebugPrintStatsCount = 5;

    virtual ~FRenderer();

    virtual bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config) = 0;
    virtual void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) = 0;
    virtual void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue) {}

    virtual void SetDepthPrepassEnabled(bool bEnabled) { bDepthPrepassEnabled = bEnabled; }
    virtual bool IsDepthPrepassEnabled() const { return bDepthPrepassEnabled; }

    virtual void SetRayTracedShadowsEnabled(bool bEnabled) { bRayTracedShadowsEnabled = bEnabled; }
    virtual bool IsRayTracedShadowsEnabled() const { return bRayTracedShadowsEnabled; }
    virtual void SetPathTracingEnabled(bool bEnabled) { bPathTracingEnabled = bEnabled; }
    virtual bool IsPathTracingEnabled() const { return bPathTracingEnabled; }
    virtual void SetPathTracingAccumulationEnabled(bool bEnabled) { }
    virtual void SetPathTracingVndfEnabled(bool bEnabled) { bPathTracingUseVndf = bEnabled; }
    bool IsPathTracingVndfEnabled() const { return bPathTracingUseVndf; }

    void SetFrameIndex(uint32_t FrameIndex);
    uint32_t GetFrameIndex() const { return CurrentFrameIndex; }

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const;
    ID3D12Resource* GetDepthBuffer() const;
    D3D12_RESOURCE_STATES& GetDepthBufferState();
    ID3D12Resource* GetSceneConstantBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetSceneConstantBufferAddress() const;
    uint8_t* GetSceneConstantBufferMapped() const;
    ID3D12Resource* GetCullingConstantBuffer(bool bLatePass = false) const;
    D3D12_GPU_VIRTUAL_ADDRESS GetCullingConstantBufferAddress(bool bLatePass = false) const;
    uint8_t* GetCullingConstantBufferMapped(bool bLatePass = false) const;
    ID3D12Resource* GetIndirectCommandBuffer() const;
    D3D12_RESOURCE_STATES& GetIndirectCommandState();
    ID3D12Resource* GetMeshletRunCountBuffer() const;
    D3D12_RESOURCE_STATES& GetMeshletRunCountState();
    uint32_t GetFramesInFlight() const { return FramesInFlight; }

    DirectX::XMFLOAT3 GetSceneCenter() const { return SceneCenter; }
    float GetSceneRadius() const { return SceneRadius; }

    void SetLightDirection(const DirectX::XMFLOAT3& Direction) { LightDirection = Direction; }
    DirectX::XMFLOAT3 GetLightDirection() const { return LightDirection; }

    void SetLightIntensity(float Intensity) { LightIntensity = Intensity; }
    float GetLightIntensity() const { return LightIntensity; }

    void SetLightColor(const DirectX::XMFLOAT3& Color) { LightColor = Color; }
    DirectX::XMFLOAT3 GetLightColor() const { return LightColor; }

    void SetCullingCameraOverride(const FCamera* Camera) { CullingCameraOverride = Camera; }
    const FCamera* GetCullingCameraOverride() const { return CullingCameraOverride; }
    void SetIndirectDrawEnabled(bool bEnabled) { bEnableIndirectDraw = bEnabled; }
    bool IsIndirectDrawEnabled() const { return bEnableIndirectDraw; }
    void SetSkinningIndirectDrawEnabled(bool bEnabled) { bEnableSkinningIndirectDraw = bEnabled; }
    bool IsSkinningIndirectDrawEnabled() const { return bEnableSkinningIndirectDraw; }
    void SetGtaoRadius(float Radius) { GtaoRadius = Radius; }
    float GetGtaoRadius() const { return GtaoRadius; }
    void SetGtaoThickness(float Thickness) { GtaoThickness = Thickness; }
    float GetGtaoThickness() const { return GtaoThickness; }
    void SetGtaoJitterEnabled(bool bEnabled) { bGtaoJitterEnabled = bEnabled; }
    bool IsGtaoJitterEnabled() const { return bGtaoJitterEnabled; }
    virtual const std::vector<FSceneModelResource>* GetSceneModels() const { return &SceneModels; }
    virtual bool GetSceneModelStats(size_t& OutTotal, size_t& OutCulled) const;
    virtual void RequestObjectIdReadback(uint32_t X, uint32_t Y);
    virtual bool ConsumeObjectIdReadback(uint32_t& OutObjectId);

protected:
    void InitializeCommonSettings(uint32_t Width, uint32_t Height, const FRendererConfig& Config);
    bool CreateShadowPipeline(
        FDX12Device* Device,
        ID3D12RootSignature* RootSignature,
        const std::vector<std::wstring>& Defines,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState);
    bool CreateShadowResources(
        FDX12Device* Device,
        uint32_t& InOutWidth,
        uint32_t& InOutHeight,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutShadowMap,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& OutShadowDsvHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE& OutShadowDsvHandle,
        D3D12_RESOURCE_STATES& OutShadowState);
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
    void DispatchBuildVisibilityLists(
        FDX12CommandContext& CmdContext,
        uint32_t VisibilityIndex,
        uint32_t VisibleListIndex,
        uint32_t InvisibleListIndex,
        uint32_t VisibleCountIndex,
        uint32_t InvisibleCountIndex,
        uint32_t VisibilityFrameIndex,
        uint32_t FrameIndex);
    void DispatchBuildEarlyRejectList(
        FDX12CommandContext& CmdContext,
        uint32_t VisibilityIndex,
        uint32_t RejectListIndex,
        uint32_t RejectCountIndex,
        uint32_t FrameIndex);
    void DispatchMergeVisibilityLists(
        FDX12CommandContext& CmdContext,
        uint32_t ListAIndex,
        uint32_t ListBIndex,
        uint32_t CountAIndex,
        uint32_t CountBIndex,
        uint32_t OutputListIndex,
        uint32_t OutputCountIndex,
        uint32_t FlagsIndex,
        uint32_t FrameIndex);
    void ConfigureHZBOcclusion(bool bEnabled, uint32_t HZBBindlessIndex, uint32_t Width, uint32_t Height, uint32_t MipCount);
    void PrepareGpuDebugPrint(FDX12CommandContext& CmdContext);
    void DispatchGpuDebugPrintStats(FDX12CommandContext& CmdContext);
    void DispatchSkinning(FDX12CommandContext& CmdContext, const DirectX::XMMATRIX& LightViewProjection);
    bool CreateSkinnedPositionBuffers();
    void UpdateRayTracingBlasRefit(FDX12CommandContext& CmdContext);
    void BuildRayTracingTlas(FDX12CommandContext& CmdContext);
    bool CreateRayTracingPipeline(FDX12Device* Device);
    bool CreateSkinningPipeline(FDX12Device* Device);
    D3D12_CPU_DESCRIPTOR_HANDLE GetBindlessCpuHandle(uint32_t Index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetBindlessCpuClearHandle(uint32_t Index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessGpuHandle(uint32_t Index) const;
    void WriteBindlessSrv(uint32_t Index, ID3D12Resource* Resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const;
    void WriteBindlessUav(uint32_t Index, ID3D12Resource* Resource, ID3D12Resource* Counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc) const;
    bool CreateGpuDebugPrintResources(FDX12Device* Device);
    bool CreateGpuDebugPrintPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateGpuDebugPrintStatsPipeline(FDX12Device* Device);
    void RenderGpuDebugPrint(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle);
    bool CreateDepthResourcesPerFrame(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format);
    bool CreateSceneConstantBuffersPerFrame(FDX12Device* Device, uint64_t BufferSize);
    bool CreateCullingConstantBuffersPerFrame(FDX12Device* Device);
    bool CreateVisibilityListPipelines(FDX12Device* Device);

    // GPU-driven rendering helper methods

    // Number of shared buffers transitioned during GPU-driven resource upload
    // (DrawData, RangeOffset, Bounds, ConeAxis, ConeApex, Debug, Stats)
    static constexpr uint32_t GpuDrivenSharedBufferCount = 7;

    // Container for GPU-driven rendering data prepared for upload to GPU.
    // Holds indirect draw commands, meshlet data, bounds, and culling cone information.
    struct FGpuDrivenPreparedData
    {
        std::vector<FIndirectDrawCommand> Commands;     // Indirect draw commands for ExecuteIndirect
        std::vector<FMeshletDrawData> MeshletDrawData;  // Per-meshlet draw metadata
        std::vector<DirectX::XMFLOAT4> Bounds;          // Bounding sphere (xyz=center, w=radius)
        std::vector<DirectX::XMFLOAT4> ConeAxisCutoff;  // Cone axis and cutoff for backface culling
        std::vector<DirectX::XMFLOAT4> ConeApex;        // Cone apex for backface culling
        std::vector<uint32_t> RangeOffsets;             // Starting offset for each draw range
    };

    bool PrepareGpuDrivenDrawData(FGpuDrivenPreparedData& OutData);
    bool CreatePerFrameIndirectBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data);
    bool CreateSharedGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data);
    bool UploadGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data);
    bool CreateCullingPipelines(FDX12Device* Device);
    bool CreateIndirectCommandSignature(FDX12Device* Device, ID3D12RootSignature* RootSignature);

    std::vector<FDepthResources> DepthResourcesPerFrame;
    std::vector<D3D12_RESOURCE_STATES> DepthBufferStates;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SceneConstantBuffers;
    std::vector<uint8_t*> SceneConstantBufferMapped;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> CullingConstantBuffers;
    std::vector<uint8_t*> CullingConstantBufferMapped;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> CullingConstantBuffersLate;
    std::vector<uint8_t*> CullingConstantBufferMappedLate;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDSVHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE ObjectIdRtvHandle{};
    DirectX::XMFLOAT3 SceneCenter{ 0.0f, 0.0f, 0.0f };
    float SceneRadius = 1.0f;

    DirectX::XMFLOAT3 LightDirection{ -0.5f, -1.0f, 0.2f };
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };

    bool bDepthPrepassEnabled = false;
    const FCamera* CullingCameraOverride = nullptr;

    std::vector<FSceneModelResource> SceneModels;
    std::vector<bool> SceneModelVisibility;
    std::vector<bool> SceneModelSkinningVisibility;
    struct FIndirectDrawRange
    {
        uint32_t Start = 0;
        uint32_t Count = 0;
        uint32_t PipelineKey = 0;
        std::array<uint32_t, 10> MaterialBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX } };
        std::wstring Name;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> SkyConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> ShadowMap;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> IndirectCommandBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> IndirectCommandTemplateBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletDrawDataBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletDrawDataUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletRangeOffsetBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletRangeOffsetUpload;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> MeshletVisibilityBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> MeshletVisibilityLateBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> PrevVisibleListBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> PrevInvisibleListBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> EarlyRejectListBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> LateListBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> LateListFlagBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> PrevVisibleCountBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> PrevInvisibleCountBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> EarlyRejectCountBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> LateListCountBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> MeshletRunCountBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> ModelBoundsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> ModelBoundsUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletConeAxisBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletConeAxisUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletConeApexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> MeshletConeApexUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintStatsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintStatsUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> ObjectIdTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> ObjectIdReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource> NullTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> EnvironmentCubeTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> BrdfLutTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShadowDSVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ObjectIdRtvHeap;
    std::unique_ptr<FTextureLoader> TextureLoader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CullingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> VisibilityListRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CullingPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CullingListPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildVisibilityListsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildEarlyRejectListPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> MergeVisibilityListsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ClearVisibilityCountsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ClearVisibilityFlagsPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> MeshletRunRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> MeshletRunClearPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> MeshletRunAppendPipeline;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> IndirectCommandSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkinningRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkinningPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GpuDebugPrintRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GpuDebugPrintPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GpuDebugPrintStatsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GpuDebugPrintStatsPipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintFontTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintGlyphBuffer;
    uint32_t GpuDebugPrintGlyphBindlessIndex = UINT32_MAX;
    uint32_t GpuDebugPrintFontBindlessIndex = UINT32_MAX;
    uint32_t GpuDebugPrintBufferBindlessIndex = UINT32_MAX;
    uint32_t GpuDebugPrintStatsBindlessIndex = UINT32_MAX;
    uint32_t GpuDebugPrintBufferUavBindlessIndex = UINT32_MAX;
    uint32_t GpuDebugPrintStatsUavBindlessIndex = UINT32_MAX;
    uint32_t ModelBoundsBindlessIndex = UINT32_MAX;
    uint32_t MeshletDrawDataBindlessIndex = UINT32_MAX;
    uint32_t MeshletRangeOffsetBindlessIndex = UINT32_MAX;
    uint32_t MeshletConeAxisBindlessIndex = UINT32_MAX;
    uint32_t MeshletConeApexBindlessIndex = UINT32_MAX;
    uint32_t HZBCullingBindlessIndex = UINT32_MAX;
    std::vector<uint32_t> IndirectCommandUavBindlessIndices;
    std::vector<uint32_t> IndirectCommandTemplateBindlessIndices;
    std::vector<uint32_t> MeshletVisibilitySrvBindlessIndices;
    std::vector<uint32_t> MeshletVisibilityUavBindlessIndices;
    std::vector<uint32_t> MeshletVisibilityLateSrvBindlessIndices;
    std::vector<uint32_t> MeshletVisibilityLateUavBindlessIndices;
    std::vector<uint32_t> PrevVisibleListSrvBindlessIndices;
    std::vector<uint32_t> PrevVisibleListUavBindlessIndices;
    std::vector<uint32_t> PrevInvisibleListSrvBindlessIndices;
    std::vector<uint32_t> PrevInvisibleListUavBindlessIndices;
    std::vector<uint32_t> EarlyRejectListSrvBindlessIndices;
    std::vector<uint32_t> EarlyRejectListUavBindlessIndices;
    std::vector<uint32_t> LateListSrvBindlessIndices;
    std::vector<uint32_t> LateListUavBindlessIndices;
    std::vector<uint32_t> LateListFlagSrvBindlessIndices;
    std::vector<uint32_t> LateListFlagUavBindlessIndices;
    std::vector<uint32_t> PrevVisibleCountSrvBindlessIndices;
    std::vector<uint32_t> PrevVisibleCountUavBindlessIndices;
    std::vector<uint32_t> PrevInvisibleCountSrvBindlessIndices;
    std::vector<uint32_t> PrevInvisibleCountUavBindlessIndices;
    std::vector<uint32_t> EarlyRejectCountSrvBindlessIndices;
    std::vector<uint32_t> EarlyRejectCountUavBindlessIndices;
    std::vector<uint32_t> LateListCountSrvBindlessIndices;
    std::vector<uint32_t> LateListCountUavBindlessIndices;
    std::vector<uint32_t> MeshletRunCountUavBindlessIndices;
    uint32_t GpuDebugPrintAtlasWidth = 0;
    uint32_t GpuDebugPrintAtlasHeight = 0;
    uint32_t GpuDebugPrintFirstChar = 32;
    uint32_t GpuDebugPrintCharCount = 96;
    float GpuDebugPrintFontSize = 16.0f;

    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    D3D12_VIEWPORT ShadowViewport{};
    D3D12_RECT ShadowScissor{};

    D3D12_RESOURCE_STATES ShadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES ObjectIdState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::vector<D3D12_RESOURCE_STATES> IndirectCommandStates;
    std::vector<D3D12_RESOURCE_STATES> MeshletVisibilityStates;
    std::vector<D3D12_RESOURCE_STATES> MeshletVisibilityLateStates;
    std::vector<D3D12_RESOURCE_STATES> PrevVisibleListStates;
    std::vector<D3D12_RESOURCE_STATES> PrevInvisibleListStates;
    std::vector<D3D12_RESOURCE_STATES> EarlyRejectListStates;
    std::vector<D3D12_RESOURCE_STATES> LateListStates;
    std::vector<D3D12_RESOURCE_STATES> LateListFlagStates;
    std::vector<D3D12_RESOURCE_STATES> PrevVisibleCountStates;
    std::vector<D3D12_RESOURCE_STATES> PrevInvisibleCountStates;
    std::vector<D3D12_RESOURCE_STATES> EarlyRejectCountStates;
    std::vector<D3D12_RESOURCE_STATES> LateListCountStates;
    std::vector<D3D12_RESOURCE_STATES> MeshletRunCountStates;
    FRayTracingDevice RayTracingDevice;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RayQueryRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQuerySsrFallbackPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQuerySsrHwPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathDebugPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathVndfPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathDebugVndfPipeline;
    bool bRayTracingPipelineReady = false;
    std::vector<uint32_t> RayTracingDepthSrvBindlessIndices;
    std::vector<ID3D12Resource*> RayTracingDepthResources;
    uint32_t RayTracingGBufferASrvBindlessIndex = UINT32_MAX;
    uint32_t RayTracingGBufferBSrvBindlessIndex = UINT32_MAX;
    uint32_t RayTracingGBufferCSrvBindlessIndex = UINT32_MAX;
    uint32_t RayTracingLightingUavBindlessIndex = UINT32_MAX;
    uint32_t RayTracingShadowMaskUavBindlessIndex = UINT32_MAX;
    ID3D12Resource* RayTracingGBufferAResource = nullptr;
    ID3D12Resource* RayTracingGBufferBResource = nullptr;
    ID3D12Resource* RayTracingGBufferCResource = nullptr;
    ID3D12Resource* RayTracingLightingResource = nullptr;
    ID3D12Resource* RayTracingShadowMaskUavResource = nullptr;
    uint32_t ShadowMaskBindlessIndex = UINT32_MAX;
    ID3D12Resource* ShadowMaskResource = nullptr;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> TlasScratchBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> TlasResultBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> TlasInstanceBuffers;
    std::vector<bool> TlasBuilt;
    uint32_t TlasInstanceCapacity = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> PathTracingInstanceDataBuffer;
    uint32_t PathTracingInstanceDataBindlessIndex = UINT32_MAX;
    D3D12_RESOURCE_STATES GpuDebugPrintState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_COMMON;

    uint8_t* SkyConstantBufferMapped = nullptr;
    uint64_t SceneConstantBufferStride = 0;
    float ShadowBias = 0.0f;
    float ShadowStrength = 1.0f;
    uint32_t ShadowMapWidth = 0;
    uint32_t ShadowMapHeight = 0;
    bool bShadowsEnabled = true;
    bool bRayTracedShadowsEnabled = false;
    bool bPathTracingEnabled = false;
    bool bPathTracingUseVndf = true;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bEnableIndirectDraw = false;
    bool bEnableSkinningIndirectDraw = false;
    bool bEnableGpuDebugPrint = false;
    bool bGtaoEnabled = true;
    bool bGtaoJitterEnabled = true;
    float EnvironmentMipCount = 1.0f;
    float GtaoRadius = 0.75f;
    float GtaoIntensity = 1.0f;
    float GtaoPower = 1.5f;
    float GtaoThickness = 0.1f;
    uint32_t GtaoDirectionCount = 6;
    uint32_t GtaoStepCount = 4;
    bool bObjectIdReadbackRequested = false;
    bool bObjectIdReadbackRecorded = false;
    uint32_t ObjectIdReadbackX = 0;
    uint32_t ObjectIdReadbackY = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ObjectIdFootprint{};
    uint32_t ObjectIdRowPitch = 0;
    uint32_t IndirectCommandCount = 0;
    std::vector<FIndirectDrawRange> IndirectDrawRanges;
    uint32_t HZBCullingWidth = 0;
    uint32_t HZBCullingHeight = 0;
    uint32_t HZBCullingMipCount = 0;
    bool bHZBOcclusionEnabled = false;

    FDX12Device* Device = nullptr;
    uint32_t FramesInFlight = 1;
    uint32_t CurrentFrameIndex = 0;
};
