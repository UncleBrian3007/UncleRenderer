#include "../CullingConstants.hlsl"
#include "ClusterDagCommon.hlsl"
#include "../GpuDebug/GpuDebugPrintCommon.hlsl"

#ifndef USE_CLUSTER_DAG_FAST
#define USE_CLUSTER_DAG_FAST 0
#endif

#ifndef USE_CLUSTER_DAG_PERSISTENT_QUEUE
#define USE_CLUSTER_DAG_PERSISTENT_QUEUE 0
#endif

cbuffer ClusterDagInitBindlessConstants : register(b1)
{
    uint RootGroupBufferIndex;
    uint QueueStateBufferIndex;
    uint GroupQueueBufferIndex;
    uint CandidateClusterQueueBufferIndex;
    uint VisitedGroupEpochBufferIndex;
    uint RunCountBufferIndex;
    uint RootGroupCount;
    uint GroupQueueCapacity;
    uint CandidateQueueCapacity;
    uint GroupCount;
    uint TraversalEpoch;
    uint DebugPrintStatsIndex;
    uint VisibleEntryCountersIndex;
    uint DrawDataVisibleEntryIndicesIndex;
};

[numthreads(64, 1, 1)]
void InitClusterDagQueuesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint threadIndex = dispatchThreadId.x;

    StructuredBuffer<uint> RootGroups = ResourceDescriptorHeap[RootGroupBufferIndex];
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    RWStructuredBuffer<uint> GroupQueue = ResourceDescriptorHeap[GroupQueueBufferIndex];
    RWStructuredBuffer<ClusterDagCandidateClusterEntry> CandidateClusterQueue = ResourceDescriptorHeap[CandidateClusterQueueBufferIndex];
    RWStructuredBuffer<uint> VisitedGroupEpochs = ResourceDescriptorHeap[VisitedGroupEpochBufferIndex];
    RWByteAddressBuffer RunCounts = ResourceDescriptorHeap[RunCountBufferIndex];
    RWByteAddressBuffer VisibleEntryCounters = ResourceDescriptorHeap[VisibleEntryCountersIndex];
    RWStructuredBuffer<uint> DrawDataVisibleEntryIndices = ResourceDescriptorHeap[DrawDataVisibleEntryIndicesIndex];

    if (threadIndex == 0u)
    {
        VisibleEntryCounters.Store(0u, 0u);
        VisibleEntryCounters.Store(4u, 0u);
        VisibleEntryCounters.Store(8u, 0u);
#if !USE_CLUSTER_DAG_FAST
        QueueState.Store(kQueueStateTotalVisibleClustersOffset, 0u);
        QueueState.Store(kQueueStatePeakGroupQueueDepthOffset, RootGroupCount);
        QueueState.Store(kQueueStatePeakCandidateQueueDepthOffset, 0u);
#endif
        QueueState.Store(kQueueStatePendingItemCountOffset, RootGroupCount);
#if !USE_CLUSTER_DAG_FAST
        QueueState.Store(kQueueStateGroupDedupCountOffset, 0u);
        QueueState.Store(kQueueStateQueueOverflowCountOffset, 0u);
#endif
        QueueState.Store(kQueueStatePass0CandidateReadOffset, 0u);
        QueueState.Store(kQueueStatePass0CandidateWriteOffset, 0u);
        QueueState.Store(kQueueStatePass0GroupReadOffset, 0u);
        QueueState.Store(kQueueStatePass0GroupWriteOffset, RootGroupCount);
        // Committed-write offsets for the persistent queue.  Root groups are written
        // by this init dispatch before the persistent kernel runs, so the group
        // committed counter starts equal to the claimed-write counter.  No candidate
        // slots have been written yet, so the candidate committed counter starts at 0.
        QueueState.Store(kQueueStatePass0GroupCommittedWriteOffset, RootGroupCount);
        QueueState.Store(kQueueStatePass0CandidateCommittedWriteOffset, 0u);
#if !USE_CLUSTER_DAG_FAST
        QueueState.Store(kQueueStatePass0GroupCountOffset, RootGroupCount);
        QueueState.Store(kQueueStatePass1CandidateReadOffset, 0u);
        QueueState.Store(kQueueStatePass1CandidateWriteOffset, 0u);
        QueueState.Store(kQueueStatePass1GroupReadOffset, 0u);
        QueueState.Store(kQueueStatePass1GroupWriteOffset, 0u);
        QueueState.Store(kQueueStatePass1GroupCountOffset, 0u);

#endif
    }

    if (threadIndex < RangeCount)
    {
        RunCounts.Store(threadIndex * 4u, 0u);
    }

    if (threadIndex < IndirectCommandCount)
    {
        DrawDataVisibleEntryIndices[threadIndex] = 0xffffffffu;
    }

#if !USE_CLUSTER_DAG_PERSISTENT_QUEUE
    if (threadIndex < GroupQueueCapacity)
    {
        GroupQueue[threadIndex] = 0xffffffffu;
    }

    if (threadIndex < CandidateQueueCapacity)
    {
        ClusterDagCandidateClusterEntry emptyEntry;
        emptyEntry.ClusterIndex = 0xffffffffu;
        emptyEntry.PageDataBase = 0xffffffffu;
        CandidateClusterQueue[threadIndex] = emptyEntry;
    }
#endif

    if (threadIndex < RootGroupCount)
    {
        const uint rootGroupIndex = RootGroups[threadIndex];
        GroupQueue[threadIndex] = rootGroupIndex;
        if (rootGroupIndex < GroupCount)
        {
            VisitedGroupEpochs[rootGroupIndex] = TraversalEpoch;
        }
    }
}
