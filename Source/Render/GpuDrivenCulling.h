#pragma once

#include "GpuResource.h"

#include <d3d12.h>
#include <wrl.h>
#include <array>
#include <cstdint>
#include <vector>

class FDX12Device;
class FDX12CommandContext;
class FCamera;
struct FDeferredPassContext;

static constexpr uint32_t kCullingConstantDwords = 60;

struct FGpuCullingConstants
{
    float    FrustumPlanes[6][4];
    float    ViewProjection[4][4];
    uint32_t IndirectCommandCount;
    uint32_t HZBEnabled;
    uint32_t HZBMipCount;
    uint32_t HZBWidth;
    uint32_t HZBHeight;
    uint32_t DebugPrintEnabled;
    uint32_t RangeCount;
    uint32_t CullingMode;
    float    CameraPosition[3];
    uint32_t Padding1;
    float    ClusterDagTargetErrorPixels;
    float    ViewportHeightPixels;
    uint32_t ClusterDagVisibleRootCount;
    uint32_t ClusterDagForceMipEnabled;
    uint32_t ClusterDagForceMipLevel;
    uint32_t ClusterDagForceMipSkipFrustumCull;
    uint32_t ClusterDagForceSoftwareRaster;
    float    ClusterDagSwRasterThresholdPixels;
};
static_assert(sizeof(FGpuCullingConstants) == sizeof(uint32_t) * kCullingConstantDwords);

class FGpuDrivenCulling
{
public:
    enum class ECullingMode : uint32_t
    {
        All = 0,
        EarlyVisible = 1,
        LateAfterEarly = 2
    };

    struct FMeshletVisibilityFrameData
    {
        ID3D12Resource* Buffer = nullptr;
        uint32_t SrvBindlessIndex = UINT32_MAX;
        uint32_t UavBindlessIndex = UINT32_MAX;

        bool IsValid() const
        {
            return Buffer != nullptr
                && IsValidBindlessIndex(SrvBindlessIndex)
                && IsValidBindlessIndex(UavBindlessIndex);
        }
    };

    struct FVisibilityListBuildIndices
    {
        uint32_t PrevVisibleListUav = UINT32_MAX;
        uint32_t PrevInvisibleListUav = UINT32_MAX;
        uint32_t PrevVisibleCountUav = UINT32_MAX;
        uint32_t PrevInvisibleCountUav = UINT32_MAX;

        bool IsValid() const
        {
            return IsValidBindlessIndex(PrevVisibleListUav)
                && IsValidBindlessIndex(PrevInvisibleListUav)
                && IsValidBindlessIndex(PrevVisibleCountUav)
                && IsValidBindlessIndex(PrevInvisibleCountUav);
        }
    };

    struct FEarlyRejectListIndices
    {
        uint32_t RejectListUav = UINT32_MAX;
        uint32_t RejectCountUav = UINT32_MAX;

        bool IsValid() const
        {
            return IsValidBindlessIndex(RejectListUav)
                && IsValidBindlessIndex(RejectCountUav);
        }
    };

    struct FLateMergeVisibilityListIndices
    {
        uint32_t PrevInvisibleListSrv = UINT32_MAX;
        uint32_t EarlyRejectListSrv = UINT32_MAX;
        uint32_t PrevInvisibleCountSrv = UINT32_MAX;
        uint32_t EarlyRejectCountSrv = UINT32_MAX;
        uint32_t LateListUav = UINT32_MAX;
        uint32_t LateListCountUav = UINT32_MAX;
        uint32_t LateListFlagUav = UINT32_MAX;

        bool IsValid() const
        {
            return IsValidBindlessIndex(PrevInvisibleListSrv)
                && IsValidBindlessIndex(EarlyRejectListSrv)
                && IsValidBindlessIndex(PrevInvisibleCountSrv)
                && IsValidBindlessIndex(EarlyRejectCountSrv)
                && IsValidBindlessIndex(LateListUav)
                && IsValidBindlessIndex(LateListCountUav)
                && IsValidBindlessIndex(LateListFlagUav);
        }
    };

    struct FVisibilityListFrameSrvIndices
    {
        uint32_t PrevVisibleListSrv = UINT32_MAX;
        uint32_t PrevVisibleCountSrv = UINT32_MAX;
        uint32_t LateListSrv = UINT32_MAX;
        uint32_t LateListCountSrv = UINT32_MAX;
    };

