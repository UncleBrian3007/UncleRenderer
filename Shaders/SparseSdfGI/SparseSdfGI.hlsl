#include "../SceneConstants.hlsl"
#include "../Common.hlsli"

cbuffer SparseSdfGIConstants : register(b1)
{
    uint OutputWidth;
    uint OutputHeight;
    uint AtlasResolution;
    uint BrickGridResolution;
    uint BrickVoxelResolution;
    uint CascadeCount;
    uint FrameIndex;
    uint DebugMode;
    uint Enabled;
    uint TraceHalfResolution;
    uint ModelTriangleCount;
    uint ModelDrawIndexStart;
    uint ModelDrawIndexCount;
    uint MaxTriangleVoxelSpan;
    float BaseVoxelSize;
    float CascadeScale;
    float Intensity;
    float MaxTraceDistance;
    float2 SparseSdfGIPadding1;
    float3 CascadeMin;
    float VoxelSize;
    float3 CascadeExtent;
    float SurfaceThicknessVoxels;
    row_major float4x4 ModelWorld;
};

cbuffer SparseSdfGIBindlessConstants : register(b2)
{
    uint SdfAtlasSrvIndex;
    uint SdfAtlasUavIndex;
    uint SdfSeedDistanceSrvIndex;
    uint SdfSeedDistanceUavIndex;
    uint CascadeBrickMapSrvIndex;
    uint CascadeBrickMapUavIndex;
    uint DiffuseGIUavIndex;
    uint DepthIndex;
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint GBufferCIndex;
    uint PositionBufferIndex;
    uint IndexBufferIndex;
    uint LinearClampSamplerIndex;
};

static const uint SPARSE_SDF_GI_SEED_DISTANCE_EMPTY = 0xffffffffu;
static const uint SPARSE_SDF_GI_INVALID_BRICK_ID = 0xffffffffu;
static const float SPARSE_SDF_GI_SEED_DISTANCE_QUANTIZATION = 65535.0f;
static const uint SPARSE_SDF_GI_BRICK_LOCAL_DIM = 8u;
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

groupshared float gs_EikonalA[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];
groupshared float gs_EikonalB[SPARSE_SDF_GI_EIKONAL_LDS_COUNT];

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

float DecodeSdfWorldDistance(float sdf)
{
    return sdf * (float)BrickVoxelResolution * VoxelSize;
}

float EncodeSdfWorldDistance(float distance)
{
    return saturate(distance / max((float)BrickVoxelResolution * VoxelSize, 1e-5f));
}

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
    const float cellDistance = (float)offset / (float)BrickVoxelResolution;

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

uint3 BrickIdToBrickCoord(uint brickId)
{
    return uint3(
        brickId % BrickGridResolution,
        (brickId / BrickGridResolution) % BrickGridResolution,
        brickId / (BrickGridResolution * BrickGridResolution));
}

float LoadSdfBrickVoxel(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, int3 globalVoxelCoord)
{
    if (any(globalVoxelCoord < 0) || any(globalVoxelCoord >= (int)AtlasResolution))
    {
        return 1.0f;
    }

    const uint3 brickCoord = (uint3)globalVoxelCoord / BrickVoxelResolution;
    if (any(brickCoord >= BrickGridResolution.xxx))
    {
        return 1.0f;
    }

    const uint brickMapIndex = LinearizeBrickCoord(brickCoord);
    const uint brickCount = BrickGridResolution * BrickGridResolution * BrickGridResolution;
    if (brickMapIndex >= brickCount)
    {
        return 1.0f;
    }

    const uint brickId = cascadeBrickMap[brickMapIndex];
    if (brickId == SPARSE_SDF_GI_INVALID_BRICK_ID || brickId >= brickCount)
    {
        return 1.0f;
    }

    const uint3 localCoord = (uint3)globalVoxelCoord - brickCoord * BrickVoxelResolution;
    const uint3 atlasCoord = BrickIdToBrickCoord(brickId) * BrickVoxelResolution + localCoord;
    if (any(atlasCoord >= AtlasResolution.xxx))
    {
        return 1.0f;
    }

    return sdfAtlas.Load(int4((int3)atlasCoord, 0)).r;
}

