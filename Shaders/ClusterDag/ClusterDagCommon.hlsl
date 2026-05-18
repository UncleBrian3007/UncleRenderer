#ifndef CLUSTER_DAG_COMMON_HLSL
#define CLUSTER_DAG_COMMON_HLSL

#include "../PipelineKeyShared.h"

static const uint kClusterDagCommandStride = 32u;
static const uint kClusterDagPipelineKeyDoubleSidedBit = RENDER_PIPELINE_KEY_DOUBLE_SIDED_BIT;
static const uint kClusterDagPipelineKeyDoubleSidedMask = RENDER_PIPELINE_KEY_DOUBLE_SIDED_MASK;
static const uint kClusterDagGroupPageIndexShift = 16u;
static const uint kClusterDagGroupPageIndexMask = 0xffffu;
static const uint kClusterDagPageResidentFlag = 1u;
static const uint kClusterDagRootPageIndex = 0u;
#include "ClusterDagShared.h"

struct ClusterDagGroupData
{
    float4 Bounds;
    float4 LodBounds;
    float ParentLODError;
    uint ChildRefStart;
    uint ChildRefCount;
    uint Flags;
    uint MipLevel;
};

struct ClusterDagClusterData
{
    float4 Bounds;
    float4 LodBounds;
    float LODError;
    float MaxEdgeLength;
    uint GroupIndex;
    uint GeneratingGroupIndex;
    uint DrawDataStart;
    uint DrawDataCount;
    uint TriangleCount;
    uint MipLevel;
};

struct ClusterChildRef
{
    uint InstanceIndex;
    uint ClusterIndex;
};

struct ClusterDagDrawData
{
    uint StartIndex;
    uint IndexCount;
    uint RangeIndex;
    uint RangeCommandStart;
    uint ModelIndex;
};

struct ClusterDagVisibleEntry
{
    uint ClusterIndex;
    uint DrawDataIndex;
    uint PageDataBase;
    uint Reserved;
};

struct ClusterDagCandidateClusterEntry
{
    uint ClusterIndex;
    uint PageDataBase;
};

struct ClusterDagStreamingRequest
{
    uint StreamingResourceId;
    uint PageIndex;
    uint Priority;
    uint Flags;
};

struct ClusterDagPageTableEntry
{
    uint PhysicalPageIndex;
    uint Flags;
    uint LastUsedFrame;
    uint Reserved;
};

bool HasClusterDagPageData(ClusterDagVisibleEntry visibleEntry)
{
    return visibleEntry.PageDataBase != 0xffffffffu;
}

bool LoadClusterDagPagedVertexIndex(
    ClusterDagVisibleEntry visibleEntry,
    ClusterDagDrawData drawData,
    uint primitiveId,
    uint cornerIndex,
    ByteAddressBuffer PageData,
    out uint vertexIndex)
{
    vertexIndex = 0u;
    if (!HasClusterDagPageData(visibleEntry))
    {
        return false;
    }

    const uint indexCount = PageData.Load(visibleEntry.PageDataBase + kClusterDagGpuPageHeaderPackedIndexCountOffset);
    const uint indexOffset = drawData.StartIndex + primitiveId * 3u + cornerIndex;
    if (indexOffset >= indexCount)
    {
        return false;
    }

    const uint indexBase = PageData.Load(visibleEntry.PageDataBase + kClusterDagGpuPageHeaderPackedIndexByteOffsetOffset);
    vertexIndex = PageData.Load(visibleEntry.PageDataBase + indexBase + indexOffset * 4u);
    return true;
}

bool LoadClusterDagPagedPackedPositionWords(
    ClusterDagVisibleEntry visibleEntry,
    uint vertexIndex,
    ByteAddressBuffer PageData,
    out uint xy,
    out uint z)
{
    xy = 0u;
    z = 0u;
    if (!HasClusterDagPageData(visibleEntry))
    {
        return false;
    }

    const uint positionCount = PageData.Load(visibleEntry.PageDataBase + kClusterDagGpuPageHeaderPackedPositionCountOffset);
    if (vertexIndex >= positionCount)
    {
        return false;
    }

    const uint positionBase = PageData.Load(visibleEntry.PageDataBase + kClusterDagGpuPageHeaderPackedPositionByteOffsetOffset);
    const uint2 words = PageData.Load2(visibleEntry.PageDataBase + positionBase + vertexIndex * 8u);
    xy = words.x;
    z = words.y;
    return true;
}

bool LoadClusterDagPagedPackedScalar(
    ClusterDagVisibleEntry visibleEntry,
    uint vertexIndex,
    uint streamByteOffsetHeaderOffset,
    uint streamCountHeaderOffset,
    ByteAddressBuffer PageData,
    out uint value)
{
    value = 0u;
    if (!HasClusterDagPageData(visibleEntry))
    {
        return false;
    }

    const uint streamCount = PageData.Load(visibleEntry.PageDataBase + streamCountHeaderOffset);
    if (vertexIndex >= streamCount)
    {
        return false;
    }

    const uint streamBase = PageData.Load(visibleEntry.PageDataBase + streamByteOffsetHeaderOffset);
    value = PageData.Load(visibleEntry.PageDataBase + streamBase + vertexIndex * 4u);
    return true;
}