    struct FGpuCullingDispatchConfig
    {
        ID3D12DescriptorHeap* BindlessDescriptorHeap = nullptr;
        uint32_t IndirectCommandCount = 0;
        uint32_t RangeCount = 0;
        uint32_t Mode = 0;
        bool bGpuDebugPrintEnabled = false;
        float ClusterDagTargetErrorPixels = 0.0f;
        float ClusterDagSwRasterThresholdPixels = 16.0f;
        float ViewportHeightPixels = 0.0f;
        uint32_t ClusterDagVisibleRootCount = 0;
        bool bClusterDagForceMipEnabled = false;
        uint32_t ClusterDagForceMipLevel = 0;
        bool bClusterDagForceMipSkipFrustumCull = false;
    };

    struct FGpuCullingDispatchFrameData
    {
        ID3D12Resource* IndirectBuffer = nullptr;
        ID3D12Resource* RunCountBuffer = nullptr;
        D3D12_RESOURCE_STATES* IndirectState = nullptr;
        D3D12_RESOURCE_STATES* RunCountState = nullptr;
        uint32_t IndirectUavBindlessIndex = UINT32_MAX;
        uint32_t TemplateSrvBindlessIndex = UINT32_MAX;
        uint32_t RunCountUavBindlessIndex = UINT32_MAX;
    };

    struct FGpuCullingDispatchIndices
    {
        uint32_t MeshletDrawDataIndex = UINT32_MAX;
        uint32_t RangeOffsetsIndex = UINT32_MAX;
        uint32_t ModelBoundsIndex = UINT32_MAX;
        uint32_t MeshletConeAxisIndex = UINT32_MAX;
        uint32_t MeshletConeApexIndex = UINT32_MAX;
        uint32_t VisibilityInputIndex = UINT32_MAX;
        uint32_t CullingListIndex = UINT32_MAX;
        uint32_t CullingListCountIndex = UINT32_MAX;
        uint32_t DebugPrintBufferIndex = UINT32_MAX;
        uint32_t DebugPrintStatsIndex = UINT32_MAX;
        bool bUseCullingList = false;
    };

    struct FGpuCullingSharedInputData
    {
        const void* BoundsData = nullptr;
        uint64_t BoundsSizeInBytes = 0;
        uint32_t BoundsElementCount = 0;

        const void* MeshletDrawData = nullptr;
        uint64_t MeshletDrawDataSizeInBytes = 0;
        uint32_t MeshletDrawDataElementCount = 0;

        const void* RangeOffsetsData = nullptr;
        uint64_t RangeOffsetsSizeInBytes = 0;
        uint32_t RangeOffsetsElementCount = 0;

        const void* ConeAxisData = nullptr;
        uint64_t ConeAxisSizeInBytes = 0;
        uint32_t ConeAxisElementCount = 0;

        const void* ConeApexData = nullptr;
        uint64_t ConeApexSizeInBytes = 0;
        uint32_t ConeApexElementCount = 0;
    };

    ~FGpuDrivenCulling() = default;

    bool HasCullingDispatchResources() const { return bCullingDispatchPersistentInputsValid; }
    bool HasSharedInputs() const { return bSharedInputPersistentInputsValid; }
    bool HasMeshletVisibilityInputs() const { return bMeshletVisibilityPersistentInputsValid; }
    bool HasVisibilityListBuildPipeline() const { return bVisibilityListPersistentInputsValid; }
    bool HasEarlyRejectListPipeline() const { return bVisibilityListPersistentInputsValid; }
    bool HasLateMergeVisibilityPipeline() const { return bVisibilityListPersistentInputsValid; }

