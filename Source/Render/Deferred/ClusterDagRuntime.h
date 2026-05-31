#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>

#include "ClusterDagStreamingManager.h"
#include "../GpuResource.h"
#include "../Renderer.h"
#include "../../Scene/ClusterDAG.h"

class FDeferredRenderer;
class FDX12Device;
class FDX12CommandContext;
class FCamera;
struct FDeferredPassContext;
struct FMeshSection;
struct FRendererConfig;

class FClusterDagRuntime
{
    friend class FClusterDagVisibilityPass;

public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPasses(FDeferredPassContext& Context) const;

    bool HasResources() const;
    bool UsesRuntimeSection(const FMeshSection& Section) const;

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
    uint32_t GetStreamingPageCount() const { return StreamingPageCount; }
    const std::vector<FClusterDagStreamingPageSource>& GetStreamingPageSources() const { return StreamingPageSources; }

private:
    // ClusterDagCommon.hlsl-aligned data structures for CPU/GPU shared data
    struct FGroupData
    {
        DirectX::XMFLOAT4 Bounds{ 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 LodBounds{ 0.0f, 0.0f, 0.0f, 0.0f };
        float ParentLODError = 0.0f;
        uint32_t ChildRefStart = 0;
        uint32_t ChildRefCount = 0;
        uint32_t Flags = 0;
        uint32_t MipLevel = 0;
    };
    static_assert(sizeof(FGroupData) == 52, "FGroupData must match cluster DAG runtime shader layout");

    struct FClusterData
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
    static_assert(sizeof(FClusterData) == 64, "FClusterData must match cluster DAG runtime shader layout");

    // Per-cluster draw data uploaded to GPU and consumed by the cluster DAG runtime shaders.
    // Each cluster may have multiple entries (one per material range).
    struct FDrawData
    {
        uint32_t StartIndex = 0;        // Byte offset into the shared index buffer where this cluster's triangles begin
        uint32_t IndexCount = 0;        // Number of indices (= triangles * 3) to draw for this cluster
        uint32_t RangeIndex = 0;        // Which IndirectDrawRange (material/pipeline group) this packet belongs to; used as RunCounts array index
        uint32_t RangeCommandStart = 0; // IndirectDrawRanges[RangeIndex].Start; base slot in the output command buffer for this range, combined with the per-range atomic counter to produce the final output slot
        uint32_t RangeCommandCount = 0; // IndirectDrawRanges[RangeIndex].Count; capacity guard for GPU command appends
        uint32_t DrawSectionIndex = 0;        // Draw-section index used by the visibility resolve path to recover per-section shading data

        static FDrawData Make(uint32_t InStartIndex, uint32_t InIndexCount, uint32_t InRangeIndex, uint32_t InRangeCommandStart, uint32_t InRangeCommandCount, uint32_t InDrawSectionIndex)
        {
            return { InStartIndex, InIndexCount, InRangeIndex, InRangeCommandStart, InRangeCommandCount, InDrawSectionIndex };
        }
    };
    static_assert(sizeof(FDrawData) == 24, "FDrawData must match cluster DAG runtime shader layout");

    struct FVisibleEntry
    {
        uint32_t ClusterIndex = 0;
        uint32_t DrawDataIndex = 0;
        uint32_t PageDataBase = 0xffffffffu;
        uint32_t Reserved = 0;
    };
    static_assert(sizeof(FVisibleEntry) == 16, "FVisibleEntry must match cluster DAG visibility shader layout");

    struct FCandidateClusterEntry
    {
        uint32_t ClusterIndex = GClusterDAGInvalidIndex;
        uint32_t PageDataBase = GClusterDAGInvalidIndex;
        uint32_t PageLocalClusterIndex = GClusterDAGInvalidIndex;
    };
    static_assert(sizeof(FCandidateClusterEntry) == 12, "FCandidateClusterEntry must match cluster DAG queue shader layout");

    struct FPreparedData
    {
        std::vector<FGroupData> Groups;
        std::vector<FClusterData> Clusters;
        std::vector<FRuntimeClusterChildRef> ChildRefs;
        std::vector<uint32_t> RootGroups;
        std::vector<FDrawData> DrawDatas;
        std::vector<FIndirectDrawCommand> CommandTemplates;
        std::vector<uint32_t> RangeOffsets;
        std::vector<FClusterDagStreamingPageSource> StreamingPageSources;
        uint32_t StreamingPageCount = 1;
    };