float SampleSdfAtlas(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    const float3 atlasFloat = (worldPosition - CascadeMin) / VoxelSize;
    const float3 sampleCoord = atlasFloat - 0.5f.xxx;
    const int3 baseCoord = (int3)floor(sampleCoord);
    const float3 fracCoord = saturate(sampleCoord - (float3)baseCoord);

    const float c000 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(0, 0, 0));
    const float c100 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(1, 0, 0));
    const float c010 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(0, 1, 0));
    const float c110 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(1, 1, 0));
    const float c001 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(0, 0, 1));
    const float c101 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(1, 0, 1));
    const float c011 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(0, 1, 1));
    const float c111 = LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, baseCoord + int3(1, 1, 1));

    const float c00 = lerp(c000, c100, fracCoord.x);
    const float c10 = lerp(c010, c110, fracCoord.x);
    const float c01 = lerp(c001, c101, fracCoord.x);
    const float c11 = lerp(c011, c111, fracCoord.x);
    const float c0 = lerp(c00, c10, fracCoord.y);
    const float c1 = lerp(c01, c11, fracCoord.y);
    return lerp(c0, c1, fracCoord.z);
}

float SampleSdfAtlasPoint(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    const float3 atlasFloat = (worldPosition - CascadeMin) / VoxelSize;
    if (any(atlasFloat < 0.0f.xxx) || any(atlasFloat >= (float)AtlasResolution))
    {
        return 1.0f;
    }

    return LoadSdfBrickVoxel(sdfAtlas, cascadeBrickMap, (int3)floor(atlasFloat));
}

float SampleSdfAtlasDebugSurface(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 worldPosition)
{
    return min(SampleSdfAtlas(sdfAtlas, cascadeBrickMap, worldPosition), SampleSdfAtlasPoint(sdfAtlas, cascadeBrickMap, worldPosition));
}

bool IsClearlyOutsideCascade(float3 worldPosition)
{
    const float3 atlasFloat = (worldPosition - CascadeMin) / VoxelSize;
    return any(atlasFloat < -0.5f.xxx) || any(atlasFloat > ((float)AtlasResolution + 0.5f));
}

bool TraceSdf(float3 rayOrigin, float3 rayDirection, Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, out uint stepCount, out float travel)
{
    stepCount = 0u;
    travel = 0.0f;

    [loop]
    for (uint stepIndex = 0u; stepIndex < 128u; ++stepIndex)
    {
        const float3 p = rayOrigin + rayDirection * travel;
        const float sdf = SampleSdfAtlas(sdfAtlas, cascadeBrickMap, p);
        const float decodedDistance = DecodeSdfWorldDistance(sdf);
        stepCount = stepIndex + 1u;
        if (decodedDistance <= VoxelSize * 0.75f)
        {
            return true;
        }

        const float stepDistance = clamp(decodedDistance, VoxelSize * 0.25f, VoxelSize * 2.0f);
        travel += stepDistance;
        if (travel > MaxTraceDistance)
        {
            break;
        }
    }

    return false;
}

[numthreads(8, 8, 8)]
void CSSeedAtlasInit(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture3D<uint> seedDistanceAtlas = ResourceDescriptorHeap[SdfSeedDistanceUavIndex];
    RWStructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapUavIndex];

    if (all(dispatchThreadId < AtlasResolution.xxx))
    {
        seedDistanceAtlas[dispatchThreadId] = SPARSE_SDF_GI_SEED_DISTANCE_EMPTY;
    }

    if (all(dispatchThreadId < BrickGridResolution.xxx))
    {
        const uint brickMapIndex = dispatchThreadId.x
            + dispatchThreadId.y * BrickGridResolution
            + dispatchThreadId.z * BrickGridResolution * BrickGridResolution;
        cascadeBrickMap[brickMapIndex] = brickMapIndex;
    }
}