    ID3D12RootSignature* GetCullingRootSignature() const { return CullingRootSignature.Get(); }
    ID3D12RootSignature* GetMeshletRunRootSignature() const { return MeshletRunRootSignature.Get(); }
    ID3D12Resource* GetCullingConstantBuffer(uint32_t FrameIndex, bool bLatePass = false) const;
    D3D12_GPU_VIRTUAL_ADDRESS GetCullingConstantBufferAddress(uint32_t FrameIndex, bool bLatePass = false) const;
    uint8_t* GetCullingConstantBufferMapped(uint32_t FrameIndex, bool bLatePass = false) const;
    ID3D12Resource* GetIndirectCommandBuffer(uint32_t FrameIndex) const;
    ID3D12Resource* GetIndirectCommandTemplateBuffer(uint32_t FrameIndex) const;
    ID3D12Resource* GetMeshletRunCountBuffer(uint32_t FrameIndex) const;
    D3D12_RESOURCE_STATES* GetIndirectCommandState(uint32_t FrameIndex);
    D3D12_RESOURCE_STATES* GetMeshletRunCountState(uint32_t FrameIndex);
    FGpuCullingDispatchFrameData GetDispatchFrameData(uint32_t FrameIndex);
    void FillDispatchSharedIndices(FGpuCullingDispatchIndices& Indices) const;
    FMeshletVisibilityFrameData GetMeshletVisibilityFrameData(uint32_t FrameIndex, bool bLatePass = false) const;
    D3D12_RESOURCE_STATES* GetMeshletVisibilityState(uint32_t FrameIndex, bool bLatePass = false);
    FVisibilityListBuildIndices GetVisibilityListBuildIndices(uint32_t FrameIndex) const;
    FEarlyRejectListIndices GetEarlyRejectListIndices(uint32_t FrameIndex) const;
    FLateMergeVisibilityListIndices GetLateMergeVisibilityListIndices(uint32_t FrameIndex) const;
    FVisibilityListFrameSrvIndices GetVisibilityListFrameSrvIndices(uint32_t FrameIndex) const;

    void RefreshCullingPersistentValidation(uint32_t FramesInFlight);
    void ConfigureHZBOcclusion(bool bEnabled, uint32_t HZBBindlessIndex, uint32_t Width, uint32_t Height, uint32_t MipCount);
    bool CreateCullingConstantBuffers(FDX12Device* Device, uint32_t FramesInFlight);
    bool CreatePerFrameCullingResources(FDX12Device* Device, uint32_t FramesInFlight, uint64_t CommandBufferSize, uint32_t IndirectCommandCount, uint32_t RangeCount);
    void ResetCullingStatesToCommon(uint32_t FramesInFlight);
    bool CreateCullingPipelines(FDX12Device* Device);
    bool CreateSharedInputResources(FDX12Device* Device, const FGpuCullingSharedInputData& Data);
    void AddSharedInputUploadPreCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const;
    void CopySharedInputData(ID3D12GraphicsCommandList* CommandList) const;
    void AddSharedInputUploadPostCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const;

    void AddInitialUploadPreCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers, uint32_t FramesInFlight) const;
    void CopyInitialData(ID3D12GraphicsCommandList* CommandList, const std::vector<FUploadBuffer>& IndirectCommandUploads, uint64_t CommandBufferSize) const;
    void AddInitialUploadPostCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers, uint32_t FramesInFlight) const;
    void ResetInitialUploadStates(uint32_t FramesInFlight);
    void RefreshVisibilityPersistentValidation(uint32_t FramesInFlight);
    bool CreateVisibilityResources(FDX12Device* Device, uint32_t FramesInFlight, uint32_t IndirectCommandCount);
    void ResetVisibilityStatesToCommon(uint32_t FramesInFlight);
    bool CreateVisibilityListPipelines(FDX12Device* Device);

    void AddVisibilityListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex, uint32_t VisibilityFrameIndex) const;
    void AddGpuCullingPass(
        FDeferredPassContext& Context,
        ECullingMode Mode,
        uint32_t VisibilityInputIndex,
        uint32_t VisibilityInputFrameIndex,
        uint32_t CullingListIndex,
        uint32_t CullingListCountIndex,
        const char* PassName) const;
    void AddEarlyRejectListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex) const;
    void AddLateListMergePass(FDeferredPassContext& Context) const;

    void DispatchBuildVisibilityLists(
        FDX12Device* Device,
        D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress,
        uint32_t IndirectCommandCount,
        FDX12CommandContext& CmdContext,
        uint32_t VisibilityIndex,
        uint32_t VisibleListIndex,
        uint32_t InvisibleListIndex,
        uint32_t VisibleCountIndex,
        uint32_t InvisibleCountIndex,
        uint32_t VisibilityFrameIndex,
        uint32_t FrameIndex);
    void DispatchBuildEarlyRejectList(
        FDX12Device* Device,
        D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress,
        uint32_t IndirectCommandCount,
        FDX12CommandContext& CmdContext,
        uint32_t VisibilityIndex,
        uint32_t RejectListIndex,
        uint32_t RejectCountIndex,
        uint32_t FrameIndex);
    void DispatchMergeVisibilityLists(
        FDX12Device* Device,
        D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress,
        uint32_t IndirectCommandCount,
        FDX12CommandContext& CmdContext,
        uint32_t ListAIndex,
        uint32_t ListBIndex,
        uint32_t CountAIndex,
        uint32_t CountBIndex,
        uint32_t OutputListIndex,
        uint32_t OutputCountIndex,
        uint32_t FlagsIndex,
        uint32_t FrameIndex);
    void DispatchGpuCulling(
        const FGpuCullingDispatchConfig& Config,
        const FGpuCullingDispatchFrameData& FrameData,
        FGpuCullingDispatchIndices Indices,
        FDX12CommandContext& CmdContext,
        const FCamera& Camera,
        const char* PassName,
        uint32_t FrameIndex,
        bool bLatePass,
        uint32_t VisibilityInputFrameIndex);

