#include "ClusterDagTraversalCommon.hlsl"
#include "../CullMeshletCommon.hlsl"

cbuffer ClusterDagSelectBindlessConstants : register(b1)
{
    uint GroupBufferIndex;
    uint ClusterBufferIndex;
    uint ChildRefBufferIndex;
    uint RootGroupBufferIndex;
    uint VisibleClusterBufferIndex;
    uint VisibleClusterCountBufferIndex;
    uint GroupCount;
    uint ClusterCount;
    uint ChildRefCount;
    uint DebugPrintStatsIndex;
    uint DebugLineBufferIndex;
};

static const uint kClusterDagTraversalStackCapacity = 256u;
static const uint kClusterDagExpandedGroupCapacity = 256u;

[numthreads(64, 1, 1)]
void SelectClusterLODCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint rootIndex = dispatchThreadId.x;
    if (rootIndex >= ClusterDagRootCount)
    {
        return;
    }

    StructuredBuffer<ClusterDagGroupData> Groups = ResourceDescriptorHeap[GroupBufferIndex];
    StructuredBuffer<ClusterDagClusterData> Clusters = ResourceDescriptorHeap[ClusterBufferIndex];
    StructuredBuffer<ClusterChildRef> ChildRefs = ResourceDescriptorHeap[ChildRefBufferIndex];
    StructuredBuffer<uint> RootGroups = ResourceDescriptorHeap[RootGroupBufferIndex];
    RWStructuredBuffer<uint> VisibleClusters = ResourceDescriptorHeap[VisibleClusterBufferIndex];
    RWByteAddressBuffer VisibleClusterCount = ResourceDescriptorHeap[VisibleClusterCountBufferIndex];

    const uint rootGroupIndex = RootGroups[rootIndex];
#if !USE_CLUSTER_DAG_FAST
    if (rootGroupIndex >= GroupCount)
    {
        return;
    }
#endif

    const ClusterDagGroupData rootGroup = Groups[rootGroupIndex];
#if !USE_CLUSTER_DAG_FAST
    if (rootGroup.ChildRefStart > ChildRefCount || rootGroup.ChildRefCount > ChildRefCount - rootGroup.ChildRefStart)
    {
        return;
    }
#endif

    uint stack[kClusterDagTraversalStackCapacity];
    uint stackSize = 0;
    [loop]
    for (uint rootChildOffset = 0u; rootChildOffset < rootGroup.ChildRefCount; ++rootChildOffset)
    {
        const ClusterChildRef rootChildRef = ChildRefs[rootGroup.ChildRefStart + rootChildOffset];
#if !USE_CLUSTER_DAG_FAST
        if (rootChildRef.ClusterIndex == 0xffffffffu || rootChildRef.ClusterIndex >= ClusterCount)
        {
            continue;
        }
#endif

        if (stackSize < kClusterDagTraversalStackCapacity)
        {
            stack[stackSize++] = rootChildRef.ClusterIndex;
        }
        else
        {
            RecordStackOverflow(DebugPrintStatsIndex);
        }
    }

    if (stackSize == 0u)
    {
        return;
    }

    uint expandedGroups[kClusterDagExpandedGroupCapacity];
    uint expandedGroupCount = 0u;
    uint iterationCount = 0u;
    const uint maxIterationCount = max(ClusterCount * 4u, kClusterDagTraversalStackCapacity);

    while (stackSize > 0)
    {
        iterationCount += 1u;
        if (iterationCount > maxIterationCount)
        {
            RecordIterationOverflow(DebugPrintStatsIndex);
            return;
        }

        const uint clusterIndex = stack[--stackSize];
#if !USE_CLUSTER_DAG_FAST
        if (clusterIndex >= ClusterCount)
        {
            return;
        }
#endif

        const ClusterDagClusterData cluster = Clusters[clusterIndex];
        const bool isLeaf = cluster.GeneratingGroupIndex == 0xffffffffu;
        const float3 lodCenter = cluster.LodBounds.xyz;
        const float lodRadius = cluster.LodBounds.w;
        const bool skipFrustumCull = ClusterDAGForceMipEnabled != 0u && ClusterDAGForceMipSkipFrustumCull != 0u;
        if (!skipFrustumCull && !IsSphereVisible(lodCenter, lodRadius))
        {
            RecordFrustumCulled(DebugPrintStatsIndex, isLeaf);

#if USE_CLUSTER_DAG_DEBUG
            if (ClusterDAGForceMipEnabled != 0u && DebugPrintEnabled != 0 && DebugLineBufferIndex != 0xffffffffu && isLeaf)
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
                return;
            }
#endif

            const float projectedError = ComputeProjectedErrorPixels(lodCenter, cluster.LODError);
            refine = projectedError > ClusterDAGTargetErrorPixels;
        }

        if (refine)
        {
            bool alreadyExpanded = false;
            [loop]
            for (uint expandedIndex = 0u; expandedIndex < expandedGroupCount; ++expandedIndex)
            {
                if (expandedGroups[expandedIndex] == cluster.GeneratingGroupIndex)
                {
                    alreadyExpanded = true;
                    break;
                }
            }

            if (alreadyExpanded)
            {
                continue;
            }

            const ClusterDagGroupData group = Groups[cluster.GeneratingGroupIndex];
#if !USE_CLUSTER_DAG_FAST
            if (group.ChildRefStart > ChildRefCount || group.ChildRefCount > ChildRefCount - group.ChildRefStart)
            {
                return;
            }

            if (group.ChildRefCount == 0u)
            {
                refine = false;
            }
            else
#endif
            {
                if (expandedGroupCount < kClusterDagExpandedGroupCapacity)
                {
                    expandedGroups[expandedGroupCount++] = cluster.GeneratingGroupIndex;
                }
                else
                {
                    RecordExpandedOverflow(DebugPrintStatsIndex);
                }

                bool pushedAnyChild = false;
                [loop]
                for (uint childOffset = 0; childOffset < group.ChildRefCount; ++childOffset)
                {
                    const ClusterChildRef childRef = ChildRefs[group.ChildRefStart + childOffset];
#if !USE_CLUSTER_DAG_FAST
                    if (childRef.ClusterIndex == 0xffffffffu)
                    {
                        continue;
                    }

                    if (childRef.ClusterIndex >= ClusterCount)
                    {
                        continue;
                    }
#endif

                    if (stackSize < kClusterDagTraversalStackCapacity)
                    {
                        stack[stackSize++] = childRef.ClusterIndex;
                        pushedAnyChild = true;
                    }
                    else
                    {
                        RecordStackOverflow(DebugPrintStatsIndex);
                    }
                }

                if (pushedAnyChild)
                {
                    continue;
                }
            }
        }

        uint visibleIndex = 0;
        VisibleClusterCount.InterlockedAdd(0u, 1u, visibleIndex);
        if (visibleIndex < ClusterCount)
        {
            VisibleClusters[visibleIndex] = clusterIndex;
            RecordVisibleCluster(DebugPrintStatsIndex, isLeaf, cluster.MipLevel);
        }
    }
}
