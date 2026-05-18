#include "ClusterDagTraversalCommon.hlsl"
#include "../CullMeshletCommon.hlsl"

cbuffer ClusterDagLevelSplitNodeCullBindlessConstants : register(b1)
{
    uint GroupBufferIndex;
    uint ClusterBufferIndex;
    uint ChildRefBufferIndex;
    uint QueueStateBufferIndex;
    uint CurrentNodeCandidateBufferIndex;
    uint NextNodeCandidateBufferIndex;
    uint NextNodeArgsBufferIndex;
    uint CandidateClusterQueueBufferIndex;
    uint VisitedGroupEpochBufferIndex;
    uint GroupCount;
    uint ClusterCount;
    uint ChildRefCount;
    uint DebugPrintStatsIndex;
    uint DebugLineBufferIndex;
    uint TraversalEpoch;
    uint PageTableBufferIndex;
    uint PageDataBufferIndex;
    uint StreamingRequestBufferIndex;
    uint StreamingRequestCapacity;
    uint StreamingResourceId;
    uint PageSlotBytes;
};

groupshared uint SharedNextGroups[kClusterDagLevelSplitMaxChildRefsPerGroup];
groupshared uint SharedCandidateClusters[kClusterDagLevelSplitMaxChildRefsPerGroup];
groupshared uint SharedCandidatePageBases[kClusterDagLevelSplitMaxChildRefsPerGroup];
groupshared uint SharedNextGroupCount;
groupshared uint SharedCandidateCount;
groupshared uint SharedNextGroupOffset;
groupshared uint SharedCandidateOffset;
groupshared uint SharedNextGroupWriteCount;
groupshared uint SharedCandidateWriteCount;

bool TryReserveLevelSplitRange(RWByteAddressBuffer Buffer, uint writeOffset, uint capacity, uint count, out uint startOffset, out uint reservedCount)
{
    startOffset = 0u;
    reservedCount = 0u;
    if (count == 0u)
    {
        return true;
    }

    [loop]
    while (true)
    {
        const uint writeValue = Buffer.Load(writeOffset);
        if (writeValue >= capacity)
        {
            return false;
        }

        const uint available = capacity - writeValue;
        const uint reserveCount = min(count, available);
        uint originalValue = 0u;
        Buffer.InterlockedCompareExchange(writeOffset, writeValue, writeValue + reserveCount, originalValue);
        if (originalValue == writeValue)
        {
            startOffset = writeValue;
            reservedCount = reserveCount;
            return reserveCount == count;
        }
    }
}

void AppendLevelSplitNextGroup(uint groupIndex)
{
    uint localIndex = 0u;
    InterlockedAdd(SharedNextGroupCount, 1u, localIndex);
    if (localIndex < kClusterDagLevelSplitMaxChildRefsPerGroup)
    {
        SharedNextGroups[localIndex] = groupIndex;
    }
}

void AppendLevelSplitCandidateCluster(uint clusterIndex, uint pageDataBase)
{
    uint localIndex = 0u;
    InterlockedAdd(SharedCandidateCount, 1u, localIndex);
    if (localIndex < kClusterDagLevelSplitMaxChildRefsPerGroup)
    {
        SharedCandidateClusters[localIndex] = clusterIndex;
        SharedCandidatePageBases[localIndex] = pageDataBase;
    }
}