private:
    void ResetSharedInputResources();
    void ResetVisibilityResources(uint32_t FramesInFlight);

    bool bCullingDispatchPersistentInputsValid = false;
    bool bSharedInputPersistentInputsValid = false;
    bool bMeshletVisibilityPersistentInputsValid = false;
    bool bVisibilityListPersistentInputsValid = false;

    std::vector<FMappedUploadBuffer> CullingConstantBuffers;
    std::vector<FMappedUploadBuffer> CullingConstantBuffersLate;
    std::vector<FBindlessBuffer> IndirectCommandBuffers;
    std::vector<FBindlessBuffer> IndirectCommandTemplateBuffers;
    std::vector<FBindlessBuffer> MeshletRunCountBuffers;
    FBindlessBuffer MeshletDrawDataBuffer;
    FUploadBuffer MeshletDrawDataUpload;
    FBindlessBuffer MeshletRangeOffsetBuffer;
    FUploadBuffer MeshletRangeOffsetUpload;
    FBindlessBuffer ModelBoundsBuffer;
    FUploadBuffer ModelBoundsUpload;
    FBindlessBuffer MeshletConeAxisBuffer;
    FUploadBuffer MeshletConeAxisUpload;
    FBindlessBuffer MeshletConeApexBuffer;
    FUploadBuffer MeshletConeApexUpload;
    std::vector<FBindlessBuffer> MeshletVisibilityBuffers;
    std::vector<FBindlessBuffer> MeshletVisibilityLateBuffers;
    std::vector<FBindlessBuffer> PrevVisibleListBuffers;
    std::vector<FBindlessBuffer> PrevInvisibleListBuffers;
    std::vector<FBindlessBuffer> EarlyRejectListBuffers;
    std::vector<FBindlessBuffer> LateListBuffers;
    std::vector<FBindlessBuffer> LateListFlagBuffers;
    std::vector<FBindlessBuffer> PrevVisibleCountBuffers;
    std::vector<FBindlessBuffer> PrevInvisibleCountBuffers;
    std::vector<FBindlessBuffer> EarlyRejectCountBuffers;
    std::vector<FBindlessBuffer> LateListCountBuffers;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> CullingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CullingPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CullingListPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> MeshletRunRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> MeshletRunClearPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> MeshletRunAppendPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> VisibilityListRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildVisibilityListsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildEarlyRejectListPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> MergeVisibilityListsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ClearVisibilityCountsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ClearVisibilityFlagsPipeline;

    uint64_t BoundsSizeInBytes = 0;
    uint64_t MeshletDrawDataSizeInBytes = 0;
    uint64_t RangeOffsetsSizeInBytes = 0;
    uint64_t ConeAxisSizeInBytes = 0;
    uint64_t ConeApexSizeInBytes = 0;

    uint32_t HZBCullingBindlessIndex = UINT32_MAX;
    uint32_t HZBCullingWidth = 0;
    uint32_t HZBCullingHeight = 0;
    uint32_t HZBCullingMipCount = 0;
    bool bHZBOcclusionEnabled = false;
};
