#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>

#include "../GpuResource.h"
#include "../Renderer.h"
#include "../../Scene/ClusterDAG.h"

class FDeferredRenderer;
class FDX12Device;
class FDX12CommandContext;
class FCamera;
struct FDeferredPassContext;
struct FSceneModelResource;
struct FRendererConfig;

class FClusterDagRuntime
{
    friend class FClusterDagVisibilityPass;

public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPasses(FDeferredPassContext& Context) const;

    bool HasResources() const;
    bool UsesRuntimePath(const FDeferredRenderer& Owner, const FSceneModelResource& Model) const;

    ID3D12Resource* GetIndirectCommandBuffer(const FDeferredRenderer& Owner) const;
    D3D12_RESOURCE_STATES& GetIndirectCommandState(FDeferredRenderer& Owner);
    ID3D12Resource* GetRunCountBuffer(const FDeferredRenderer& Owner) const;
    D3D12_RESOURCE_STATES& GetRunCountState(FDeferredRenderer& Owner);
    ID3D12Resource* GetDrawDataBuffer() const;
    D3D12_RESOURCE_STATES& GetDrawDataState();

    const std::vector<FRenderer::FIndirectDrawRange>& GetIndirectDrawRanges() const { return IndirectDrawRanges; }

    void ApplyConfig(const FRendererConfig& Config);
    bool IsEnabled() const { return bEnabled; }
    bool IsFastShaderEnabled() const { return bFastShaderEnabled; }
    bool IsDebugEnabled() const { return bDebugEnabled; }
    EClusterDAGTraversalMode GetTraversalMode() const { return ActiveTraversalMode; }
    float GetTargetErrorPixels() const { return TargetErrorPixels; }
    bool IsForceMipEnabled() const { return bForceMipEnabled; }
    uint32_t GetForceMipLevel() const { return ForceMipLevel; }
    bool IsForceMipSkipFrustumCull() const { return bForceMipSkipFrustumCull; }
    float GetSwRasterThresholdPixels() const { return SwRasterThresholdPixels; }
    uint32_t GetVisibleRootCount() const { return VisibleRootCount; }
    uint32_t GetClusterCount() const { return ClusterCount; }

private:
    struct FSceneGroupData
    {
        DirectX::XMFLOAT4 Bounds{ 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 LodBounds{ 0.0f, 0.0f, 0.0f, 0.0f };
        float ParentLODError = 0.0f;
        uint32_t ChildRefStart = 0;
        uint32_t ChildRefCount = 0;
        uint32_t Flags = 0;
        uint32_t MipLevel = 0;
    };
    static_assert(sizeof(FSceneGroupData) == 52, "FSceneGroupData must match cluster DAG runtime shader layout");

    struct FSceneClusterData
    {
        DirectX::XMFLOAT4 Bounds{ 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 LodBounds{ 0.0f, 0.0f, 0.0f, 0.0f };
        float LODError = 0.0f;
        float MaxEdgeLength = 0.0f;
        uint32_t GroupIndex = GClusterDAGInvalidIndex;
        uint32_t GeneratingGroupIndex = GClusterDAGInvalidIndex;
        uint32_t DrawDataStart = 0;
        uint32_t DrawDataCount = 0;
        uint32_t TriangleCount = 0;
        uint32_t MipLevel = 0;
    };
    static_assert(sizeof(FSceneClusterData) == 64, "FSceneClusterData must match cluster DAG runtime shader layout");

    // Per-cluster draw data uploaded to GPU and consumed by the cluster DAG runtime shaders.
    // Each cluster may have multiple entries (one per material range).
    struct FClusterDrawData
    {
        uint32_t StartIndex = 0;        // Byte offset into the shared index buffer where this cluster's triangles begin
        uint32_t IndexCount = 0;        // Number of indices (= triangles * 3) to draw for this cluster
        uint32_t RangeIndex = 0;        // Which IndirectDrawRange (material/pipeline group) this packet belongs to; used as RunCounts array index
        uint32_t RangeCommandStart = 0; // IndirectDrawRanges[RangeIndex].Start; base slot in the output command buffer for this range, combined with the per-range atomic counter to produce the final output slot
        uint32_t ModelIndex = 0;        // Scene-model index used by the visibility resolve path to recover per-model shading data
    };

    struct FVisibleEntry
    {
        uint32_t ClusterIndex = 0;
        uint32_t DrawDataIndex = 0;
    };
    static_assert(sizeof(FVisibleEntry) == 8, "FVisibleEntry must match cluster DAG visibility shader layout");

    struct FPreparedData
    {
        std::vector<FSceneGroupData> Groups;
        std::vector<FSceneClusterData> Clusters;
        std::vector<FRuntimeClusterChildRef> ChildRefs;
        std::vector<uint32_t> RootGroups;
        std::vector<FClusterDrawData> DrawDatas;
        std::vector<FIndirectDrawCommand> CommandTemplates;
        std::vector<uint32_t> RangeOffsets;
    };

