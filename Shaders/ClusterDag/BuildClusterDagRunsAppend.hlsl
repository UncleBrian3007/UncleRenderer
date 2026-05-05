#include "../CullingConstants.hlsl"
#include "ClusterDagCommon.hlsl"

cbuffer ClusterDagRunBindlessConstants : register(b1)
{
    uint VisibleClustersIndex;
    uint VisibleClusterCountIndex;
    uint ClusterDataIndex;
    uint DrawDataIndex;
    uint CommandTemplatesIndex;
    uint OutputCommandsIndex;
    uint RunCountsIndex;
    uint Padding0;
};

[numthreads(64, 1, 1)]
void BuildClusterDagRunsAppendCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint visibleListIndex = dispatchThreadId.x;
    if (visibleListIndex >= IndirectCommandCount)
    {
        return;
    }

    StructuredBuffer<uint> VisibleClusters = ResourceDescriptorHeap[VisibleClustersIndex];
    ByteAddressBuffer VisibleClusterCount = ResourceDescriptorHeap[VisibleClusterCountIndex];
    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterDataIndex];
    StructuredBuffer<ClusterDagDrawData> DrawDatas = ResourceDescriptorHeap[DrawDataIndex];
    ByteAddressBuffer CommandTemplates = ResourceDescriptorHeap[CommandTemplatesIndex];
    RWByteAddressBuffer OutputCommands = ResourceDescriptorHeap[OutputCommandsIndex];
    RWByteAddressBuffer RunCounts = ResourceDescriptorHeap[RunCountsIndex];

    const uint totalVisibleClusters = VisibleClusterCount.Load(0u);
    if (visibleListIndex >= totalVisibleClusters)
    {
        return;
    }

    const uint clusterIndex = VisibleClusters[visibleListIndex];
    const ClusterDagClusterData cluster = Clusters[clusterIndex];

    [loop]
    for (uint packetOffset = 0; packetOffset < cluster.DrawDataCount; ++packetOffset)
    {
        const uint drawDataIndex = cluster.DrawDataStart + packetOffset;
        const ClusterDagDrawData drawData = DrawDatas[drawDataIndex];
        uint runOffset = 0;
        RunCounts.InterlockedAdd(drawData.RangeIndex * 4u, 1u, runOffset);
        const uint outputIndex = drawData.RangeCommandStart + runOffset;
        CopyClusterDagCommandTemplate(drawDataIndex, outputIndex, CommandTemplates, OutputCommands);
    }
}