[numthreads(64, 1, 1)]
void CSVoxelizeStaticMesh(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= ModelTriangleCount)
    {
        return;
    }

    StructuredBuffer<float3> positions = ResourceDescriptorHeap[PositionBufferIndex];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[IndexBufferIndex];
    RWTexture3D<uint> seedDistanceAtlas = ResourceDescriptorHeap[SdfSeedDistanceUavIndex];

    const uint triBase = ModelDrawIndexStart + dispatchThreadId.x * 3u;
    if (triBase + 2u >= ModelDrawIndexStart + ModelDrawIndexCount)
    {
        return;
    }

    const uint i0 = indices[triBase + 0u];
    const uint i1 = indices[triBase + 1u];
    const uint i2 = indices[triBase + 2u];
    const float3 p0 = mul(float4(positions[i0], 1.0f), ModelWorld).xyz;
    const float3 p1 = mul(float4(positions[i1], 1.0f), ModelWorld).xyz;
    const float3 p2 = mul(float4(positions[i2], 1.0f), ModelWorld).xyz;

    const float3 triMinWorld = min(p0, min(p1, p2)) - VoxelSize * SurfaceThicknessVoxels;
    const float3 triMaxWorld = max(p0, max(p1, p2)) + VoxelSize * SurfaceThicknessVoxels;
    const int3 minCoord = clamp((int3)floor((triMinWorld - CascadeMin) / VoxelSize), 0, (int)AtlasResolution - 1);
    const int3 maxCoord = clamp((int3)ceil((triMaxWorld - CascadeMin) / VoxelSize), 0, (int)AtlasResolution - 1);
    const int3 span = maxCoord - minCoord + 1;
    if (any(span <= 0) || any(span > (int)MaxTriangleVoxelSpan))
    {
        return;
    }

    [loop]
    for (int z = minCoord.z; z <= maxCoord.z; ++z)
    {
        [loop]
        for (int y = minCoord.y; y <= maxCoord.y; ++y)
        {
            [loop]
            for (int x = minCoord.x; x <= maxCoord.x; ++x)
            {
                const float3 voxelCenter = CascadeMin + ((float3(x, y, z) + 0.5f.xxx) * VoxelSize);
                const float distanceToTriangle = PointTriangleDistance(voxelCenter, p0, p1, p2);
                const float surfaceBand = max(VoxelSize * SurfaceThicknessVoxels, 1e-5f);
                if (distanceToTriangle <= surfaceBand)
                {
                    const uint quantizedDistance = (uint)round(EncodeSdfWorldDistance(distanceToTriangle) * SPARSE_SDF_GI_SEED_DISTANCE_QUANTIZATION);
                    InterlockedMin(seedDistanceAtlas[uint3(x, y, z)], quantizedDistance);
                }
            }
        }
    }
}

