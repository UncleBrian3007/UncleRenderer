# ClusterDAG Streaming v1/v2

## Overview

Initial version (v1) of the ClusterDAG streaming system. It implements a GPU-driven feedback loop that detects non-resident pages during traversal, reports them to the CPU, and toggles resident bits in the page table.

**Important**: v2 still does **not perform actual disk I/O or page data uploads**. It adds a CPU-side priority scheduler on top of the v1 feedback loop: request `Priority` is consumed, duplicate page requests are merged with `max(Priority)`, and pending pages are installed from highest priority to lowest.

---

## Architecture

### Frame Flow

```text
GPU (each frame)
  - Detect non-resident pages during ClusterDAG node traversal
  - Write streaming requests to feedback UAV
  - Feedback buffer remains in GPU memory

Readback (each frame)
  - Copy feedback buffer to CPU readback buffer
  - Readback becomes mappable after GPU fence signal

CPU (OnFrameFenceSignaled)
  - ConsumeFeedback()    : read requests from readback buffer
  - QueueRequestedPage() : merge non-resident pages by max priority
  - MarkPageResident()   : set resident bit in priority order

GPU (next frame)
  - Upload page table if bPageTableUploadDirty == true
  - Shader calls IsClusterDagPageResident()
  - Resident pages continue refinement
  - Non-resident pages record a request and halt refinement
```

### Key Resources

| Resource | Purpose | Created | Written by |
|----------|---------|---------|------------|
| **PageTableBuffer** | Page table SRV (GPU read) | yes | CPU on each resident bit change |
| **PageTableUpload** | Persistently mapped upload buffer | yes | `UpdateMappedPageTable()` |
| **FeedbackBuffers** (per-frame) | Shader feedback UAV | yes | Shaders write requests |
| **FeedbackReadbackBuffers** (per-frame) | CPU-mappable readback buffer | yes | GPU copies feedback into it |
| **PageDataBuffer** | Physical page data pool | yes | **Not yet implemented** |

---

## Implementation

### 1. Initialization (InitializeResources)

```cpp
// Page table: CPU-side mirror + GPU buffer for all page entries
PageTableEntries = std::vector<FClusterDagPageTableEntry>(PageCount);
CreateBindlessBuffer(..., PageTableBuffer, ...);  // GPU SRV

// Persistently mapped upload buffer for page table updates
CreateMappedUploadBuffer(..., PageTableUpload);

// Per-frame feedback UAV and readback buffers
for each frame:
    CreateBindlessBuffer(..., FeedbackBuffers[frame], ...);
    CreateReadbackBuffer(..., FeedbackReadbackBuffers[frame], ...);

// Reserved for future page data uploads, not used by v1/v2
PageDataBuffer = CreateBindlessBuffer(StreamingPoolMB * 1MB);
```

### 2. Page Table Initialization (InitializePageTable)

```cpp
// Page 0 (root): always resident at startup
// All other pages: non-resident until requested
for (uint32_t PageIndex = 0; PageIndex < PageCount; ++PageIndex)
{
    if (PageIndex == 0)
    {
        Entry.Flags |= GClusterDagPageResidentFlag;
        Entry.PhysicalPageIndex = 0;
    }
    else
    {
        Entry.Flags = 0;
        Entry.PhysicalPageIndex = 0xffffffffu;
    }
}
```

### 3. Begin Frame Pass (AddBeginFramePass)

```cpp
void AddBeginFramePass(FDeferredPassContext& Context)
{
    // 1. Mark pending pages as resident.
    InstallPendingPages(FrameNumber);

    // 2. Upload page table to GPU if resident state changed.
    if (bPageTableUploadDirty)
    {
        UpdateMappedPageTable();
        // RenderGraph pass copies PageTableUpload to PageTableBuffer.
    }

    // 3. Clear feedback UAV counter.
}
```

### 4. Feedback Readback and Consume

