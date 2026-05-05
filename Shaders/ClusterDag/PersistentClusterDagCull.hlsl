#include "ClusterDagTraversalCommon.hlsl"
#include "../CullMeshletCommon.hlsl"

static const uint kPersistentClusterDagInlineCandidateDrawDataCount = 1u;

cbuffer ClusterDagPersistentBindlessConstants : register(b1)
{
    uint GroupBufferIndex;
    uint ClusterBufferIndex;
    uint ChildRefBufferIndex;
    uint DrawDataIndex;
    uint CommandTemplatesIndex;
    uint OutputCommandsIndex;
    uint RunCountsIndex;
    uint QueueStateBufferIndex;
    uint GroupQueueBufferIndex;
    uint CandidateClusterQueueBufferIndex;
    uint VisitedGroupEpochBufferIndex;
    uint GroupCount;
    uint ClusterCount;
    uint ChildRefCount;
    uint DebugPrintStatsIndex;
    uint DebugLineBufferIndex;
    uint TraversalEpoch;
};

void EmitVisibleClusterDagCandidate(
    ClusterDagClusterData cluster,
    StructuredBuffer<ClusterDagDrawData> DrawDatas,
    ByteAddressBuffer CommandTemplates,
    RWByteAddressBuffer OutputCommands,
    RWByteAddressBuffer RunCounts,
    RWByteAddressBuffer QueueState,
    uint DebugPrintStatsIndex)
{
    const bool isLeaf = cluster.GeneratingGroupIndex == 0xffffffffu;
    TrackVisibleClusterDagCandidate(QueueState);
    RecordVisibleCluster(DebugPrintStatsIndex, isLeaf, cluster.MipLevel);

    [loop]
    for (uint packetOffset = 0u; packetOffset < cluster.DrawDataCount; ++packetOffset)
    {
        const uint drawDataIndex = cluster.DrawDataStart + packetOffset;
        const ClusterDagDrawData drawData = DrawDatas[drawDataIndex];
        uint runOffset = 0u;
        RunCounts.InterlockedAdd(drawData.RangeIndex * 4u, 1u, runOffset);
        const uint outputIndex = drawData.RangeCommandStart + runOffset;
        CopyClusterDagCommandTemplate(drawDataIndex, outputIndex, CommandTemplates, OutputCommands);
    }
}

/*
QueueState
{
    GroupReadOffset;
    GroupWriteOffset;
    GroupCommittedWriteOffset;

    CandidateReadOffset;
    CandidateWriteOffset;
    CandidateCommittedWriteOffset;

    PendingItemCount;
    ...
}
*/
// QueueState: Storage for read/write offsets and pending counters
// GroupQueue: Array containing actual group indices
// CandidateClusterQueue: Array containing actual cluster indices
// queueSlot: The specific index/slot allocated within the queue array
// TryReserveClusterDagQueueSlot: A function that reserves a slot index without writing the actual data

// WriteOffset: Counter used by producers to reserve a slot for writing
// CommittedWriteOffset: Counter indicating the point up to which consumers can safely read
// ReadOffset: The position from which the consumer will read next
// QueueSlot: The actual index within the Queue array

/*
// 1. Reserve a slot
uint queueSlot = Reserve(WriteOffset);

// 2. Record the actual queue payload
GroupQueue[queueSlot] = groupIndex;

// 3. Increment pending count
PendingItemCount++;

// 4. Memory barrier
// Ensures the written payload is globally visible across all threads prior to the commit.
DeviceMemoryBarrier();

// 5. Commit the write
Commit(CommittedWriteOffset, queueSlot);
*/

