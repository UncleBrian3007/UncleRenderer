#include "ClusterDagCommon.hlsl"

cbuffer ClusterDagSwRasterArgsConstants : register(b0)
{
    uint VisibleEntryCounterBufferIndex;
    uint SwRasterDispatchArgsBufferIndex;
};

[numthreads(1, 1, 1)]
void PrepareClusterDagSwRasterArgsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ByteAddressBuffer VisibleEntryCounters = ResourceDescriptorHeap[VisibleEntryCounterBufferIndex];
    RWByteAddressBuffer SwRasterDispatchArgs = ResourceDescriptorHeap[SwRasterDispatchArgsBufferIndex];

    const uint swEntryCount = VisibleEntryCounters.Load(8u);
    SwRasterDispatchArgs.Store(0u, swEntryCount);
    SwRasterDispatchArgs.Store(4u, 1u);
    SwRasterDispatchArgs.Store(8u, 1u);
}
