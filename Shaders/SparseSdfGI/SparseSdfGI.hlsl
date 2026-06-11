#include "SparseSdfGIShared.h"
#include "../SceneConstants.hlsl"
#include "../Common.hlsli"
#include "../CommonSH.hlsli"
#include "../BlueNoiseSobolSampler.hlsli"
#include "../GpuDebug/GpuDebugPrintCommon.hlsl"

cbuffer SparseSdfGIConstants : register(b1)
{
    uint OutputWidth;
    uint OutputHeight;
    uint AtlasResolution;
    uint BrickGridResolution;
    uint BrickVoxelResolution;
    uint CascadeIndex;
    uint FrameIndex;
    uint DebugMode;
    uint Enabled;
    uint TraceHalfResolution;
    uint ModelTriangleCount;
    uint ModelDrawIndexStart;
    uint ModelDrawIndexCount;
    uint MaxBrickTriangleReferences;
    float Intensity;
    float MaxTraceDistance;
    float3 CascadeMin;
    float VoxelSize;
    float3 CascadeExtent;
    float SurfaceThicknessVoxels;
    row_major float4x4 ModelWorld;
    float BounceStrength;
    uint ProbeTileSize;
    uint ProbeCountX;
    uint ProbeCountY;
    uint ProbeRaysPerProbe;
    uint ProbeDebugMode;
    uint ProbeHistoryValid;
    float SurfaceHitThresholdVoxels;
    uint TrianglePoolCapacity;
    uint BuildWorkOffset;
    uint CascadeCount;
    float CascadeScale;
    uint PhysicalBrickBase;
    uint ScatterBrickCapacity;
    uint CascadeDataSrvIndex;
};

#if defined(SPARSE_SDF_GI_SCATTER_INIT_SHADER)
cbuffer SparseSdfGIScatterInitBindlessConstants : register(b2)
{
    uint SdfAtlasUavIndex;
    uint CascadeBrickMapUavIndex;
    uint BrickMetadataUavIndex;
    uint ScatterTouchedBricksUavIndex;
    uint ScatterCountersUavIndex;
    uint ReferenceStatsUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_PREPARE_SHADER)
cbuffer SparseSdfGIScatterPrepareBindlessConstants : register(b2)
{
    uint PositionBufferIndex;
    uint IndexBufferIndex;
    uint ScatterJobsUavIndex;
    uint ScatterCountersUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_SCAN_JOBS_SHADER)
cbuffer SparseSdfGIScatterScanJobsBindlessConstants : register(b2)
{
    uint ScatterJobsSrvIndex;
    uint ScatterJobOffsetsUavIndex;
    uint ScatterGroupSumsUavIndex;
    uint ScatterCountersSrvIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_SCAN_GROUPS_SHADER)
cbuffer SparseSdfGIScatterScanGroupsBindlessConstants : register(b2)
{
    uint ScatterInputGroupSumsSrvIndex;
    uint ScatterGroupOffsetsUavIndex;
    uint ScatterOutputGroupSumsUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_SCAN_GROUP2_SHADER)
cbuffer SparseSdfGIScatterScanGroup2BindlessConstants : register(b2)
{
    uint ScatterGroup2SumsSrvIndex;
    uint ScatterGroup2OffsetsUavIndex;
    uint ScatterCountersUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_ADD_OFFSETS_SHADER)
cbuffer SparseSdfGIScatterAddOffsetsBindlessConstants : register(b2)
{
    uint ScatterJobOffsetsUavIndex;
    uint ScatterGroupOffsetsSrvIndex;
    uint ScatterGroup2OffsetsSrvIndex;
    uint ScatterCountersSrvIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_BUILD_SAMPLE_ARGS_SHADER) || defined(SPARSE_SDF_GI_SCATTER_BUILD_BRICK_ARGS_SHADER)
cbuffer SparseSdfGIScatterBuildArgsBindlessConstants : register(b2)
{
    uint ScatterCountersSrvIndex;
    uint ScatterDispatchArgsUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_MARK_TOUCHED_SHADER)
cbuffer SparseSdfGIScatterMarkTouchedBindlessConstants : register(b2)
{
    uint ScatterJobsSrvIndex;
    uint ScatterJobOffsetsSrvIndex;
    uint ScatterTouchedBricksUavIndex;
    uint ScatterCountersSrvIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_ALLOCATE_BRICKS_SHADER)
cbuffer SparseSdfGIScatterAllocateBindlessConstants : register(b2)
{
    uint ScatterTouchedBricksSrvIndex;
    uint CascadeBrickMapUavIndex;
    uint ScatterBrickListUavIndex;
    uint ScatterCountersUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_CLEAR_BRICK_STORAGE_SHADER)
cbuffer SparseSdfGIScatterClearBrickBindlessConstants : register(b2)
{
    uint ScatterBrickSdfUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_SDF_SAMPLES_SHADER)
cbuffer SparseSdfGIScatterSamplesBindlessConstants : register(b2)
{
    uint ScatterJobsSrvIndex;
    uint ScatterJobOffsetsSrvIndex;
    uint CascadeBrickMapSrvIndex;
    uint ScatterBrickSdfUavIndex;
    uint ScatterCountersSrvIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_EIKONAL_SHADER)
cbuffer SparseSdfGIScatterEikonalBindlessConstants : register(b2)
{
    uint ScatterBrickSdfUavIndex;
};
#elif defined(SPARSE_SDF_GI_SCATTER_FINALIZE_BRICKS_SHADER)
cbuffer SparseSdfGIScatterFinalizeBindlessConstants : register(b2)
{
    uint SdfAtlasUavIndex;
    uint BrickMetadataUavIndex;
    uint ScatterBrickListSrvIndex;
    uint ScatterBrickSdfSrvIndex;
    uint ScatterCountersSrvIndex;
    uint ReferenceStatsUavIndex;
};
#elif defined(SPARSE_SDF_GI_BUILD_TRACE_HIERARCHY_BOTTOM_SHADER)
cbuffer SparseSdfGIBuildTraceHierarchyBottomBindlessConstants : register(b2)
{
    uint CascadeBrickMapSrvIndex;
    uint BrickMetadataSrvIndex;
    uint TraceHierarchyBottomUavIndex;
};
#elif defined(SPARSE_SDF_GI_BUILD_TRACE_HIERARCHY_TOP_SHADER)
cbuffer SparseSdfGIBuildTraceHierarchyTopBindlessConstants : register(b2)
{
    uint TraceHierarchyBottomSrvIndex;
    uint TraceHierarchyTopUavIndex;
};
#elif defined(SPARSE_SDF_GI_REFERENCE_STATS_SHADER)
cbuffer SparseSdfGIReferenceStatsBindlessConstants : register(b2)
{
    uint ReferenceStatsSrvIndex;
    uint DebugPrintStatsUavIndex;
};
#elif defined(SPARSE_SDF_GI_RADIANCE_CLEAR_SHADER)
cbuffer SparseSdfGIRadianceClearBindlessConstants : register(b2)
{
    uint BrickRadianceAccumUavIndex;
};
#elif defined(SPARSE_SDF_GI_RADIANCE_INJECT_SHADER)
cbuffer SparseSdfGIRadianceInjectBindlessConstants : register(b2)
{
    uint DepthIndex;
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint BrickRadianceAccumUavIndex;
    uint ShadowTextureIndex;
    uint ShadowMode;
    uint CascadeBrickMapSrvIndex;
    uint BrickIrradianceReadIndex;
};

static const uint SPARSE_SDF_GI_SHADOW_MODE_OFF = 0u;
static const uint SPARSE_SDF_GI_SHADOW_MODE_SCREEN_MASK = 1u;
static const uint SPARSE_SDF_GI_SHADOW_MODE_SHADOW_MAP = 2u;
#elif defined(SPARSE_SDF_GI_IRRADIANCE_ACCUM_SHADER)
cbuffer SparseSdfGIIrradianceAccumulateBindlessConstants : register(b2)
{
    uint DepthIndex;
    uint DiffuseGIIndex;
    uint BrickIrradianceAccumUavIndex;
    uint CascadeBrickMapSrvIndex;
};
#elif defined(SPARSE_SDF_GI_RADIANCE_RESOLVE_SHADER)
cbuffer SparseSdfGIRadianceResolveBindlessConstants : register(b2)
{
    uint BrickRadianceAccumSrvIndex;
    uint BrickRadianceHistorySrvIndex;
    uint BrickRadianceUavIndex;
    uint RadianceHistoryValid;
};
#elif defined(SPARSE_SDF_GI_BRICK_SH_PROPAGATE_SHADER)
cbuffer SparseSdfGIBrickShPropagateBindlessConstants : register(b2)
{
    uint CascadeBrickMapSrvIndex;
    uint BrickShSourceSrvIndex;
    uint BrickShDestUavIndex;
};
#elif defined(SPARSE_SDF_GI_TRACE_SHADER)
cbuffer SparseSdfGITraceBindlessConstants : register(b2)
{
    uint SdfAtlasSrvIndex;
    uint CascadeBrickMapSrvIndex;
    uint BrickMetadataSrvIndex;
    uint DiffuseGIUavIndex;
    uint DepthIndex;
    uint GBufferAIndex;
    uint EnvironmentCubeIndex;
    uint LinearClampSamplerIndex;
    uint BrickRadianceSrvIndex;
    uint InputSHUavIndex;
    uint VarianceUavIndex;
    uint TraceHierarchyBottomSrvIndex;
    uint TraceHierarchyTopSrvIndex;
};
#elif defined(SPARSE_SDF_GI_PROBE_SPAWN_SHADER)
cbuffer SparseSdfGIProbeSpawnBindlessConstants : register(b2)
{
    uint DepthIndex;
    uint GBufferAIndex;
    uint ProbeHeaderUavIndex;
    uint JitterEnabled;
    uint BlueNoiseSobolTextureIndex;
    uint BlueNoiseScramblingRankingTextureIndex;
    uint VelocityIndex;
};
#elif defined(SPARSE_SDF_GI_PROBE_TRACE_SHADER)
cbuffer SparseSdfGIProbeTraceBindlessConstants : register(b2)
{
    uint SdfAtlasSrvIndex;
    uint CascadeBrickMapSrvIndex;
    uint BrickMetadataSrvIndex;
    uint BrickRadianceSrvIndex;
    uint EnvironmentCubeIndex;
    uint LinearClampSamplerIndex;
    uint ProbeHeaderSrvIndex;
    uint ProbeSHUavIndex;
    uint ProbeVarianceUavIndex;
    uint ProbeHistoryReadSrvIndex;
    uint ProbeHistoryWriteUavIndex;
    uint TraceHierarchyBottomSrvIndex;
    uint TraceHierarchyTopSrvIndex;
};
#elif defined(SPARSE_SDF_GI_PROBE_INTERPOLATE_SHADER)
cbuffer SparseSdfGIProbeInterpolateBindlessConstants : register(b2)
{
    uint DepthIndex;
    uint GBufferAIndex;
    uint DiffuseGIUavIndex;
    uint ProbeHeaderSrvIndex;
    uint ProbeSHSrvIndex;
    uint ProbeVarianceSrvIndex;
    uint InputSHUavIndex;
    uint VarianceUavIndex;
};
#else
#error SparseSdfGI shader entry wrapper must define a bindless layout macro.
#endif

static const uint SPARSE_SDF_GI_INVALID_BRICK_ID = 0xffffffffu;
static const uint SPARSE_SDF_GI_INVALID_REFERENCE = 0xffffffffu;
uint GetTraceBlueNoiseSobolTextureIndex() { return TrianglePoolCapacity; }
uint GetTraceBlueNoiseScramblingRankingTextureIndex() { return BuildWorkOffset; }
// referenceCounters[] slot meanings (shared by init/emit/solve).
static const uint SPARSE_SDF_GI_REF_COUNTER_TRIANGLE = 0u;
static const uint SPARSE_SDF_GI_REF_COUNTER_REFERENCE = 1u;
static const uint SPARSE_SDF_GI_REF_COUNTER_TRIANGLE_OVERFLOW = 2u;
static const uint SPARSE_SDF_GI_REF_COUNTER_REFERENCE_OVERFLOW = 3u;
static const uint SPARSE_SDF_GI_REF_COUNTER_OCCUPIED_BRICK = 4u;
static const uint SPARSE_SDF_GI_SCATTER_COUNTER_JOB = 0u;
static const uint SPARSE_SDF_GI_SCATTER_COUNTER_SAMPLE = 1u;
static const uint SPARSE_SDF_GI_SCATTER_COUNTER_ALLOCATED_BRICK = 2u;
static const uint SPARSE_SDF_GI_SCATTER_COUNTER_JOB_OVERFLOW = 3u;
static const uint SPARSE_SDF_GI_SCATTER_COUNTER_BRICK_OVERFLOW = 4u;
static const uint SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE = 256u;
static const uint SPARSE_SDF_GI_SCATTER_SAMPLE_GROUP_SIZE = 128u;
static const uint SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE = 512u;
static const uint SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT = 65535u;
static const uint SPARSE_SDF_GI_SCATTER_DISTANCE_FAR_UINT = 0x00ffffffu;
static const float SPARSE_SDF_GI_SCATTER_BAND_VOXELS = 3.0f;
static const uint SPARSE_SDF_GI_BRICK_LOCAL_DIM = 8u;
static const uint SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT = SPARSE_SDF_GI_BRICK_LOCAL_DIM - 1u;
static const uint SPARSE_SDF_GI_BRICK_LOCAL_DIM_LOG2 = 3u;
static const uint SPARSE_SDF_GI_BRICK_LOCAL_AREA = SPARSE_SDF_GI_BRICK_LOCAL_DIM * SPARSE_SDF_GI_BRICK_LOCAL_DIM;
static const uint SPARSE_SDF_GI_EIKONAL_LDS_COUNT = SPARSE_SDF_GI_BRICK_LOCAL_AREA * SPARSE_SDF_GI_BRICK_LOCAL_DIM;
static const uint SPARSE_SDF_GI_SURFACE_TRACE_MAX_STEPS = 2048u;
// Visualization scale for the Step Count debug view. Most traces use far fewer
// steps than SPARSE_SDF_GI_SURFACE_TRACE_MAX_STEPS, so a smaller divisor keeps the
// heat ramp readable instead of crushing everything to dark.
static const float SPARSE_SDF_GI_STEP_COUNT_DEBUG_SCALE = 512.0f;
static const uint SPARSE_SDF_GI_TRACE_STATUS_HIT = 0u;
static const uint SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS = 1u;
static const uint SPARSE_SDF_GI_TRACE_STATUS_MAX_DISTANCE = 2u;
static const uint SPARSE_SDF_GI_TRACE_STATUS_CASCADE_EXIT = 3u;
static const uint SPARSE_SDF_GI_TRACE_STATUS_ATLAS_OUTSIDE = 4u;
static const uint SPARSE_SDF_GI_TRACE_STATUS_ITER_LIMIT = 5u;
static const uint SPARSE_SDF_GI_BRICK_METADATA_OCCUPIED = 1u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_OCCUPIED = 1u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION = 16u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION = 4u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_CELL_SIZE = 4u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_CELL_SIZE = 16u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_ITER_LIMIT = 32u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_ITER_LIMIT_SINGLE_CASCADE = 64u;
static const uint SPARSE_SDF_GI_TRACE_HIERARCHY_INNER_STEPS = 8u;
static const float SPARSE_SDF_GI_BRICK_AABB_MARGIN_VOXELS = 1.0f;
// Metadata stays at the maximum tunable hit threshold so AABB skip remains conservative while
// the runtime hit threshold is lowered for debug comparisons.
static const float SPARSE_SDF_GI_SURFACE_METADATA_VOXELS = 0.75f;
static const uint SPARSE_SDF_GI_MAX_TRIANGLE_BRICK_REFERENCES = 4096u;
static const float SPARSE_SDF_GI_RADIANCE_ACCUM_SCALE = 16.0f;
static const float SPARSE_SDF_GI_RADIANCE_MAX_SAMPLE = 32.0f;
static const float SPARSE_SDF_GI_RADIANCE_HISTORY_DECAY = 0.985f;
static const float SPARSE_SDF_GI_RADIANCE_CONFIDENCE_THRESHOLD = 0.05f;
static const float SPARSE_SDF_GI_BRICK_SH_MAX_SAMPLE_COUNT = 64.0f;
static const float SPARSE_SDF_GI_BRICK_SH_DIRECTIONALITY_THRESHOLD = 0.05f;
static const float SPARSE_SDF_GI_BRICK_SH_MAX_COEFF = SPARSE_SDF_GI_RADIANCE_MAX_SAMPLE * 4.0f;
static const float SPARSE_SDF_GI_BRICK_SH_PROPAGATION_DECAY = 0.9f;
static const float SPARSE_SDF_GI_PROBE_MIN_VARIANCE = 0.05f;
static const uint SPARSE_SDF_GI_PROBE_MAX_RAYS = 64u;

struct FSparseSdfGIProbeHeader
{
    float4 WorldPositionDepth;
    float4 NormalValid;
    uint2 Pixel;
    uint PrevProbeIndex;
    uint PrevProbeValid;
};

struct FSparseSdfGITraceHierarchyNode
{
    uint MinPacked;
    uint MaxPacked;
    uint Flags;
    uint Reserved;
};

struct FBrickShSample
{
    uint4 PackedSH;
    float SampleCount;
    float3 Reserved;
};

struct FBrickShAccum
{
    int4 ShY;
    int Co;
    int Cg;
    uint SampleCount;
    uint Reserved;
};

struct FSparseSdfGICascadeData
{
    float4 MinVoxelSize;
    float4 Extent;
    uint4 Offsets;
};

FSparseSdfGICascadeData LoadSparseSdfGICascadeData(uint cascadeIndex)
{
    StructuredBuffer<FSparseSdfGICascadeData> cascadeData = ResourceDescriptorHeap[CascadeDataSrvIndex];
    return cascadeData[min(cascadeIndex, max(CascadeCount, 1u) - 1u)];
}

float3 GetCascadeMin(FSparseSdfGICascadeData cascade)
{
    return cascade.MinVoxelSize.xyz;
}

float GetCascadeVoxelSize(FSparseSdfGICascadeData cascade)
{
    return cascade.MinVoxelSize.w;
}

float3 GetCascadeExtent(FSparseSdfGICascadeData cascade)
{
    return cascade.Extent.xyz;
}

struct FSparseSdfGIScatterJob
{
    float4 P0;
    float4 P1;
    float4 P2;
    uint4 PairMinSampleCount;
    uint4 PairCount;
};

static const float SPARSE_SDF_GI_PROBE_TEMPORAL_MAX_SAMPLES = 32.0f;
static const float SPARSE_SDF_GI_PROBE_TEMPORAL_MIN_ALPHA = 0.05f;

struct FScreenProbeHistory
{
    float4 WorldPositionCount;
    float4 NormalDepth;
    uint4 PackedSH;
};

