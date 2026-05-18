#pragma once

#include <cstdint>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>
#include <wrl.h>
#include <d3d12.h>

#include "../GpuResource.h"
#include "../../Scene/ClusterDAG.h"

class FDeferredRenderer;
class FClusterDagRuntime;
class FDX12Device;
class FDX12CommandContext;
struct FDeferredPassContext;
struct FRendererConfig;

struct FClusterDagStreamingRequest
{
    uint32_t StreamingResourceId = 0;
    uint32_t PageIndex = 0;
    uint32_t Priority = 0;
    uint32_t Flags = 0;
};

static_assert(sizeof(FClusterDagStreamingRequest) == 16, "FClusterDagStreamingRequest must match shader layout");

struct FClusterDagPageTableEntry
{
    uint32_t PhysicalPageIndex = 0xffffffffu;
    uint32_t Flags = 0;
    uint32_t LastUsedFrame = 0;
    uint32_t Reserved = 0;
};

static_assert(sizeof(FClusterDagPageTableEntry) == 16, "FClusterDagPageTableEntry must match shader layout");

struct FClusterDagStreamingPageSource
{
    struct FSceneClusterRecord
    {
        uint32_t GlobalClusterIndex = GClusterDAGInvalidIndex;
        float Bounds[4] = {};
        float LodBounds[4] = {};
        float LODError = 0.0f;
        float MaxEdgeLength = 0.0f;
        uint32_t GroupIndex = GClusterDAGInvalidIndex;
        uint32_t GeneratingGroupIndex = GClusterDAGInvalidIndex;
        uint32_t DrawDataStart = 0u;
        uint32_t DrawDataCount = 0u;
        uint32_t TriangleCount = 0u;
        uint32_t MipLevel = 0u;
    };

    struct FSceneDrawDataRecord
    {
        uint32_t GlobalDrawDataIndex = GClusterDAGInvalidIndex;
        uint32_t StartIndex = 0u;
        uint32_t IndexCount = 0u;
        uint32_t RangeIndex = 0u;
        uint32_t RangeCommandStart = 0u;
        uint32_t ModelIndex = 0u;
    };

    bool bValid = false;
    uint32_t PageIndex = 0xffffffffu;
    uint32_t MeshIndex = 0xffffffffu;
    uint32_t DagIndex = 0xffffffffu;
    uint32_t LocalPageIndex = 0xffffffffu;
    uint32_t GlobalGroupIndex = 0xffffffffu;
    float SceneGroupBounds[4] = {};
    float SceneGroupLodBounds[4] = {};
    float SceneGroupParentLODError = 0.0f;
    uint32_t SceneGroupChildRefCount = 0u;
    uint32_t SceneGroupFlags = 0u;
    uint32_t SceneGroupMipLevel = 0u;
    std::vector<FRuntimeClusterChildRef> SceneGroupChildRefs;
    std::vector<FSceneClusterRecord> ScenePageClusters;
    std::vector<FSceneDrawDataRecord> ScenePageDrawDatas;
    std::vector<uint32_t> ScenePagePackedIndices;
    std::vector<FClusterDagPackedPosition> ScenePagePackedPositions;
    std::vector<uint32_t> ScenePagePackedNormals;
    std::vector<uint32_t> ScenePagePackedUVs;
    std::vector<uint32_t> ScenePagePackedTangents;
    std::vector<uint32_t> ScenePagePackedColors;
    std::wstring SourceFilePath;
    std::wstring CacheFilePath;
};

