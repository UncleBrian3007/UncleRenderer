// ClusterDagShared.h
// Included by both C++ (ClusterDagStreamingManager.cpp) and HLSL (ClusterDagCommon.hlsl).
// Contains only #define constants so the file is compatible with both compilers.
// C++ side uses these with offsetof static_asserts to verify struct layout matches.
#pragma once

// GPU page payload identity
#define kClusterDagGpuPagePayloadMagic                              0x47504443u
#define kClusterDagGpuPagePayloadVersion                            5u

// Shared traversal limits
#define kClusterDagMaxChildRefsPerGroup                             256u

// Payload header field byte offsets
#define kClusterDagGpuPageHeaderMagicOffset                         0u
#define kClusterDagGpuPageHeaderVersionOffset                       4u
#define kClusterDagGpuPageHeaderPageIndexOffset                     8u
#define kClusterDagGpuPageHeaderGlobalGroupIndexOffset              12u
#define kClusterDagGpuPageHeaderGroupByteOffsetOffset               16u
#define kClusterDagGpuPageHeaderChildRefByteOffsetOffset            20u
#define kClusterDagGpuPageHeaderChildRefCountOffset                 24u
#define kClusterDagGpuPageHeaderClusterDataByteOffsetOffset         28u
#define kClusterDagGpuPageHeaderClusterDataCountOffset              32u
#define kClusterDagGpuPageHeaderDrawDataByteOffsetOffset            36u
#define kClusterDagGpuPageHeaderDrawDataCountOffset                 40u
#define kClusterDagGpuPageHeaderPackedIndexByteOffsetOffset         44u
#define kClusterDagGpuPageHeaderPackedIndexCountOffset              48u
#define kClusterDagGpuPageHeaderPackedPositionByteOffsetOffset      52u
#define kClusterDagGpuPageHeaderPackedPositionCountOffset           56u
#define kClusterDagGpuPageHeaderPackedNormalByteOffsetOffset        60u
#define kClusterDagGpuPageHeaderPackedNormalCountOffset             64u
#define kClusterDagGpuPageHeaderPackedUvByteOffsetOffset            68u
#define kClusterDagGpuPageHeaderPackedUvCountOffset                 72u
#define kClusterDagGpuPageHeaderPackedTangentByteOffsetOffset       76u
#define kClusterDagGpuPageHeaderPackedTangentCountOffset            80u
#define kClusterDagGpuPageHeaderPackedColorByteOffsetOffset         84u
#define kClusterDagGpuPageHeaderPackedColorCountOffset              88u

// Group data field byte offsets
#define kClusterDagGpuPageGroupBoundsOffset                         0u
#define kClusterDagGpuPageGroupLodBoundsOffset                      16u
#define kClusterDagGpuPageGroupParentLODErrorOffset                 32u
#define kClusterDagGpuPageGroupChildRefStartOffset                  36u
#define kClusterDagGpuPageGroupChildRefCountOffset                  40u
#define kClusterDagGpuPageGroupFlagsOffset                          44u
#define kClusterDagGpuPageGroupMipLevelOffset                       48u

// Child ref record stride
#define kClusterDagGpuPageChildRefStride                            8u

// Cluster data stride and field byte offsets
#define kClusterDagGpuPageClusterDataStride                         80u
#define kClusterDagGpuPageClusterDataGlobalClusterIndexOffset       0u
#define kClusterDagGpuPageClusterDataBoundsOffset                   16u
#define kClusterDagGpuPageClusterDataLodBoundsOffset                32u
#define kClusterDagGpuPageClusterDataLODErrorOffset                 48u
#define kClusterDagGpuPageClusterDataMaxEdgeLengthOffset            52u
#define kClusterDagGpuPageClusterDataGroupIndexOffset               56u
#define kClusterDagGpuPageClusterDataGeneratingGroupIndexOffset     60u
#define kClusterDagGpuPageClusterDataDrawDataStartOffset            64u
#define kClusterDagGpuPageClusterDataDrawDataCountOffset            68u
#define kClusterDagGpuPageClusterDataTriangleCountOffset            72u
#define kClusterDagGpuPageClusterDataMipLevelOffset                 76u

// Draw data stride and field byte offsets
#define kClusterDagGpuPageDrawDataStride                            32u
#define kClusterDagGpuPageDrawDataGlobalDrawDataIndexOffset         0u
#define kClusterDagGpuPageDrawDataStartIndexOffset                  4u
#define kClusterDagGpuPageDrawDataIndexCountOffset                  8u
#define kClusterDagGpuPageDrawDataRangeIndexOffset                  12u
#define kClusterDagGpuPageDrawDataRangeCommandStartOffset           16u
#define kClusterDagGpuPageDrawDataRangeCommandCountOffset           20u
#define kClusterDagGpuPageDrawDataDrawSectionIndexOffset            24u