groupshared float gs_EikonalA[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared float gs_EikonalB[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_MetadataMinX[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_MetadataMinY[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_MetadataMinZ[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_MetadataMaxX[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_MetadataMaxY[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_MetadataMaxZ[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_MetadataOccupied[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared uint gs_ScatterScan[SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE];

// Cooperative triangle cache for the brick solve. The per-brick reference list is gathered into LDS
// once (by lane 0) and then consumed by all 512 voxel threads, instead of every thread independently
// pointer-chasing the same list through global memory (the previous 512xR global-load cost that
// tripped the GPU TDR watchdog). Gathered in batches so arbitrarily long lists stay fully covered
// within a fixed LDS budget.
static const uint SPARSE_SDF_GI_SOLVE_TRIANGLE_CACHE = 256u;
groupshared float3 gs_SolveTriP0[SPARSE_SDF_GI_SOLVE_TRIANGLE_CACHE];
groupshared float3 gs_SolveTriP1[SPARSE_SDF_GI_SOLVE_TRIANGLE_CACHE];
groupshared float3 gs_SolveTriP2[SPARSE_SDF_GI_SOLVE_TRIANGLE_CACHE];
groupshared uint gs_SolveWalkCursor;
groupshared uint gs_SolveBatchCount;

uint FlattenBrickLocalCoord(uint3 coord)
{
    return coord.x + coord.y * SPARSE_SDF_GI_BRICK_LOCAL_DIM + coord.z * SPARSE_SDF_GI_BRICK_LOCAL_AREA;
}

uint3 UnflattenBrickLocalCoord(uint index)
{
    const uint localMask = SPARSE_SDF_GI_BRICK_LOCAL_DIM - 1u;
    return uint3(
        index & localMask,
        (index >> SPARSE_SDF_GI_BRICK_LOCAL_DIM_LOG2) & localMask,
        (index >> (SPARSE_SDF_GI_BRICK_LOCAL_DIM_LOG2 * 2u)) & localMask);
}

bool IsExactSharedBorderSdf()
{
    return true;
}

uint GetBrickIntervalResolution()
{
    return IsExactSharedBorderSdf() ? SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT : BrickVoxelResolution;
}

float GetSdfWorldDistanceScale()
{
    return (float)GetBrickIntervalResolution() * VoxelSize;
}

float DecodeSdfWorldDistance(float sdf)
{
    return sdf * GetSdfWorldDistanceScale();
}

float EncodeSdfWorldDistance(float distance)
{
    return saturate(distance / max(GetSdfWorldDistanceScale(), 1e-5f));
}

#if defined(SPARSE_SDF_GI_TRACE_SHADER) || defined(SPARSE_SDF_GI_PROBE_TRACE_SHADER)
float3 EvaluateSparseSdfGISky(float3 direction)
{
    TextureCube EnvironmentMap = ResourceDescriptorHeap[EnvironmentCubeIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return EnvironmentMap.SampleLevel(LinearSampler, direction, 0.0f).rgb;
}
#endif

float EikonalLoad(bool readFromA, int3 coord)
{
    if (any(coord < 0) || any(coord >= (int)SPARSE_SDF_GI_BRICK_LOCAL_DIM))
    {
        return 1.0f;
    }

    const uint index = FlattenBrickLocalCoord((uint3)coord);
    return readFromA ? gs_EikonalA[index] : gs_EikonalB[index];
}

void EikonalStore(bool writeToA, uint index, float value)
{
    if (writeToA)
    {
        gs_EikonalA[index] = value;
    }
    else
    {
        gs_EikonalB[index] = value;
    }
}

float SolveEikonal1D(float x, float y, float z, float d)
{
    return min(x, min(y, z)) + d;
}

float SolveEikonal2D(float x, float y, float d)
{
    const float xy = x + y;
    const float v = xy * xy - 2.0f * (x * x + y * y - d * d);
    return (v < 0.0f) ? 1.0f : 0.5f * (xy + sqrt(v));
}

float SolveEikonal3D(float x, float y, float z, float d)
{
    const float xyz = x + y + z;
    const float v = xyz * xyz - 3.0f * (x * x + y * y + z * z - d * d);
    return (v < 0.0f) ? 1.0f : (1.0f / 3.0f) * (xyz + sqrt(v));
}

float RelaxEikonalVoxel(bool readFromA, uint3 localCoord, uint offset)
{
    const int d = (int)offset;
    const int3 coord = (int3)localCoord;
    const float oldValue = EikonalLoad(readFromA, coord);
    const float minX = min(EikonalLoad(readFromA, coord + int3(d, 0, 0)), EikonalLoad(readFromA, coord + int3(-d, 0, 0)));
    const float minY = min(EikonalLoad(readFromA, coord + int3(0, d, 0)), EikonalLoad(readFromA, coord + int3(0, -d, 0)));
    const float minZ = min(EikonalLoad(readFromA, coord + int3(0, 0, d)), EikonalLoad(readFromA, coord + int3(0, 0, -d)));
    const float cellDistance = (float)offset / (float)GetBrickIntervalResolution();

    float e = oldValue;
    e = min(e, SolveEikonal1D(minX, minY, minZ, cellDistance));
    e = min(e, SolveEikonal2D(minX, minY, cellDistance));
    e = min(e, SolveEikonal2D(minX, minZ, cellDistance));
    e = min(e, SolveEikonal2D(minZ, minY, cellDistance));
    e = min(e, SolveEikonal3D(minX, minY, minZ, cellDistance));
    return saturate(e);
}

float PointTriangleDistance(float3 p, float3 a, float3 b, float3 c)
{
    const float3 ab = b - a;
    const float3 ac = c - a;
    const float3 ap = p - a;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
    {
        return length(ap);
    }

    const float3 bp = p - b;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
    {
        return length(bp);
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        const float v = d1 / (d1 - d3);
        return length(p - (a + v * ab));
    }

    const float3 cp = p - c;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
    {
        return length(cp);
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        const float w = d2 / (d2 - d6);
        return length(p - (a + w * ac));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(p - (b + w * (c - b)));
    }

    const float3 n = normalize(cross(ab, ac));
    return abs(dot(p - a, n));
}

uint LinearizeBrickCoord(uint3 brickCoord)
{
    return brickCoord.x
        + brickCoord.y * BrickGridResolution
        + brickCoord.z * BrickGridResolution * BrickGridResolution;
}

uint GetBrickMapOffsetForCascade(uint cascadeIndex)
{
    return cascadeIndex * BrickGridResolution * BrickGridResolution * BrickGridResolution;
}

uint GetCurrentBrickMapOffset()
{
    return GetBrickMapOffsetForCascade(CascadeIndex);
}

uint GetTraceHierarchyBottomOffsetForCascade(uint cascadeIndex)
{
    return cascadeIndex
        * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION
        * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION
        * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION;
}

uint GetTraceHierarchyTopOffsetForCascade(uint cascadeIndex)
{
    return cascadeIndex
        * SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION
        * SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION
        * SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION;
}

uint3 BrickIdToBrickCoord(uint brickId)
{
    return uint3(
        brickId % BrickGridResolution,
        (brickId / BrickGridResolution) % BrickGridResolution,
        brickId / (BrickGridResolution * BrickGridResolution));
}

uint PackTraceHierarchyCoord(uint3 coord)
{
    return (coord.x & 0x3fu)
        | ((coord.y & 0x3fu) << 6u)
        | ((coord.z & 0x3fu) << 12u);
}

uint3 UnpackTraceHierarchyCoord(uint packedCoord)
{
    return uint3(
        packedCoord & 0x3fu,
        (packedCoord >> 6u) & 0x3fu,
        (packedCoord >> 12u) & 0x3fu);
}

uint GetSparseSdfGIBrickCapacity()
{
    return BrickGridResolution * BrickGridResolution * BrickGridResolution;
}

uint GetScatterBrickCapacity()
{
    return ScatterBrickCapacity;
}

uint GetPhysicalBrickId(uint localPhysicalBrick)
{
    return PhysicalBrickBase + localPhysicalBrick;
}

float GetBrickShConfidence(float sampleCount)
{
    return saturate(sampleCount / SPARSE_SDF_GI_BRICK_SH_MAX_SAMPLE_COUNT);
}

bool IsBrickShSampleValid(FBrickShSample sample)
{
    return GetBrickShConfidence(sample.SampleCount) >= SPARSE_SDF_GI_RADIANCE_CONFIDENCE_THRESHOLD;
}

FBrickShSample MakeInvalidBrickShSample()
{
    FBrickShSample sample;
    sample.PackedSH = uint4(0u, 0u, 0u, 0u);
    sample.SampleCount = 0.0f;
    sample.Reserved = 0.0f.xxx;
    return sample;
}

int QuantizeBrickShCoeff(float value)
{
    return (int)round(clamp(value, -SPARSE_SDF_GI_BRICK_SH_MAX_COEFF, SPARSE_SDF_GI_BRICK_SH_MAX_COEFF) * SPARSE_SDF_GI_RADIANCE_ACCUM_SCALE);
}

float DequantizeBrickShCoeff(int value, float invWeight)
{
    return (float)value * invWeight;
}

void AccumulateBrickSH(RWStructuredBuffer<FBrickShAccum> brickAccum, uint brickIndex, FPackedSh sh)
{
    InterlockedAdd(brickAccum[brickIndex].ShY.x, QuantizeBrickShCoeff(sh.ShY.x));
    InterlockedAdd(brickAccum[brickIndex].ShY.y, QuantizeBrickShCoeff(sh.ShY.y));
    InterlockedAdd(brickAccum[brickIndex].ShY.z, QuantizeBrickShCoeff(sh.ShY.z));
    InterlockedAdd(brickAccum[brickIndex].ShY.w, QuantizeBrickShCoeff(sh.ShY.w));
    InterlockedAdd(brickAccum[brickIndex].Co, QuantizeBrickShCoeff(sh.Co));
    InterlockedAdd(brickAccum[brickIndex].Cg, QuantizeBrickShCoeff(sh.Cg));
    InterlockedAdd(brickAccum[brickIndex].SampleCount, 1u);
}

FPackedSh DecodeBrickShSample(FBrickShSample sample)
{
    return UnpackSh(sample.PackedSH);
}

FBrickShSample EncodeBrickShSample(FPackedSh sh, float sampleCount)
{
    FBrickShSample sample;
    sample.PackedSH = PackSh(sh);
    sample.SampleCount = min(sampleCount, SPARSE_SDF_GI_BRICK_SH_MAX_SAMPLE_COUNT);
    sample.Reserved = 0.0f.xxx;
    return sample;
}

float GetBrickShDirectionality(FPackedSh sh)
{
    return saturate(length(sh.ShY.yzw) / max(abs(sh.ShY.x), 1e-5f));
}

float3 GetBrickShDominantDirection(FPackedSh sh)
{
    const float3 direction = float3(-sh.ShY.w, -sh.ShY.y, sh.ShY.z);
    const float lengthSq = dot(direction, direction);
    return (lengthSq > 1e-8f) ? direction * rsqrt(lengthSq) : 0.0f.xxx;
}

float3 EvaluateBrickShSample(FBrickShSample sample, float3 normal, bool normalReliable)
{
    const FPackedSh sh = DecodeBrickShSample(sample);
    return normalReliable ? UnprojectIrradiance(sh, normal) : ApproxRadiance(sh);
}

uint GetScatterPairForFirstLogicalSample(uint logicalSample)
{
    return logicalSample + ((logicalSample > 0u) ? ((logicalSample - 1u) / SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT) : 0u);
}

uint GetScatterPairForLastLogicalSample(uint logicalSample)
{
    return logicalSample + min(logicalSample / SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT, BrickGridResolution - 1u);
}

void DecodeScatterPair(uint pair, out uint brickCoord, out uint localCoord)
{
    brickCoord = min(pair / SPARSE_SDF_GI_BRICK_LOCAL_DIM, BrickGridResolution - 1u);
    localCoord = pair - brickCoord * SPARSE_SDF_GI_BRICK_LOCAL_DIM;
}

uint EncodeScatterDistanceUint(float distance)
{
    const float normalized = saturate(distance / max(GetSdfWorldDistanceScale(), 1e-5f));
    return min((uint)round(normalized * (float)SPARSE_SDF_GI_SCATTER_DISTANCE_FAR_UINT), SPARSE_SDF_GI_SCATTER_DISTANCE_FAR_UINT);
}

float DecodeScatterDistanceUint(uint encoded)
{
    return (float)min(encoded, SPARSE_SDF_GI_SCATTER_DISTANCE_FAR_UINT) / (float)SPARSE_SDF_GI_SCATTER_DISTANCE_FAR_UINT;
}

uint GetScatterLinearDispatchId(uint3 groupId, uint groupThreadIndex, uint groupSize)
{
    const uint groupLinear = groupId.x
        + groupId.y * SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT
        + groupId.z * SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT * SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT;
    return groupLinear * groupSize + groupThreadIndex;
}

void StoreScatterDispatchArgs(RWByteAddressBuffer args, uint itemCount, uint groupSize)
{
    const uint groupCount = (itemCount + groupSize - 1u) / groupSize;
    const uint dispatchX = min(groupCount, SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT);
    const uint dispatchY = (groupCount + SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT - 1u) / SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT;
    args.Store(0u, dispatchX);
    args.Store(4u, max(dispatchY, 1u));
    args.Store(8u, 1u);
}

uint ScanScatterJobOffsets(StructuredBuffer<uint> offsets, uint jobCount, uint sampleIndex)
{
    uint lo = 0u;
    uint hi = jobCount;
    [loop]
    while (lo < hi)
    {
        const uint mid = (lo + hi) >> 1u;
        const uint endOffset = offsets[mid];
        if (sampleIndex < endOffset)
        {
            hi = mid;
        }
        else
        {
            lo = mid + 1u;
        }
    }
    return min(lo, max(jobCount, 1u) - 1u);
}

bool ResolveScatterSample(
    StructuredBuffer<FSparseSdfGIScatterJob> jobs,
    StructuredBuffer<uint> offsets,
    uint jobCount,
    uint sampleCount,
    uint sampleIndex,
    out FSparseSdfGIScatterJob job,
    out uint3 brickCoord,
    out uint3 localCoord,
    out float3 worldPosition)
{
    if (jobCount == 0u || sampleIndex >= sampleCount)
    {
        return false;
    }

    const uint jobIndex = ScanScatterJobOffsets(offsets, jobCount, sampleIndex);
    job = jobs[jobIndex];
    const uint jobStart = (jobIndex == 0u) ? 0u : offsets[jobIndex - 1u];
    uint localSample = sampleIndex - jobStart;
    const uint cx = max(job.PairCount.x, 1u);
    const uint cy = max(job.PairCount.y, 1u);
    const uint pairX = job.PairMinSampleCount.x + (localSample % cx);
    localSample /= cx;
    const uint pairY = job.PairMinSampleCount.y + (localSample % cy);
    localSample /= cy;
    const uint pairZ = job.PairMinSampleCount.z + localSample;

    DecodeScatterPair(pairX, brickCoord.x, localCoord.x);
    DecodeScatterPair(pairY, brickCoord.y, localCoord.y);
    DecodeScatterPair(pairZ, brickCoord.z, localCoord.z);
    if (any(brickCoord >= BrickGridResolution.xxx) || any(localCoord >= SPARSE_SDF_GI_BRICK_LOCAL_DIM.xxx))
    {
        return false;
    }

    worldPosition = CascadeMin + ((float3)(brickCoord * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT + localCoord) * VoxelSize);
    return true;
}

uint PackBrickLocalAabb(uint3 localMin, uint3 localMax)
{
    return (localMin.x & 0xfu)
        | ((localMin.y & 0xfu) << 4u)
        | ((localMin.z & 0xfu) << 8u)
        | ((localMax.x & 0xfu) << 12u)
        | ((localMax.y & 0xfu) << 16u)
        | ((localMax.z & 0xfu) << 20u);
}

uint3 UnpackBrickLocalAabbMin(uint packedAabb)
{
    return uint3(
        packedAabb & 0xfu,
        (packedAabb >> 4u) & 0xfu,
        (packedAabb >> 8u) & 0xfu);
}

uint3 UnpackBrickLocalAabbMax(uint packedAabb)
{
    return uint3(
        (packedAabb >> 12u) & 0xfu,
        (packedAabb >> 16u) & 0xfu,
        (packedAabb >> 20u) & 0xfu);
}

float LoadSdfBrickLocal(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint brickMapOffset, uint3 brickCoord, uint3 localCoord)
{
    if (any(brickCoord >= BrickGridResolution.xxx) || any(localCoord >= BrickVoxelResolution.xxx))
    {
        return 1.0f;
    }

    const uint brickMapIndex = LinearizeBrickCoord(brickCoord);
    const uint brickCount = BrickGridResolution * BrickGridResolution * BrickGridResolution;
    if (brickMapIndex >= brickCount)
    {
        return 1.0f;
    }

    const uint brickId = cascadeBrickMap[brickMapOffset + brickMapIndex];
    if (brickId == SPARSE_SDF_GI_INVALID_BRICK_ID || brickId >= brickCount)
    {
        return 1.0f;
    }

    const uint3 atlasCoord = BrickIdToBrickCoord(brickId) * BrickVoxelResolution + localCoord;
    if (any(atlasCoord >= AtlasResolution.xxx))
    {
        return 1.0f;
    }

    return sdfAtlas.Load(int4((int3)atlasCoord, 0)).r;
}

void GetExactSharedBorderCell(float3 worldPosition, out uint3 brickCoord, out uint3 localCoord, out float3 fracCoord, out bool valid)
{
    const float intervalCount = (float)(BrickGridResolution * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT);
    const float3 logicalFloat = (worldPosition - CascadeMin) / VoxelSize;
    valid = !any(logicalFloat < 0.0f.xxx) && !any(logicalFloat > intervalCount.xxx);
    const float3 clampedLogical = min(logicalFloat, (intervalCount - 1e-4f).xxx);
    const int3 baseLogical = (int3)floor(clampedLogical);
    fracCoord = saturate(clampedLogical - (float3)baseLogical);
    brickCoord = (uint3)baseLogical / SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    localCoord = (uint3)baseLogical - brickCoord * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    valid = valid && all(brickCoord < BrickGridResolution.xxx) && all(localCoord < SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT.xxx);
}

float SampleSdfAtlasExactSharedBorder(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    uint3 brickCoord = uint3(0u, 0u, 0u);
    uint3 localCoord = uint3(0u, 0u, 0u);
    float3 fracCoord = 0.0f.xxx;
    bool valid = false;
    GetExactSharedBorderCell(worldPosition, brickCoord, localCoord, fracCoord, valid);
    if (!valid)
    {
        return 1.0f;
    }

    const uint brickMapOffset = GetCurrentBrickMapOffset();
    const float c000 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 0u, 0u));
    const float c100 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 0u, 0u));
    const float c010 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 1u, 0u));
    const float c110 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 1u, 0u));
    const float c001 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 0u, 1u));
    const float c101 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 0u, 1u));
    const float c011 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 1u, 1u));
    const float c111 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 1u, 1u));

    const float c00 = lerp(c000, c100, fracCoord.x);
    const float c10 = lerp(c010, c110, fracCoord.x);
    const float c01 = lerp(c001, c101, fracCoord.x);
    const float c11 = lerp(c011, c111, fracCoord.x);
    const float c0 = lerp(c00, c10, fracCoord.y);
    const float c1 = lerp(c01, c11, fracCoord.y);
    return lerp(c0, c1, fracCoord.z);
}

float SampleSdfAtlas(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    return SampleSdfAtlasExactSharedBorder(sdfAtlas, cascadeBrickMap, worldPosition);
}

float SampleSdfAtlasPointExactSharedBorder(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    const float intervalCount = (float)(BrickGridResolution * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT);
    const float3 logicalFloat = (worldPosition - CascadeMin) / VoxelSize;
    if (any(logicalFloat < 0.0f.xxx) || any(logicalFloat > intervalCount.xxx))
    {
        return 1.0f;
    }

    const int3 roundedLogical = (int3)floor(min(logicalFloat + 0.5f.xxx, intervalCount.xxx));
    const uint3 brickCoord = min((uint3)roundedLogical / SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT, (BrickGridResolution - 1u).xxx);
    const uint3 localCoord = min((uint3)roundedLogical - brickCoord * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT, (BrickVoxelResolution - 1u).xxx);
    return LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, GetCurrentBrickMapOffset(), brickCoord, localCoord);
}

float SampleSdfAtlasPoint(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    return SampleSdfAtlasPointExactSharedBorder(sdfAtlas, cascadeBrickMap, worldPosition);
}

float SampleSdfAtlasDebugSurface(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    return min(SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition), SampleSdfAtlasPoint(sdfAtlas, cascadeBrickMap, worldPosition));
}

float SampleExactSdfBrickLocalSmooth(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint brickMapOffset, uint3 brickCoord, float3 localPosition)
{
    const float maxLocal = (float)SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    const float3 clampedLocal = clamp(localPosition, 0.0f.xxx, maxLocal.xxx);
    const int3 baseLocal = (int3)floor(min(clampedLocal, (maxLocal - 1e-4f).xxx));
    float3 fracCoord = saturate(clampedLocal - (float3)baseLocal);
    fracCoord = fracCoord * fracCoord * (3.0f - 2.0f * fracCoord);
    const uint3 localCoord = (uint3)baseLocal;

    const float c000 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 0u, 0u));
    const float c100 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 0u, 0u));
    const float c010 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 1u, 0u));
    const float c110 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 1u, 0u));
    const float c001 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 0u, 1u));
    const float c101 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 0u, 1u));
    const float c011 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(0u, 1u, 1u));
    const float c111 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localCoord + uint3(1u, 1u, 1u));

    const float c00 = lerp(c000, c100, fracCoord.x);
    const float c10 = lerp(c010, c110, fracCoord.x);
    const float c01 = lerp(c001, c101, fracCoord.x);
    const float c11 = lerp(c011, c111, fracCoord.x);
    const float c0 = lerp(c00, c10, fracCoord.y);
    const float c1 = lerp(c01, c11, fracCoord.y);
    return lerp(c0, c1, fracCoord.z);
}

