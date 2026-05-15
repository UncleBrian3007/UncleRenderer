#ifndef CLUSTER_DAG_TRAVERSAL_COMMON_HLSL
#define CLUSTER_DAG_TRAVERSAL_COMMON_HLSL

#include "../CullingConstants.hlsl"
#include "ClusterDagCommon.hlsl"

#ifndef USE_CLUSTER_DAG_DEBUG
#define USE_CLUSTER_DAG_DEBUG 0
#endif

#ifndef USE_CLUSTER_DAG_FAST
#define USE_CLUSTER_DAG_FAST 0
#endif

#include "../GpuDebug/GpuDebugPrintCommon.hlsl"

#if USE_CLUSTER_DAG_DEBUG
#include "../GpuDebug/GpuDebugLineCommon.hlsl"
#endif

float ComputeProjectedErrorPixels(float3 center, float objectSpaceError)
{
    const float distanceToCamera = max(length(center - CameraPosition), 1e-3f);
    return objectSpaceError * ViewportHeightPixels / distanceToCamera;
}

float ComputeProjectedObjectLengthPixels(float3 center, float objectSpaceLength)
{
    const float distanceToCamera = max(length(center - CameraPosition), 1e-3f);
    return objectSpaceLength * ViewportHeightPixels / distanceToCamera;
}

bool ShouldRasterizeClusterSW(ClusterDagClusterData cluster)
{
    const float maxEdgeLength = cluster.MaxEdgeLength > 0.0f ? cluster.MaxEdgeLength : cluster.Bounds.w * 2.0f;
    return ClusterDAGForceSoftwareRaster != 0u
        || ComputeProjectedObjectLengthPixels(cluster.Bounds.xyz, maxEdgeLength) < ClusterDAGSwRasterThresholdPixels;
}

uint GetClusterDagGroupPageIndex(ClusterDagGroupData group)
{
    return (group.Flags >> kClusterDagGroupPageIndexShift) & kClusterDagGroupPageIndexMask;
}

bool IsClusterDagPageResident(uint pageIndex, StructuredBuffer<ClusterDagPageTableEntry> PageTable)
{
    const ClusterDagPageTableEntry entry = PageTable[pageIndex];
    return (entry.Flags & kClusterDagPageResidentFlag) != 0u;
}

void RecordClusterDagStreamingRequest(uint debugPrintStatsIndex, bool overflow)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kClusterDagStreamingRequestStatIndex, overflow ? 0u : 1u);
        DebugPrintStats.InterlockedAdd(4u * kClusterDagStreamingFallbackStatIndex, 1u);
        if (overflow)
        {
            DebugPrintStats.InterlockedAdd(4u * kClusterDagStreamingRequestOverflowStatIndex, 1u);
        }
    }
}

void RequestClusterDagStreamingPage(
    uint streamingResourceId,
    uint pageIndex,
    uint priority,
    uint requestCapacity,
    RWStructuredBuffer<ClusterDagStreamingRequest> StreamingRequests,
    uint debugPrintStatsIndex)
{
    uint requestIndex = 0u;
    InterlockedAdd(StreamingRequests[0].StreamingResourceId, 1u, requestIndex);
    const bool overflow = requestIndex >= requestCapacity;
    RecordClusterDagStreamingRequest(debugPrintStatsIndex, overflow);
    if (overflow)
    {
        return;
    }

    ClusterDagStreamingRequest request;
    request.StreamingResourceId = streamingResourceId;
    request.PageIndex = pageIndex;
    request.Priority = priority;
    request.Flags = 0u;
    StreamingRequests[requestIndex + 1u] = request;
}

bool ShouldRefineClusterDagStreamingPage(
    bool streamingEnabled,
    ClusterDagGroupData nextGroup,
    uint streamingResourceId,
    uint requestCapacity,
    StructuredBuffer<ClusterDagPageTableEntry> PageTable,
    RWStructuredBuffer<ClusterDagStreamingRequest> StreamingRequests,
    uint debugPrintStatsIndex)
{
    if (!streamingEnabled)
    {
        return true;
    }

    const uint pageIndex = GetClusterDagGroupPageIndex(nextGroup);
    if (pageIndex == kClusterDagRootPageIndex || IsClusterDagPageResident(pageIndex, PageTable))
    {
        return true;
    }

    RequestClusterDagStreamingPage(
        streamingResourceId,
        pageIndex,
        max(asuint(nextGroup.ParentLODError), 1u),
        requestCapacity,
        StreamingRequests,
        debugPrintStatsIndex);
    return false;
}