class FClusterDagStreamingManager
{
    friend class FClusterDagRuntime;

public:
    void ApplyConfig(const FRendererConfig& Config);
    bool InitializeResources(
        FDeferredRenderer& Owner,
        FDX12Device* Device,
        uint32_t PageCount,
        uint32_t FramesInFlight,
        const std::vector<FClusterDagStreamingPageSource>& PageSources);
    void AddBeginFramePass(FDeferredPassContext& Context);
    void AddFeedbackReadbackPass(FDeferredPassContext& Context);
    void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue);

    bool IsEnabled() const { return bEnabled && bResourcesReady; }
    uint32_t GetStreamingResourceId() const { return StreamingResourceId; }
    uint32_t GetPageCount() const { return PageCount; }
    uint32_t GetRequestCapacity() const { return RequestBufferCapacity; }
    uint32_t GetPageTableSrvBindlessIndex() const { return PageTableBuffer.SrvBindlessIndex; }
    uint32_t GetPageDataSrvBindlessIndex() const { return PageDataBuffer.SrvBindlessIndex; }
    FBindlessBuffer& GetPageDataBuffer() { return PageDataBuffer; }
    uint32_t GetFeedbackUavBindlessIndex(uint32_t FrameIndex) const;

    uint32_t GetResidentPageCount() const { return ResidentPageCount; }
    uint32_t GetPendingPageCount() const { return static_cast<uint32_t>(PendingPages.size()); }
    uint32_t GetLastFrameRequestCount() const { return LastFrameRequestCount; }
    uint32_t GetLastFrameInstallCount() const { return LastFrameInstallCount; }
    uint32_t GetDroppedRequestCount() const { return DroppedRequestCount; }
    uint32_t GetReplacedRequestCount() const { return ReplacedRequestCount; }
    uint32_t GetPageSlotBytes() const { return PageSlotPool.SlotBytes; }
    uint32_t GetPageSlotCount() const { return PageSlotPool.SlotCount; }
    uint32_t GetUsedPageSlotCount() const { return static_cast<uint32_t>(PageSlotPool.LogicalPageToSlot.size()); }
    uint32_t GetFreePageSlotCount() const { return static_cast<uint32_t>(PageSlotPool.FreeSlots.size()); }
    uint32_t GetSlotPoolFullDropCount() const { return SlotPoolFullDropCount; }
    uint32_t GetIoInFlightCount() const;
    uint32_t GetLastFrameIoIssueCount() const { return LastFrameIoIssueCount; }
    uint32_t GetLastFrameIoCompleteCount() const { return LastFrameIoCompleteCount; }
    uint32_t GetLastFrameIoFailCount() const { return LastFrameIoFailCount; }
    uint32_t GetUploadInFlightCount() const { return static_cast<uint32_t>(GpuUploadingPages.size()); }
    uint32_t GetLastFrameUploadIssueCount() const { return LastFrameUploadIssueCount; }
    uint32_t GetLastFrameUploadCompleteCount() const { return LastFrameUploadCompleteCount; }
    uint32_t GetLastFrameUploadBytes() const { return LastFrameUploadBytes; }