float3 ComputeExactBrickLocalSdfGradientAnalytic(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint brickMapOffset, uint3 brickCoord, float3 localPosition)
{
    const float maxLocal = (float)SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    const float3 clampedLocal = clamp(localPosition, 0.0f.xxx, maxLocal.xxx);
    const int3 baseLocal = (int3)floor(min(clampedLocal, (maxLocal - 1e-4f).xxx));
    const float3 f = saturate(clampedLocal - (float3)baseLocal);
    const uint3 lc = (uint3)baseLocal;

    const float c000 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(0u, 0u, 0u));
    const float c100 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(1u, 0u, 0u));
    const float c010 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(0u, 1u, 0u));
    const float c110 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(1u, 1u, 0u));
    const float c001 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(0u, 0u, 1u));
    const float c101 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(1u, 0u, 1u));
    const float c011 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(0u, 1u, 1u));
    const float c111 = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, lc + uint3(1u, 1u, 1u));

    const float dx =
        lerp(lerp(c100 - c000, c110 - c010, f.y),
             lerp(c101 - c001, c111 - c011, f.y), f.z);
    const float dy =
        lerp(lerp(c010 - c000, c110 - c100, f.x),
             lerp(c011 - c001, c111 - c101, f.x), f.z);
    const float dz =
        lerp(lerp(c001 - c000, c101 - c100, f.x),
             lerp(c011 - c010, c111 - c110, f.x), f.y);

    return float3(dx, dy, dz);
}

float3 ComputeExactBrickLocalSdfNormal(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint brickMapOffset, uint3 brickCoord, uint3 localCoord, float3 fracCoord)
{
    const float3 localPosition = (float3)localCoord + fracCoord;
    const float3 gradient = ComputeExactBrickLocalSdfGradientAnalytic(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localPosition);
    const float gradientLengthSq = dot(gradient, gradient);
    if (gradientLengthSq > 1e-8f)
    {
        return gradient * rsqrt(gradientLengthSq);
    }

    const float3 uvw = localPosition / (float)SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    const float3 fallback = uvw - 0.5f.xxx;
    const float fallbackLengthSq = dot(fallback, fallback);
    return (fallbackLengthSq > 1e-8f) ? fallback * rsqrt(fallbackLengthSq) : 0.0f.xxx;
}

float3 ComputeExactBrickLocalSdfNormalRounded(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint brickMapOffset, uint3 brickCoord, uint3 localCoord, float3 fracCoord)
{
    const float intervalDim = (float)SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    float3 localPosition = (float3)localCoord + fracCoord;
    localPosition += (intervalDim / 512.0f).xxx;

    const float eps = 0.25f;
    const float halfDim = intervalDim * 0.5f;
    float3 k = 1.0f.xxx;
    k.x = (localPosition.x > halfDim) ? -1.0f : 1.0f;
    k.y = (localPosition.y > halfDim) ? -1.0f : 1.0f;
    k.z = (localPosition.z > halfDim) ? -1.0f : 1.0f;

    const float center = SampleExactSdfBrickLocalSmooth(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localPosition);
    const float3 gradient =
        float3(k.x, 0.0f, 0.0f) * (SampleExactSdfBrickLocalSmooth(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localPosition + float3(k.x * eps, 0.0f, 0.0f)) - center) +
        float3(0.0f, k.y, 0.0f) * (SampleExactSdfBrickLocalSmooth(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localPosition + float3(0.0f, k.y * eps, 0.0f)) - center) +
        float3(0.0f, 0.0f, k.z) * (SampleExactSdfBrickLocalSmooth(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, localPosition + float3(0.0f, 0.0f, k.z * eps)) - center);

    const float gradientLengthSq = dot(gradient, gradient);
    if (gradientLengthSq > 1e-8f)
    {
        return gradient * rsqrt(gradientLengthSq);
    }

    const float3 uvw = localPosition / intervalDim;
    const float3 fallback = uvw - 0.5f.xxx;
    const float fallbackLengthSq = dot(fallback, fallback);
    return (fallbackLengthSq > 1e-8f) ? fallback * rsqrt(fallbackLengthSq) : 0.0f.xxx;
}

bool IsClearlyOutsideCascade(float3 worldPosition)
{
    const float3 atlasFloat = (worldPosition - CascadeMin) / VoxelSize;
    const float intervalCount = IsExactSharedBorderSdf()
        ? (float)(BrickGridResolution * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT)
        : (float)AtlasResolution;
    return any(atlasFloat < -0.5f.xxx) || any(atlasFloat > (intervalCount + 0.5f).xxx);
}

bool TryGetBrickCoordFromWorld(float3 worldPosition, out uint3 brickCoord)
{
    const float3 atlasFloat = (worldPosition - CascadeMin) / VoxelSize;
    const float intervalCount = IsExactSharedBorderSdf()
        ? (float)(BrickGridResolution * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT)
        : (float)AtlasResolution;
    if (any(atlasFloat < 0.0f.xxx) || any(atlasFloat >= intervalCount.xxx))
    {
        brickCoord = uint3(0u, 0u, 0u);
        return false;
    }

    brickCoord = (uint3)floor(atlasFloat) / GetBrickIntervalResolution();
    return all(brickCoord < BrickGridResolution.xxx);
}

float3 GetSafeRayDirection(float3 rayDirection)
{
    float3 safeDirection = rayDirection;
    safeDirection.x = (abs(safeDirection.x) < 1e-6f) ? ((safeDirection.x < 0.0f) ? -1e-6f : 1e-6f) : safeDirection.x;
    safeDirection.y = (abs(safeDirection.y) < 1e-6f) ? ((safeDirection.y < 0.0f) ? -1e-6f : 1e-6f) : safeDirection.y;
    safeDirection.z = (abs(safeDirection.z) < 1e-6f) ? ((safeDirection.z < 0.0f) ? -1e-6f : 1e-6f) : safeDirection.z;
    return safeDirection;
}

bool RayAabbIntersectRange(float3 rayOrigin, float3 rayDirection, float3 boxMin, float3 boxMax, out float tEnter, out float tExit)
{
    const float3 safeDirection = GetSafeRayDirection(rayDirection);
    const float3 t0 = (boxMin - rayOrigin) / safeDirection;
    const float3 t1 = (boxMax - rayOrigin) / safeDirection;
    const float3 tNear = min(t0, t1);
    const float3 tFar = max(t0, t1);

    tEnter = max(max(tNear.x, tNear.y), tNear.z);
    tExit = min(min(tFar.x, tFar.y), tFar.z);
    return tExit >= max(tEnter, 0.0f);
}

void GetBrickWorldBounds(uint3 brickCoord, out float3 brickMin, out float3 brickMax)
{
    const uint intervalResolution = GetBrickIntervalResolution();
    const uint3 globalMin = brickCoord * intervalResolution;
    const uint3 globalMax = globalMin + intervalResolution;
    brickMin = CascadeMin + (float3)globalMin * VoxelSize;
    brickMax = CascadeMin + (float3)globalMax * VoxelSize;
}

uint LinearizeTraceHierarchyCoord(uint3 coord, uint resolution)
{
    return coord.x + coord.y * resolution + coord.z * resolution * resolution;
}

uint3 BrickIdToAtlasBrickCoord(uint brickId)
{
    return uint3(
        brickId % BrickGridResolution,
        (brickId / BrickGridResolution) % BrickGridResolution,
        brickId / (BrickGridResolution * BrickGridResolution));
}

bool TryGetBrickCoordFromWorldCascade(FSparseSdfGICascadeData cascade, float3 worldPosition, out uint3 brickCoord)
{
    const float intervalResolution = (float)GetBrickIntervalResolution();
    const float3 logical = (worldPosition - GetCascadeMin(cascade)) / max(GetCascadeVoxelSize(cascade), 1e-5f);
    if (any(logical < 0.0f.xxx) || any(logical >= (float)BrickGridResolution * intervalResolution))
    {
        brickCoord = 0u.xxx;
        return false;
    }

    brickCoord = min((uint3)(logical / intervalResolution), (BrickGridResolution - 1u).xxx);
    return true;
}

bool TryGetPhysicalBrickIdFromWorld(StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition, out uint brickId)
{
    const uint activeCascadeCount = clamp(CascadeCount, 1u, 4u);
    [loop]
    for (uint cascadeIndex = 0u; cascadeIndex < activeCascadeCount; ++cascadeIndex)
    {
        const FSparseSdfGICascadeData cascade = LoadSparseSdfGICascadeData(cascadeIndex);
        uint3 brickCoord = 0u.xxx;
        if (!TryGetBrickCoordFromWorldCascade(cascade, worldPosition, brickCoord))
        {
            continue;
        }

        brickId = cascadeBrickMap[cascade.Offsets.x + LinearizeBrickCoord(brickCoord)];
        if (brickId != SPARSE_SDF_GI_INVALID_BRICK_ID && brickId < GetSparseSdfGIBrickCapacity())
        {
            return true;
        }
    }

    brickId = 0u;
    return false;
}

void GetBrickWorldBoundsCascade(FSparseSdfGICascadeData cascade, uint3 brickCoord, out float3 brickMin, out float3 brickMax)
{
    const uint intervalResolution = GetBrickIntervalResolution();
    const uint3 globalMin = brickCoord * intervalResolution;
    const uint3 globalMax = globalMin + intervalResolution.xxx;
    brickMin = GetCascadeMin(cascade) + (float3)globalMin * GetCascadeVoxelSize(cascade);
    brickMax = GetCascadeMin(cascade) + (float3)globalMax * GetCascadeVoxelSize(cascade);
}

uint LinearizeTraceHierarchyCoordOffset(uint3 coord, uint resolution, uint offset)
{
    return offset + LinearizeTraceHierarchyCoord(coord, resolution);
}

bool TryGetTraceHierarchyCoordCascade(FSparseSdfGICascadeData cascade, float3 worldPosition, uint cellSizeBricks, uint resolution, out uint3 cellCoord)
{
    uint3 brickCoord = 0u.xxx;
    if (!TryGetBrickCoordFromWorldCascade(cascade, worldPosition, brickCoord))
    {
        cellCoord = 0u.xxx;
        return false;
    }

    cellCoord = brickCoord / cellSizeBricks;
    return all(cellCoord < resolution.xxx);
}

void GetTraceHierarchyCellWorldBoundsCascade(FSparseSdfGICascadeData cascade, uint3 cellCoord, uint cellSizeBricks, out float3 cellMin, out float3 cellMax)
{
    const uint intervalResolution = GetBrickIntervalResolution();
    const uint3 brickMinCoord = cellCoord * cellSizeBricks;
    const uint3 brickMaxCoord = min(brickMinCoord + cellSizeBricks, BrickGridResolution.xxx);
    cellMin = GetCascadeMin(cascade) + (float3)(brickMinCoord * intervalResolution) * GetCascadeVoxelSize(cascade);
    cellMax = GetCascadeMin(cascade) + (float3)(brickMaxCoord * intervalResolution) * GetCascadeVoxelSize(cascade);
}

void GetTraceHierarchyNodeWorldBoundsCascade(FSparseSdfGICascadeData cascade, FSparseSdfGITraceHierarchyNode node, out float3 nodeMin, out float3 nodeMax)
{
    float3 unused = 0.0f.xxx;
    GetBrickWorldBoundsCascade(cascade, UnpackTraceHierarchyCoord(node.MinPacked), nodeMin, unused);
    GetBrickWorldBoundsCascade(cascade, UnpackTraceHierarchyCoord(node.MaxPacked), unused, nodeMax);
}

float LoadSdfBrickLocalCascade(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, FSparseSdfGICascadeData cascade, uint3 brickCoord, uint3 localCoord)
{
    if (any(brickCoord >= BrickGridResolution.xxx) || any(localCoord >= BrickVoxelResolution.xxx))
    {
        return 1.0f;
    }

    const uint brickMapIndex = cascade.Offsets.x + LinearizeBrickCoord(brickCoord);
    const uint brickId = cascadeBrickMap[brickMapIndex];
    if (brickId == SPARSE_SDF_GI_INVALID_BRICK_ID || brickId >= GetSparseSdfGIBrickCapacity())
    {
        return 1.0f;
    }

    const uint3 atlasCoord = BrickIdToAtlasBrickCoord(brickId) * BrickVoxelResolution + localCoord;
    return any(atlasCoord >= AtlasResolution.xxx) ? 1.0f : sdfAtlas.Load(int4((int3)atlasCoord, 0)).r;
}

void GetExactSharedBorderCellCascade(FSparseSdfGICascadeData cascade, float3 worldPosition, out uint3 brickCoord, out uint3 localCoord, out float3 fracCoord, out bool valid)
{
    const float intervalCount = (float)(BrickGridResolution * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT);
    const float3 logicalFloat = (worldPosition - GetCascadeMin(cascade)) / GetCascadeVoxelSize(cascade);
    valid = !any(logicalFloat < 0.0f.xxx) && !any(logicalFloat > intervalCount.xxx);
    const float3 clampedLogical = min(logicalFloat, (intervalCount - 1e-4f).xxx);
    const int3 baseLogical = (int3)floor(clampedLogical);
    fracCoord = saturate(clampedLogical - (float3)baseLogical);
    brickCoord = (uint3)baseLogical / SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    localCoord = (uint3)baseLogical - brickCoord * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    valid = valid && all(brickCoord < BrickGridResolution.xxx) && all(localCoord < SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT.xxx);
}

float SampleSdfAtlasCascade(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, FSparseSdfGICascadeData cascade, float3 worldPosition, bool pointSample)
{
    if (pointSample)
    {
        const float intervalCount = (float)(BrickGridResolution * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT);
        const float3 logicalFloat = (worldPosition - GetCascadeMin(cascade)) / GetCascadeVoxelSize(cascade);
        if (any(logicalFloat < 0.0f.xxx) || any(logicalFloat > intervalCount.xxx))
        {
            return 1.0f;
        }

        const int3 roundedLogical = (int3)floor(min(logicalFloat + 0.5f.xxx, intervalCount.xxx));
        const uint3 brickCoord = min((uint3)roundedLogical / SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT, (BrickGridResolution - 1u).xxx);
        const uint3 localCoord = min((uint3)roundedLogical - brickCoord * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT, (BrickVoxelResolution - 1u).xxx);
        return LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord);
    }

    uint3 brickCoord = 0u.xxx;
    uint3 localCoord = 0u.xxx;
    float3 fracCoord = 0.0f.xxx;
    bool valid = false;
    GetExactSharedBorderCellCascade(cascade, worldPosition, brickCoord, localCoord, fracCoord, valid);
    if (!valid)
    {
        return 1.0f;
    }

    const float c000 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(0u, 0u, 0u));
    const float c100 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(1u, 0u, 0u));
    const float c010 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(0u, 1u, 0u));
    const float c110 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(1u, 1u, 0u));
    const float c001 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(0u, 0u, 1u));
    const float c101 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(1u, 0u, 1u));
    const float c011 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(0u, 1u, 1u));
    const float c111 = LoadSdfBrickLocalCascade(sdfAtlas, cascadeBrickMap, cascade, brickCoord, localCoord + uint3(1u, 1u, 1u));
    const float c00 = lerp(c000, c100, fracCoord.x);
    const float c10 = lerp(c010, c110, fracCoord.x);
    const float c01 = lerp(c001, c101, fracCoord.x);
    const float c11 = lerp(c011, c111, fracCoord.x);
    return lerp(lerp(c00, c10, fracCoord.y), lerp(c01, c11, fracCoord.y), fracCoord.z);
}

float SampleTraceSdfCascade(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, FSparseSdfGICascadeData cascade, float3 worldPosition, bool debugSurface)
{
    const float linearSdf = SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition, false);
    return debugSurface ? min(linearSdf, SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition, true)) : linearSdf;
}

bool TryGetLeafAabbCascade(
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    FSparseSdfGICascadeData cascade,
    uint3 brickCoord,
    out float3 aabbMin,
    out float3 aabbMax)
{
    aabbMin = 0.0f.xxx;
    aabbMax = 0.0f.xxx;

    const uint brickId = cascadeBrickMap[cascade.Offsets.x + LinearizeBrickCoord(brickCoord)];
    const uint4 metadata = (brickId < GetSparseSdfGIBrickCapacity()) ? brickMetadata[brickId] : uint4(0u, 0u, 0u, 0u);
    if (brickId >= GetSparseSdfGIBrickCapacity() || (metadata.y & SPARSE_SDF_GI_BRICK_METADATA_OCCUPIED) == 0u)
    {
        return false;
    }

    const uint3 localMin = UnpackBrickLocalAabbMin(metadata.x);
    const uint3 localMax = UnpackBrickLocalAabbMax(metadata.x);
    const uint intervalResolution = GetBrickIntervalResolution();
    const uint3 globalMin = brickCoord * intervalResolution + localMin;
    const uint3 globalMax = brickCoord * intervalResolution + localMax + uint3(1u, 1u, 1u);
    const float margin = SPARSE_SDF_GI_BRICK_AABB_MARGIN_VOXELS * GetCascadeVoxelSize(cascade);
    aabbMin = max(GetCascadeMin(cascade), GetCascadeMin(cascade) + (float3)globalMin * GetCascadeVoxelSize(cascade) - margin.xxx);
    aabbMax = min(GetCascadeMin(cascade) + GetCascadeExtent(cascade), GetCascadeMin(cascade) + (float3)globalMax * GetCascadeVoxelSize(cascade) + margin.xxx);
    return true;
}

void StoreTraceHitCellCascade(FSparseSdfGICascadeData cascade, float3 hitPosition, uint3 brickCoord, out uint3 hitBrickCoord, out uint3 hitLocalCoord, out float3 hitFracCoord)
{
    const float intervalDim = (float)GetBrickIntervalResolution();
    const float3 logical = (hitPosition - GetCascadeMin(cascade)) / GetCascadeVoxelSize(cascade);
    const float3 localPos = clamp(logical - (float3)brickCoord * intervalDim, 0.0f.xxx, intervalDim.xxx);
    const float3 baseLocal = floor(min(localPos, (intervalDim - 1e-4f).xxx));
    hitBrickCoord = brickCoord;
    hitLocalCoord = (uint3)baseLocal;
    hitFracCoord = saturate(localPos - baseLocal);
}

bool TryTraceHierarchyLeafCascade(
    Texture3D<float> sdfAtlas,
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    FSparseSdfGICascadeData cascade,
    float3 rayOrigin,
    float3 rayDirection,
    uint3 brickCoord,
    float leafExit,
    bool debugSurface,
    inout float travel,
    inout uint stepCount,
    out uint3 hitBrickCoord,
    out uint3 hitLocalCoord,
    out float3 hitFracCoord)
{
    float3 leafAabbMin = 0.0f.xxx;
    float3 leafAabbMax = 0.0f.xxx;
    if (!TryGetLeafAabbCascade(cascadeBrickMap, brickMetadata, cascade, brickCoord, leafAabbMin, leafAabbMax))
    {
        return false;
    }

    float leafAabbEnter = 0.0f;
    float leafAabbExit = 0.0f;
    if (!RayAabbIntersectRange(rayOrigin, rayDirection, leafAabbMin, leafAabbMax, leafAabbEnter, leafAabbExit) || leafAabbExit < travel || leafAabbEnter > leafExit)
    {
        return false;
    }

    const float voxelSize = GetCascadeVoxelSize(cascade);
    const float hitThreshold = voxelSize * max(SurfaceHitThresholdVoxels, 0.01f);
    const float minStepDistance = max(voxelSize * 0.25f, 1e-4f);
    const float maxStepDistance = max(voxelSize * 2.0f, minStepDistance);
    float innerTravel = max(travel, leafAabbEnter);

    [loop]
    for (uint innerIndex = 0u; innerIndex < SPARSE_SDF_GI_TRACE_HIERARCHY_INNER_STEPS && innerTravel <= min(leafAabbExit, leafExit); ++innerIndex)
    {
        const float3 samplePosition = rayOrigin + rayDirection * innerTravel;
        const float decodedDistance = SampleTraceSdfCascade(sdfAtlas, cascadeBrickMap, cascade, samplePosition, debugSurface) * GetBrickIntervalResolution() * voxelSize;
        ++stepCount;
        if (decodedDistance <= hitThreshold)
        {
            travel = innerTravel;
            StoreTraceHitCellCascade(cascade, samplePosition, brickCoord, hitBrickCoord, hitLocalCoord, hitFracCoord);
            return true;
        }
        innerTravel += clamp(decodedDistance, minStepDistance, maxStepDistance);
    }

    return false;
}