[numthreads(512, 1, 1)]
void CSEikonalBrickLocal(uint3 groupId : SV_GroupID, uint groupThreadIndex : SV_GroupIndex)
{
    if (groupThreadIndex >= SPARSE_SDF_GI_EIKONAL_LDS_COUNT || any(groupId >= BrickGridResolution.xxx))
    {
        return;
    }

    Texture3D<uint> seedDistanceAtlas = ResourceDescriptorHeap[SdfSeedDistanceSrvIndex];
    RWTexture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasUavIndex];

    const uint3 localCoord = UnflattenBrickLocalCoord(groupThreadIndex);
    const uint3 atlasCoord = groupId * BrickVoxelResolution + localCoord;
    const uint seedDistance = seedDistanceAtlas.Load(int4(atlasCoord, 0)).r;
    const float seedValue = (seedDistance == SPARSE_SDF_GI_SEED_DISTANCE_EMPTY)
        ? 1.0f
        : saturate((float)seedDistance / SPARSE_SDF_GI_SEED_DISTANCE_QUANTIZATION);

    gs_EikonalA[groupThreadIndex] = seedValue;
    gs_EikonalB[groupThreadIndex] = seedValue;
    GroupMemoryBarrierWithGroupSync();

    bool readFromA = true;
    [unroll]
    for (uint sweep = 0u; sweep < 4u; ++sweep)
    {
        [unroll]
        for (uint offsetIndex = 0u; offsetIndex < 3u; ++offsetIndex)
        {
            const uint offset = (offsetIndex == 0u) ? 4u : ((offsetIndex == 1u) ? 2u : 1u);
            const float relaxedValue = RelaxEikonalVoxel(readFromA, localCoord, offset);
            EikonalStore(!readFromA, groupThreadIndex, relaxedValue);
            GroupMemoryBarrierWithGroupSync();
            readFromA = !readFromA;
        }
    }

    sdfAtlas[atlasCoord] = readFromA ? gs_EikonalA[groupThreadIndex] : gs_EikonalB[groupThreadIndex];
}

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
    float3 safeDirection = rayDirection;
    safeDirection.x = (abs(safeDirection.x) < 1e-6f) ? ((safeDirection.x < 0.0f) ? -1e-6f : 1e-6f) : safeDirection.x;
    safeDirection.y = (abs(safeDirection.y) < 1e-6f) ? ((safeDirection.y < 0.0f) ? -1e-6f : 1e-6f) : safeDirection.y;
    safeDirection.z = (abs(safeDirection.z) < 1e-6f) ? ((safeDirection.z < 0.0f) ? -1e-6f : 1e-6f) : safeDirection.z;

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

float3 DebugVoxelProjection(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint2 pixel)
{
    const float3 rayDirection = GetWorldRayDirection(pixel);

    float tEnter = 0.0f;
    float tExit = 0.0f;
    if (!RayBoxIntersect(CameraPosition, rayDirection, tEnter, tExit))
    {
        return 0.0f.xxx;
    }

    const float startT = max(tEnter, 0.0f);
    const float endT = min(tExit, MaxTraceDistance);
    const float stepDistance = max(VoxelSize * 0.5f, 1e-4f);

    [loop]
    for (uint stepIndex = 0u; stepIndex < 1024u; ++stepIndex)
    {
        const float t = startT + (float)stepIndex * stepDistance;
        if (t > endT)
        {
            break;
        }

        const float sdf = SampleSdfAtlasPoint(sdfAtlas, cascadeBrickMap, CameraPosition + rayDirection * t);
        if (sdf < 0.999f)
        {
            const float occupancy = saturate(1.0f - sdf);
            const float depthT = saturate((t - startT) / max(endT - startT, VoxelSize));
            const float gray = lerp(0.25f, 1.0f, occupancy) * (1.0f - depthT * 0.65f);
            return gray.xxx;
        }
    }

    return 0.0f.xxx;
}

