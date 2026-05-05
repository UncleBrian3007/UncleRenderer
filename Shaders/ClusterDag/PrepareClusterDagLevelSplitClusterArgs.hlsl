#include "ClusterDagCommon.hlsl"

cbuffer ClusterDagLevelSplitPrepareClusterArgsConstants : register(b1)
{
    uint QueueStateBufferIndex;
    uint ClusterDispatchArgsBufferIndex;
};

[numthreads(1, 1, 1)]
void PrepareClusterDagLevelSplitClusterArgsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    RWByteAddressBuffer ClusterDispatchArgs = ResourceDescriptorHeap[ClusterDispatchArgsBufferIndex];

    const uint candidateCount = QueueState.Load(kLevelSplitQueueStateCandidateWriteOffset);
    const uint dispatchX = (candidateCount + 63u) / 64u;
    ClusterDispatchArgs.Store(0u, dispatchX);
    ClusterDispatchArgs.Store(4u, 1u);
    ClusterDispatchArgs.Store(8u, 1u);
}
