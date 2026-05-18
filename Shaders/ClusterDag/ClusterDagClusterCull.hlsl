#include "ClusterDagTraversalCommon.hlsl"

cbuffer ClusterDagClusterCullBindlessConstants : register(b1)
{
    uint ClusterBufferIndex;
    uint DrawDataIndex;
    uint CommandTemplatesIndex;
    uint OutputCommandsIndex;
    uint RunCountsIndex;
    uint QueueStateBufferIndex;
    uint CandidateClusterQueueBufferIndex;
    uint ClusterCount;
    uint DebugPrintStatsIndex;
    uint VisibleEntriesIndex;
    uint VisibleEntryCountersIndex;
    uint HwVisibleEntryIndicesIndex;
    uint SwVisibleEntryIndicesIndex;
    uint DrawDataVisibleEntryIndicesIndex;
};

[numthreads(64, 1, 1)]
void ClusterDagClusterCullCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;

    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterBufferIndex];
    StructuredBuffer<ClusterDagDrawData> DrawDatas = ResourceDescriptorHeap[DrawDataIndex];
    ByteAddressBuffer CommandTemplates = ResourceDescriptorHeap[CommandTemplatesIndex];
    RWByteAddressBuffer OutputCommands = ResourceDescriptorHeap[OutputCommandsIndex];
    RWByteAddressBuffer RunCounts = ResourceDescriptorHeap[RunCountsIndex];
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    StructuredBuffer<ClusterDagCandidateClusterEntry> CandidateClusterQueue = ResourceDescriptorHeap[CandidateClusterQueueBufferIndex];
    RWStructuredBuffer<ClusterDagVisibleEntry> VisibleEntries = ResourceDescriptorHeap[VisibleEntriesIndex];
    RWByteAddressBuffer VisibleEntryCounters = ResourceDescriptorHeap[VisibleEntryCountersIndex];
    RWStructuredBuffer<uint> HwVisibleEntryIndices = ResourceDescriptorHeap[HwVisibleEntryIndicesIndex];
    RWStructuredBuffer<uint> SwVisibleEntryIndices = ResourceDescriptorHeap[SwVisibleEntryIndicesIndex];
    RWStructuredBuffer<uint> DrawDataVisibleEntryIndices = ResourceDescriptorHeap[DrawDataVisibleEntryIndicesIndex];

    const uint candidateCount = QueueState.Load(kQueueStatePass0CandidateCommittedWriteOffset);
    if (candidateIndex >= candidateCount)
    {
        return;
    }

    const ClusterDagCandidateClusterEntry candidateEntry = CandidateClusterQueue[candidateIndex];
    const uint clusterIndex = candidateEntry.ClusterIndex;
#if USE_CLUSTER_DAG_FAST
    if (true)
#else
    if (clusterIndex < ClusterCount)
#endif
    {
        const ClusterDagClusterData cluster = Clusters[clusterIndex];
        const bool isLeaf = cluster.GeneratingGroupIndex == 0xffffffffu;
        const bool rasterizeSW = ShouldRasterizeClusterSW(cluster);
        TrackVisibleClusterDagCandidate(QueueState);
        RecordVisibleCluster(DebugPrintStatsIndex, isLeaf, cluster.MipLevel);
        RecordRasterPath(DebugPrintStatsIndex, rasterizeSW);

        [loop]
        for (uint packetOffset = 0u; packetOffset < cluster.DrawDataCount; ++packetOffset)
        {
            const uint drawDataIndex = cluster.DrawDataStart + packetOffset;
#if !USE_CLUSTER_DAG_FAST
            if (drawDataIndex >= IndirectCommandCount)
            {
                continue;
            }
#endif

            const ClusterDagDrawData drawData = DrawDatas[drawDataIndex];
            ReserveClusterDagVisibleEntry(
                clusterIndex,
                drawDataIndex,
                0xffffffffu,
                rasterizeSW,
                VisibleEntries,
                VisibleEntryCounters,
                HwVisibleEntryIndices,
                SwVisibleEntryIndices,
                DrawDataVisibleEntryIndices);
            if (!rasterizeSW)
            {
                EmitClusterDagHWCommand(drawDataIndex, drawData, CommandTemplates, OutputCommands, RunCounts);
            }
        }
    }
}