    bool PrepareRuntimeData(FDeferredRenderer& Owner, FPreparedData& OutData);
    bool ValidatePreparedRuntimeData(const FPreparedData& Data) const;
    uint32_t ResolveDrawDataSectionIndex(
        const FWorldSectionList& DrawSections,
        uint32_t SortedIndex,
        const FRuntimeClusterDrawData& RuntimeDrawData) const;
    uint32_t GetOrAddRangeForSection(
        const FWorldSectionList& DrawSections,
        uint32_t DrawSectionIndex);
    void AppendSectionDrawDatas(
        const FWorldSectionList& DrawSections,
        const FMeshSection& Section,
        uint32_t SortedIndex,
        const FRuntimeClusterHierarchy& RuntimeHierarchy,
        D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase,
        uint32_t SceneConstantBufferStride,
        FPreparedData& OutData,
        std::vector<uint32_t>& OutLocalDrawDataSectionIndices,
        std::vector<uint32_t>& OutLocalDrawDataRangeIndices);
    void AppendSectionGroupsAndStreamingPages(
        const FMeshSection& Section,
        uint32_t SortedIndex,
        const FRuntimeClusterHierarchy& RuntimeHierarchy,
        uint32_t BaseGroupIndex,
        uint32_t BaseClusterIndex,
        uint32_t BaseChildRefIndex,
        uint32_t BaseDrawDataIndex,
        const DirectX::XMMATRIX& World,
        float SectionScale,
        const std::vector<uint32_t>& LocalDrawDataSectionIndices,
        const std::vector<uint32_t>& LocalDrawDataRangeIndices,
        FPreparedData& OutData);
    void InitializeStreamingPageSource(
        const FMeshSection& Section,
        const FRuntimeClusterHierarchy& RuntimeHierarchy,
        const FRuntimeClusterGroup& RuntimeGroup,
        uint32_t LocalGroupIndex,
        uint32_t PageIndex,
        uint32_t BaseGroupIndex,
        uint32_t BaseClusterIndex,
        const FGroupData& GroupData,
        FPreparedData& OutData) const;
    void BuildStreamingPageClusterPayload(
        const FMeshSection& Section,
        uint32_t SortedIndex,
        const FRuntimeClusterHierarchy& RuntimeHierarchy,
        const FRuntimeClusterGroup& RuntimeGroup,
        uint32_t BaseGroupIndex,
        uint32_t BaseClusterIndex,
        uint32_t BaseDrawDataIndex,
        const DirectX::XMMATRIX& World,
        float SectionScale,
        const std::vector<uint32_t>& LocalDrawDataSectionIndices,
        const std::vector<uint32_t>& LocalDrawDataRangeIndices,
        FClusterDagStreamingPageSource& PageSource) const;
    void AppendSectionChildRefs(
        const FRuntimeClusterHierarchy& RuntimeHierarchy,
        uint32_t BaseClusterIndex,
        FPreparedData& OutData) const;
    void AppendSectionClusters(
        const FRuntimeClusterHierarchy& RuntimeHierarchy,
        uint32_t BaseGroupIndex,
        uint32_t BaseDrawDataIndex,
        const DirectX::XMMATRIX& World,
        float SectionScale,
        FPreparedData& OutData) const;
    bool CreateRuntimeResources(FDeferredRenderer& Owner, FDX12Device* Device, const FPreparedData& Data);
    bool UploadRuntimeResources(FDeferredRenderer& Owner, FDX12Device* Device, const FPreparedData& Data);
    void PopulateCullingConstants(FDeferredRenderer& Owner, const FCamera& Camera) const;
    void AddInitQueuePass(FDeferredPassContext& Context, const char* PassName) const;
    void AddPersistentCullPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddLevelSplitInitPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddLevelSplitPrepareNodePass(FDeferredPassContext& Context, const char* PassName, uint32_t Level) const;
    void AddLevelSplitNodeCullPass(FDeferredPassContext& Context, const char* PassName, uint32_t Level) const;
    void AddLevelSplitPrepareClusterPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddLevelSplitClusterCullPass(FDeferredPassContext& Context, const char* PassName) const;
    void AddFinalizeIndirectArgsPass(FDeferredPassContext& Context, const char* PassName, const char* PixGroupName = nullptr) const;
    void DispatchInitQueues(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const FCamera& Camera, const char* PassName) const;
    void DispatchPersistentCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;
    void DispatchLevelSplitInit(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const FCamera& Camera, const char* PassName) const;
    void DispatchLevelSplitPrepareNode(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, uint32_t Level, const char* PassName) const;
    void DispatchLevelSplitNodeCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, uint32_t Level, const char* PassName) const;
    void DispatchLevelSplitPrepareCluster(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;
    void DispatchLevelSplitClusterCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const;

private:
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> InitQueuePipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> PersistentCullPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> LevelSplitInitPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> LevelSplitPrepareNodePipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> LevelSplitNodeCullPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> LevelSplitPrepareClusterPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> LevelSplitClusterCullPipelines;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> DispatchCommandSignature;

    FBindlessBuffer GroupBuffer;
    FBindlessBuffer ClusterBuffer;
    FBindlessBuffer ChildRefBuffer;
    FBindlessBuffer RootGroupBuffer;
    FBindlessBuffer DrawDataBuffer;

    std::vector<FBindlessBuffer> QueueStateBuffers;
    std::vector<FBindlessBuffer> GroupQueueBuffers;
    std::vector<FBindlessBuffer> CandidateClusterEntryBuffers;
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
    bool bForceSoftwareRaster = false;
    EClusterDAGTraversalMode ActiveTraversalMode = EClusterDAGTraversalMode::LevelSplitQueue;
    uint32_t VisibleRootCount = 0;
    uint32_t ClusterCount = 0;
    uint32_t RuntimeGroupCount = 0;
    uint32_t RuntimeCommandCount = 0;
    uint32_t RuntimeChildRefCount = 0;
    uint32_t StreamingPageCount = 1;
    uint32_t RuntimeMaxTraversalLevels = 1;
    bool bResourcesReady = false;
    std::vector<FRenderer::FIndirectDrawRange> IndirectDrawRanges;
    std::vector<FClusterDagStreamingPageSource> StreamingPageSources;
};