```cpp
// AddFeedbackReadbackPass()
// GPU copies FeedbackBuffer to FeedbackReadback.

// After OnFrameFenceSignaled()
void ConsumeFeedback(uint32_t FrameIndex)
{
    FClusterDagStreamingRequest* Requests = Map(FeedbackReadbackBuffers[FrameIndex]);

    // Requests[0].StreamingResourceId is repurposed as the total request count.
    const uint32_t RequestCount = Requests[0].StreamingResourceId;
    const uint32_t FrameNumber = Owner->GetFrameNumber();

    for (uint32_t RequestIndex = 0; RequestIndex < RequestCount; ++RequestIndex)
    {
        const FClusterDagStreamingRequest& Request = Requests[RequestIndex + 1u];
        if (Request.StreamingResourceId == StreamingResourceId
            && Request.PageIndex < PageCount)
        {
            QueueRequestedPage(Request.PageIndex, Request.Priority, FrameNumber);
        }
    }

    Unmap(FeedbackReadbackBuffers[FrameIndex]);
}
```

### 5. Priority Scheduler (v2)

```cpp
// Pending entry:
// { PageIndex, Priority, FirstRequestSerial, LastRequestFrame }

// Duplicate page requests merge by max priority.
Pending.Priority = max(Pending.Priority, Request.Priority);

// When the pending cap is full, a new request replaces the current lowest
// priority pending page only if its priority is strictly higher.

// Installation order:
// 1. Priority descending
// 2. FirstRequestSerial ascending
```

### 6. Mark Page Resident (MarkPageResident)

```cpp
void MarkPageResident(uint32_t PageIndex, uint32_t FrameNumber)
{
    FClusterDagPageTableEntry& Entry = PageTableEntries[PageIndex];

    const bool bWasResident = (Entry.Flags & GClusterDagPageResidentFlag) != 0u;
    Entry.PhysicalPageIndex = PageIndex;
    Entry.Flags |= GClusterDagPageResidentFlag;
    Entry.LastUsedFrame = FrameNumber;

    if (!bWasResident)
    {
        bPageTableUploadDirty = true;
    }
}
```

### 7. GPU Shader Residency Check (IsClusterDagPageResident)

```hlsl
bool IsClusterDagPageResident(uint pageIndex, StructuredBuffer<ClusterDagPageTableEntry> PageTable)
{
    const ClusterDagPageTableEntry entry = PageTable[pageIndex];
    return (entry.Flags & kClusterDagPageResidentFlag) != 0u;
}

bool ShouldRefineClusterDagStreamingPage(...)
{
    if (!streamingEnabled)
        return true;

    const uint pageIndex = GetClusterDagGroupPageIndex(nextGroup);
    if (pageIndex == 0 || IsClusterDagPageResident(pageIndex, PageTable))
        return true;

    RequestClusterDagStreamingPage(
        streamingResourceId,
        pageIndex,
        max(asuint(nextGroup.ParentLODError), 1u),
        StreamingRequests);
    return false;
}
```

---

## Configuration

```ini
[ClusterDAG]
bEnableClusterDAGStreaming=true/false
ClusterDAGStreamingPoolMB=256
ClusterDAGStreamingRequestBufferCapacity=65536
ClusterDAGStreamingMaxPendingPages=64
ClusterDAGStreamingMaxPageInstallsPerFrame=16
```

### UI (ImGui)

- "Cluster DAG Streaming" toggle
- "Cluster DAG Stream Pool MB" input
- "Cluster DAG Installs/Frame" input
- Read-only scheduler stats: resident/pending/install and request/drop/replacement counts

### Debug Stats

Added in `GpuDebugPrintStats.hlsl`:

- `STREQ`: streaming requests recorded this frame
- `STFALL`: traversal halts due to a non-resident page
- `STDROP`: requests dropped due to feedback buffer overflow

---

## Current Limitations

### Not Yet Implemented (v3+)