bool TryLoadClusterDagVisibleEntryDrawData(
    ClusterDagVisibleEntry visibleEntry,
    uint drawDataIndex,
    ByteAddressBuffer PageData,
    out ClusterDagDrawData drawData)
{
    drawData = (ClusterDagDrawData)0;
    if (!HasClusterDagPageData(visibleEntry))
    {
        return false;
    }

    const uint drawDataRecordBase = PageData.Load(visibleEntry.PageDataBase + kClusterDagGpuPageHeaderDrawDataRecordByteOffsetOffset);
    const uint drawDataRecordCount = PageData.Load(visibleEntry.PageDataBase + kClusterDagGpuPageHeaderDrawDataRecordCountOffset);
    [loop]
    for (uint recordIndex = 0u; recordIndex < drawDataRecordCount; ++recordIndex)
    {
        const uint recordBase = visibleEntry.PageDataBase + drawDataRecordBase + recordIndex * kClusterDagGpuPageDrawDataRecordStride;
        if (PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordGlobalDrawDataIndexOffset) == drawDataIndex)
        {
            drawData.StartIndex = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordStartIndexOffset);
            drawData.IndexCount = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordIndexCountOffset);
            drawData.RangeIndex = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordRangeIndexOffset);
            drawData.RangeCommandStart = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordRangeCommandStartOffset);
            drawData.ModelIndex = PageData.Load(recordBase + kClusterDagGpuPageDrawDataRecordModelIndexOffset);
            return true;
        }
    }

    return false;
}

void CopyClusterDagCommandTemplate(uint srcIndex, uint dstIndex, ByteAddressBuffer CommandTemplates, RWByteAddressBuffer OutputCommands)
{
    const uint srcBase = srcIndex * kClusterDagCommandStride;
    const uint dstBase = dstIndex * kClusterDagCommandStride;
    [unroll]
    for (uint i = 0u; i < kClusterDagCommandStride / 16u; ++i)
    {
        const uint4 values = CommandTemplates.Load4(srcBase + i * 16u);
        OutputCommands.Store4(dstBase + i * 16u, values);
    }
}

static const uint kClusterDagNonLeafVisibleStatIndex = 11u;
static const uint kClusterDagNonLeafFrustumCulledStatIndex = 13u;
static const uint kClusterDagVisibleMipHistogramBaseStatIndex = 14u;
static const uint kClusterDagVisibleMipHistogramBucketCount = 6u;
static const uint kClusterDagVisibleMipHistogramOverflowBucket = kClusterDagVisibleMipHistogramBucketCount - 1u;
static const uint kClusterDagStreamingRequestStatIndex = 26u;
static const uint kClusterDagStreamingFallbackStatIndex = 27u;
static const uint kClusterDagStreamingRequestOverflowStatIndex = 28u;

static const uint kQueueStateTotalVisibleClustersOffset = 0u;
static const uint kQueueStatePeakGroupQueueDepthOffset = 4u;
static const uint kQueueStatePeakCandidateQueueDepthOffset = 8u;
static const uint kQueueStatePendingItemCountOffset = 12u;
static const uint kQueueStateGroupDedupCountOffset = 16u;
static const uint kQueueStateQueueOverflowCountOffset = 20u;
static const uint kQueueStatePass0CandidateReadOffset = 24u;
static const uint kQueueStatePass0CandidateWriteOffset = 28u;
static const uint kQueueStatePass0GroupReadOffset = 32u;
static const uint kQueueStatePass0GroupWriteOffset = 36u;
static const uint kQueueStatePass0GroupCountOffset = 40u;
static const uint kQueueStatePass1CandidateReadOffset = 44u;
static const uint kQueueStatePass1CandidateWriteOffset = 48u;
static const uint kQueueStatePass1GroupReadOffset = 52u;
static const uint kQueueStatePass1GroupWriteOffset = 56u;
static const uint kQueueStatePass1GroupCountOffset = 60u;

static const uint kLevelSplitQueueStateTotalVisibleClustersOffset = 0u;
static const uint kLevelSplitQueueStatePeakGroupQueueDepthOffset = 4u;
static const uint kLevelSplitQueueStatePeakCandidateQueueDepthOffset = 8u;
static const uint kLevelSplitQueueStateGroupDedupCountOffset = 12u;
static const uint kLevelSplitQueueStateQueueOverflowCountOffset = 16u;
static const uint kLevelSplitQueueStateCandidateWriteOffset = 20u;
static const uint kLevelSplitQueueStateUintCount = 6u;
static const uint kLevelSplitQueueStateByteSize = kLevelSplitQueueStateUintCount * 4u;

static const uint kClusterDagLevelSplitNodeThreadGroupSize = 64u;
static const uint kClusterDagLevelSplitMaxChildRefsPerGroup = 256u;

static const uint kLevelSplitNodeArgsDispatchXOffset = 0u;
static const uint kLevelSplitNodeArgsDispatchYOffset = 4u;
static const uint kLevelSplitNodeArgsDispatchZOffset = 8u;
static const uint kLevelSplitNodeArgsNodeCountOffset = 12u;
static const uint kLevelSplitNodeArgsLevelStartOffset = 16u;
static const uint kLevelSplitNodeArgsNodeWriteOffset = 20u;
static const uint kLevelSplitNodeArgsUintCount = 6u;
static const uint kLevelSplitNodeArgsByteSize = kLevelSplitNodeArgsUintCount * 4u;

// Commit-counter offsets for the persistent queue (Group and Candidate).
// These are separate from the claimed-write offsets (kQueueStatePass0GroupWriteOffset /
// kQueueStatePass0CandidateWriteOffset) and are only advanced by a producer AFTER the
// corresponding slot has been written and a DeviceMemoryBarrier has been issued.
// Consumers read up to CommittedWrite, so no sentinel spin-wait is needed.
static const uint kQueueStatePass0GroupCommittedWriteOffset = 64u;
static const uint kQueueStatePass0CandidateCommittedWriteOffset = 68u;

#endif // CLUSTER_DAG_COMMON_HLSL