[numthreads(64, 1, 1)]
void PersistentClusterDagCullCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<ClusterDagGroupData> Groups = ResourceDescriptorHeap[GroupBufferIndex];
    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterBufferIndex];
    StructuredBuffer<ClusterChildRef> ChildRefs = ResourceDescriptorHeap[ChildRefBufferIndex];
    StructuredBuffer<ClusterDagDrawData> DrawDatas = ResourceDescriptorHeap[DrawDataIndex];
    ByteAddressBuffer CommandTemplates = ResourceDescriptorHeap[CommandTemplatesIndex];
    RWByteAddressBuffer OutputCommands = ResourceDescriptorHeap[OutputCommandsIndex];
    RWByteAddressBuffer RunCounts = ResourceDescriptorHeap[RunCountsIndex];
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    RWStructuredBuffer<uint> GroupQueue = ResourceDescriptorHeap[GroupQueueBufferIndex];
    RWStructuredBuffer<uint> CandidateClusterQueue = ResourceDescriptorHeap[CandidateClusterQueueBufferIndex];
    RWStructuredBuffer<uint> VisitedGroupEpochs = ResourceDescriptorHeap[VisitedGroupEpochBufferIndex];

    const uint currentEpoch = TraversalEpoch;
    const uint maxLoopCount = max((GroupCount + ClusterCount) * 8u, 1024u);
    uint loopCount = 0u;

    [loop]
    while (true)
    {
        loopCount += 1u;
        if (loopCount > maxLoopCount)
        {
            RecordIterationOverflow(DebugPrintStatsIndex);
            return;
        }

        uint workIndex = 0xffffffffu;
        if (TryPopClusterDagQueue(QueueState, kQueueStatePass0GroupReadOffset, kQueueStatePass0GroupCommittedWriteOffset, workIndex))
        {
            // workIndex < committed guarantees the slot was fully written before commit was
            // advanced -- no sentinel spin-wait needed.
            const uint groupIndex = GroupQueue[workIndex];

            TrackPoppedClusterDagGroup(QueueState);
#if USE_CLUSTER_DAG_FAST
            if (true)
#else
            if (groupIndex < GroupCount)
#endif
            {
                const ClusterDagGroupData group = Groups[groupIndex];
#if USE_CLUSTER_DAG_FAST
                if (true)
#else
                if (group.ChildRefStart <= ChildRefCount && group.ChildRefCount <= ChildRefCount - group.ChildRefStart)
#endif
                {
                    [loop]
                    for (uint childOffset = 0u; childOffset < group.ChildRefCount; ++childOffset)
                    {
                        const ClusterChildRef childRef = ChildRefs[group.ChildRefStart + childOffset];
#if !USE_CLUSTER_DAG_FAST
                        if (childRef.ClusterIndex == 0xffffffffu || childRef.ClusterIndex >= ClusterCount)
                        {
                            continue;
                        }
#endif

                        const ClusterDagClusterData cluster = Clusters[childRef.ClusterIndex];
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
                                else
#endif
                                {
                                    bool alreadyVisited = false;
                                    MarkClusterDagVisitedCurrentEpoch(VisitedGroupEpochs, cluster.GeneratingGroupIndex, currentEpoch, alreadyVisited);
                                    if (alreadyVisited)
                                    {
                                        TrackClusterDagGroupDedup(QueueState);
                                    }
                                    else
                                    {
                                        uint queueSlot = 0xffffffffu;
                                        if (TryReserveClusterDagQueueSlot(QueueState, kQueueStatePass0GroupWriteOffset, GroupCount, queueSlot))
                                        {
                                            GroupQueue[queueSlot] = cluster.GeneratingGroupIndex;
                                            // Increment pending before committing so consumers
                                            // cannot observe pending==0 between commit and increment.
                                            IncrementClusterDagPending(QueueState);
                                            DeviceMemoryBarrier();
                                            CommitClusterDagQueueSlot(
                                                QueueState,
                                                kQueueStatePass0GroupCommittedWriteOffset,
                                                queueSlot,
                                                DebugPrintStatsIndex,
                                                kDebugPrintStatsClusterDagPersistentGroupCommitSpinIndex);
                                            TrackQueuedClusterDagGroup(QueueState, queueSlot);
                                        }
                                        else
                                        {
                                            TrackClusterDagQueueOverflow(QueueState);
                                            RecordQueueOverflow(DebugPrintStatsIndex);
                                        }
                                    }

                                    continue;
                                }
                            }
                        }

                        if (cluster.DrawDataCount <= kPersistentClusterDagInlineCandidateDrawDataCount)
                        {
                            EmitVisibleClusterDagCandidate(cluster, DrawDatas, CommandTemplates, OutputCommands, RunCounts, QueueState, DebugPrintStatsIndex);
                        }
                        else
                        {
                            uint candidateSlot = 0xffffffffu;
                            if (TryReserveClusterDagQueueSlot(QueueState, kQueueStatePass0CandidateWriteOffset, ClusterCount, candidateSlot))
                            {
                                CandidateClusterQueue[candidateSlot] = childRef.ClusterIndex;
                                IncrementClusterDagPending(QueueState);
                                DeviceMemoryBarrier();
                                CommitClusterDagQueueSlot(
                                    QueueState,
                                    kQueueStatePass0CandidateCommittedWriteOffset,
                                    candidateSlot,
                                    DebugPrintStatsIndex,
                                    kDebugPrintStatsClusterDagPersistentCandidateCommitSpinIndex);
                                TrackQueuedClusterDagCandidate(QueueState, candidateSlot);
                            }
                            else
                            {
                                TrackClusterDagQueueOverflow(QueueState);
                                RecordQueueOverflow(DebugPrintStatsIndex);
                            }
                        }
                    }
                }
            }

            DecrementClusterDagPending(QueueState);
            continue;
        }

        if (TryPopClusterDagQueue(QueueState, kQueueStatePass0CandidateReadOffset, kQueueStatePass0CandidateCommittedWriteOffset, workIndex))
        {
            const uint clusterIndex = CandidateClusterQueue[workIndex];

#if USE_CLUSTER_DAG_FAST
            if (true)
#else
            if (clusterIndex < ClusterCount)
#endif
            {
                const ClusterDagClusterData cluster = Clusters[clusterIndex];
                EmitVisibleClusterDagCandidate(cluster, DrawDatas, CommandTemplates, OutputCommands, RunCounts, QueueState, DebugPrintStatsIndex);
            }

            DecrementClusterDagPending(QueueState);
            continue;
        }

        if (QueueState.Load(kQueueStatePendingItemCountOffset) == 0u)
        {
            break;
        }
    }
}