uint ReserveClusterDagVisibleEntry(
    uint clusterIndex,
    uint drawDataIndex,
    bool rasterizeSW,
    RWStructuredBuffer<ClusterDagVisibleEntry> VisibleEntries,
    RWByteAddressBuffer VisibleEntryCounters,
    RWStructuredBuffer<uint> HwVisibleEntryIndices,
    RWStructuredBuffer<uint> SwVisibleEntryIndices,
    RWStructuredBuffer<uint> DrawDataVisibleEntryIndices)
{
    uint visibleEntryIndex = 0u;
    VisibleEntryCounters.InterlockedAdd(0u, 1u, visibleEntryIndex);
    ClusterDagVisibleEntry visibleEntry;
    visibleEntry.ClusterIndex = clusterIndex;
    visibleEntry.DrawDataIndex = drawDataIndex;
    VisibleEntries[visibleEntryIndex] = visibleEntry;
    DrawDataVisibleEntryIndices[drawDataIndex] = visibleEntryIndex;

    uint pathEntryIndex = 0u;
    if (rasterizeSW)
    {
        VisibleEntryCounters.InterlockedAdd(8u, 1u, pathEntryIndex);
        SwVisibleEntryIndices[pathEntryIndex] = visibleEntryIndex;
    }
    else
    {
        VisibleEntryCounters.InterlockedAdd(4u, 1u, pathEntryIndex);
        HwVisibleEntryIndices[pathEntryIndex] = visibleEntryIndex;
    }

    return visibleEntryIndex;
}

void EmitClusterDagHWCommand(
    uint drawDataIndex,
    ClusterDagDrawData drawData,
    ByteAddressBuffer CommandTemplates,
    RWByteAddressBuffer OutputCommands,
    RWByteAddressBuffer RunCounts)
{
    uint runOffset = 0u;
    RunCounts.InterlockedAdd(drawData.RangeIndex * 4u, 1u, runOffset);
    const uint outputIndex = drawData.RangeCommandStart + runOffset;
    CopyClusterDagCommandTemplate(drawDataIndex, outputIndex, CommandTemplates, OutputCommands);
}

#if USE_CLUSTER_DAG_DEBUG
void DebugDrawClusterBoundsCross(uint debugLineBufferIndex, float3 center, float radius, uint packedColor)
{
    const float extent = max(radius, 0.01f);
    DebugDrawLine(debugLineBufferIndex, center + float3(-extent, 0.0f, 0.0f), center + float3(extent, 0.0f, 0.0f), packedColor);
    DebugDrawLine(debugLineBufferIndex, center + float3(0.0f, -extent, 0.0f), center + float3(0.0f, extent, 0.0f), packedColor);
    DebugDrawLine(debugLineBufferIndex, center + float3(0.0f, 0.0f, -extent), center + float3(0.0f, 0.0f, extent), packedColor);
}

void RecordFrustumCulled(uint debugPrintStatsIndex, bool isLeaf)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        const uint statIndex = isLeaf ? kDebugPrintStatsClusterDagLeafFrustumCulledIndex : kClusterDagNonLeafFrustumCulledStatIndex;
        DebugPrintStats.InterlockedAdd(4u * statIndex, 1u);
    }
}

void RecordStackOverflow(uint debugPrintStatsIndex)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagStackOverflowIndex, 1u);
    }
}

void RecordExpandedOverflow(uint debugPrintStatsIndex)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagExpandedOverflowIndex, 1u);
    }
}

void RecordIterationOverflow(uint debugPrintStatsIndex)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagIterationOverflowIndex, 1u);
    }
}

void RecordVisibleCluster(uint debugPrintStatsIndex, bool isLeaf, uint mipLevel)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagVisibleIndex, 1u);
        if (isLeaf)
        {
            DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagLeafVisibleIndex, 1u);
        }
        else
        {
            DebugPrintStats.InterlockedAdd(4u * kClusterDagNonLeafVisibleStatIndex, 1u);
        }

        const uint mipBucket = min(mipLevel, kClusterDagVisibleMipHistogramBucketCount - 1u);
        DebugPrintStats.InterlockedAdd(4u * (kClusterDagVisibleMipHistogramBaseStatIndex + mipBucket), 1u);
    }
}

void RecordRasterPath(uint debugPrintStatsIndex, bool rasterizeSW)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        const uint statIndex = rasterizeSW ? kDebugPrintStatsClusterDagSwRasterIndex : kDebugPrintStatsClusterDagHwRasterIndex;
        DebugPrintStats.InterlockedAdd(4u * statIndex, 1u);
    }
}

void RecordQueueOverflow(uint debugPrintStatsIndex)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagPersistentOverflowIndex, 1u);
    }
}

void RecordCommitSpinIterations(uint debugPrintStatsIndex, uint statIndex, uint spinIterations)
{
    if (spinIterations != 0u && DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * statIndex, spinIterations);
    }
}
#else
void RecordFrustumCulled(uint debugPrintStatsIndex, bool isLeaf) {}
void RecordStackOverflow(uint debugPrintStatsIndex) {}
void RecordExpandedOverflow(uint debugPrintStatsIndex) {}
void RecordIterationOverflow(uint debugPrintStatsIndex) {}
void RecordVisibleCluster(uint debugPrintStatsIndex, bool isLeaf, uint mipLevel) {}
void RecordRasterPath(uint debugPrintStatsIndex, bool rasterizeSW) {}
void RecordQueueOverflow(uint debugPrintStatsIndex) {}
void RecordCommitSpinIterations(uint debugPrintStatsIndex, uint statIndex, uint spinIterations) {}
#endif

