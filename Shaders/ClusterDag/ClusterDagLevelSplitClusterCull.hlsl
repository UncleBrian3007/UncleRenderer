cbuffer ClusterDagLevelSplitClusterCullBindlessConstants : register(b1)
{
    uint ClusterBufferIndex;
    uint DrawDataIndex;
    uint CommandTemplatesIndex;
    uint OutputCommandsIndex;
    uint RunCountsIndex;
    uint QueueStateBufferIndex;
    uint CandidateClusterEntryBufferIndex;
    uint ClusterCount;
    uint DebugPrintStatsIndex;
    uint VisibleEntriesIndex;
    uint VisibleEntryCountersIndex;
    uint HwVisibleEntryIndicesIndex;
    uint SwVisibleEntryIndicesIndex;
    uint DrawDataVisibleEntryIndicesIndex;
    uint PageDataBufferIndex;
    uint DrawDataCount;
    float ClusterDAGTargetErrorPixels;
    float ViewportHeightPixels;
    uint ClusterDAGForceMipEnabled;
    uint ClusterDAGForceMipLevel;
    uint ClusterDAGForceSoftwareRaster;
    float ClusterDAGSwRasterThresholdPixels;
};

#include "ClusterDagTraversalCommon.hlsl"

[numthreads(64, 1, 1)]
void ClusterDagLevelSplitClusterCullCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;

    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterBufferIndex];
    StructuredBuffer<ClusterDagDrawData> DrawDatas = ResourceDescriptorHeap[DrawDataIndex];
    ByteAddressBuffer CommandTemplates = ResourceDescriptorHeap[CommandTemplatesIndex];
    RWByteAddressBuffer OutputCommands = ResourceDescriptorHeap[OutputCommandsIndex];
    RWByteAddressBuffer RunCounts = ResourceDescriptorHeap[RunCountsIndex];
    RWByteAddressBuffer QueueState = ResourceDescriptorHeap[QueueStateBufferIndex];
    StructuredBuffer<ClusterDagCandidateClusterEntry> CandidateClusterEntry = ResourceDescriptorHeap[CandidateClusterEntryBufferIndex];
    RWStructuredBuffer<ClusterDagVisibleEntry> VisibleEntries = ResourceDescriptorHeap[VisibleEntriesIndex];
    RWByteAddressBuffer VisibleEntryCounters = ResourceDescriptorHeap[VisibleEntryCountersIndex];
    RWStructuredBuffer<uint> HwVisibleEntryIndices = ResourceDescriptorHeap[HwVisibleEntryIndicesIndex];
    RWStructuredBuffer<uint> SwVisibleEntryIndices = ResourceDescriptorHeap[SwVisibleEntryIndicesIndex];
    RWStructuredBuffer<uint> DrawDataVisibleEntryIndices = ResourceDescriptorHeap[DrawDataVisibleEntryIndicesIndex];
    ByteAddressBuffer PageData = ResourceDescriptorHeap[PageDataBufferIndex];

    const uint candidateCount = QueueState.Load(kLevelSplitQueueStateCandidateWriteOffset);
    if (candidateIndex >= candidateCount)
    {
        return;
    }

    const ClusterDagCandidateClusterEntry candidateEntry = CandidateClusterEntry[candidateIndex];
    const uint clusterIndex = candidateEntry.ClusterIndex;
    const bool usePagedCandidate = candidateEntry.PageDataBase != 0xffffffffu;
#if USE_CLUSTER_DAG_FAST
    if (true)
#else
    if (clusterIndex < ClusterCount)
#endif
    {
        const ClusterDagClusterData cluster = LoadClusterDagCluster(clusterIndex, candidateEntry.PageLocalClusterIndex, usePagedCandidate, candidateEntry.PageDataBase, Clusters, PageData);
        const bool rasterizeSW = ShouldRasterizeClusterSW(cluster);
        TrackLevelSplitVisibleClusterDagCandidate(QueueState);
        RecordVisibleCluster(DebugPrintStatsIndex, cluster.MipLevel);
        RecordRasterPath(DebugPrintStatsIndex, rasterizeSW);

        [loop]
        for (uint packetOffset = 0u; packetOffset < cluster.DrawDataCount; ++packetOffset)
        {
            const uint pageLocalDrawDataIndex = cluster.DrawDataStart + packetOffset;
            uint drawDataIndex = pageLocalDrawDataIndex;
#if !USE_CLUSTER_DAG_FAST
            if (!usePagedCandidate && drawDataIndex >= DrawDataCount)
            {
                continue;
            }
#endif

            uint resolvedDrawDataIndex = 0xffffffffu;
            const ClusterDagDrawData drawData = LoadClusterDagDrawData(drawDataIndex, pageLocalDrawDataIndex, usePagedCandidate, candidateEntry.PageDataBase, DrawDatas, PageData, resolvedDrawDataIndex);
            drawDataIndex = resolvedDrawDataIndex;
            if (drawDataIndex == 0xffffffffu)
            {
                continue;
            }
#if !USE_CLUSTER_DAG_FAST
            if (drawDataIndex >= DrawDataCount)
            {
                continue;
            }
#endif
            ReserveClusterDagVisibleEntry(
                clusterIndex,
                drawDataIndex,
                usePagedCandidate ? candidateEntry.PageDataBase : 0xffffffffu,
                usePagedCandidate ? pageLocalDrawDataIndex : 0xffffffffu,
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