bool TraceSdfSurface(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, float3 rayOrigin, float3 rayDirection, out float hitTravel, out uint stepCount, out uint traceStatus)
{
    hitTravel = 0.0f;
    stepCount = 0u;
    traceStatus = SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS;

    float tEnter = 0.0f;
    float tExit = 0.0f;
    if (!RayBoxIntersect(rayOrigin, rayDirection, tEnter, tExit))
    {
        return false;
    }

    const float startT = max(tEnter, 0.0f);
    const float endT = min(tExit, MaxTraceDistance);
    traceStatus = (endT < tExit) ? SPARSE_SDF_GI_TRACE_STATUS_MAX_DISTANCE : SPARSE_SDF_GI_TRACE_STATUS_CASCADE_EXIT;
    const float hitThreshold = VoxelSize * 0.75f;
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

        const float sdf = SampleSdfAtlasDebugSurface(sdfAtlas, cascadeBrickMap, samplePosition);
        const float decodedDistance = DecodeSdfWorldDistance(sdf);
        stepCount = stepIndex + 1u;
        if (decodedDistance <= hitThreshold)
        {
            float bestTravel = travel;
            float bestDistance = decodedDistance;
            const float refineStart = max(startT, previousTravel);
            const float refineEnd = min(endT, travel + VoxelSize * 2.0f);

            [unroll]
            for (uint refineIndex = 0u; refineIndex <= 24u; ++refineIndex)
            {
                const float t = lerp(refineStart, refineEnd, (float)refineIndex / 24.0f);
                const float refineDistance = DecodeSdfWorldDistance(SampleSdfAtlasDebugSurface(sdfAtlas, cascadeBrickMap, rayOrigin + rayDirection * t));
                if (refineDistance < bestDistance)
                {
                    bestDistance = refineDistance;
                    bestTravel = t;
                }
            }

            hitTravel = bestTravel;
            traceStatus = SPARSE_SDF_GI_TRACE_STATUS_HIT;
            return true;
        }

        previousTravel = travel;
        travel += clamp(decodedDistance, minStepDistance, maxStepDistance);
    }

    return false;
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

float3 DebugBrickLocalSdfSurface(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint2 pixel)
{
    const float3 rayDirection = GetWorldRayDirection(pixel);

    float hitTravel = 0.0f;
    uint stepCount = 0u;
    uint traceStatus = SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS;
    if (!TraceSdfSurface(sdfAtlas, cascadeBrickMap, CameraPosition, rayDirection, hitTravel, stepCount, traceStatus))
    {
        return 0.0f.xxx;
    }

    const float3 hitPosition = CameraPosition + rayDirection * hitTravel;
    float3 normal = ComputeSdfNormal(sdfAtlas, cascadeBrickMap, hitPosition);
    if (dot(normal, normal) <= 1e-8f)
    {
        normal = -rayDirection;
    }
    if (dot(normal, -rayDirection) < 0.0f)
    {
        normal = -normal;
    }

    const float3 lightDirection = normalize(float3(0.45f, 0.75f, -0.35f));
    const float diffuse = saturate(dot(normal, lightDirection));
    const float facing = saturate(dot(normal, -rayDirection));
    const float rim = pow(saturate(1.0f - facing), 2.0f);
    const float stepFade = lerp(1.0f, 0.75f, saturate((float)stepCount / (float)SPARSE_SDF_GI_SURFACE_TRACE_MAX_STEPS));
    const float gray = saturate((0.12f + 0.78f * diffuse + 0.10f * rim) * stepFade);
    return gray.xxx;
}

float3 DebugStepCount(Texture3D<float> sdfAtlas, StructuredBuffer<uint> cascadeBrickMap, uint2 pixel)
{
    const float3 rayDirection = GetWorldRayDirection(pixel);

    float hitTravel = 0.0f;
    uint stepCount = 0u;
    uint traceStatus = SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS;
    const bool hit = TraceSdfSurface(sdfAtlas, cascadeBrickMap, CameraPosition, rayDirection, hitTravel, stepCount, traceStatus);
    const float stepT = saturate((float)stepCount / SPARSE_SDF_GI_STEP_COUNT_DEBUG_SCALE);
    const float cost = sqrt(stepT);

    if (hit)
    {
        return lerp(0.18f.xxx, 1.0f.xxx, cost);
    }

    if (traceStatus == SPARSE_SDF_GI_TRACE_STATUS_CASCADE_MISS)
    {
        return 0.0f.xxx;
    }
    if (traceStatus == SPARSE_SDF_GI_TRACE_STATUS_MAX_DISTANCE)
    {
        return lerp(float3(0.01f, 0.02f, 0.07f), float3(0.05f, 0.12f, 0.34f), cost);
    }
    if (traceStatus == SPARSE_SDF_GI_TRACE_STATUS_ATLAS_OUTSIDE)
    {
        return lerp(float3(0.00f, 0.08f, 0.10f), float3(0.10f, 0.45f, 0.50f), cost);
    }

    return lerp(float3(0.03f, 0.01f, 0.08f), float3(0.18f, 0.08f, 0.30f), cost);
}

