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

uint GetClusterDagPhysicalPageSlot(uint pageIndex, StructuredBuffer<ClusterDagPageTableEntry> PageTable)
{
    return PageTable[pageIndex].PhysicalPageIndex;
}

ClusterDagGroupData LoadClusterDagGroupFromPage(uint pageBase, uint groupByteOffset, ByteAddressBuffer PageData)
{
    const uint groupBase = pageBase + groupByteOffset;
    ClusterDagGroupData group;
    group.Bounds = asfloat(PageData.Load4(groupBase + kClusterDagGpuPageGroupBoundsOffset));
    group.LodBounds = asfloat(PageData.Load4(groupBase + kClusterDagGpuPageGroupLodBoundsOffset));
    group.ParentLODError = asfloat(PageData.Load(groupBase + kClusterDagGpuPageGroupParentLODErrorOffset));
    group.ChildRefStart = PageData.Load(groupBase + kClusterDagGpuPageGroupChildRefStartOffset);
    group.ChildRefCount = PageData.Load(groupBase + kClusterDagGpuPageGroupChildRefCountOffset);
    group.Flags = PageData.Load(groupBase + kClusterDagGpuPageGroupFlagsOffset);
    group.MipLevel = PageData.Load(groupBase + kClusterDagGpuPageGroupMipLevelOffset);
    return group;
}

bool TryLoadClusterDagPagedGroup(
    uint groupIndex,
    ClusterDagGroupData fallbackGroup,
    bool pagedFetchEnabled,
    StructuredBuffer<ClusterDagPageTableEntry> PageTable,
    ByteAddressBuffer PageData,
    uint pageSlotBytes,
    out ClusterDagGroupData group,
    out bool usePagedChildRefs,
    out uint pagedPageBase,
    out uint pagedChildRefBase)
{
    group = fallbackGroup;
    usePagedChildRefs = false;
    pagedPageBase = 0u;
    pagedChildRefBase = 0u;
    if (!pagedFetchEnabled)
    {
        return false;
    }

    const uint pageIndex = GetClusterDagGroupPageIndex(fallbackGroup);
    if (pageIndex == kClusterDagRootPageIndex || !IsClusterDagPageResident(pageIndex, PageTable))
    {
        return false;
    }

    const uint physicalSlot = GetClusterDagPhysicalPageSlot(pageIndex, PageTable);
    if (physicalSlot == 0xffffffffu)
    {
        return false;
    }

    const uint pageBase = physicalSlot * pageSlotBytes;
    pagedPageBase = pageBase;
    if (PageData.Load(pageBase + kClusterDagGpuPageHeaderMagicOffset) != kClusterDagGpuPagePayloadMagic
        || PageData.Load(pageBase + kClusterDagGpuPageHeaderVersionOffset) != kClusterDagGpuPagePayloadVersion
        || PageData.Load(pageBase + kClusterDagGpuPageHeaderPageIndexOffset) != pageIndex
        || PageData.Load(pageBase + kClusterDagGpuPageHeaderGlobalGroupIndexOffset) != groupIndex)
    {
        return false;
    }

    const uint groupByteOffset = PageData.Load(pageBase + kClusterDagGpuPageHeaderGroupByteOffsetOffset);
    const uint childRefByteOffset = PageData.Load(pageBase + kClusterDagGpuPageHeaderChildRefByteOffsetOffset);
    const uint childRefCount = PageData.Load(pageBase + kClusterDagGpuPageHeaderChildRefCountOffset);
    group = LoadClusterDagGroupFromPage(pageBase, groupByteOffset, PageData);
    group.ChildRefStart = 0u;
    group.ChildRefCount = childRefCount;
    usePagedChildRefs = true;
    pagedChildRefBase = pageBase + childRefByteOffset;
    return true;
}

