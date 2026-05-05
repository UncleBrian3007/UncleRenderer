#include "ClusterDagTraversalCommon.hlsl"
#include "../CullMeshletCommon.hlsl"

cbuffer ClusterDagNodeCullBindlessConstants : register(b1)
{
    uint GroupBufferIndex;
    uint ClusterBufferIndex;
    uint ChildRefBufferIndex;
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

[numthreads(64, 1, 1)]
void ClusterDagNodeCullCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<ClusterDagGroupData> Groups = ResourceDescriptorHeap[GroupBufferIndex];
    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterBufferIndex];
    StructuredBuffer<ClusterChildRef> ChildRefs = ResourceDescriptorHeap[ChildRefBufferIndex];
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    RWStructuredBuffer<uint> GroupQueue = ResourceDescriptorHeap[GroupQueueBufferIndex];
    RWStructuredBuffer<uint> CandidateClusterQueue = ResourceDescriptorHeap[CandidateClusterQueueBufferIndex];
    RWStructuredBuffer<uint> VisitedGroupEpochs = ResourceDescriptorHeap[VisitedGroupEpochBufferIndex];

    const uint currentEpoch = TraversalEpoch;
    const uint maxLoopCount = max(GroupCount * 8u, 1024u);
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

                        uint candidateSlot = 0xffffffffu;
                        if (TryReserveClusterDagQueueSlot(QueueState, kQueueStatePass0CandidateWriteOffset, ClusterCount, candidateSlot))
                        {
                            CandidateClusterQueue[candidateSlot] = childRef.ClusterIndex;
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

            DecrementClusterDagPending(QueueState);
            continue;
        }

        if (QueueState.Load(kQueueStatePendingItemCountOffset) == 0u)
        {
            break;
        }
    }
}