private:
    enum class EPendingPageState : uint8_t
    {
        Pending,
        IoInFlight,
        IoComplete,
        GpuUploading,
        Failed
    };

    struct FPageReadSource
    {
        bool bValid = false;
        uint64_t FileOffset = 0;
        uint32_t PayloadBytes = 0u;
        std::wstring CacheFilePath;
    };

    struct FPageReadResult
    {
        bool bSucceeded = false;
        uint32_t PageIndex = 0xffffffffu;
        std::vector<uint8_t> Payload;
    };

    struct FPendingPage
    {
        uint32_t PageIndex = 0xffffffffu;
        uint32_t Priority = 0u;
        uint64_t FirstRequestSerial = 0u;
        uint32_t LastRequestFrame = 0u;
        EPendingPageState State = EPendingPageState::Pending;
        std::vector<uint8_t> Payload;
        std::future<FPageReadResult> ReadFuture;
    };

    struct FFeedbackBuffer
    {
        FBindlessBuffer Gpu;
        FReadbackBuffer Readback;

        bool IsValid() const { return Gpu.IsValid() && Readback.IsValid(); }
    };

    struct FPageSlotPool
    {
        uint32_t SlotBytes = 131072u;
        uint32_t SlotCount = 0u;
        std::vector<uint32_t> FreeSlots;
        std::unordered_map<uint32_t, uint32_t> LogicalPageToSlot;
    };

    struct FGpuUploadingPage
    {
        uint32_t PageIndex = 0xffffffffu;
        uint32_t PhysicalSlotIndex = 0xffffffffu;
        uint32_t PayloadBytes = 0u;
        uint32_t SubmissionFrameIndex = 0xffffffffu;
        uint32_t RequestFrameNumber = 0u;
        uint64_t FenceValue = 0u;
        bool bCopySubmitted = false;
        FUploadBuffer UploadBuffer;
    };

    void Reset();
    void InitializePageSlotPool(uint64_t PoolBytes);
    void InitializePageReadSources(const std::vector<FClusterDagStreamingPageSource>& Sources);
    bool AllocatePhysicalPageSlot(uint32_t PageIndex, uint32_t& OutSlotIndex);
    void InitializePageTable();
    void MarkPageResident(uint32_t PageIndex, uint32_t PhysicalSlotIndex, uint32_t FrameNumber);
    void ConsumeFeedback(uint32_t FrameIndex);
    void QueueRequestedPage(uint32_t PageIndex, uint32_t Priority, uint32_t FrameNumber);
    void UpdateStreamingWork(uint32_t FrameIndex, uint32_t FrameNumber);
    void IssuePageReads();
    void PollPageReads();
    void IssuePageUploads(uint32_t FrameIndex, uint32_t FrameNumber);
    void MarkUploadCopySubmitted(uint32_t PageIndex, uint32_t FrameIndex);
    void AssignSubmittedUploadFence(uint32_t FrameIndex, uint64_t FenceValue);
    void RetireCompletedUploads(uint64_t CompletedFenceValue, bool bUpdateFrameStats);
    bool HasValidPageReadSource(uint32_t PageIndex) const;
    uint32_t CountIoInFlightPages() const;
    uint32_t CountPagesAwaitingSlots() const;
    static FPageReadResult ReadPagePayload(const FPageReadSource& Source, uint32_t PageIndex);
    bool BuildGpuPagePayload(uint32_t PageIndex, const std::vector<uint8_t>& DiskPayload, std::vector<uint8_t>& OutPayload) const;
    void ClearPendingPages();
    void RebuildPendingPageIndices();
    void UpdateMappedPageTable();

private:
    FDeferredRenderer* Owner = nullptr;
    FDX12Device* Device = nullptr;

    std::vector<FFeedbackBuffer> FeedbackBuffers;
    FUploadBuffer FeedbackClearUpload;
    FBindlessBuffer PageTableBuffer;
    FMappedUploadBuffer PageTableUpload;
    FBindlessBuffer PageDataBuffer;
    FPageSlotPool PageSlotPool;

    std::vector<FClusterDagPageTableEntry> PageTableEntries;
    std::vector<FPageReadSource> PageReadSources;
    std::vector<FClusterDagStreamingPageSource> PageSources;
    std::vector<FPendingPage> PendingPages;
    std::vector<FGpuUploadingPage> GpuUploadingPages;
    std::unordered_map<uint32_t, uint32_t> PendingPageIndices;

    bool bEnabled = false;
    bool bResourcesReady = false;
    bool bPageTableUploadDirty = false;
    uint32_t StreamingResourceId = 1;
    uint64_t NextRequestSerial = 0;
    uint32_t PageCount = 0;
    uint32_t RequestBufferCapacity = 65536;
    uint32_t MaxPendingPages = 64;
    uint32_t MaxPageInstallsPerFrame = 16;
    uint32_t StreamingPoolMB = 256;
    uint32_t PageSlotBytes = 131072;
    uint32_t MaxIoInFlight = 8;
    uint32_t MaxPageUploadBytesPerFrame = 8388608;
    uint32_t ResidentPageCount = 0;
    uint32_t LastFrameRequestCount = 0;
    uint32_t LastFrameInstallCount = 0;
    uint32_t LastFrameIoIssueCount = 0;
    uint32_t LastFrameIoCompleteCount = 0;
    uint32_t LastFrameIoFailCount = 0;
    uint32_t LastFrameUploadIssueCount = 0;
    uint32_t LastFrameUploadCompleteCount = 0;
    uint32_t LastFrameUploadBytes = 0;
    uint32_t DroppedRequestCount = 0;
    uint32_t ReplacedRequestCount = 0;
    uint32_t SlotPoolFullDropCount = 0;
};