ClusterDagClusterData LoadClusterDagClusterFromPageRecord(uint recordBase, ByteAddressBuffer PageData)
{
    ClusterDagClusterData cluster;
    cluster.Bounds = asfloat(PageData.Load4(recordBase + kClusterDagGpuPageClusterRecordBoundsOffset));
    cluster.LodBounds = asfloat(PageData.Load4(recordBase + kClusterDagGpuPageClusterRecordLodBoundsOffset));
    cluster.LODError = asfloat(PageData.Load(recordBase + kClusterDagGpuPageClusterRecordLODErrorOffset));
    cluster.MaxEdgeLength = asfloat(PageData.Load(recordBase + kClusterDagGpuPageClusterRecordMaxEdgeLengthOffset));
    cluster.GroupIndex = PageData.Load(recordBase + kClusterDagGpuPageClusterRecordGroupIndexOffset);
    cluster.GeneratingGroupIndex = PageData.Load(recordBase + kClusterDagGpuPageClusterRecordGeneratingGroupIndexOffset);
    cluster.DrawDataStart = PageData.Load(recordBase + kClusterDagGpuPageClusterRecordDrawDataStartOffset);
    cluster.DrawDataCount = PageData.Load(recordBase + kClusterDagGpuPageClusterRecordDrawDataCountOffset);
    cluster.TriangleCount = PageData.Load(recordBase + kClusterDagGpuPageClusterRecordTriangleCountOffset);
    cluster.MipLevel = PageData.Load(recordBase + kClusterDagGpuPageClusterRecordMipLevelOffset);
    return cluster;
}

bool TryLoadClusterDagPagedCluster(
    uint clusterIndex,
    bool usePagedPage,
    uint pagedPageBase,
    ByteAddressBuffer PageData,
    out ClusterDagClusterData cluster)
{
    cluster = (ClusterDagClusterData)0;
    if (!usePagedPage)
    {
        return false;
    }

    const uint clusterRecordBase = PageData.Load(pagedPageBase + kClusterDagGpuPageHeaderClusterRecordByteOffsetOffset);
    const uint clusterRecordCount = PageData.Load(pagedPageBase + kClusterDagGpuPageHeaderClusterRecordCountOffset);
    [loop]
    for (uint recordIndex = 0u; recordIndex < clusterRecordCount; ++recordIndex)
    {
        const uint recordBase = pagedPageBase + clusterRecordBase + recordIndex * kClusterDagGpuPageClusterRecordStride;
        if (PageData.Load(recordBase + kClusterDagGpuPageClusterRecordGlobalClusterIndexOffset) == clusterIndex)
        {
            cluster = LoadClusterDagClusterFromPageRecord(recordBase, PageData);
            return true;
        }
    }

    return false;
}

ClusterDagDrawData LoadClusterDagDrawDataFromPageRecord(uint recordBase, ByteAddressBuffer PageData)
{
    ClusterDagDrawData drawData;
    drawData.StartIndex = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordStartIndexOffset);
    drawData.IndexCount = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordIndexCountOffset);
    drawData.RangeIndex = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordRangeIndexOffset);
    drawData.RangeCommandStart = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordRangeCommandStartOffset);
    drawData.RangeCommandCount = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordRangeCommandCountOffset);
    drawData.ModelIndex = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordModelIndexOffset);
    return drawData;
}

bool TryLoadClusterDagPagedDrawData(
    uint drawDataIndex,
    bool usePagedPage,
    uint pagedPageBase,
    ByteAddressBuffer PageData,
    out ClusterDagDrawData drawData)
{
    drawData = (ClusterDagDrawData)0;
    if (!usePagedPage)
    {
        return false;
    }

    const uint drawDataRecordBase = PageData.Load(pagedPageBase + kClusterDagGpuPageHeaderDrawDataRecordByteOffsetOffset);
    const uint drawDataRecordCount = PageData.Load(pagedPageBase + kClusterDagGpuPageHeaderDrawDataRecordCountOffset);
    [loop]
    for (uint recordIndex = 0u; recordIndex < drawDataRecordCount; ++recordIndex)
    {
        const uint recordBase = pagedPageBase + drawDataRecordBase + recordIndex * kClusterDagGpuPageDrawDataRecordStride;
        if (PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordGlobalDrawDataIndexOffset) == drawDataIndex)
        {
            drawData = LoadClusterDagDrawDataFromPageRecord(recordBase, PageData);
            return true;
        }
    }

    return false;
}