bool TrySkipCurrentBrick(
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    float3 rayOrigin,
    float3 rayDirection,
    float travel,
    out float nextTravel)
{
    nextTravel = travel;

    const float3 samplePosition = rayOrigin + rayDirection * travel;
    uint3 brickCoord = uint3(0u, 0u, 0u);
    if (!TryGetBrickCoordFromWorld(samplePosition, brickCoord))
    {
        return false;
    }

    const uint brickMapIndex = LinearizeBrickCoord(brickCoord);
    const uint brickCount = BrickGridResolution * BrickGridResolution * BrickGridResolution;
    if (brickMapIndex >= brickCount)
    {
        return false;
    }

    float3 brickMin = 0.0f.xxx;
    float3 brickMax = 0.0f.xxx;
    GetBrickWorldBounds(brickCoord, brickMin, brickMax);

    float brickEnter = 0.0f;
    float brickExit = 0.0f;
    if (!RayAabbIntersectRange(rayOrigin, rayDirection, brickMin, brickMax, brickEnter, brickExit))
    {
        return false;
    }

    const float epsilon = max(VoxelSize * 0.01f, 1e-4f);
    const uint brickId = cascadeBrickMap[GetCurrentBrickMapOffset() + brickMapIndex];
    const uint4 metadata = (brickId < brickCount) ? brickMetadata[brickId] : uint4(0u, 0u, 0u, 0u);
    bool shouldSkip = brickId == SPARSE_SDF_GI_INVALID_BRICK_ID || brickId >= brickCount || (metadata.y & SPARSE_SDF_GI_BRICK_METADATA_OCCUPIED) == 0u;

    if (!shouldSkip)
    {
        const uint3 localMin = UnpackBrickLocalAabbMin(metadata.x);
        const uint3 localMax = UnpackBrickLocalAabbMax(metadata.x);
        const uint intervalResolution = GetBrickIntervalResolution();
        const uint3 globalMin = brickCoord * intervalResolution + localMin;
        const uint3 globalMax = brickCoord * intervalResolution + localMax + uint3(1u, 1u, 1u);
        const float margin = SPARSE_SDF_GI_BRICK_AABB_MARGIN_VOXELS * VoxelSize;
        const float3 margin3 = float3(margin, margin, margin);
        const float3 aabbMin = max(CascadeMin, CascadeMin + (float3)globalMin * VoxelSize - margin3);
        const float3 aabbMax = min(CascadeMin + CascadeExtent, CascadeMin + (float3)globalMax * VoxelSize + margin3);

        float aabbEnter = 0.0f;
        float aabbExit = 0.0f;
        const bool intersectsAabb = RayAabbIntersectRange(rayOrigin, rayDirection, aabbMin, aabbMax, aabbEnter, aabbExit)
            && aabbExit >= travel
            && aabbEnter <= brickExit;
        shouldSkip = !intersectsAabb;
    }

    if (!shouldSkip)
    {
        return false;
    }

    const float candidateTravel = brickExit + epsilon;
    if (candidateTravel <= travel)
    {
        return false;
    }

    nextTravel = candidateTravel;
    return true;
}

bool TraceSdfHierarchicalRawMulti(
    Texture3D<float> sdfAtlas,
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop,
    float3 rayOrigin,
    float3 rayDirection,
    bool debugSurface,
    out float hitTravel,
    out uint stepCount,
    out uint traceStatus,
    out uint3 hitBrickCoord,
    out uint3 hitLocalCoord,
    out float3 hitFracCoord,
    out uint hitCascadeIndex)
{
    hitTravel = 0.0f;
    stepCount = 0u;
    traceStatus = SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS;
    hitBrickCoord = 0u.xxx;
    hitLocalCoord = 0u.xxx;
    hitFracCoord = 0.0f.xxx;
    hitCascadeIndex = 0u;

    float rayT = 0.0f;
    const uint activeCascadeCount = clamp(CascadeCount, 1u, 4u);
    const uint cascadeIterLimit = (activeCascadeCount <= 1u) ? SPARSE_SDF_GI_TRACE_HIERARCHY_ITER_LIMIT_SINGLE_CASCADE : SPARSE_SDF_GI_TRACE_HIERARCHY_ITER_LIMIT;
    [loop]
    for (uint cascadeIndex = 0u; cascadeIndex < activeCascadeCount; ++cascadeIndex)
    {
        const FSparseSdfGICascadeData cascade = LoadSparseSdfGICascadeData(cascadeIndex);
        const float3 cascadeMin = GetCascadeMin(cascade);
        const float3 cascadeMax = cascadeMin + GetCascadeExtent(cascade);
        const float voxelSize = GetCascadeVoxelSize(cascade);
        const float epsilon = max(voxelSize * 0.01f, 1e-4f);

        float cascadeEnter = 0.0f;
        float cascadeExit = 0.0f;
        if (!RayAabbIntersectRange(rayOrigin, rayDirection, cascadeMin, cascadeMax, cascadeEnter, cascadeExit))
        {
            continue;
        }

        const float endT = min(cascadeExit, MaxTraceDistance);
        if (rayT > endT)
        {
            continue;
        }

        float travel = max(rayT, max(cascadeEnter, 0.0f));
        uint traversalIter = 0u;
        bool cascadeLimited = false;
        [loop]
        while (traversalIter < cascadeIterLimit && travel <= endT)
        {
            ++traversalIter;
            ++stepCount;
            const float3 samplePosition = rayOrigin + rayDirection * travel;
            uint3 topCoord = 0u.xxx;
            if (!TryGetTraceHierarchyCoordCascade(cascade, samplePosition, SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_CELL_SIZE, SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION, topCoord))
            {
                break;
            }

            float3 topCellMin = 0.0f.xxx;
            float3 topCellMax = 0.0f.xxx;
            GetTraceHierarchyCellWorldBoundsCascade(cascade, topCoord, SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_CELL_SIZE, topCellMin, topCellMax);
            float topCellEnter = 0.0f;
            float topCellExit = 0.0f;
            RayAabbIntersectRange(rayOrigin, rayDirection, topCellMin, topCellMax, topCellEnter, topCellExit);
            topCellExit = min(topCellExit, endT);

            const FSparseSdfGITraceHierarchyNode topNode = hierarchyTop[LinearizeTraceHierarchyCoordOffset(topCoord, SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION, cascade.Offsets.z)];
            float3 topNodeMin = 0.0f.xxx;
            float3 topNodeMax = 0.0f.xxx;
            float topNodeEnter = 0.0f;
            float topNodeExit = 0.0f;
            if ((topNode.Flags & SPARSE_SDF_GI_TRACE_HIERARCHY_OCCUPIED) == 0u)
            {
                travel = topCellExit + epsilon;
                continue;
            }
            GetTraceHierarchyNodeWorldBoundsCascade(cascade, topNode, topNodeMin, topNodeMax);
            if (!RayAabbIntersectRange(rayOrigin, rayDirection, topNodeMin, topNodeMax, topNodeEnter, topNodeExit) || topNodeExit < travel || topNodeEnter > topCellExit)
            {
                travel = topCellExit + epsilon;
                continue;
            }

            bool advancedInsideTop = false;
            [loop]
            while (traversalIter < cascadeIterLimit && travel <= min(topCellExit, endT))
            {
                ++traversalIter;
                ++stepCount;
                uint3 bottomCoord = 0u.xxx;
                if (!TryGetTraceHierarchyCoordCascade(cascade, rayOrigin + rayDirection * travel, SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_CELL_SIZE, SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION, bottomCoord))
                {
                    travel = endT + epsilon;
                    break;
                }

                float3 bottomCellMin = 0.0f.xxx;
                float3 bottomCellMax = 0.0f.xxx;
                GetTraceHierarchyCellWorldBoundsCascade(cascade, bottomCoord, SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_CELL_SIZE, bottomCellMin, bottomCellMax);
                float bottomCellEnter = 0.0f;
                float bottomCellExit = 0.0f;
                RayAabbIntersectRange(rayOrigin, rayDirection, bottomCellMin, bottomCellMax, bottomCellEnter, bottomCellExit);
                bottomCellExit = min(bottomCellExit, min(topCellExit, endT));

                const FSparseSdfGITraceHierarchyNode bottomNode = hierarchyBottom[LinearizeTraceHierarchyCoordOffset(bottomCoord, SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION, cascade.Offsets.y)];
                float3 bottomNodeMin = 0.0f.xxx;
                float3 bottomNodeMax = 0.0f.xxx;
                float bottomNodeEnter = 0.0f;
                float bottomNodeExit = 0.0f;
                if ((bottomNode.Flags & SPARSE_SDF_GI_TRACE_HIERARCHY_OCCUPIED) == 0u)
                {
                    travel = bottomCellExit + epsilon;
                    advancedInsideTop = true;
                    continue;
                }
                GetTraceHierarchyNodeWorldBoundsCascade(cascade, bottomNode, bottomNodeMin, bottomNodeMax);
                if (!RayAabbIntersectRange(rayOrigin, rayDirection, bottomNodeMin, bottomNodeMax, bottomNodeEnter, bottomNodeExit) || bottomNodeExit < travel || bottomNodeEnter > bottomCellExit)
                {
                    travel = bottomCellExit + epsilon;
                    advancedInsideTop = true;
                    continue;
                }

                [loop]
                while (traversalIter < cascadeIterLimit && travel <= min(bottomCellExit, endT))
                {
                    ++traversalIter;
                    ++stepCount;
                    uint3 brickCoord = 0u.xxx;
                    if (!TryGetBrickCoordFromWorldCascade(cascade, rayOrigin + rayDirection * travel, brickCoord))
                    {
                        travel = endT + epsilon;
                        break;
                    }

                    float3 brickMin = 0.0f.xxx;
                    float3 brickMax = 0.0f.xxx;
                    GetBrickWorldBoundsCascade(cascade, brickCoord, brickMin, brickMax);
                    float brickEnter = 0.0f;
                    float brickExit = 0.0f;
                    RayAabbIntersectRange(rayOrigin, rayDirection, brickMin, brickMax, brickEnter, brickExit);
                    brickExit = min(brickExit, min(bottomCellExit, endT));

                    if (TryTraceHierarchyLeafCascade(sdfAtlas, cascadeBrickMap, brickMetadata, cascade, rayOrigin, rayDirection, brickCoord, brickExit, debugSurface, travel, stepCount, hitBrickCoord, hitLocalCoord, hitFracCoord))
                    {
                        hitTravel = travel;
                        hitCascadeIndex = cascadeIndex;
                        traceStatus = SPARSE_SDF_GI_TRACE_STATUS_HIT;
                        return true;
                    }

                    travel = brickExit + epsilon;
                    advancedInsideTop = true;
                }
            }

            if (!advancedInsideTop && travel <= endT)
            {
                travel = topCellExit + epsilon;
            }
        }

        cascadeLimited = traversalIter >= cascadeIterLimit && travel <= endT;
        if (cascadeLimited)
        {
            rayT = max(max(cascadeEnter, 0.0f), travel - voxelSize);
            if (cascadeIndex + 1u == activeCascadeCount)
            {
                traceStatus = SPARSE_SDF_GI_TRACE_STATUS_ITER_LIMIT;
                hitTravel = rayT;
                return false;
            }
            continue;
        }

        rayT = max(rayT, endT);
        traceStatus = (endT < cascadeExit) ? SPARSE_SDF_GI_TRACE_STATUS_MAX_DISTANCE : SPARSE_SDF_GI_TRACE_STATUS_CASCADE_EXIT;
        if (rayT >= MaxTraceDistance)
        {
            break;
        }
    }

    hitTravel = rayT;
    return false;
}

bool TraceSdfVisibility(
    float3 rayOrigin,
    float3 rayDirection,
    Texture3D<float> sdfAtlas,
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop,
    out uint stepCount,
    out float travel)
{
    stepCount = 0u;
    travel = 0.0f;

#if defined(SPARSE_SDF_GI_USE_HIERARCHICAL_TRACE)
    uint traceStatus = SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS;
    uint3 hitBrickCoord = 0u.xxx;
    uint3 hitLocalCoord = 0u.xxx;
    float3 hitFracCoord = 0.0f.xxx;
    uint hitCascadeIndex = 0u;
    return TraceSdfHierarchicalRawMulti(
        sdfAtlas,
        cascadeBrickMap,
        brickMetadata,
        hierarchyBottom,
        hierarchyTop,
        rayOrigin,
        rayDirection,
        false,
        travel,
        stepCount,
        traceStatus,
        hitBrickCoord,
        hitLocalCoord,
        hitFracCoord,
        hitCascadeIndex);
#else
    [loop]
    for (uint stepIndex = 0u; stepIndex < 128u; ++stepIndex)
    {
        const float3 p = rayOrigin + rayDirection * travel;
        float skippedTravel = travel;
        if (TrySkipCurrentBrick(cascadeBrickMap, brickMetadata, rayOrigin, rayDirection, travel, skippedTravel))
        {
            stepCount = stepIndex + 1u;
            travel = skippedTravel;
            if (travel > MaxTraceDistance)
            {
                break;
            }
            continue;
        }

        const float sdf = SampleSdfAtlas(sdfAtlas, cascadeBrickMap, p);
        const float decodedDistance = DecodeSdfWorldDistance(sdf);
        stepCount = stepIndex + 1u;
        const float hitThreshold = VoxelSize * max(SurfaceHitThresholdVoxels, 0.01f);
        if (decodedDistance <= hitThreshold)
        {
            float refineDistance = decodedDistance;
            [loop]
            for (uint refineIndex = 0u; refineIndex < 8u && refineDistance > hitThreshold * 0.25f; ++refineIndex)
            {
                travel += max(refineDistance, 1e-5f);
                refineDistance = DecodeSdfWorldDistance(SampleSdfAtlas(sdfAtlas, cascadeBrickMap, rayOrigin + rayDirection * travel));
            }
            return true;
        }

        const float stepDistance = max(min(decodedDistance, VoxelSize * 2.0f), 1e-5f);
        travel += stepDistance;
        if (travel > MaxTraceDistance)
        {
            break;
        }
    }

    return false;
#endif
}