[numthreads(kClusterDagLevelSplitNodeThreadGroupSize, 1, 1)]
void ClusterDagLevelSplitNodeCullCS(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    StructuredBuffer<ClusterDagGroupData> Groups = ResourceDescriptorHeap[GroupBufferIndex];
    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterBufferIndex];
    StructuredBuffer<ClusterChildRef> ChildRefs = ResourceDescriptorHeap[ChildRefBufferIndex];
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    StructuredBuffer<uint> CurrentNodeCandidates = ResourceDescriptorHeap[CurrentNodeCandidateBufferIndex];
    RWStructuredBuffer<uint> NextNodeCandidates = ResourceDescriptorHeap[NextNodeCandidateBufferIndex];
    RWByteAddressBuffer NextNodeArgs = ResourceDescriptorHeap[NextNodeArgsBufferIndex];
    RWStructuredBuffer<ClusterDagCandidateClusterEntry> CandidateClusterQueue = ResourceDescriptorHeap[CandidateClusterQueueBufferIndex];
    RWStructuredBuffer<uint> VisitedGroupEpochs = ResourceDescriptorHeap[VisitedGroupEpochBufferIndex];
    const bool streamingEnabled = PageTableBufferIndex != 0xffffffffu && StreamingRequestBufferIndex != 0xffffffffu && StreamingRequestCapacity > 0u;
    const bool pagedFetchEnabled = streamingEnabled && PageDataBufferIndex != 0xffffffffu && PageSlotBytes > 0u;
    StructuredBuffer<ClusterDagPageTableEntry> PageTable = ResourceDescriptorHeap[streamingEnabled ? PageTableBufferIndex : GroupBufferIndex];
    ByteAddressBuffer PageData = ResourceDescriptorHeap[pagedFetchEnabled ? PageDataBufferIndex : CurrentNodeCandidateBufferIndex];
    RWStructuredBuffer<ClusterDagStreamingRequest> StreamingRequests = ResourceDescriptorHeap[streamingEnabled ? StreamingRequestBufferIndex : VisitedGroupEpochBufferIndex];

    if (groupThreadIndex == 0u)
    {
        SharedNextGroupCount = 0u;
        SharedCandidateCount = 0u;
        SharedNextGroupOffset = 0u;
        SharedCandidateOffset = 0u;
        SharedNextGroupWriteCount = 0u;
        SharedCandidateWriteCount = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint candidateIndex = groupId.x;
    const uint groupIndex = CurrentNodeCandidates[candidateIndex];
#if USE_CLUSTER_DAG_FAST
    if (true)
#else
    if (groupIndex < GroupCount)
#endif
    {
        const ClusterDagGroupData fallbackGroup = Groups[groupIndex];
        bool usePagedChildRefs = false;
        uint pagedPageBase = 0u;
        uint pagedChildRefBase = 0u;
        ClusterDagGroupData group = fallbackGroup;
        TryLoadClusterDagPagedGroup(
            groupIndex,
            fallbackGroup,
            pagedFetchEnabled,
            PageTable,
            PageData,
            PageSlotBytes,
            group,
            usePagedChildRefs,
            pagedPageBase,
            pagedChildRefBase);
#if USE_CLUSTER_DAG_FAST
        if (true)
#else
        if (group.ChildRefStart <= ChildRefCount && group.ChildRefCount <= ChildRefCount - group.ChildRefStart)
#endif
        {
            const uint childRefCount = min(group.ChildRefCount, kClusterDagLevelSplitMaxChildRefsPerGroup);
#if !USE_CLUSTER_DAG_FAST
            if (groupThreadIndex == 0u && group.ChildRefCount > kClusterDagLevelSplitMaxChildRefsPerGroup)
            {
                TrackClusterDagQueueOverflow(QueueState);
                RecordQueueOverflow(DebugPrintStatsIndex);
            }
#endif
            for (uint childOffset = groupThreadIndex; childOffset < childRefCount; childOffset += kClusterDagLevelSplitNodeThreadGroupSize)
            {
                const ClusterChildRef childRef = LoadClusterDagChildRef(childOffset, group, usePagedChildRefs, pagedChildRefBase, ChildRefs, PageData);
#if !USE_CLUSTER_DAG_FAST
                if (childRef.ClusterIndex == 0xffffffffu || childRef.ClusterIndex >= ClusterCount)
                {
                    continue;
                }
#endif

                const ClusterDagClusterData cluster = LoadClusterDagCluster(childRef.ClusterIndex, usePagedChildRefs, pagedPageBase, Clusters, PageData);
                const bool isLeaf = cluster.GeneratingGroupIndex == 0xffffffffu;
                const float3 lodCenter = cluster.LodBounds.xyz;
                const float lodRadius = cluster.LodBounds.w;
                const bool skipFrustumCull = ClusterDAGForceMipEnabled != 0u && ClusterDAGForceMipSkipFrustumCull != 0u;
                if (!skipFrustumCull && !IsSphereVisible(lodCenter, lodRadius))
                {
                    RecordFrustumCulled(DebugPrintStatsIndex, isLeaf);
#if USE_CLUSTER_DAG_DEBUG
                    if (ClusterDAGForceMipEnabled != 0u && DebugPrintEnabled != 0u && DebugLineBufferIndex != 0xffffffffu && isLeaf)
                    {
                        DebugDrawClusterBoundsCross(DebugLineBufferIndex, lodCenter, lodRadius, 0xff3030ffu);
                    }
#endif
                    continue;
                }

                bool refine = false;
                if (ClusterDAGForceMipEnabled != 0u)
                {
                    refine = cluster.MipLevel > ClusterDAGForceMipLevel && !isLeaf;
                }
                else if (!isLeaf)
                {
#if !USE_CLUSTER_DAG_FAST
                    if (cluster.GeneratingGroupIndex >= GroupCount)
                    {
                        continue;
                    }
#endif
                    const float projectedError = ComputeProjectedErrorPixels(lodCenter, cluster.LODError);
                    refine = projectedError > ClusterDAGTargetErrorPixels;
                }

                if (refine)
                {
#if !USE_CLUSTER_DAG_FAST
                    if (cluster.GeneratingGroupIndex >= GroupCount)
                    {
                        refine = false;
                    }
                    else
#endif
                    {
#if !USE_CLUSTER_DAG_FAST
                        const ClusterDagGroupData nextGroup = Groups[cluster.GeneratingGroupIndex];
                        if (nextGroup.ChildRefStart > ChildRefCount || nextGroup.ChildRefCount > ChildRefCount - nextGroup.ChildRefStart || nextGroup.ChildRefCount == 0u)
                        {
                            refine = false;
                        }
                        else if (!ShouldRefineClusterDagStreamingPage(streamingEnabled, nextGroup, StreamingResourceId, StreamingRequestCapacity, PageTable, StreamingRequests, DebugPrintStatsIndex))
                        {
                            refine = false;
                        }
                        else
#endif
                        {
                            bool alreadyVisited = false;
                            MarkClusterDagVisitedCurrentEpoch(VisitedGroupEpochs, cluster.GeneratingGroupIndex, TraversalEpoch, alreadyVisited);
                            if (alreadyVisited)
                            {
                                TrackLevelSplitClusterDagGroupDedup(QueueState);
                            }
                            else
                            {
                                AppendLevelSplitNextGroup(cluster.GeneratingGroupIndex);
                            }
                        }
                    }
                }

                if (!refine)
                {
                    AppendLevelSplitCandidateCluster(childRef.ClusterIndex, usePagedChildRefs ? pagedPageBase : 0xffffffffu);
                }
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    if (groupThreadIndex == 0u)
    {
        const uint nextGroupCount = min(SharedNextGroupCount, kClusterDagLevelSplitMaxChildRefsPerGroup);
        const uint candidateCount = min(SharedCandidateCount, kClusterDagLevelSplitMaxChildRefsPerGroup);
        bool allNextGroupsReserved = TryReserveLevelSplitRange(
            NextNodeArgs,
            kLevelSplitNodeArgsNodeWriteOffset,
            GroupCount,
            nextGroupCount,
            SharedNextGroupOffset,
            SharedNextGroupWriteCount);
        bool allCandidatesReserved = TryReserveLevelSplitRange(
            QueueState,
            kLevelSplitQueueStateCandidateWriteOffset,
            ClusterCount,
            candidateCount,
            SharedCandidateOffset,
            SharedCandidateWriteCount);

#if !USE_CLUSTER_DAG_FAST
        if (!allNextGroupsReserved || SharedNextGroupCount > kClusterDagLevelSplitMaxChildRefsPerGroup
            || !allCandidatesReserved || SharedCandidateCount > kClusterDagLevelSplitMaxChildRefsPerGroup)
        {
            TrackLevelSplitClusterDagQueueOverflow(QueueState);
            RecordQueueOverflow(DebugPrintStatsIndex);
        }
        QueueState.InterlockedMax(kLevelSplitQueueStatePeakGroupQueueDepthOffset, SharedNextGroupOffset + SharedNextGroupWriteCount);
        QueueState.InterlockedMax(kLevelSplitQueueStatePeakCandidateQueueDepthOffset, SharedCandidateOffset + SharedCandidateWriteCount);
#endif
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint writeIndex = groupThreadIndex; writeIndex < SharedNextGroupWriteCount; writeIndex += kClusterDagLevelSplitNodeThreadGroupSize)
    {
        NextNodeCandidates[SharedNextGroupOffset + writeIndex] = SharedNextGroups[writeIndex];
    }

    for (uint writeIndex = groupThreadIndex; writeIndex < SharedCandidateWriteCount; writeIndex += kClusterDagLevelSplitNodeThreadGroupSize)
    {
        ClusterDagCandidateClusterEntry candidateEntry;
        candidateEntry.ClusterIndex = SharedCandidateClusters[writeIndex];
        candidateEntry.PageDataBase = SharedCandidatePageBases[writeIndex];
        CandidateClusterQueue[SharedCandidateOffset + writeIndex] = candidateEntry;
    }
}