#if USE_CLUSTER_DAG_FAST
void TrackPoppedClusterDagGroup(RWByteAddressBuffer QueueState) {}
void TrackQueuedClusterDagGroup(RWByteAddressBuffer QueueState, uint queueSlot) {}
void TrackClusterDagGroupDedup(RWByteAddressBuffer QueueState) {}
void TrackClusterDagQueueOverflow(RWByteAddressBuffer QueueState) {}
void TrackQueuedClusterDagCandidate(RWByteAddressBuffer QueueState, uint queueSlot) {}
void TrackVisibleClusterDagCandidate(RWByteAddressBuffer QueueState) {}
void TrackLevelSplitClusterDagGroupDedup(RWByteAddressBuffer QueueState) {}
void TrackLevelSplitClusterDagQueueOverflow(RWByteAddressBuffer QueueState) {}
void TrackLevelSplitVisibleClusterDagCandidate(RWByteAddressBuffer QueueState) {}
#else
void TrackPoppedClusterDagGroup(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kQueueStatePass0GroupCountOffset, 0xffffffffu, previous);
}

void TrackQueuedClusterDagGroup(RWByteAddressBuffer QueueState, uint queueSlot)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kQueueStatePass0GroupCountOffset, 1u, previous);
    QueueState.InterlockedMax(kQueueStatePeakGroupQueueDepthOffset, queueSlot + 1u);
}

void TrackClusterDagGroupDedup(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kQueueStateGroupDedupCountOffset, 1u, previous);
}

void TrackClusterDagQueueOverflow(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kQueueStateQueueOverflowCountOffset, 1u, previous);
}

void TrackQueuedClusterDagCandidate(RWByteAddressBuffer QueueState, uint queueSlot)
{
    QueueState.InterlockedMax(kQueueStatePeakCandidateQueueDepthOffset, queueSlot + 1u);
}

void TrackVisibleClusterDagCandidate(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kQueueStateTotalVisibleClustersOffset, 1u, previous);
}

void TrackLevelSplitClusterDagGroupDedup(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kLevelSplitQueueStateGroupDedupCountOffset, 1u, previous);
}

void TrackLevelSplitClusterDagQueueOverflow(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kLevelSplitQueueStateQueueOverflowCountOffset, 1u, previous);
}

void TrackLevelSplitVisibleClusterDagCandidate(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kLevelSplitQueueStateTotalVisibleClustersOffset, 1u, previous);
}
#endif

bool TryPopClusterDagQueue(RWByteAddressBuffer QueueState, uint readOffset, uint writeOffset, out uint queueIndex)
{
    queueIndex = 0xffffffffu;
    [loop]
    while (true)
    {
        const uint readValue = QueueState.Load(readOffset);
        const uint writeValue = QueueState.Load(writeOffset);
        if (readValue >= writeValue)
        {
            return false;
        }

        uint originalValue = 0u;
        QueueState.InterlockedCompareExchange(readOffset, readValue, readValue + 1u, originalValue);
        if (originalValue == readValue)
        {
            queueIndex = readValue;
            return true;
        }
    }
}

bool TryReserveClusterDagQueueSlot(RWByteAddressBuffer QueueState, uint writeOffset, uint capacity, out uint queueIndex)
{
    queueIndex = 0xffffffffu;
    [loop]
    while (true)
    {
        const uint writeValue = QueueState.Load(writeOffset);
        if (writeValue >= capacity)
        {
            return false;
        }

        uint originalValue = 0u;
        QueueState.InterlockedCompareExchange(writeOffset, writeValue, writeValue + 1u, originalValue);
        if (originalValue == writeValue)
        {
            queueIndex = writeValue;
            return true;
        }
    }
}

void DecrementClusterDagPending(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kQueueStatePendingItemCountOffset, 0xffffffffu, previous);
}

void IncrementClusterDagPending(RWByteAddressBuffer QueueState)
{
    uint previous = 0u;
    QueueState.InterlockedAdd(kQueueStatePendingItemCountOffset, 1u, previous);
}

void MarkClusterDagVisitedCurrentEpoch(RWStructuredBuffer<uint> VisitedGroupEpochs, uint groupIndex, uint currentEpoch, out bool alreadyVisited)
{
    uint previousEpoch = 0u;
    InterlockedExchange(VisitedGroupEpochs[groupIndex], currentEpoch, previousEpoch);
    alreadyVisited = previousEpoch == currentEpoch;
}

void CommitClusterDagQueueSlot(RWByteAddressBuffer QueueState, uint committedWriteOffset, uint claimedIndex, uint debugPrintStatsIndex, uint commitSpinStatIndex)
{
    uint spinIterations = 0u;
    [loop]
    while (true)
    {
        uint orig = 0u;
        QueueState.InterlockedCompareExchange(committedWriteOffset, claimedIndex, claimedIndex + 1u, orig);
        if (orig == claimedIndex)
        {
            break;
        }
        spinIterations += 1u;
        DeviceMemoryBarrier();
    }

    RecordCommitSpinIterations(debugPrintStatsIndex, commitSpinStatIndex, spinIterations);
}

#endif // CLUSTER_DAG_TRAVERSAL_COMMON_HLSL