#if defined(SPARSE_SDF_GI_REFERENCE_STATS_SHADER)
[numthreads(1, 1, 1)]
void CSStoreReferenceStatsToGpuDebug(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (ReferenceStatsSrvIndex == 0xffffffffu || DebugPrintStatsUavIndex == 0xffffffffu)
    {
        return;
    }

    StructuredBuffer<uint> referenceStats = ResourceDescriptorHeap[ReferenceStatsSrvIndex];
    RWByteAddressBuffer debugPrintStats = ResourceDescriptorHeap[DebugPrintStatsUavIndex];
    debugPrintStats.Store(4u * kDebugPrintStatsSparseSdfGITriangleIndex, referenceStats[SPARSE_SDF_GI_REF_COUNTER_TRIANGLE]);
    debugPrintStats.Store(4u * kDebugPrintStatsSparseSdfGIReferenceIndex, referenceStats[SPARSE_SDF_GI_REF_COUNTER_REFERENCE]);
    debugPrintStats.Store(4u * kDebugPrintStatsSparseSdfGIOccupiedBrickIndex, referenceStats[SPARSE_SDF_GI_REF_COUNTER_OCCUPIED_BRICK]);
    debugPrintStats.Store(4u * kDebugPrintStatsSparseSdfGITriangleOverflowIndex, referenceStats[SPARSE_SDF_GI_REF_COUNTER_TRIANGLE_OVERFLOW]);
    debugPrintStats.Store(4u * kDebugPrintStatsSparseSdfGIReferenceOverflowIndex, referenceStats[SPARSE_SDF_GI_REF_COUNTER_REFERENCE_OVERFLOW]);
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_INIT_SHADER)
[numthreads(8, 8, 8)]
void CSInitDistributedScatterBuild(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasUavIndex];
    RWStructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapUavIndex];
    RWStructuredBuffer<uint4> brickMetadata = ResourceDescriptorHeap[BrickMetadataUavIndex];
    RWStructuredBuffer<uint> touchedBricks = ResourceDescriptorHeap[ScatterTouchedBricksUavIndex];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersUavIndex];
    RWStructuredBuffer<uint> referenceStats = ResourceDescriptorHeap[ReferenceStatsUavIndex];

    if (all(dispatchThreadId < AtlasResolution.xxx))
    {
        if (CascadeIndex == 0u)
        {
            sdfAtlas[dispatchThreadId] = 1.0f;
        }
    }

    if (all(dispatchThreadId < BrickGridResolution.xxx))
    {
        const uint brickIndex = LinearizeBrickCoord(dispatchThreadId);
        cascadeBrickMap[GetCurrentBrickMapOffset() + brickIndex] = SPARSE_SDF_GI_INVALID_BRICK_ID;
        if (CascadeIndex == 0u)
        {
            brickMetadata[brickIndex] = uint4(0u, 0u, 0u, 0u);
        }
        touchedBricks[brickIndex] = 0u;
    }

    if (all(dispatchThreadId == 0u.xxx))
    {
        [unroll]
        for (uint i = 0u; i < 8u; ++i)
        {
            counters[i] = 0u;
        }

        if (CascadeIndex == 0u)
        {
            referenceStats[SPARSE_SDF_GI_REF_COUNTER_TRIANGLE] = 0u;
            referenceStats[SPARSE_SDF_GI_REF_COUNTER_REFERENCE] = 0u;
            referenceStats[SPARSE_SDF_GI_REF_COUNTER_TRIANGLE_OVERFLOW] = 0u;
            referenceStats[SPARSE_SDF_GI_REF_COUNTER_REFERENCE_OVERFLOW] = 0u;
            referenceStats[SPARSE_SDF_GI_REF_COUNTER_OCCUPIED_BRICK] = 0u;
        }
    }
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_PREPARE_SHADER)
[numthreads(64, 1, 1)]
void CSPrepareScatterJobs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= ModelTriangleCount)
    {
        return;
    }

    StructuredBuffer<float3> positions = ResourceDescriptorHeap[PositionBufferIndex];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[IndexBufferIndex];
    RWStructuredBuffer<FSparseSdfGIScatterJob> jobs = ResourceDescriptorHeap[ScatterJobsUavIndex];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersUavIndex];

    const uint triBase = ModelDrawIndexStart + dispatchThreadId.x * 3u;
    if (triBase + 2u >= ModelDrawIndexStart + ModelDrawIndexCount)
    {
        return;
    }

    const uint i0 = indices[triBase + 0u];
    const uint i1 = indices[triBase + 1u];
    const uint i2 = indices[triBase + 2u];
    if (max(i0, max(i1, i2)) >= BuildWorkOffset)
    {
        InterlockedAdd(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB_OVERFLOW], 1u);
        return;
    }

    const float3 p0 = mul(float4(positions[i0], 1.0f), ModelWorld).xyz;
    const float3 p1 = mul(float4(positions[i1], 1.0f), ModelWorld).xyz;
    const float3 p2 = mul(float4(positions[i2], 1.0f), ModelWorld).xyz;
    const float band = SPARSE_SDF_GI_SCATTER_BAND_VOXELS * VoxelSize;
    const float3 triMinWorld = min(p0, min(p1, p2)) - band.xxx;
    const float3 triMaxWorld = max(p0, max(p1, p2)) + band.xxx;
    const float3 cascadeMax = CascadeMin + CascadeExtent;
    if (any(triMaxWorld < CascadeMin) || any(triMinWorld > cascadeMax))
    {
        return;
    }

    const uint logicalMax = BrickGridResolution * SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT;
    const int3 minLogical = clamp((int3)floor((triMinWorld - CascadeMin) / VoxelSize), 0, (int)logicalMax);
    const int3 maxLogical = clamp((int3)ceil((triMaxWorld - CascadeMin) / VoxelSize), 0, (int)logicalMax);
    if (any(maxLogical < minLogical))
    {
        return;
    }

    const uint3 pairMin = uint3(
        GetScatterPairForFirstLogicalSample((uint)minLogical.x),
        GetScatterPairForFirstLogicalSample((uint)minLogical.y),
        GetScatterPairForFirstLogicalSample((uint)minLogical.z));
    const uint3 pairMax = uint3(
        GetScatterPairForLastLogicalSample((uint)maxLogical.x),
        GetScatterPairForLastLogicalSample((uint)maxLogical.y),
        GetScatterPairForLastLogicalSample((uint)maxLogical.z));
    const uint3 pairCount = pairMax - pairMin + 1u.xxx;
    const uint sampleCount = pairCount.x * pairCount.y * pairCount.z;
    if (sampleCount == 0u)
    {
        return;
    }

    uint jobIndex = 0u;
    InterlockedAdd(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB], 1u, jobIndex);
    if (jobIndex >= TrianglePoolCapacity)
    {
        InterlockedAdd(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB_OVERFLOW], 1u);
        return;
    }

    FSparseSdfGIScatterJob job;
    job.P0 = float4(p0, 0.0f);
    job.P1 = float4(p1, 0.0f);
    job.P2 = float4(p2, 0.0f);
    job.PairMinSampleCount = uint4(pairMin, sampleCount);
    job.PairCount = uint4(pairCount, 0u);
    jobs[jobIndex] = job;
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_SCAN_JOBS_SHADER)
[numthreads(256, 1, 1)]
void CSScanScatterJobCounts(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    StructuredBuffer<FSparseSdfGIScatterJob> jobs = ResourceDescriptorHeap[ScatterJobsSrvIndex];
    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[ScatterJobOffsetsUavIndex];
    RWStructuredBuffer<uint> groupSums = ResourceDescriptorHeap[ScatterGroupSumsUavIndex];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersSrvIndex];
    const uint jobCount = min(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB], TrianglePoolCapacity);
    const uint jobIndex = groupId.x * SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE + groupThreadIndex;
    gs_ScatterScan[groupThreadIndex] = (jobIndex < jobCount) ? jobs[jobIndex].PairMinSampleCount.w : 0u;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 1u; stride < SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE; stride <<= 1u)
    {
        const uint addend = (groupThreadIndex >= stride) ? gs_ScatterScan[groupThreadIndex - stride] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_ScatterScan[groupThreadIndex] += addend;
        GroupMemoryBarrierWithGroupSync();
    }

    if (jobIndex < TrianglePoolCapacity)
    {
        offsets[jobIndex] = gs_ScatterScan[groupThreadIndex];
    }
    if (groupThreadIndex == SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE - 1u)
    {
        groupSums[groupId.x] = gs_ScatterScan[groupThreadIndex];
    }
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_SCAN_GROUPS_SHADER)
[numthreads(256, 1, 1)]
void CSScanScatterGroupSums(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    StructuredBuffer<uint> inputSums = ResourceDescriptorHeap[ScatterInputGroupSumsSrvIndex];
    RWStructuredBuffer<uint> groupOffsets = ResourceDescriptorHeap[ScatterGroupOffsetsUavIndex];
    RWStructuredBuffer<uint> outputSums = ResourceDescriptorHeap[ScatterOutputGroupSumsUavIndex];
    const uint groupIndex = groupId.x * SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE + groupThreadIndex;
    gs_ScatterScan[groupThreadIndex] = (groupIndex < ModelTriangleCount) ? inputSums[groupIndex] : 0u;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 1u; stride < SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE; stride <<= 1u)
    {
        const uint addend = (groupThreadIndex >= stride) ? gs_ScatterScan[groupThreadIndex - stride] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_ScatterScan[groupThreadIndex] += addend;
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex < ModelTriangleCount)
    {
        groupOffsets[groupIndex] = gs_ScatterScan[groupThreadIndex];
    }
    if (groupThreadIndex == SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE - 1u)
    {
        outputSums[groupId.x] = gs_ScatterScan[groupThreadIndex];
    }
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_SCAN_GROUP2_SHADER)
[numthreads(256, 1, 1)]
void CSScanScatterGroup2Sums(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    StructuredBuffer<uint> group2Sums = ResourceDescriptorHeap[ScatterGroup2SumsSrvIndex];
    RWStructuredBuffer<uint> group2Offsets = ResourceDescriptorHeap[ScatterGroup2OffsetsUavIndex];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersUavIndex];
    gs_ScatterScan[groupThreadIndex] = (groupThreadIndex < ModelTriangleCount) ? group2Sums[groupThreadIndex] : 0u;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = 1u; stride < SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE; stride <<= 1u)
    {
        const uint addend = (groupThreadIndex >= stride) ? gs_ScatterScan[groupThreadIndex - stride] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_ScatterScan[groupThreadIndex] += addend;
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupThreadIndex < ModelTriangleCount)
    {
        group2Offsets[groupThreadIndex] = gs_ScatterScan[groupThreadIndex];
    }
    if (groupThreadIndex == 0u)
    {
        counters[SPARSE_SDF_GI_SCATTER_COUNTER_SAMPLE] = (ModelTriangleCount == 0u) ? 0u : gs_ScatterScan[ModelTriangleCount - 1u];
    }
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_ADD_OFFSETS_SHADER)
[numthreads(256, 1, 1)]
void CSAddScatterJobGroupOffsets(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint jobIndex = dispatchThreadId.x;
    if (jobIndex >= TrianglePoolCapacity)
    {
        return;
    }

    RWStructuredBuffer<uint> offsets = ResourceDescriptorHeap[ScatterJobOffsetsUavIndex];
    StructuredBuffer<uint> groupOffsets = ResourceDescriptorHeap[ScatterGroupOffsetsSrvIndex];
    StructuredBuffer<uint> group2Offsets = ResourceDescriptorHeap[ScatterGroup2OffsetsSrvIndex];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersSrvIndex];
    const uint jobCount = min(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB], TrianglePoolCapacity);
    if (jobIndex >= jobCount)
    {
        offsets[jobIndex] = (jobCount == 0u) ? 0u : offsets[jobCount - 1u];
        return;
    }

    const uint groupIndex = jobIndex / SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE;
    const uint group2Index = groupIndex / SPARSE_SDF_GI_SCATTER_SCAN_GROUP_SIZE;
    const uint localGroupOffset = (groupIndex == 0u) ? 0u : groupOffsets[groupIndex - 1u];
    const uint group2Offset = (group2Index == 0u) ? 0u : group2Offsets[group2Index - 1u];
    offsets[jobIndex] += localGroupOffset + group2Offset;
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_BUILD_SAMPLE_ARGS_SHADER)
[numthreads(1, 1, 1)]
void CSBuildScatterSampleArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersSrvIndex];
    RWByteAddressBuffer args = ResourceDescriptorHeap[ScatterDispatchArgsUavIndex];
    StoreScatterDispatchArgs(args, counters[SPARSE_SDF_GI_SCATTER_COUNTER_SAMPLE], SPARSE_SDF_GI_SCATTER_SAMPLE_GROUP_SIZE);
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_MARK_TOUCHED_SHADER)
[numthreads(128, 1, 1)]
void CSMarkScatterTouchedBricks(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    const uint sampleIndex = GetScatterLinearDispatchId(groupId, groupThreadIndex, SPARSE_SDF_GI_SCATTER_SAMPLE_GROUP_SIZE);
    StructuredBuffer<FSparseSdfGIScatterJob> jobs = ResourceDescriptorHeap[ScatterJobsSrvIndex];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[ScatterJobOffsetsSrvIndex];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersSrvIndex];
    RWStructuredBuffer<uint> touchedBricks = ResourceDescriptorHeap[ScatterTouchedBricksUavIndex];

    const uint jobCount = min(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB], TrianglePoolCapacity);
    const uint sampleCount = counters[SPARSE_SDF_GI_SCATTER_COUNTER_SAMPLE];

    FSparseSdfGIScatterJob job;
    uint3 brickCoord;
    uint3 localCoord;
    float3 worldPosition;
    if (!ResolveScatterSample(jobs, offsets, jobCount, sampleCount, sampleIndex, job, brickCoord, localCoord, worldPosition))
    {
        return;
    }

    const float distanceToTriangle = PointTriangleDistance(worldPosition, job.P0.xyz, job.P1.xyz, job.P2.xyz);
    if (distanceToTriangle <= SPARSE_SDF_GI_SCATTER_BAND_VOXELS * VoxelSize)
    {
        touchedBricks[LinearizeBrickCoord(brickCoord)] = 1u;
    }
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_ALLOCATE_BRICKS_SHADER)
[numthreads(256, 1, 1)]
void CSAllocateScatterBricks(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint logicalBrick = dispatchThreadId.x;
    if (logicalBrick >= GetSparseSdfGIBrickCapacity())
    {
        return;
    }

    StructuredBuffer<uint> touchedBricks = ResourceDescriptorHeap[ScatterTouchedBricksSrvIndex];
    RWStructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapUavIndex];
    RWStructuredBuffer<uint> brickList = ResourceDescriptorHeap[ScatterBrickListUavIndex];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersUavIndex];
    if (touchedBricks[logicalBrick] == 0u)
    {
        return;
    }

    uint physicalBrick = 0u;
    InterlockedAdd(counters[SPARSE_SDF_GI_SCATTER_COUNTER_ALLOCATED_BRICK], 1u, physicalBrick);
    if (physicalBrick >= GetScatterBrickCapacity())
    {
        InterlockedAdd(counters[SPARSE_SDF_GI_SCATTER_COUNTER_BRICK_OVERFLOW], 1u);
        return;
    }

    const uint physicalBrickId = GetPhysicalBrickId(physicalBrick);
    cascadeBrickMap[GetCurrentBrickMapOffset() + logicalBrick] = physicalBrickId;
    brickList[physicalBrick] = logicalBrick;
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_BUILD_BRICK_ARGS_SHADER)
[numthreads(1, 1, 1)]
void CSBuildScatterBrickArgs(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersSrvIndex];
    RWByteAddressBuffer args = ResourceDescriptorHeap[ScatterDispatchArgsUavIndex];
    const uint allocated = min(counters[SPARSE_SDF_GI_SCATTER_COUNTER_ALLOCATED_BRICK], GetScatterBrickCapacity());
    StoreScatterDispatchArgs(args, max(allocated, 1u), 1u);
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_CLEAR_BRICK_STORAGE_SHADER)
[numthreads(512, 1, 1)]
void CSClearScatterBrickStorage(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    RWStructuredBuffer<uint> brickSdf = ResourceDescriptorHeap[ScatterBrickSdfUavIndex];
    const uint physicalBrick = groupId.x + groupId.y * SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT;
    if (physicalBrick >= GetScatterBrickCapacity() || groupThreadIndex >= SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE)
    {
        return;
    }

    brickSdf[physicalBrick * SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE + groupThreadIndex] = SPARSE_SDF_GI_SCATTER_DISTANCE_FAR_UINT;
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_SDF_SAMPLES_SHADER)
[numthreads(128, 1, 1)]
void CSScatterSdfSamples(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    const uint sampleIndex = GetScatterLinearDispatchId(groupId, groupThreadIndex, SPARSE_SDF_GI_SCATTER_SAMPLE_GROUP_SIZE);
    StructuredBuffer<FSparseSdfGIScatterJob> jobs = ResourceDescriptorHeap[ScatterJobsSrvIndex];
    StructuredBuffer<uint> offsets = ResourceDescriptorHeap[ScatterJobOffsetsSrvIndex];
    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersSrvIndex];
    RWStructuredBuffer<uint> brickSdf = ResourceDescriptorHeap[ScatterBrickSdfUavIndex];

    const uint jobCount = min(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB], TrianglePoolCapacity);
    const uint sampleCount = counters[SPARSE_SDF_GI_SCATTER_COUNTER_SAMPLE];
    FSparseSdfGIScatterJob job;
    uint3 brickCoord;
    uint3 localCoord;
    float3 worldPosition;
    if (!ResolveScatterSample(jobs, offsets, jobCount, sampleCount, sampleIndex, job, brickCoord, localCoord, worldPosition))
    {
        return;
    }

    const float distanceToTriangle = PointTriangleDistance(worldPosition, job.P0.xyz, job.P1.xyz, job.P2.xyz);
    if (distanceToTriangle > SPARSE_SDF_GI_SCATTER_BAND_VOXELS * VoxelSize)
    {
        return;
    }

    const uint physicalBrickId = cascadeBrickMap[GetCurrentBrickMapOffset() + LinearizeBrickCoord(brickCoord)];
    if (physicalBrickId < PhysicalBrickBase || physicalBrickId >= PhysicalBrickBase + GetScatterBrickCapacity())
    {
        return;
    }
    const uint physicalBrick = physicalBrickId - PhysicalBrickBase;

    const uint encoded = EncodeScatterDistanceUint(distanceToTriangle);
    InterlockedMin(brickSdf[physicalBrick * SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE + FlattenBrickLocalCoord(localCoord)], encoded);
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_EIKONAL_SHADER)
[numthreads(512, 1, 1)]
void CSEikonalScatterBricks(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    RWStructuredBuffer<uint> brickSdf = ResourceDescriptorHeap[ScatterBrickSdfUavIndex];
    const uint physicalBrick = groupId.x + groupId.y * SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT;
    if (physicalBrick >= GetScatterBrickCapacity() || groupThreadIndex >= SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE)
    {
        return;
    }

    const uint scratchIndex = physicalBrick * SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE + groupThreadIndex;
    const uint3 localCoord = UnflattenBrickLocalCoord(groupThreadIndex);
    const float initial = DecodeScatterDistanceUint(brickSdf[scratchIndex]);
    gs_EikonalA[groupThreadIndex] = initial;
    gs_EikonalB[groupThreadIndex] = initial;
    GroupMemoryBarrierWithGroupSync();

    bool readFromA = true;
    [unroll]
    for (uint sweep = 0u; sweep < 3u; ++sweep)
    {
        [unroll]
        for (uint offsetIndex = 0u; offsetIndex < 3u; ++offsetIndex)
        {
            const uint offset = 1u << (2u - offsetIndex);
            const float relaxed = RelaxEikonalVoxel(readFromA, localCoord, offset);
            EikonalStore(!readFromA, groupThreadIndex, relaxed);
            readFromA = !readFromA;
            GroupMemoryBarrierWithGroupSync();
        }
    }

    const float finalSdf = EikonalLoad(readFromA, (int3)localCoord);
    brickSdf[scratchIndex] = EncodeScatterDistanceUint(finalSdf * GetSdfWorldDistanceScale());
}
#endif

#if defined(SPARSE_SDF_GI_SCATTER_FINALIZE_BRICKS_SHADER)
[numthreads(512, 1, 1)]
void CSFinalizeScatterBricks(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    RWTexture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasUavIndex];
    RWStructuredBuffer<uint4> brickMetadata = ResourceDescriptorHeap[BrickMetadataUavIndex];
    StructuredBuffer<uint> brickList = ResourceDescriptorHeap[ScatterBrickListSrvIndex];
    StructuredBuffer<uint> brickSdf = ResourceDescriptorHeap[ScatterBrickSdfSrvIndex];
    StructuredBuffer<uint> counters = ResourceDescriptorHeap[ScatterCountersSrvIndex];
    RWStructuredBuffer<uint> referenceStats = ResourceDescriptorHeap[ReferenceStatsUavIndex];

    const uint physicalBrick = groupId.x + groupId.y * SPARSE_SDF_GI_SCATTER_DISPATCH_GROUP_LIMIT;
    const uint physicalBrickId = GetPhysicalBrickId(physicalBrick);
    const uint allocatedBrickCount = min(counters[SPARSE_SDF_GI_SCATTER_COUNTER_ALLOCATED_BRICK], GetScatterBrickCapacity());
    if (physicalBrick == 0u && groupThreadIndex == 0u)
    {
        InterlockedAdd(referenceStats[SPARSE_SDF_GI_REF_COUNTER_TRIANGLE], min(counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB], TrianglePoolCapacity));
        InterlockedAdd(referenceStats[SPARSE_SDF_GI_REF_COUNTER_REFERENCE], counters[SPARSE_SDF_GI_SCATTER_COUNTER_SAMPLE]);
        InterlockedAdd(referenceStats[SPARSE_SDF_GI_REF_COUNTER_TRIANGLE_OVERFLOW], counters[SPARSE_SDF_GI_SCATTER_COUNTER_JOB_OVERFLOW]);
        InterlockedAdd(referenceStats[SPARSE_SDF_GI_REF_COUNTER_REFERENCE_OVERFLOW], counters[SPARSE_SDF_GI_SCATTER_COUNTER_BRICK_OVERFLOW]);
        InterlockedAdd(referenceStats[SPARSE_SDF_GI_REF_COUNTER_OCCUPIED_BRICK], allocatedBrickCount);
    }

    if (physicalBrick >= allocatedBrickCount || groupThreadIndex >= SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE)
    {
        return;
    }

    const uint logicalBrick = brickList[physicalBrick];
    const uint3 logicalBrickCoord = BrickIdToBrickCoord(logicalBrick);
    const uint3 physicalBrickCoord = BrickIdToBrickCoord(physicalBrickId);
    const uint3 localCoord = UnflattenBrickLocalCoord(groupThreadIndex);
    const uint encoded = brickSdf[physicalBrick * SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE + groupThreadIndex];
    const float finalSdf = DecodeScatterDistanceUint(encoded);
    sdfAtlas[physicalBrickCoord * BrickVoxelResolution + localCoord] = finalSdf;

    const bool occupied = DecodeSdfWorldDistance(finalSdf) <= VoxelSize * SPARSE_SDF_GI_SURFACE_METADATA_VOXELS;
    gs_MetadataMinX[groupThreadIndex] = occupied ? localCoord.x : 0xffffffffu;
    gs_MetadataMinY[groupThreadIndex] = occupied ? localCoord.y : 0xffffffffu;
    gs_MetadataMinZ[groupThreadIndex] = occupied ? localCoord.z : 0xffffffffu;
    gs_MetadataMaxX[groupThreadIndex] = occupied ? localCoord.x : 0u;
    gs_MetadataMaxY[groupThreadIndex] = occupied ? localCoord.y : 0u;
    gs_MetadataMaxZ[groupThreadIndex] = occupied ? localCoord.z : 0u;
    gs_MetadataOccupied[groupThreadIndex] = occupied ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint stride = SPARSE_SDF_GI_SCATTER_BRICK_GROUP_SIZE >> 1u; stride > 0u; stride >>= 1u)
    {
        if (groupThreadIndex < stride)
        {
            const uint other = groupThreadIndex + stride;
            gs_MetadataMinX[groupThreadIndex] = min(gs_MetadataMinX[groupThreadIndex], gs_MetadataMinX[other]);
            gs_MetadataMinY[groupThreadIndex] = min(gs_MetadataMinY[groupThreadIndex], gs_MetadataMinY[other]);
            gs_MetadataMinZ[groupThreadIndex] = min(gs_MetadataMinZ[groupThreadIndex], gs_MetadataMinZ[other]);
            gs_MetadataMaxX[groupThreadIndex] = max(gs_MetadataMaxX[groupThreadIndex], gs_MetadataMaxX[other]);
            gs_MetadataMaxY[groupThreadIndex] = max(gs_MetadataMaxY[groupThreadIndex], gs_MetadataMaxY[other]);
            gs_MetadataMaxZ[groupThreadIndex] = max(gs_MetadataMaxZ[groupThreadIndex], gs_MetadataMaxZ[other]);
            gs_MetadataOccupied[groupThreadIndex] += gs_MetadataOccupied[other];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupThreadIndex == 0u)
    {
        if (gs_MetadataOccupied[0] == 0u)
        {
            brickMetadata[physicalBrickId] = uint4(0u, 0u, 0u, 0u);
            return;
        }

        const uint3 localMin = uint3(gs_MetadataMinX[0], gs_MetadataMinY[0], gs_MetadataMinZ[0]);
        const uint3 localMax = uint3(gs_MetadataMaxX[0], gs_MetadataMaxY[0], gs_MetadataMaxZ[0]);
        brickMetadata[physicalBrickId] = uint4(PackBrickLocalAabb(localMin, localMax), SPARSE_SDF_GI_BRICK_METADATA_OCCUPIED, 0u, 0u);
    }
}
#endif