    bool PrepareRuntimeData(FDeferredRenderer& Owner, FPreparedData& OutData);
    bool ValidatePreparedRuntimeData(const FPreparedData& Data) const;
    bool CreateRuntimeResources(FDeferredRenderer& Owner, FDX12Device* Device, const FPreparedData& Data);
    void PopulateCullingConstants(FDeferredRenderer& Owner, const FCamera& Camera) const;
    void AddInitQueuePass(FDeferredPassContext& Context, const char* PassName) const;
    void AddPersistentCullPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddSplitNodeCullPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddSplitClusterCullPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddLevelSplitInitPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddLevelSplitPrepareNodePass(FDeferredPassContext& Context, const char* PassName, uint32_t Level) const;
    void AddLevelSplitNodeCullPass(FDeferredPassContext& Context, const char* PassName, uint32_t Level) const;
    void AddLevelSplitPrepareClusterPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddLevelSplitClusterCullPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddFinalizeIndirectArgsPass(FDeferredPassContext& Context, const char* PassName, const char* PixGroupName = nullptr) const;
    void DispatchInitQueues(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const FCamera& Camera, const char* PassName) const;
    void DispatchPersistentCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;
    void DispatchSplitNodeCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;
    void DispatchSplitClusterCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;
    void DispatchLevelSplitInit(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const FCamera& Camera, const char* PassName) const;
    void DispatchLevelSplitPrepareNode(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, uint32_t Level, const char* PassName) const;
    void DispatchLevelSplitNodeCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, uint32_t Level, const char* PassName) const;
    void DispatchLevelSplitPrepareCluster(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;
    void DispatchLevelSplitClusterCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;

private:
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> InitQueuePipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> PersistentCullPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> SplitNodeCullPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> SplitClusterCullPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> LevelSplitInitPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> LevelSplitPrepareNodePipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> LevelSplitNodeCullPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> LevelSplitPrepareClusterPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> LevelSplitClusterCullPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RunAppendPipeline;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> DispatchCommandSignature;

    FBindlessBuffer GroupBuffer;
    FUploadBuffer GroupUpload;
    FBindlessBuffer ClusterBuffer;
    FUploadBuffer ClusterUpload;
    FBindlessBuffer ChildRefBuffer;
    FUploadBuffer ChildRefUpload;
    FBindlessBuffer RootGroupBuffer;
    FUploadBuffer RootGroupUpload;
    FBindlessBuffer DrawDataBuffer;
    FUploadBuffer DrawDataUpload;

    std::vector<FBindlessBuffer> QueueStateBuffers;
    std::vector<FBindlessBuffer> GroupQueueBuffers;
    std::vector<FBindlessBuffer> CandidateClusterQueueBuffers;
    std::vector<FBindlessBuffer> VisitedGroupEpochBuffers;
    std::array<std::vector<FBindlessBuffer>, 2> LevelSplitNodeCandidateBuffers;
    std::array<std::vector<FBindlessBuffer>, 2> LevelSplitNodeArgsBuffers;
    std::vector<FBindlessBuffer> LevelSplitClusterArgsBuffers;
    std::vector<FBindlessBuffer> SwRasterDispatchArgsBuffers;
    std::vector<FBindlessBuffer> IndirectCommandBuffers;
    std::vector<FBindlessBuffer> IndirectCommandTemplateBuffers;
    std::vector<FBindlessBuffer> RunCountBuffers;
    std::vector<FBindlessBuffer> VisibleEntryBuffers;
    std::vector<FBindlessBuffer> VisibleEntryCounterBuffers;
    std::vector<FBindlessBuffer> HwVisibleEntryIndexBuffers;
    std::vector<FBindlessBuffer> SwVisibleEntryIndexBuffers;
    std::vector<FBindlessBuffer> DrawDataVisibleEntryIndexBuffers;

    bool bEnabled = false;
    bool bFastShaderEnabled = false;
    bool bDebugEnabled = false;
    float TargetErrorPixels = 1.0f;
    float SwRasterThresholdPixels = 16.0f;
    bool bForceMipEnabled = false;
    uint32_t ForceMipLevel = 0;
    bool bForceMipSkipFrustumCull = false;
    bool bForceSoftwareRaster = false;
    EClusterDAGTraversalMode ActiveTraversalMode = EClusterDAGTraversalMode::LevelSplitQueue;
    uint32_t VisibleRootCount = 0;
    uint32_t ClusterCount = 0;
    uint32_t RuntimeGroupCount = 0;
    uint32_t RuntimeCommandCount = 0;
    uint32_t RuntimeChildRefCount = 0;
    uint32_t RuntimeMaxTraversalLevels = 1;
    bool bResourcesReady = false;
    std::vector<FRenderer::FIndirectDrawRange> IndirectDrawRanges;
};