ClusterChildRef LoadClusterDagChildRef(
    uint childOffset,
    ClusterDagGroupData group,
    bool usePagedChildRefs,
    uint pagedChildRefBase,
    StructuredBuffer<ClusterChildRef> ChildRefs,
    ByteAddressBuffer PageData)
{
    if (usePagedChildRefs)
    {
        ClusterChildRef childRef;
        const uint childRefBase = pagedChildRefBase + childOffset * kClusterDagGpuPageChildRefStride;
        childRef.InstanceIndex = PageData.Load(childRefBase);
        childRef.ClusterIndex = PageData.Load(childRefBase + 4u);
        return childRef;
    }

    return ChildRefs[group.ChildRefStart + childOffset];
}

ClusterDagClusterData LoadClusterDagCluster(
    uint clusterIndex,
    bool usePagedPage,
    uint pagedPageBase,
    StructuredBuffer<ClusterDagClusterData> Clusters,
    ByteAddressBuffer PageData)
{
    ClusterDagClusterData cluster;
    if (TryLoadClusterDagPagedCluster(clusterIndex, usePagedPage, pagedPageBase, PageData, cluster))
    {
        return cluster;
    }

    return Clusters[clusterIndex];
}

ClusterDagDrawData LoadClusterDagDrawData(
    uint drawDataIndex,
    bool usePagedPage,
    uint pagedPageBase,
    StructuredBuffer<ClusterDagDrawData> DrawDatas,
    ByteAddressBuffer PageData)
{
    ClusterDagDrawData drawData;
    if (TryLoadClusterDagPagedDrawData(drawDataIndex, usePagedPage, pagedPageBase, PageData, drawData))
    {
        return drawData;
    }

    return DrawDatas[drawDataIndex];
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
    uint pageDataBase,
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
    visibleEntry.PageDataBase = pageDataBase;
    visibleEntry.Reserved = 0u;
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
    if (runOffset >= drawData.RangeCommandCount)
    {
        return;
    }

    const uint outputIndex = drawData.RangeCommandStart + runOffset;
    CopyClusterDagCommandTemplate(drawDataIndex, outputIndex, CommandTemplates, OutputCommands);
    const uint outputBase = outputIndex * kClusterDagCommandStride;
    OutputCommands.Store(outputBase + 8u, drawData.StartIndex);
    OutputCommands.Store(outputBase + 12u, drawDataIndex);
}

#if USE_CLUSTER_DAG_DEBUG
void DebugDrawClusterBoundsCross(uint debugLineBufferIndex, float3 center, float radius, uint packedColor)
{
    const float extent = max(radius, 0.01f);
    DebugDrawLine(debugLineBufferIndex, center + float3(-extent, 0.0f, 0.0f), center + float3(extent, 0.0f, 0.0f), packedColor);
    DebugDrawLine(debugLineBufferIndex, center + float3(0.0f, -extent, 0.0f), center + float3(0.0f, extent, 0.0f), packedColor);
    DebugDrawLine(debugLineBufferIndex, center + float3(0.0f, 0.0f, -extent), center + float3(0.0f, 0.0f, extent), packedColor);
}

void RecordFrustumCulled(uint debugPrintStatsIndex)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagCulledIndex, 1u);
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

void RecordVisibleCluster(uint debugPrintStatsIndex, uint mipLevel)
{
    if (DebugPrintEnabled != 0u && debugPrintStatsIndex != 0xffffffffu)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[debugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(4u * kDebugPrintStatsClusterDagVisibleIndex, 1u);

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
#else
void RecordFrustumCulled(uint debugPrintStatsIndex) {}
void RecordStackOverflow(uint debugPrintStatsIndex) {}
void RecordExpandedOverflow(uint debugPrintStatsIndex) {}
void RecordIterationOverflow(uint debugPrintStatsIndex) {}
void RecordVisibleCluster(uint debugPrintStatsIndex, uint mipLevel) {}
void RecordRasterPath(uint debugPrintStatsIndex, bool rasterizeSW) {}
void RecordQueueOverflow(uint debugPrintStatsIndex) {}
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

void CommitClusterDagQueueSlot(RWByteAddressBuffer QueueState, uint committedWriteOffset, uint claimedIndex)
{
    [loop]
    while (true)
    {
        uint orig = 0u;
        QueueState.InterlockedCompareExchange(committedWriteOffset, claimedIndex, claimedIndex + 1u, orig);
        if (orig == claimedIndex)
        {
            break;
        }
        DeviceMemoryBarrier();
    }
}

#endif // CLUSTER_DAG_TRAVERSAL_COMMON_HLSL
