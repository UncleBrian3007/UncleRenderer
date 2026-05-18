// ClusterDagShared.h
// Included by both C++ (ClusterDagStreamingManager.cpp) and HLSL (ClusterDagCommon.hlsl).
// Contains only #define constants so the file is compatible with both compilers.
// C++ side uses these with offsetof static_asserts to verify struct layout matches.
#pragma once

// GPU page payload identity
#define kClusterDagGpuPagePayloadMagic                              0x47504443u
#define kClusterDagGpuPagePayloadVersion                            3u

// Payload header field byte offsets
#define kClusterDagGpuPageHeaderMagicOffset                         0u
#define kClusterDagGpuPageHeaderVersionOffset                       4u
#define kClusterDagGpuPageHeaderPageIndexOffset                     8u
#define kClusterDagGpuPageHeaderGlobalGroupIndexOffset              12u
#define kClusterDagGpuPageHeaderGroupByteOffsetOffset               16u
#define kClusterDagGpuPageHeaderChildRefByteOffsetOffset            20u
#define kClusterDagGpuPageHeaderChildRefCountOffset                 24u
#define kClusterDagGpuPageHeaderClusterRecordByteOffsetOffset       28u
#define kClusterDagGpuPageHeaderClusterRecordCountOffset            32u
#define kClusterDagGpuPageHeaderDrawDataRecordByteOffsetOffset      36u
#define kClusterDagGpuPageHeaderDrawDataRecordCountOffset           40u
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

// Cluster record stride and field byte offsets
#define kClusterDagGpuPageClusterRecordStride                       80u
#define kClusterDagGpuPageClusterRecordGlobalClusterIndexOffset     0u
#define kClusterDagGpuPageClusterRecordBoundsOffset                 16u
#define kClusterDagGpuPageClusterRecordLodBoundsOffset              32u
#define kClusterDagGpuPageClusterRecordLODErrorOffset               48u
#define kClusterDagGpuPageClusterRecordMaxEdgeLengthOffset          52u
#define kClusterDagGpuPageClusterRecordGroupIndexOffset             56u
#define kClusterDagGpuPageClusterRecordGeneratingGroupIndexOffset   60u
#define kClusterDagGpuPageClusterRecordDrawDataStartOffset          64u
#define kClusterDagGpuPageClusterRecordDrawDataCountOffset          68u
#define kClusterDagGpuPageClusterRecordTriangleCountOffset          72u
#define kClusterDagGpuPageClusterRecordMipLevelOffset               76u

// Draw data record stride and field byte offsets
#define kClusterDagGpuPageDrawDataRecordStride                      32u
#define kClusterDagGpuPageDrawDataRecordGlobalDrawDataIndexOffset   0u
#define kClusterDagGpuPageDrawDataRecordStartIndexOffset            4u
#define kClusterDagGpuPageDrawDataRecordIndexCountOffset            8u
#define kClusterDagGpuPageDrawDataRecordRangeIndexOffset            12u
#define kClusterDagGpuPageDrawDataRecordRangeCommandStartOffset     16u
#define kClusterDagGpuPageDrawDataRecordModelIndexOffset            20u
