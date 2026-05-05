#include "ClusterDagCommon.hlsl"

cbuffer ClusterDagLevelSplitPrepareNodeArgsConstants : register(b1)
{
    uint CurrentNodeArgsBufferIndex;
    uint NextNodeArgsBufferIndex;
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

[numthreads(1, 1, 1)]
void PrepareClusterDagLevelSplitNodeArgsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWByteAddressBuffer CurrentArgs = ResourceDescriptorHeap[CurrentNodeArgsBufferIndex];
    RWByteAddressBuffer NextArgs = ResourceDescriptorHeap[NextNodeArgsBufferIndex];

    const uint nodeCount = CurrentArgs.Load(kLevelSplitNodeArgsNodeWriteOffset);
    CurrentArgs.Store(kLevelSplitNodeArgsDispatchXOffset, nodeCount);
    CurrentArgs.Store(kLevelSplitNodeArgsDispatchYOffset, 1u);
    CurrentArgs.Store(kLevelSplitNodeArgsDispatchZOffset, 1u);
    CurrentArgs.Store(kLevelSplitNodeArgsNodeCountOffset, nodeCount);
    CurrentArgs.Store(kLevelSplitNodeArgsLevelStartOffset, 0u);
    CurrentArgs.Store(kLevelSplitNodeArgsNodeWriteOffset, 0u);

    ClearLevelSplitNodeArgs(NextArgs);
}