1. **Actual disk I/O**
   - `PageDataBuffer` is allocated but never populated from disk.
   - v1/v2 only signal "this page is needed"; no page data is transferred.

2. **Page eviction policy**
   - Once resident, a page is never evicted.
   - There is no policy for pool exhaustion yet.

3. **Bandwidth / QoS control**
   - `MaxPageInstallsPerFrame` is a CPU-side throttle only.
   - There is no GPU-to-CPU bandwidth management yet.

### Implemented

- GPU feedback loop (per-frame request collection via UAV)
- CPU readback after GPU writes
- Priority scheduling with max-priority request merge
- Resident bit update and GPU page table synchronization
- Request statistics for overflow, pending drops, and priority replacement

---

## Data Structures

### FClusterDagPageTableEntry

```cpp
struct FClusterDagPageTableEntry
{
    uint32_t PhysicalPageIndex;  // physical slot index in the streaming pool
    uint32_t Flags;              // bit 0: resident flag
    uint32_t LastUsedFrame;      // frame number when last marked resident
    uint32_t Reserved;
};
// 16 bytes per entry
```

### FClusterDagStreamingRequest

```cpp
struct FClusterDagStreamingRequest
{
    // Requests[0].StreamingResourceId is repurposed as a total request counter.
    // For Requests[1+], this field holds the source resource ID for validation.
    uint32_t StreamingResourceId;
    uint32_t PageIndex;
    uint32_t Priority;  // max(asuint(ParentLODError), 1); consumed by v2 CPU scheduler
    uint32_t Flags;
};
```

### FClusterDagGroupData (Shader)

```hlsl
struct ClusterDagGroupData
{
    uint4 BvhData;
    uint Flags;          // bits [31:16]: streaming page index
    float ParentLODError;
    // ...
};
// Page index extraction: (Flags >> 16) & 0xffff
```

---

## Page Indexing

### Root Page (index 0)

```cpp
GClusterDagRootPageIndex = 0;
PageTableEntries[0].Flags |= GClusterDagPageResidentFlag;
```

### Non-Root Pages (index 1+)

```cpp
// Assigned in ClusterDagRuntime.cpp during DAG build.
PageIndex = BaseGroupIndex + LocalGroupIndex + 1;
Group.Flags = (PageIndex & 0xffff) << 16;
```

### Page Count

```cpp
StreamingPageCount = Groups.size();
```

---

## Performance Characteristics

| Property | Detail |
|----------|--------|
| **GPU overhead** | `InterlockedAdd`-based feedback, so atomic contention is possible |
| **CPU overhead** | O(RequestCount) readback processing plus O(PendingCount log PendingCount) install sorting |
| **Memory** | PageTableBuffer: 16 bytes * page count; Feedback UAV: per-frame |
| **Latency** | Request readback and page table visibility are delayed by frame scheduling |

---

## Planned Work (v3+)

1. **Disk I/O integration**
   - Read actual page data from `.vmesh` asset files.
   - Populate `PageDataBuffer` via upload heaps or DMA.

2. **Page eviction**
   - Track resident page usage with `LastUsedFrame`.
   - Evict LRU pages when the pool is full.

3. **Fallback under memory pressure**
   - Define behavior when the feedback buffer overflows.
   - Degrade gracefully instead of silently dropping important pages.

---

## Reference Files

- `Source/Render/Deferred/ClusterDagStreamingManager.h/cpp`: streaming manager lifecycle
- `Source/Render/Deferred/ClusterDagRuntime.cpp`: page index assignment and shader binding setup
- `Shaders/ClusterDag/ClusterDagTraversalCommon.hlsl`: residency check and request recording
- `Shaders/ClusterDag/ClusterDagNodeCull.hlsl`: streaming gate during traversal
- `Source/Core/RendererConfig.h/cpp`: configuration fields
- `Source/Core/Application.cpp`: ImGui UI controls