#if defined(SPARSE_SDF_GI_BUILD_TRACE_HIERARCHY_BOTTOM_SHADER)
[numthreads(64, 1, 1)]
void CSBuildTraceHierarchyBottom(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint nodeIndex = dispatchThreadId.x;
    const uint nodeCount = SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION;
    if (nodeIndex >= nodeCount)
    {
        return;
    }

    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    StructuredBuffer<uint4> brickMetadata = ResourceDescriptorHeap[BrickMetadataSrvIndex];
    RWStructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom = ResourceDescriptorHeap[TraceHierarchyBottomUavIndex];

    const uint3 nodeCoord = uint3(
        nodeIndex % SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION,
        (nodeIndex / SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION) % SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION,
        nodeIndex / (SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION));
    const uint3 baseBrickCoord = nodeCoord * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_CELL_SIZE;

    uint3 minCoord = 0xffffffffu.xxx;
    uint3 maxCoord = 0u.xxx;
    bool occupied = false;
    [loop]
    for (uint z = 0u; z < SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_CELL_SIZE; ++z)
    {
        [loop]
        for (uint y = 0u; y < SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_CELL_SIZE; ++y)
        {
            [loop]
            for (uint x = 0u; x < SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_CELL_SIZE; ++x)
            {
                const uint3 brickCoord = baseBrickCoord + uint3(x, y, z);
                if (any(brickCoord >= BrickGridResolution.xxx))
                {
                    continue;
                }

                const uint brickIndex = LinearizeBrickCoord(brickCoord);
                const uint brickId = cascadeBrickMap[GetCurrentBrickMapOffset() + brickIndex];
                const uint4 metadata = (brickId < GetSparseSdfGIBrickCapacity()) ? brickMetadata[brickId] : uint4(0u, 0u, 0u, 0u);
                if (brickId < GetSparseSdfGIBrickCapacity() && (metadata.y & SPARSE_SDF_GI_BRICK_METADATA_OCCUPIED) != 0u)
                {
                    occupied = true;
                    minCoord = min(minCoord, brickCoord);
                    maxCoord = max(maxCoord, brickCoord);
                }
            }
        }
    }

    FSparseSdfGITraceHierarchyNode node;
    node.MinPacked = occupied ? PackTraceHierarchyCoord(minCoord) : 0u;
    node.MaxPacked = occupied ? PackTraceHierarchyCoord(maxCoord) : 0u;
    node.Flags = occupied ? SPARSE_SDF_GI_TRACE_HIERARCHY_OCCUPIED : 0u;
    node.Reserved = 0u;
    hierarchyBottom[GetTraceHierarchyBottomOffsetForCascade(CascadeIndex) + nodeIndex] = node;
}
#endif

#if defined(SPARSE_SDF_GI_BUILD_TRACE_HIERARCHY_TOP_SHADER)
[numthreads(64, 1, 1)]
void CSBuildTraceHierarchyTop(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint nodeIndex = dispatchThreadId.x;
    const uint nodeCount = SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION * SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION * SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION;
    if (nodeIndex >= nodeCount)
    {
        return;
    }

    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom = ResourceDescriptorHeap[TraceHierarchyBottomSrvIndex];
    RWStructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop = ResourceDescriptorHeap[TraceHierarchyTopUavIndex];

    const uint3 nodeCoord = uint3(
        nodeIndex % SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION,
        (nodeIndex / SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION) % SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION,
        nodeIndex / (SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION * SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION));
    const uint3 baseBottomCoord = nodeCoord * SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION;

    uint3 minCoord = 0xffffffffu.xxx;
    uint3 maxCoord = 0u.xxx;
    bool occupied = false;
    [loop]
    for (uint z = 0u; z < SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION; ++z)
    {
        [loop]
        for (uint y = 0u; y < SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION; ++y)
        {
            [loop]
            for (uint x = 0u; x < SPARSE_SDF_GI_TRACE_HIERARCHY_TOP_RESOLUTION; ++x)
            {
                const uint3 bottomCoord = baseBottomCoord + uint3(x, y, z);
                const uint bottomIndex = bottomCoord.x
                    + bottomCoord.y * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION
                    + bottomCoord.z * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION * SPARSE_SDF_GI_TRACE_HIERARCHY_BOTTOM_RESOLUTION;
                const FSparseSdfGITraceHierarchyNode child = hierarchyBottom[GetTraceHierarchyBottomOffsetForCascade(CascadeIndex) + bottomIndex];
                if ((child.Flags & SPARSE_SDF_GI_TRACE_HIERARCHY_OCCUPIED) != 0u)
                {
                    occupied = true;
                    minCoord = min(minCoord, UnpackTraceHierarchyCoord(child.MinPacked));
                    maxCoord = max(maxCoord, UnpackTraceHierarchyCoord(child.MaxPacked));
                }
            }
        }
    }

    FSparseSdfGITraceHierarchyNode node;
    node.MinPacked = occupied ? PackTraceHierarchyCoord(minCoord) : 0u;
    node.MaxPacked = occupied ? PackTraceHierarchyCoord(maxCoord) : 0u;
    node.Flags = occupied ? SPARSE_SDF_GI_TRACE_HIERARCHY_OCCUPIED : 0u;
    node.Reserved = 0u;
    hierarchyTop[GetTraceHierarchyTopOffsetForCascade(CascadeIndex) + nodeIndex] = node;
}
#endif

#if defined(SPARSE_SDF_GI_RADIANCE_CLEAR_SHADER)
[numthreads(64, 1, 1)]
void CSClearBrickRadianceAccum(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint brickIndex = dispatchThreadId.x;
    if (brickIndex >= GetSparseSdfGIBrickCapacity())
    {
        return;
    }

    RWStructuredBuffer<FBrickShAccum> brickRadianceAccum = ResourceDescriptorHeap[BrickRadianceAccumUavIndex];
    FBrickShAccum accum;
    accum.ShY = int4(0, 0, 0, 0);
    accum.Co = 0;
    accum.Cg = 0;
    accum.SampleCount = 0u;
    accum.Reserved = 0u;
    brickRadianceAccum[brickIndex] = accum;
}
#endif

#if defined(SPARSE_SDF_GI_RADIANCE_INJECT_SHADER)
float ComputeShadowMapVisibility(Texture2D<float> shadowMap, float3 worldPosition)
{
    const float4 shadowPosition = mul(float4(worldPosition, 1.0f), LightViewProjection);
    const float3 shadowCoord = shadowPosition.xyz / max(shadowPosition.w, 1e-6f);
    const float2 shadowUV = shadowCoord.xy * float2(0.5f, -0.5f) + 0.5f;
    if (ShadowStrength <= 0.0f || any(shadowUV < 0.0f.xx) || any(shadowUV > 1.0f.xx))
    {
        return 1.0f;
    }

    // Intentionally one software-bilinear tap, not the lighting pass 4-tap hardware PCF.
    const float shadowCompare = shadowCoord.z - ShadowBias;
    const float2 texelCoord = shadowUV * ShadowMapSize - 0.5f.xx;
    const int2 maxTexel = int2(ShadowMapSize) - int2(1, 1);
    const int2 baseTexel = clamp(int2(floor(texelCoord)), int2(0, 0), maxTexel);
    const float2 fracTexel = saturate(texelCoord - (float2)baseTexel);
    const int2 texelX = min(baseTexel + int2(1, 0), maxTexel);
    const int2 texelY = min(baseTexel + int2(0, 1), maxTexel);
    const int2 texelXY = min(baseTexel + int2(1, 1), maxTexel);
    const float lit00 = (shadowCompare <= shadowMap.Load(int3(baseTexel, 0))) ? 1.0f : 0.0f;
    const float lit10 = (shadowCompare <= shadowMap.Load(int3(texelX, 0))) ? 1.0f : 0.0f;
    const float lit01 = (shadowCompare <= shadowMap.Load(int3(texelY, 0))) ? 1.0f : 0.0f;
    const float lit11 = (shadowCompare <= shadowMap.Load(int3(texelXY, 0))) ? 1.0f : 0.0f;
    const float lit = lerp(lerp(lit00, lit10, fracTexel.x), lerp(lit01, lit11, fracTexel.x), fracTexel.y);
    return lerp(1.0f, lit, ShadowStrength);
}

[numthreads(8, 8, 1)]
void CSInjectBrickRadiance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputWidth || dispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    Texture2D depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D gbufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D gbufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D gbufferC = ResourceDescriptorHeap[GBufferCIndex];
    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    RWStructuredBuffer<FBrickShAccum> brickRadianceAccum = ResourceDescriptorHeap[BrickRadianceAccumUavIndex];

    const uint2 pixel = dispatchThreadId.xy;
    const float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (depth <= 0.0f)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
    const float3 viewPosition = ReconstructViewPositionFromDepth(uv, depth, Projection);
    const float3 worldPosition = mul(float4(viewPosition, 1.0f), ViewInverse).xyz;

    uint brickIndex = 0u;
    if (!TryGetPhysicalBrickIdFromWorld(cascadeBrickMap, worldPosition, brickIndex))
    {
        return;
    }

    const float3 normal = normalize(gbufferA.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    const float4 smr = gbufferB.Load(int3(pixel, 0));
    const float3 albedo = gbufferC.Load(int3(pixel, 0)).rgb;
    const float metallic = saturate(smr.y);
    const float3 lightDirection = normalize(LightDirection);
    const float directNdotL = saturate(dot(normal, lightDirection));
    float shadowVisibility = 1.0f;
    if (ShadowMode == SPARSE_SDF_GI_SHADOW_MODE_SCREEN_MASK)
    {
        Texture2D shadowMaskTexture = ResourceDescriptorHeap[ShadowTextureIndex];
        shadowVisibility = lerp(1.0f, shadowMaskTexture.Load(int3(pixel, 0)).r, ShadowStrength);
    }
    else if (ShadowMode == SPARSE_SDF_GI_SHADOW_MODE_SHADOW_MAP)
    {
        Texture2D<float> shadowMap = ResourceDescriptorHeap[ShadowTextureIndex];
        shadowVisibility = ComputeShadowMapVisibility(shadowMap, worldPosition);
    }
    float3 indirectIrradiance = 0.0f.xxx;
    if (BounceStrength > 0.0f && BrickIrradianceReadIndex != 0xFFFFFFFFu)
    {
        StructuredBuffer<FBrickShSample> brickIrradiance = ResourceDescriptorHeap[BrickIrradianceReadIndex];
        const FBrickShSample irradianceSample = brickIrradiance[brickIndex];
        if (IsBrickShSampleValid(irradianceSample))
        {
            indirectIrradiance = EvaluateBrickShSample(irradianceSample, normal, true) * BounceStrength;
        }
    }

    const float3 directIrradiance = LightColor * LightIntensity * directNdotL * saturate(shadowVisibility);
    const float3 sourceRadiance = saturate(albedo) * (1.0f - metallic) * (directIrradiance + indirectIrradiance);
    const float3 clampedRadiance = min(sourceRadiance, SPARSE_SDF_GI_RADIANCE_MAX_SAMPLE.xxx);
    AccumulateBrickSH(brickRadianceAccum, brickIndex, ProjectSh(clampedRadiance, normal));
}
#endif

#if defined(SPARSE_SDF_GI_IRRADIANCE_ACCUM_SHADER)
[numthreads(8, 8, 1)]
void CSAccumulateBrickIrradiance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputWidth || dispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    Texture2D depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> diffuseGI = ResourceDescriptorHeap[DiffuseGIIndex];
    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    RWStructuredBuffer<FBrickShAccum> brickIrradianceAccum = ResourceDescriptorHeap[BrickIrradianceAccumUavIndex];

    const uint2 pixel = dispatchThreadId.xy;
    const float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (depth <= 0.0f)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
    const float3 viewPosition = ReconstructViewPositionFromDepth(uv, depth, Projection);
    const float3 worldPosition = mul(float4(viewPosition, 1.0f), ViewInverse).xyz;

    uint brickIndex = 0u;
    if (!TryGetPhysicalBrickIdFromWorld(cascadeBrickMap, worldPosition, brickIndex))
    {
        return;
    }

    const float3 irradiance = max(diffuseGI.Load(int3(pixel, 0)).rgb, 0.0f.xxx);
    const float3 clampedIrradiance = min(irradiance, SPARSE_SDF_GI_RADIANCE_MAX_SAMPLE.xxx);
    AccumulateBrickSH(brickIrradianceAccum, brickIndex, ProjectIrradianceSh(clampedIrradiance));
}
#endif

#if defined(SPARSE_SDF_GI_RADIANCE_RESOLVE_SHADER)
[numthreads(64, 1, 1)]
void CSResolveBrickRadianceTemporal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint brickIndex = dispatchThreadId.x;
    if (brickIndex >= GetSparseSdfGIBrickCapacity())
    {
        return;
    }

    StructuredBuffer<FBrickShAccum> brickRadianceAccum = ResourceDescriptorHeap[BrickRadianceAccumSrvIndex];
    StructuredBuffer<FBrickShSample> brickRadianceHistory = ResourceDescriptorHeap[BrickRadianceHistorySrvIndex];
    RWStructuredBuffer<FBrickShSample> brickRadiance = ResourceDescriptorHeap[BrickRadianceUavIndex];
    const FBrickShAccum accum = brickRadianceAccum[brickIndex];
    if (accum.SampleCount == 0u)
    {
        const FBrickShSample history = brickRadianceHistory[brickIndex];
        if (RadianceHistoryValid != 0u && IsBrickShSampleValid(history))
        {
            const FPackedSh decayed = ScaleSh(DecodeBrickShSample(history), SPARSE_SDF_GI_RADIANCE_HISTORY_DECAY);
            brickRadiance[brickIndex] = EncodeBrickShSample(decayed, history.SampleCount * SPARSE_SDF_GI_RADIANCE_HISTORY_DECAY);
        }
        else
        {
            brickRadiance[brickIndex] = MakeInvalidBrickShSample();
        }
        return;
    }

    const float invWeight = 1.0f / (SPARSE_SDF_GI_RADIANCE_ACCUM_SCALE * (float)accum.SampleCount);
    FPackedSh sh;
    sh.ShY = float4(
        DequantizeBrickShCoeff(accum.ShY.x, invWeight),
        DequantizeBrickShCoeff(accum.ShY.y, invWeight),
        DequantizeBrickShCoeff(accum.ShY.z, invWeight),
        DequantizeBrickShCoeff(accum.ShY.w, invWeight));
    sh.Co = DequantizeBrickShCoeff(accum.Co, invWeight);
    sh.Cg = DequantizeBrickShCoeff(accum.Cg, invWeight);
    brickRadiance[brickIndex] = EncodeBrickShSample(sh, (float)accum.SampleCount);
}
#endif

#if defined(SPARSE_SDF_GI_BRICK_SH_PROPAGATE_SHADER)
[numthreads(64, 1, 1)]
void CSPropagateBrickSH(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint logicalBrickCount = GetSparseSdfGIBrickCapacity();
    const uint totalLogicalBrickCount = logicalBrickCount * max(CascadeCount, 1u);
    const uint logicalIndex = dispatchThreadId.x;
    if (logicalIndex >= totalLogicalBrickCount)
    {
        return;
    }

    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    StructuredBuffer<FBrickShSample> source = ResourceDescriptorHeap[BrickShSourceSrvIndex];
    RWStructuredBuffer<FBrickShSample> dest = ResourceDescriptorHeap[BrickShDestUavIndex];

    const uint basePhysicalBrick = cascadeBrickMap[logicalIndex];
    if (basePhysicalBrick == SPARSE_SDF_GI_INVALID_BRICK_ID || basePhysicalBrick >= GetSparseSdfGIBrickCapacity())
    {
        return;
    }

    const uint cascadeIndex = logicalIndex / logicalBrickCount;
    const uint cascadeLogicalOffset = cascadeIndex * logicalBrickCount;
    const uint localLogicalIndex = logicalIndex - cascadeLogicalOffset;
    const uint3 brickCoord = BrickIdToBrickCoord(localLogicalIndex);

    const FBrickShSample baseSample = source[basePhysicalBrick];
    if (!IsBrickShSampleValid(baseSample))
    {
        dest[basePhysicalBrick] = baseSample;
        return;
    }

    const FPackedSh baseSh = DecodeBrickShSample(baseSample);
    const float baseWeight = baseSample.SampleCount * baseSample.SampleCount;
    FPackedSh accumSh = ScaleSh(baseSh, baseWeight);
    float weightSum = baseWeight;
    float sampleCountAccum = baseSample.SampleCount * baseWeight;
    const float baseDirectionality = GetBrickShDirectionality(baseSh);
    const float3 baseDirection = GetBrickShDominantDirection(baseSh);

    [unroll]
    for (int z = -1; z <= 1; ++z)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            [unroll]
            for (int x = -1; x <= 1; ++x)
            {
                if (x == 0 && y == 0 && z == 0)
                {
                    continue;
                }

                const int3 neighborCoordInt = (int3)brickCoord + int3(x, y, z);
                if (any(neighborCoordInt < 0) || any(neighborCoordInt >= (int)BrickGridResolution))
                {
                    continue;
                }

                const uint3 neighborCoord = (uint3)neighborCoordInt;
                const uint neighborLogicalIndex = cascadeLogicalOffset
                    + neighborCoord.x
                    + neighborCoord.y * BrickGridResolution
                    + neighborCoord.z * BrickGridResolution * BrickGridResolution;
                const uint neighborPhysicalBrick = cascadeBrickMap[neighborLogicalIndex];
                if (neighborPhysicalBrick == SPARSE_SDF_GI_INVALID_BRICK_ID || neighborPhysicalBrick >= GetSparseSdfGIBrickCapacity())
                {
                    continue;
                }

                const FBrickShSample neighborSample = source[neighborPhysicalBrick];
                if (!IsBrickShSampleValid(neighborSample))
                {
                    continue;
                }

                const FPackedSh neighborSh = DecodeBrickShSample(neighborSample);
                const float neighborDirectionality = GetBrickShDirectionality(neighborSh);
                if (baseDirectionality >= SPARSE_SDF_GI_BRICK_SH_DIRECTIONALITY_THRESHOLD
                    && neighborDirectionality >= SPARSE_SDF_GI_BRICK_SH_DIRECTIONALITY_THRESHOLD)
                {
                    const float3 neighborDirection = GetBrickShDominantDirection(neighborSh);
                    if (dot(baseDirection, neighborDirection) < 0.0f)
                    {
                        continue;
                    }
                }

                const float distanceSq = (float)(x * x + y * y + z * z);
                const float weight = 1.0f / max(distanceSq, 1.0f);
                accumSh = AddSh(accumSh, ScaleSh(neighborSh, weight));
                sampleCountAccum += neighborSample.SampleCount * weight * SPARSE_SDF_GI_BRICK_SH_PROPAGATION_DECAY;
                weightSum += weight;
            }
        }
    }

    if (weightSum <= 0.0f)
    {
        dest[basePhysicalBrick] = baseSample;
        return;
    }

    dest[basePhysicalBrick] = EncodeBrickShSample(ScaleSh(accumSh, rcp(weightSum)), sampleCountAccum / weightSum);
}
#endif

