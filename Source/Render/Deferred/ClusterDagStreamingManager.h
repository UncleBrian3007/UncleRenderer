#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <wrl.h>
#include <d3d12.h>

#include "../GpuResource.h"

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

class FClusterDagStreamingManager
{
    friend class FClusterDagRuntime;

public:
    void ApplyConfig(const FRendererConfig& Config);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t PageCount, uint32_t FramesInFlight);
    void AddBeginFramePass(FDeferredPassContext& Context);
    void AddFeedbackReadbackPass(FDeferredPassContext& Context);
    void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue);

    bool IsEnabled() const { return bEnabled && bResourcesReady; }
    uint32_t GetStreamingResourceId() const { return StreamingResourceId; }
    uint32_t GetPageCount() const { return PageCount; }
    uint32_t GetRequestCapacity() const { return RequestBufferCapacity; }
    uint32_t GetPageTableSrvBindlessIndex() const { return PageTableBuffer.SrvBindlessIndex; }
    uint32_t GetFeedbackUavBindlessIndex(uint32_t FrameIndex) const;

    uint32_t GetResidentPageCount() const { return ResidentPageCount; }
    uint32_t GetPendingPageCount() const { return static_cast<uint32_t>(PendingPages.size()); }
    uint32_t GetLastFrameRequestCount() const { return LastFrameRequestCount; }
    uint32_t GetLastFrameInstallCount() const { return LastFrameInstallCount; }
    uint32_t GetDroppedRequestCount() const { return DroppedRequestCount; }
    uint32_t GetReplacedRequestCount() const { return ReplacedRequestCount; }

private:
    struct FPendingPage
    {
        uint32_t PageIndex = 0xffffffffu;
        uint32_t Priority = 0u;
        uint64_t FirstRequestSerial = 0u;
        uint32_t LastRequestFrame = 0u;
    };

    struct FFeedbackBuffer
    {
        FBindlessBuffer Gpu;
        FReadbackBuffer Readback;

        bool IsValid() const { return Gpu.IsValid() && Readback.IsValid(); }
    };

    void Reset();
    void InitializePageTable();
    void MarkPageResident(uint32_t PageIndex, uint32_t FrameNumber);
    void ConsumeFeedback(uint32_t FrameIndex);
    void QueueRequestedPage(uint32_t PageIndex, uint32_t Priority, uint32_t FrameNumber);
    void InstallPendingPages(uint32_t FrameNumber);
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

    std::vector<FClusterDagPageTableEntry> PageTableEntries;
    std::vector<FPendingPage> PendingPages;
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
    uint32_t ResidentPageCount = 0;
    uint32_t LastFrameRequestCount = 0;
    uint32_t LastFrameInstallCount = 0;
    uint32_t DroppedRequestCount = 0;
    uint32_t ReplacedRequestCount = 0;
};
