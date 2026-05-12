#include "../CullingConstants.hlsl"
#include "ClusterDagCommon.hlsl"
#include "../GpuDebug/GpuDebugPrintCommon.hlsl"

#ifndef USE_CLUSTER_DAG_FAST
#define USE_CLUSTER_DAG_FAST 0
#endif

cbuffer ClusterDagLevelSplitInitBindlessConstants : register(b1)
{
    uint RootGroupBufferIndex;
    uint QueueStateBufferIndex;
    uint NodeCandidateBuffer0Index;
    uint NodeCandidateBuffer1Index;
    uint NodeArgsBuffer0Index;
    uint NodeArgsBuffer1Index;
    uint CandidateClusterQueueBufferIndex;
    uint VisitedGroupEpochBufferIndex;
    uint RunCountBufferIndex;
    uint RootGroupCount;
    uint GroupCount;
    uint CandidateQueueCapacity;
    uint TraversalEpoch;
    uint DebugPrintStatsIndex;
    uint VisibleEntryCountersIndex;
    uint DrawDataVisibleEntryIndicesIndex;
};

void ClearLevelSplitNodeArgs(RWByteAddressBuffer Args)
{
    Args.Store(kLevelSplitNodeArgsDispatchXOffset, 0u);
    Args.Store(kLevelSplitNodeArgsDispatchYOffset, 1u);
    Args.Store(kLevelSplitNodeArgsDispatchZOffset, 1u);
    Args.Store(kLevelSplitNodeArgsNodeCountOffset, 0u);
    Args.Store(kLevelSplitNodeArgsLevelStartOffset, 0u);
    Args.Store(kLevelSplitNodeArgsNodeWriteOffset, 0u);
}

[numthreads(64, 1, 1)]
void InitClusterDagLevelSplitQueuesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint threadIndex = dispatchThreadId.x;

    StructuredBuffer<uint> RootGroups = ResourceDescriptorHeap[RootGroupBufferIndex];
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    RWStructuredBuffer<uint> NodeCandidates0 = ResourceDescriptorHeap[NodeCandidateBuffer0Index];
    RWStructuredBuffer<uint> NodeCandidates1 = ResourceDescriptorHeap[NodeCandidateBuffer1Index];
    RWByteAddressBuffer NodeArgs0 = ResourceDescriptorHeap[NodeArgsBuffer0Index];
    RWByteAddressBuffer NodeArgs1 = ResourceDescriptorHeap[NodeArgsBuffer1Index];
    RWStructuredBuffer<uint> CandidateClusterQueue = ResourceDescriptorHeap[CandidateClusterQueueBufferIndex];
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
        QueueState.Store(kLevelSplitQueueStateTotalVisibleClustersOffset, 0u);
        QueueState.Store(kLevelSplitQueueStatePeakGroupQueueDepthOffset, RootGroupCount);
        QueueState.Store(kLevelSplitQueueStatePeakCandidateQueueDepthOffset, 0u);
        QueueState.Store(kLevelSplitQueueStateGroupDedupCountOffset, 0u);
        QueueState.Store(kLevelSplitQueueStateQueueOverflowCountOffset, 0u);
#endif
        QueueState.Store(kLevelSplitQueueStateCandidateWriteOffset, 0u);

        ClearLevelSplitNodeArgs(NodeArgs0);
        ClearLevelSplitNodeArgs(NodeArgs1);
        NodeArgs0.Store(kLevelSplitNodeArgsNodeWriteOffset, RootGroupCount);
    }

    if (threadIndex < RangeCount)
    {
        RunCounts.Store(threadIndex * 4u, 0u);
    }

    if (threadIndex < IndirectCommandCount)
    {
        DrawDataVisibleEntryIndices[threadIndex] = 0xffffffffu;
    }

    if (threadIndex < CandidateQueueCapacity)
    {
        CandidateClusterQueue[threadIndex] = 0xffffffffu;
    }

    if (threadIndex < GroupCount)
    {
        NodeCandidates0[threadIndex] = 0xffffffffu;
        NodeCandidates1[threadIndex] = 0xffffffffu;
    }

    if (threadIndex < RootGroupCount)
    {
        const uint rootGroupIndex = RootGroups[threadIndex];
        NodeCandidates0[threadIndex] = rootGroupIndex;
        if (rootGroupIndex < GroupCount)
        {
            VisitedGroupEpochs[rootGroupIndex] = TraversalEpoch;
        }
    }
}