float3 GetWorldRayDirection(uint2 pixel)
{
    const float2 uv = (float2(pixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 farH = mul(float4(ndc, 0.0f, 1.0f), ViewProjectionInverse);
    const float3 farWorld = farH.xyz / max(farH.w, 1e-5f);
    return normalize(farWorld - CameraPosition);
}

bool RayBoxIntersect(float3 rayOrigin, float3 rayDirection, out float tEnter, out float tExit)
{
    const float3 safeDirection = GetSafeRayDirection(rayDirection);

    const float3 boxMin = CascadeMin;
    const float3 boxMax = CascadeMin + CascadeExtent;
    const float3 t0 = (boxMin - rayOrigin) / safeDirection;
    const float3 t1 = (boxMax - rayOrigin) / safeDirection;
    const float3 tNear = min(t0, t1);
    const float3 tFar = max(t0, t1);

    tEnter = max(max(tNear.x, tNear.y), tNear.z);
    tExit = min(min(tFar.x, tFar.y), tFar.z);
    return tExit >= max(tEnter, 0.0f);
}

bool TraceSdfDebugSurface(
    Texture3D<float> sdfAtlas,
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop,
    float3 rayOrigin,
    float3 rayDirection,
    out float hitTravel,
    out uint stepCount,
    out uint traceStatus,
    out uint3 hitBrickCoord,
    out uint3 hitLocalCoord,
    out float3 hitFracCoord,
    out uint hitCascadeIndex)
{
    hitTravel = 0.0f;
    stepCount = 0u;
    traceStatus = SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS;
    hitBrickCoord = 0u.xxx;
    hitLocalCoord = 0u.xxx;
    hitFracCoord = 0.0f.xxx;
    hitCascadeIndex = 0u;

#if defined(SPARSE_SDF_GI_USE_HIERARCHICAL_TRACE)
    return TraceSdfHierarchicalRawMulti(
        sdfAtlas,
        cascadeBrickMap,
        brickMetadata,
        hierarchyBottom,
        hierarchyTop,
        rayOrigin,
        rayDirection,
        true,
        hitTravel,
        stepCount,
        traceStatus,
        hitBrickCoord,
        hitLocalCoord,
        hitFracCoord,
        hitCascadeIndex);
#else
    float tEnter = 0.0f;
    float tExit = 0.0f;
    if (!RayBoxIntersect(rayOrigin, rayDirection, tEnter, tExit))
    {
        return false;
    }

    const float startT = max(tEnter, 0.0f);
    const float endT = min(tExit, MaxTraceDistance);
    traceStatus = (endT < tExit) ? SPARSE_SDF_GI_TRACE_STATUS_MAX_DISTANCE : SPARSE_SDF_GI_TRACE_STATUS_CASCADE_EXIT;
    const float hitThreshold = VoxelSize * max(SurfaceHitThresholdVoxels, 0.01f);
    const float minStepDistance = max(VoxelSize * 0.25f, 1e-4f);
    const float maxStepDistance = max(VoxelSize * 2.0f, minStepDistance);

    float travel = startT;
    float previousTravel = startT;

    [loop]
    for (uint stepIndex = 0u; stepIndex < SPARSE_SDF_GI_SURFACE_TRACE_MAX_STEPS; ++stepIndex)
    {
        if (travel > endT)
        {
            break;
        }

        const float3 samplePosition = rayOrigin + rayDirection * travel;
        if (IsClearlyOutsideCascade(samplePosition))
        {
            traceStatus = SPARSE_SDF_GI_TRACE_STATUS_ATLAS_OUTSIDE;
            break;
        }

        float skippedTravel = travel;
        if (TrySkipCurrentBrick(cascadeBrickMap, brickMetadata, rayOrigin, rayDirection, travel, skippedTravel))
        {
            stepCount = stepIndex + 1u;
            previousTravel = travel;
            travel = skippedTravel;
            continue;
        }

        const float sdf = SampleSdfAtlasDebugSurface(sdfAtlas, cascadeBrickMap, samplePosition);
        const float decodedDistance = DecodeSdfWorldDistance(sdf);
        stepCount = stepIndex + 1u;
        if (decodedDistance <= hitThreshold)
        {
            float refineTravel = travel;
            float refineDistance = decodedDistance;
            float bestTravel = travel;
            float bestDistance = decodedDistance;

            [loop]
            for (uint refineIndex = 0u; refineIndex < 8u && refineDistance > hitThreshold * 0.25f; ++refineIndex)
            {
                refineTravel += max(refineDistance, 1e-5f);
                refineDistance = DecodeSdfWorldDistance(SampleSdfAtlasDebugSurface(sdfAtlas, cascadeBrickMap, rayOrigin + rayDirection * refineTravel));
                if (refineDistance < bestDistance)
                {
                    bestDistance = refineDistance;
                    bestTravel = refineTravel;
                }
            }

            hitTravel = bestTravel;
            bool hitCellValid = false;
            GetExactSharedBorderCell(rayOrigin + rayDirection * bestTravel, hitBrickCoord, hitLocalCoord, hitFracCoord, hitCellValid);
            if (!hitCellValid)
            {
                hitBrickCoord = 0u.xxx;
                hitLocalCoord = 0u.xxx;
                hitFracCoord = 0.0f.xxx;
            }
            traceStatus = SPARSE_SDF_GI_TRACE_STATUS_HIT;
            return true;
        }

        previousTravel = travel;
        travel += clamp(decodedDistance, minStepDistance, maxStepDistance);
    }

    return false;
#endif
}

float3 ComputeSdfNormal(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    const float e = max(VoxelSize, 1e-4f);
    const float dx = SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition + float3(e, 0.0f, 0.0f))
        - SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition - float3(e, 0.0f, 0.0f));
    const float dy = SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition + float3(0.0f, e, 0.0f))
        - SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition - float3(0.0f, e, 0.0f));
    const float dz = SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition + float3(0.0f, 0.0f, e))
        - SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition - float3(0.0f, 0.0f, e));

    const float3 gradient = float3(dx, dy, dz);
    const float gradientLengthSq = dot(gradient, gradient);
    return (gradientLengthSq > 1e-8f) ? gradient * rsqrt(gradientLengthSq) : 0.0f.xxx;
}

float3 ComputeSdfNormalCascade(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, FSparseSdfGICascadeData cascade, float3 worldPosition)
{
    const float e = max(GetCascadeVoxelSize(cascade), 1e-4f);
    const float dx = SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition + float3(e, 0.0f, 0.0f), false)
        - SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition - float3(e, 0.0f, 0.0f), false);
    const float dy = SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition + float3(0.0f, e, 0.0f), false)
        - SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition - float3(0.0f, e, 0.0f), false);
    const float dz = SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition + float3(0.0f, 0.0f, e), false)
        - SampleSdfAtlasCascade(sdfAtlas, cascadeBrickMap, cascade, worldPosition - float3(0.0f, 0.0f, e), false);

    const float3 gradient = float3(dx, dy, dz);
    const float gradientLengthSq = dot(gradient, gradient);
    return (gradientLengthSq > 1e-8f) ? gradient * rsqrt(gradientLengthSq) : 0.0f.xxx;
}

bool TrySampleBrickRadiance(
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<FBrickShSample> brickRadiance,
    float3 worldPosition,
    float3 hitNormal,
    bool normalReliable,
    out float3 radiance)
{
    radiance = 0.0f.xxx;
    uint brickIndex = 0u;
    if (!TryGetPhysicalBrickIdFromWorld(cascadeBrickMap, worldPosition, brickIndex))
    {
        return false;
    }

    const FBrickShSample sample = brickRadiance[brickIndex];
    if (!IsBrickShSampleValid(sample))
    {
        return false;
    }

    radiance = EvaluateBrickShSample(sample, hitNormal, normalReliable);
    return true;
}

#if defined(SPARSE_SDF_GI_TRACE_SHADER) || defined(SPARSE_SDF_GI_PROBE_TRACE_SHADER)
float3 EvaluateSparseSdfGIRayRadiance(
    Texture3D<float> sdfAtlas,
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop,
    StructuredBuffer<FBrickShSample> brickRadiance,
    float3 rayOrigin,
    float3 traceDirection,
    out bool hit)
{
    uint stepCount = 0u;
    float travel = 0.0f;
    hit = TraceSdfVisibility(rayOrigin, traceDirection, sdfAtlas, cascadeBrickMap, brickMetadata, hierarchyBottom, hierarchyTop, stepCount, travel);

    float3 radiance = EvaluateSparseSdfGISky(traceDirection);
    if (!hit)
    {
        return radiance;
    }

    const float3 hitPosition = rayOrigin + traceDirection * travel;
    float3 hitNormal = ComputeSdfNormal(sdfAtlas, cascadeBrickMap, hitPosition);
    const bool bHitNormalReliable = dot(hitNormal, hitNormal) > 1e-8f;
    if (!bHitNormalReliable)
    {
        hitNormal = -traceDirection;
    }
    if (dot(hitNormal, -traceDirection) < 0.0f)
    {
        hitNormal = -hitNormal;
    }

    const float3 lightDirection = normalize(LightDirection);
    uint lightStepCount = 0u;
    float lightTravel = 0.0f;
    const float3 lightRayOrigin = hitPosition + hitNormal * (VoxelSize * 2.0f) + lightDirection * (VoxelSize * 2.0f);
    const float lightVisibility = TraceSdfVisibility(lightRayOrigin, lightDirection, sdfAtlas, cascadeBrickMap, brickMetadata, hierarchyBottom, hierarchyTop, lightStepCount, lightTravel) ? 0.0f : 1.0f;
    const float3 directBounce = LightColor * LightIntensity * saturate(dot(hitNormal, lightDirection)) * lightVisibility;
    float3 cachedRadiance = 0.0f.xxx;
    return TrySampleBrickRadiance(cascadeBrickMap, brickRadiance, hitPosition, hitNormal, bHitNormalReliable, cachedRadiance) ? cachedRadiance : directBounce;
}
#endif

float3 DebugSharedSampleMismatch(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, Texture2D depthTexture, uint2 pixel)
{
    if (!IsExactSharedBorderSdf())
    {
        return 0.0f.xxx;
    }

    const float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (depth <= 0.0f)
    {
        return 0.0f.xxx;
    }

    const float2 uv = (float2(pixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
    const float3 viewPosition = ReconstructViewPositionFromDepth(uv, depth, Projection);
    const float3 hitPosition = mul(float4(viewPosition, 1.0f), ViewInverse).xyz;
    uint3 brickCoord = uint3(0u, 0u, 0u);
    uint3 localCoord = uint3(0u, 0u, 0u);
    float3 fracCoord = 0.0f.xxx;
    bool valid = false;
    GetExactSharedBorderCell(hitPosition, brickCoord, localCoord, fracCoord, valid);
    if (!valid)
    {
        return 0.0f.xxx;
    }

    const uint brickMapOffset = GetCurrentBrickMapOffset();
    float mismatch = 0.0f;
    if (brickCoord.x + 1u < BrickGridResolution)
    {
        const uint y = min(localCoord.y, BrickVoxelResolution - 1u);
        const uint z = min(localCoord.z, BrickVoxelResolution - 1u);
        const float a = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, uint3(BrickVoxelResolution - 1u, y, z));
        const float b = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord + uint3(1u, 0u, 0u), uint3(0u, y, z));
        mismatch = max(mismatch, abs(a - b));
    }
    if (brickCoord.y + 1u < BrickGridResolution)
    {
        const uint x = min(localCoord.x, BrickVoxelResolution - 1u);
        const uint z = min(localCoord.z, BrickVoxelResolution - 1u);
        const float a = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, uint3(x, BrickVoxelResolution - 1u, z));
        const float b = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord + uint3(0u, 1u, 0u), uint3(x, 0u, z));
        mismatch = max(mismatch, abs(a - b));
    }
    if (brickCoord.z + 1u < BrickGridResolution)
    {
        const uint x = min(localCoord.x, BrickVoxelResolution - 1u);
        const uint y = min(localCoord.y, BrickVoxelResolution - 1u);
        const float a = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord, uint3(x, y, BrickVoxelResolution - 1u));
        const float b = LoadSdfBrickLocal(sdfAtlas, cascadeBrickMap, brickMapOffset, brickCoord + uint3(0u, 0u, 1u), uint3(x, y, 0u));
        mismatch = max(mismatch, abs(a - b));
    }

    const float t = saturate(mismatch * 8.0f);
    return lerp(float3(0.0f, 0.04f, 0.08f), float3(1.0f, 0.0f, 0.0f), t);
}

float3 DebugTraceModeColor(
    Texture3D<float> sdfAtlas,
    StructuredBuffer<uint> cascadeBrickMap,
    StructuredBuffer<uint4> brickMetadata,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom,
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop,
    uint2 pixel)
{
    const float3 rayDirection = GetWorldRayDirection(pixel);
    float hitTravel = 0.0f;
    uint stepCount = 0u;
    uint traceStatus = SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS;
    uint3 hitBrickCoord = 0u.xxx;
    uint3 hitLocalCoord = 0u.xxx;
    float3 hitFracCoord = 0.0f.xxx;
    uint hitCascadeIndex = 0u;
    const bool hit = TraceSdfDebugSurface(sdfAtlas, cascadeBrickMap, brickMetadata, hierarchyBottom, hierarchyTop, CameraPosition, rayDirection, hitTravel, stepCount, traceStatus, hitBrickCoord, hitLocalCoord, hitFracCoord, hitCascadeIndex);

    if (DebugMode == SPARSE_SDF_GI_DEBUG_MODE_STEP_COUNT)
    {
        const float stepT = saturate((float)stepCount / SPARSE_SDF_GI_STEP_COUNT_DEBUG_SCALE);
        const float cost = sqrt(stepT);
        if (hit)
        {
            return lerp(0.18f.xxx, 1.0f.xxx, cost); // gray 
        }
        if (traceStatus == SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS)
        {
            return 0.0f.xxx;
        }
        if (traceStatus == SPARSE_SDF_GI_TRACE_STATUS_MAX_DISTANCE) // blue
        {
            return lerp(float3(0.01f, 0.02f, 0.07f), float3(0.05f, 0.12f, 0.34f), cost);
        }
        if (traceStatus == SPARSE_SDF_GI_TRACE_STATUS_ATLAS_OUTSIDE) // green 
        {
            return lerp(float3(0.00f, 0.08f, 0.10f), float3(0.10f, 0.45f, 0.50f), cost);
        }
        if (traceStatus == SPARSE_SDF_GI_TRACE_STATUS_ITER_LIMIT) // brown 
        {
            return lerp(float3(0.12f, 0.04f, 0.0f), float3(1.0f, 0.45f, 0.0f), cost);
        }
        // magenta
        return lerp(float3(0.03f, 0.01f, 0.08f), float3(0.18f, 0.08f, 0.30f), cost); 
    }

    if (!hit)
    {
        return 0.0f.xxx;
    }

    const bool exact = IsExactSharedBorderSdf();

    if (DebugMode == SPARSE_SDF_GI_DEBUG_MODE_HIT_UVW)
    {
        return exact ? saturate(((float3)hitLocalCoord + hitFracCoord) / (float)SPARSE_SDF_GI_BRICK_INTERVAL_DIM_EXACT) : 0.0f.xxx;
    }

    if (DebugMode == SPARSE_SDF_GI_DEBUG_MODE_BRICK_ID)
    {
        if (!exact)
        {
            return 0.0f.xxx;
        }
        const FSparseSdfGICascadeData cascade = LoadSparseSdfGICascadeData(hitCascadeIndex);
        const uint brickMapIndex = cascade.Offsets.x + LinearizeBrickCoord(hitBrickCoord);
        const uint brickCount = GetSparseSdfGIBrickCapacity();
        const uint brickMapCapacity = BrickGridResolution * BrickGridResolution * BrickGridResolution * max(CascadeCount, 1u);
        if (brickMapIndex >= brickMapCapacity)
        {
            return 0.0f.xxx;
        }
        const uint brickId = cascadeBrickMap[brickMapIndex];
        if (brickId == SPARSE_SDF_GI_INVALID_BRICK_ID || brickId >= brickCount)
        {
            return 0.0f.xxx;
        }
        return HashToColor(brickId);
    }

    const FSparseSdfGICascadeData hitCascade = LoadSparseSdfGICascadeData(hitCascadeIndex);
    float3 normal;
    if (exact && DebugMode == SPARSE_SDF_GI_DEBUG_MODE_BRICK_LOCAL_GRADIENT)
    {
        normal = ComputeExactBrickLocalSdfNormal(sdfAtlas, cascadeBrickMap, hitCascade.Offsets.x, hitBrickCoord, hitLocalCoord, hitFracCoord);
    }
    else if (exact && DebugMode == SPARSE_SDF_GI_DEBUG_MODE_BRICK_LOCAL_GRADIENT_ROUNDED)
    {
        normal = ComputeExactBrickLocalSdfNormalRounded(sdfAtlas, cascadeBrickMap, hitCascade.Offsets.x, hitBrickCoord, hitLocalCoord, hitFracCoord);
    }
    else
    {
        normal = ComputeSdfNormalCascade(sdfAtlas, cascadeBrickMap, hitCascade, CameraPosition + rayDirection * hitTravel);
    }

    if (dot(normal, normal) <= 1e-8f)
    {
        normal = -rayDirection;
    }
    if (dot(normal, -rayDirection) < 0.0f)
    {
        normal = -normal;
    }
    return normal * 0.5f + 0.5f;
}

#if defined(SPARSE_SDF_GI_TRACE_SHADER)
[numthreads(8, 8, 1)]
void CSDebugTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputWidth || dispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    Texture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasSrvIndex];
    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    StructuredBuffer<uint4> brickMetadata = ResourceDescriptorHeap[BrickMetadataSrvIndex];
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom = ResourceDescriptorHeap[TraceHierarchyBottomSrvIndex];
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop = ResourceDescriptorHeap[TraceHierarchyTopSrvIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[DiffuseGIUavIndex];
    const uint2 pixel = dispatchThreadId.xy;
    if (DebugMode == SPARSE_SDF_GI_DEBUG_MODE_SHARED_SAMPLE_MISMATCH)
    {
        Texture2D depthTexture = ResourceDescriptorHeap[DepthIndex];
        outputTexture[pixel] = float4(DebugSharedSampleMismatch(sdfAtlas, cascadeBrickMap, depthTexture, pixel), 1.0f);
        return;
    }
    if (DebugMode >= SPARSE_SDF_GI_DEBUG_MODE_GRADIENT && DebugMode <= SPARSE_SDF_GI_DEBUG_MODE_MAX)
    {
        outputTexture[pixel] = float4(DebugTraceModeColor(sdfAtlas, cascadeBrickMap, brickMetadata, hierarchyBottom, hierarchyTop, pixel), 1.0f);
        return;
    }

    const float3 rayDirection = GetWorldRayDirection(pixel);
    uint stepCount = 0u;
    float travel = 0.0f;
    const bool hit = TraceSdfVisibility(CameraPosition, rayDirection, sdfAtlas, cascadeBrickMap, brickMetadata, hierarchyBottom, hierarchyTop, stepCount, travel);
    const float stepT = saturate((float)stepCount / 128.0f);
    const float travelT = saturate(travel / max(MaxTraceDistance, VoxelSize));

    float3 color = hit
        ? lerp(float3(1.0f, 0.28f, 0.05f), float3(1.0f, 0.9f, 0.2f), stepT)
        : lerp(float3(0.03f, 0.06f, 0.12f), float3(0.05f, 0.35f, 0.95f), travelT);
    if (DebugMode == SPARSE_SDF_GI_DEBUG_MODE_CASCADE_SLICE)
    {
        const float3 slicePosition = CascadeMin + float3(
            ((float)pixel.x / max(1.0f, (float)OutputWidth)) * CascadeExtent.x,
            ((float)pixel.y / max(1.0f, (float)OutputHeight)) * CascadeExtent.y,
            CascadeExtent.z * 0.5f);
        color = SampleSdfAtlas(sdfAtlas, cascadeBrickMap, slicePosition).xxx;
    }

    outputTexture[pixel] = float4(color, 1.0f);
}