[numthreads(8, 8, 1)]
void CSDebugTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputWidth || dispatchThreadId.y >= OutputHeight)
    {
        return;
    }

    Texture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasSrvIndex];
    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[DiffuseGIUavIndex];
    const uint2 pixel = dispatchThreadId.xy;
    if (DebugMode == 3u)
    {
        outputTexture[pixel] = float4(DebugVoxelProjection(sdfAtlas, cascadeBrickMap, pixel), 1.0f);
        return;
    }
    if (DebugMode == 4u)
    {
        outputTexture[pixel] = float4(DebugBrickLocalSdfSurface(sdfAtlas, cascadeBrickMap, pixel), 1.0f);
        return;
    }
    if (DebugMode == 5u)
    {
        outputTexture[pixel] = float4(DebugStepCount(sdfAtlas, cascadeBrickMap, pixel), 1.0f);
        return;
    }

    const float3 rayDirection = GetWorldRayDirection(pixel);
    uint stepCount = 0u;
    float travel = 0.0f;
    const bool hit = TraceSdf(CameraPosition, rayDirection, sdfAtlas, cascadeBrickMap, stepCount, travel);
    const float stepT = saturate((float)stepCount / 128.0f);
    const float travelT = saturate(travel / max(MaxTraceDistance, VoxelSize));

    float3 color = hit
        ? lerp(float3(1.0f, 0.28f, 0.05f), float3(1.0f, 0.9f, 0.2f), stepT)
        : lerp(float3(0.03f, 0.06f, 0.12f), float3(0.05f, 0.35f, 0.95f), travelT);
    if (DebugMode == 2u)
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
    Texture2D gbufferC = ResourceDescriptorHeap[GBufferCIndex];
    Texture3D<float> sdfAtlas = ResourceDescriptorHeap[SdfAtlasSrvIndex];
    StructuredBuffer<uint> cascadeBrickMap = ResourceDescriptorHeap[CascadeBrickMapSrvIndex];
    RWTexture2D<float4> outputTexture = ResourceDescriptorHeap[DiffuseGIUavIndex];

    const uint2 pixel = dispatchThreadId.xy;
    const float2 uv = (float2(pixel) + 0.5f.xx) / float2(max(OutputWidth, 1u), max(OutputHeight, 1u));
    const float depth = depthTexture.Load(int3(pixel, 0)).r;
    if (depth <= 0.0f)
    {
        outputTexture[pixel] = 0.0f.xxxx;
        return;
    }

    const float3 normal = normalize(gbufferA.Load(int3(pixel, 0)).xyz * 2.0f - 1.0f);
    const float3 albedo = gbufferC.Load(int3(pixel, 0)).rgb;
    const float3 viewPosition = ReconstructViewPositionFromDepth(uv, depth, Projection);
    const float3 worldPosition = mul(float4(viewPosition, 1.0f), ViewInverse).xyz;

    uint stepCount = 0u;
    float travel = 0.0f;
    const float3 rayOrigin = worldPosition + normal * (VoxelSize * 2.0f);
    const float3 traceDirection = SampleHemisphereCosine(Random2(pixel, FrameIndex), normal);
    const bool hit = TraceSdf(rayOrigin, traceDirection, sdfAtlas, cascadeBrickMap, stepCount, travel);

    // This is an occlusion-based placeholder until radiance cache / hit lighting exists.
    const float visibility = hit ? 0.25f : 1.0f;
    const float sky = saturate(traceDirection.y * 0.5f + 0.5f);
    const float3 irradiance = lerp(float3(0.025f, 0.03f, 0.04f), float3(0.18f, 0.22f, 0.28f), sky) * visibility * Intensity;
    outputTexture[pixel] = float4(irradiance * max(albedo, 0.02f.xxx), 1.0f);
}