[numthreads(8, 8, 1)]
void CSDiffuseTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputWidth || dispatchThreadId.y >= OutputHeight)
    {
        return;
    }


    Texture2D depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D gbufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasSrvIndex];
    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    StructuredBuffer<uint4> brickMetadata = ResourceDescriptorHeap[BrickMetadataSrvIndex];
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom = ResourceDescriptorHeap[TraceHierarchyBottomSrvIndex];
    StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop = ResourceDescriptorHeap[TraceHierarchyTopSrvIndex];
    StructuredBuffer<FBrickShSample> brickRadiance = ResourceDescriptorHeap[BrickRadianceSrvIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[DiffuseGIUavIndex];
    RWTexture2D<uint4> inputSHOut = ResourceDescriptorHeap[InputSHUavIndex];
    RWTexture2D<float> varianceOut = ResourceDescriptorHeap[VarianceUavIndex];

    const uint2 pixel = dispatchThreadId.xy;
    const float2 uv = (float2(pixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
    const float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (depth <= 0.0f)
    {
        outputTexture[pixel] = 0.0f.xxxx;
        inputSHOut[pixel] = uint4(0u, 0u, 0u, 0u);
        varianceOut[pixel] = 0.0f;
        return;
    }

    const float3 normal = normalize(gbufferA.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    const float3 viewPosition = ReconstructViewPositionFromDepth(uv, depth, Projection);
    const float3 worldPosition = mul(float4(viewPosition, 1.0f), ViewInverse).xyz;

    const float3 rayOrigin = worldPosition + normal * (VoxelSize * 2.0f);
    FBlueNoiseSobolSampler diffuseSampler = BlueNoiseSobolSamplerCreate(pixel, uint2(max(OutputWidth, 1u), max(OutputHeight, 1u)), FrameIndex);
    const float2 Xi = BlueNoiseSobolSamplerRandomFloat2(diffuseSampler, GetTraceBlueNoiseSobolTextureIndex(), GetTraceBlueNoiseScramblingRankingTextureIndex());
    const float3 traceDirection = SampleHemisphereCosine(Xi, normal);
    bool hit = false;
    const float3 irradiance = EvaluateSparseSdfGIRayRadiance(sdfAtlas, cascadeBrickMap, brickMetadata, hierarchyBottom, hierarchyTop, brickRadiance, rayOrigin, traceDirection, hit);

    const float3 outputIrradiance = irradiance * Intensity * BounceStrength;
    outputTexture[pixel] = float4(outputIrradiance, 1.0f);
    // SparseSdfGI emits an integrated, directionless irradiance, so pack it as a DC-only SH;
    // this round-trips through the denoiser's UnprojectIrradiance without brightening the result.
    const FPackedSh outputSH = ProjectIrradianceSh(outputIrradiance);
    inputSHOut[pixel] = PackSh(outputSH);
    // 1-spp input is uniformly noisy: keep a strong but adaptive denoise floor (was pinned to 1.0).
    varianceOut[pixel] = max(ShVariance(outputSH), 0.5f);
}
#endif

#if defined(SPARSE_SDF_GI_PROBE_SPAWN_SHADER)
uint GetScreenProbeIndex(uint2 probeCoord)
{
    return probeCoord.x + probeCoord.y * ProbeCountX;
}

[numthreads(8, 8, 1)]
void CSSpawnScreenProbes(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 probeCoord = dispatchThreadId.xy;
    if (probeCoord.x >= ProbeCountX || probeCoord.y >= ProbeCountY)
    {
        return;
    }

    Texture2D<float> depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> gbufferA = ResourceDescriptorHeap[GBufferAIndex];
    RWStructuredBuffer<FSparseSdfGIProbeHeader> probeHeaders = ResourceDescriptorHeap[ProbeHeaderUavIndex];

    const uint tileSize = max(ProbeTileSize, 1u);
    const uint2 tileStart = probeCoord * tileSize;
    const uint2 tileEnd = min(tileStart + tileSize, uint2(OutputWidth, OutputHeight));
    const uint2 tileExtent = max(tileEnd - tileStart, 1u.xx);
    uint2 candidate = min(tileStart + tileExtent / 2u, uint2(OutputWidth - 1u, OutputHeight - 1u));
    if (JitterEnabled != 0u)
    {
        FBlueNoiseSobolSampler jitterSampler = BlueNoiseSobolSamplerCreate(probeCoord, uint2(max(ProbeCountX, 1u), max(ProbeCountY, 1u)), FrameIndex);
        const float2 jitter = BlueNoiseSobolSamplerRandomFloat2(jitterSampler, BlueNoiseSobolTextureIndex, BlueNoiseScramblingRankingTextureIndex) - 0.5f.xx;
        const int2 jitterPixels = int2(round(jitter * (float)tileSize * 0.5f));
        candidate = uint2(clamp(int2(candidate) + jitterPixels, int2(tileStart), int2(max(tileEnd, uint2(1u, 1u)) - 1u)));
    }

    uint2 bestPixel = candidate;
    float bestDepth = depthTexture.Load(int3(bestPixel, 0)).r;
    bool valid = bestDepth > 0.0f;
    [loop]
    for (uint y = 0u; y < 16u && !valid; ++y)
    {
        if (y >= tileExtent.y)
        {
            break;
        }
        [loop]
        for (uint x = 0u; x < 16u; ++x)
        {
            if (x >= tileExtent.x)
            {
                break;
            }
            const uint2 pixel = tileStart + uint2(x, y);
            const float depth = depthTexture.Load(int3(pixel, 0)).r;
            if (depth > 0.0f)
            {
                bestPixel = pixel;
                bestDepth = depth;
                valid = true;
                break;
            }
        }
    }

    FSparseSdfGIProbeHeader header;
    header.WorldPositionDepth = 0.0f.xxxx;
    header.NormalValid = 0.0f.xxxx;
    header.Pixel = bestPixel;
    header.PrevProbeIndex = GetScreenProbeIndex(probeCoord);
    header.PrevProbeValid = 0u;
    if (valid)
    {
        const float2 uv = (float2(bestPixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
        const float3 viewPosition = ReconstructViewPositionFromDepth(uv, bestDepth, Projection);
        const float3 worldPosition = mul(float4(viewPosition, 1.0f), ViewInverse).xyz;
        const float3 normal = normalize(gbufferA.Load(int3(bestPixel, 0)).xyz * 2.0f - 1.0f);
        header.WorldPositionDepth = float4(worldPosition, bestDepth);
        header.NormalValid = float4(normal, 1.0f);

        uint prevProbeIndex = GetScreenProbeIndex(probeCoord);
        uint prevProbeValid = 1u;
        if (VelocityIndex != 0xFFFFFFFFu)
        {
            Texture2D<float4> velocityTexture = ResourceDescriptorHeap[VelocityIndex];
            const float3 velocityNdc = velocityTexture.Load(int3(bestPixel, 0)).xyz;
            const float2 prevUv = float2(uv.x - velocityNdc.x * 0.5f, uv.y + velocityNdc.y * 0.5f);
            if (all(prevUv >= 0.0f.xx) && all(prevUv <= 1.0f.xx))
            {
                const uint2 prevPixel = (uint2)(prevUv * float2(OutputWidth, OutputHeight));
                const uint2 prevTile = min(prevPixel / max(ProbeTileSize, 1u), uint2(max(ProbeCountX, 1u) - 1u, max(ProbeCountY, 1u) - 1u));
                prevProbeIndex = prevTile.x + prevTile.y * ProbeCountX;
            }
            else
            {
                prevProbeValid = 0u;
            }
        }
        header.PrevProbeIndex = prevProbeIndex;
        header.PrevProbeValid = prevProbeValid;
    }

    probeHeaders[GetScreenProbeIndex(probeCoord)] = header;
}
#endif

#if defined(SPARSE_SDF_GI_PROBE_TRACE_SHADER)
groupshared float3 gs_ProbeRadiance[SPARSE_SDF_GI_PROBE_MAX_RAYS];
groupshared float gs_ProbeLuminance[SPARSE_SDF_GI_PROBE_MAX_RAYS];
groupshared float gs_ProbeHit[SPARSE_SDF_GI_PROBE_MAX_RAYS];
#if defined(SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH)
groupshared float4 gs_ProbeShY[SPARSE_SDF_GI_PROBE_MAX_RAYS];
groupshared float gs_ProbeCo[SPARSE_SDF_GI_PROBE_MAX_RAYS];
groupshared float gs_ProbeCg[SPARSE_SDF_GI_PROBE_MAX_RAYS];
#endif

[numthreads(64, 1, 1)]
void CSTraceScreenProbes(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    const uint probeIndex = groupId.x + groupId.y * ProbeCountX;
    const uint lane = groupThreadId.x;
    const uint probeCount = ProbeCountX * ProbeCountY;
    if (probeIndex >= probeCount)
    {
        return;
    }

    StructuredBuffer<FSparseSdfGIProbeHeader> probeHeaders = ResourceDescriptorHeap[ProbeHeaderSrvIndex];
    RWStructuredBuffer<uint4> probeSH = ResourceDescriptorHeap[ProbeSHUavIndex];
    RWStructuredBuffer<float4> probeVariance = ResourceDescriptorHeap[ProbeVarianceUavIndex];
    const FSparseSdfGIProbeHeader header = probeHeaders[probeIndex];
    const uint rayCount = clamp(ProbeRaysPerProbe, 1u, SPARSE_SDF_GI_PROBE_MAX_RAYS);

    if (lane < SPARSE_SDF_GI_PROBE_MAX_RAYS)
    {
        gs_ProbeRadiance[lane] = 0.0f.xxx;
        gs_ProbeLuminance[lane] = 0.0f;
        gs_ProbeHit[lane] = 0.0f;
#if defined(SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH)
        gs_ProbeShY[lane] = 0.0f.xxxx;
        gs_ProbeCo[lane] = 0.0f;
        gs_ProbeCg[lane] = 0.0f;
#endif
    }
    GroupMemoryBarrierWithGroupSync();

    if (header.NormalValid.w <= 0.5f)
    {
        if (lane == 0u)
        {
            probeSH[probeIndex] = uint4(0u, 0u, 0u, 0u);
            probeVariance[probeIndex] = float4(1.0f, 0.0f, 0.0f, 0.0f);
            RWStructuredBuffer<FScreenProbeHistory> probeHistoryWrite = ResourceDescriptorHeap[ProbeHistoryWriteUavIndex];
            FScreenProbeHistory invalidHistory;
            invalidHistory.WorldPositionCount = 0.0f.xxxx;
            invalidHistory.NormalDepth = 0.0f.xxxx;
            invalidHistory.PackedSH = uint4(0u, 0u, 0u, 0u);
            probeHistoryWrite[probeIndex] = invalidHistory;
        }
        return;
    }

    if (lane < rayCount)
    {
        Texture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasSrvIndex];
        StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
        StructuredBuffer<uint4> brickMetadata = ResourceDescriptorHeap[BrickMetadataSrvIndex];
        StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyBottom = ResourceDescriptorHeap[TraceHierarchyBottomSrvIndex];
        StructuredBuffer<FSparseSdfGITraceHierarchyNode> hierarchyTop = ResourceDescriptorHeap[TraceHierarchyTopSrvIndex];
        StructuredBuffer<FBrickShSample> brickRadiance = ResourceDescriptorHeap[BrickRadianceSrvIndex];
        const float3 normal = normalize(header.NormalValid.xyz);
        const float3 rayOrigin = header.WorldPositionDepth.xyz + normal * (VoxelSize * 2.0f);
        FBlueNoiseSobolSampler raySampler = BlueNoiseSobolSamplerCreate(header.Pixel, uint2(max(OutputWidth, 1u), max(OutputHeight, 1u)), FrameIndex);
        const float2 Xi = float2(
            BlueNoiseSobolSamplerSample(raySampler, lane, 0u, GetTraceBlueNoiseSobolTextureIndex(), GetTraceBlueNoiseScramblingRankingTextureIndex()),
            BlueNoiseSobolSamplerSample(raySampler, lane, 1u, GetTraceBlueNoiseSobolTextureIndex(), GetTraceBlueNoiseScramblingRankingTextureIndex()));
#if defined(SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH)
        const float3 traceDirection = SampleHemisphereUniform(Xi, normal);
#else
        const float3 traceDirection = SampleHemisphereCosine(Xi, normal);
#endif
        bool hit = false;
        const float3 radiance = EvaluateSparseSdfGIRayRadiance(sdfAtlas, cascadeBrickMap, brickMetadata, hierarchyBottom, hierarchyTop, brickRadiance, rayOrigin, traceDirection, hit) * Intensity * BounceStrength;
        gs_ProbeRadiance[lane] = radiance;
        gs_ProbeLuminance[lane] = dot(radiance, float3(0.2126f, 0.7152f, 0.0722f));
        gs_ProbeHit[lane] = hit ? 1.0f : 0.0f;
#if defined(SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH)
        const FPackedSh raySh = ProjectSh(radiance, traceDirection);
        gs_ProbeShY[lane] = raySh.ShY;
        gs_ProbeCo[lane] = raySh.Co;
        gs_ProbeCg[lane] = raySh.Cg;
#endif
    }
    GroupMemoryBarrierWithGroupSync();

    if (lane == 0u)
    {
        float3 sumRadiance = 0.0f.xxx;
        float sumLum = 0.0f;
        float sumLumSq = 0.0f;
        float hitCount = 0.0f;
#if defined(SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH)
        FPackedSh sumSh;
        sumSh.ShY = 0.0f.xxxx;
        sumSh.Co = 0.0f;
        sumSh.Cg = 0.0f;
#endif
        [loop]
        for (uint i = 0u; i < rayCount; ++i)
        {
            sumRadiance += gs_ProbeRadiance[i];
            sumLum += gs_ProbeLuminance[i];
            sumLumSq += gs_ProbeLuminance[i] * gs_ProbeLuminance[i];
            hitCount += gs_ProbeHit[i];
#if defined(SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH)
            sumSh.ShY += gs_ProbeShY[i];
            sumSh.Co += gs_ProbeCo[i];
            sumSh.Cg += gs_ProbeCg[i];
#endif
        }

        const float invCount = rcp((float)rayCount);
        const float3 meanRadiance = sumRadiance * invCount;
#if defined(SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH)
        const FPackedSh currentSh = ScaleSh(sumSh, invCount);
#else
        const FPackedSh currentSh = ProjectIrradianceSh(meanRadiance);
#endif
        const float meanLum = sumLum * invCount;
        const float lumVariance = max(0.0f, sumLumSq * invCount - meanLum * meanLum);
        const float normalizedVariance = saturate(lumVariance / max(meanLum * meanLum + 1e-4f, 1e-4f));

        FPackedSh resolvedSh = currentSh;
        float sampleCount = 1.0f;
        if (ProbeHistoryValid != 0u && header.PrevProbeValid != 0u)
        {
            StructuredBuffer<FScreenProbeHistory> probeHistoryRead = ResourceDescriptorHeap[ProbeHistoryReadSrvIndex];
            const FScreenProbeHistory prev = probeHistoryRead[header.PrevProbeIndex];
            const float prevCount = prev.WorldPositionCount.w;
            const float prevDepth = prev.NormalDepth.w;
            const bool geometryMatch =
                prevDepth > 0.0f &&
                prevCount > 0.0f &&
                dot(normalize(prev.NormalDepth.xyz), normalize(header.NormalValid.xyz)) > 0.9f &&
                length(prev.WorldPositionCount.xyz - header.WorldPositionDepth.xyz) <= VoxelSize * 4.0f;
            if (geometryMatch)
            {
                sampleCount = min(prevCount + 1.0f, SPARSE_SDF_GI_PROBE_TEMPORAL_MAX_SAMPLES);
                const float alpha = max(rcp(sampleCount), SPARSE_SDF_GI_PROBE_TEMPORAL_MIN_ALPHA);
                resolvedSh = LerpSh(UnpackSh(prev.PackedSH), currentSh, alpha);
            }
        }

        RWStructuredBuffer<FScreenProbeHistory> probeHistoryWrite = ResourceDescriptorHeap[ProbeHistoryWriteUavIndex];
        FScreenProbeHistory outHistory;
        outHistory.WorldPositionCount = float4(header.WorldPositionDepth.xyz, sampleCount);
        outHistory.NormalDepth = float4(header.NormalValid.xyz, header.WorldPositionDepth.w);
        outHistory.PackedSH = PackSh(resolvedSh);
        probeHistoryWrite[probeIndex] = outHistory;

        probeSH[probeIndex] = PackSh(resolvedSh);
        const float resolvedVariance = max(normalizedVariance / sampleCount, SPARSE_SDF_GI_PROBE_MIN_VARIANCE);
        probeVariance[probeIndex] = float4(resolvedVariance, hitCount * invCount, 1.0f, 0.0f);
    }
}
#endif

#if defined(SPARSE_SDF_GI_PROBE_INTERPOLATE_SHADER)
uint GetScreenProbeIndexClamped(int2 probeCoord)
{
    const int2 clamped = clamp(probeCoord, int2(0, 0), int2((int)ProbeCountX - 1, (int)ProbeCountY - 1));
    return (uint)clamped.x + (uint)clamped.y * ProbeCountX;
}

[numthreads(8, 8, 1)]
void CSInterpolateScreenProbes(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= OutputWidth || pixel.y >= OutputHeight)
    {
        return;
    }

    Texture2D<float> depthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> gbufferA = ResourceDescriptorHeap[GBufferAIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[DiffuseGIUavIndex];
    RWTexture2D<uint4> inputSHOut = ResourceDescriptorHeap[InputSHUavIndex];
    RWTexture2D<float> varianceOut = ResourceDescriptorHeap[VarianceUavIndex];

    const float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (depth <= 0.0f)
    {
        outputTexture[pixel] = 0.0f.xxxx;
        inputSHOut[pixel] = uint4(0u, 0u, 0u, 0u);
        varianceOut[pixel] = 0.0f;
        return;
    }

    StructuredBuffer<FSparseSdfGIProbeHeader> probeHeaders = ResourceDescriptorHeap[ProbeHeaderSrvIndex];
    StructuredBuffer<uint4> probeSH = ResourceDescriptorHeap[ProbeSHSrvIndex];
    StructuredBuffer<float4> probeVariance = ResourceDescriptorHeap[ProbeVarianceSrvIndex];
    const float2 uv = (float2(pixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
    const float3 viewPosition = ReconstructViewPositionFromDepth(uv, depth, Projection);
    const float3 worldPosition = mul(float4(viewPosition, 1.0f), ViewInverse).xyz;
    const float3 normal = normalize(gbufferA.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    const float2 probeSpace = (float2(pixel) + 0.5f.xx) / (float)max(ProbeTileSize, 1u) - 0.5f.xx;
    const int2 baseProbe = int2(floor(probeSpace));

    FPackedSh accumSh;
    accumSh.ShY = 0.0f.xxxx;
    accumSh.Co = 0.0f;
    accumSh.Cg = 0.0f;
    float totalWeight = 0.0f;
    float weightedVariance = 0.0f;
    float weightedHitRatio = 0.0f;

    [unroll]
    for (int oy = -1; oy <= 1; ++oy)
    {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox)
        {
            const int2 probeCoord = baseProbe + int2(ox, oy);
            if (any(probeCoord < int2(0, 0)) || probeCoord.x >= (int)ProbeCountX || probeCoord.y >= (int)ProbeCountY)
            {
                continue;
            }

            const uint probeIndex = GetScreenProbeIndexClamped(probeCoord);
            const FSparseSdfGIProbeHeader header = probeHeaders[probeIndex];
            if (header.NormalValid.w <= 0.5f)
            {
                continue;
            }

            const float2 probeCenter = (float2(probeCoord) + 0.5f.xx) * (float)max(ProbeTileSize, 1u);
            const float2 screenDelta = (float2(pixel) + 0.5f.xx - probeCenter) / (float)max(ProbeTileSize, 1u);
            const float screenWeight = exp(-dot(screenDelta, screenDelta) * 1.5f);
            const float3 probeNormal = normalize(header.NormalValid.xyz);
            const float normalWeight = pow(saturate(dot(normal, probeNormal)), 32.0f);
            const float depthWeight = exp(-abs(depth - header.WorldPositionDepth.w) / max(depth * 0.05f, 1e-3f));
            const float planeDistance = abs(dot(probeNormal, worldPosition - header.WorldPositionDepth.xyz));
            const float planeWeight = exp(-planeDistance / max(VoxelSize * 4.0f, 1e-3f));
            const float weight = screenWeight * normalWeight * depthWeight * planeWeight;
            if (weight <= 1e-5f)
            {
                continue;
            }

            accumSh = AddSh(accumSh, ScaleSh(UnpackSh(probeSH[probeIndex]), weight));
            const float4 stats = probeVariance[probeIndex];
            weightedVariance += stats.x * weight;
            weightedHitRatio += stats.y * weight;
            totalWeight += weight;
        }
    }

    if (totalWeight <= 1e-5f)
    {
        outputTexture[pixel] = 0.0f.xxxx;
        inputSHOut[pixel] = uint4(0u, 0u, 0u, 0u);
        varianceOut[pixel] = 1.0f;
        return;
    }

    const float invWeight = rcp(totalWeight);
    const FPackedSh outputSH = ScaleSh(accumSh, invWeight);
    const float confidence = saturate(totalWeight);
    const float probeVarianceValue = weightedVariance * invWeight;
    const float outputVariance = saturate(max(SPARSE_SDF_GI_PROBE_MIN_VARIANCE, probeVarianceValue) + (1.0f - confidence));
    const float3 irradiance = UnprojectIrradiance(outputSH, normal);

    inputSHOut[pixel] = PackSh(outputSH);
    varianceOut[pixel] = outputVariance;

    float3 output = irradiance;
    if (ProbeDebugMode == SPARSE_SDF_GI_PROBE_DEBUG_MODE_PLACEMENT)
    {
        const uint2 representative = probeHeaders[GetScreenProbeIndexClamped(baseProbe)].Pixel;
        const float d = length(float2(pixel) - float2(representative)) / max((float)ProbeTileSize, 1.0f);
        output = lerp(float3(0.1f, 0.1f, 0.1f), float3(0.0f, 0.8f, 1.0f), saturate(1.0f - d));
    }
    else if (ProbeDebugMode == SPARSE_SDF_GI_PROBE_DEBUG_MODE_VALIDITY)
    {
        output = confidence.xxx;
    }
    else if (ProbeDebugMode == SPARSE_SDF_GI_PROBE_DEBUG_MODE_HIT_RATIO)
    {
        output = (weightedHitRatio * invWeight).xxx;
    }
    else if (ProbeDebugMode == SPARSE_SDF_GI_PROBE_DEBUG_MODE_VARIANCE)
    {
        output = outputVariance.xxx;
    }
    else if (ProbeDebugMode == SPARSE_SDF_GI_PROBE_DEBUG_MODE_CONFIDENCE)
    {
        output = float3(confidence, saturate(totalWeight * 0.25f), 0.0f);
    }
    else if (ProbeDebugMode == SPARSE_SDF_GI_PROBE_DEBUG_MODE_IRRADIANCE)
    {
        output = irradiance;
    }

    outputTexture[pixel] = float4(output, 1.0f);
}
#endif
