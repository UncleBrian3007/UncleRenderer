#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "../Math/MathTypes.h"

constexpr uint32_t GClusterDAGInvalidIndex = 0xffffffffu;

enum class EClusterGroupReducerBackend : uint32_t
{
    Auto = 0,
    Meshopt = 1,
    PositionQem = 2,
};

struct FClusterBounds
{
    FFloat3 Center{ 0.0f, 0.0f, 0.0f };
    float Radius = 0.0f;
    FFloat3 ConeApex{ 0.0f, 0.0f, 0.0f };
    float ConeCutoff = -1.0f;
    FFloat3 ConeAxis{ 0.0f, 0.0f, 1.0f };
    float Padding = 0.0f;
};

struct FClusterRef
{
    uint32_t InstanceIndex = GClusterDAGInvalidIndex;
    uint32_t ClusterIndex = GClusterDAGInvalidIndex;

    FClusterRef() = default;
    explicit FClusterRef(uint32_t InClusterIndex)
        : ClusterIndex(InClusterIndex)
    {
    }

    FClusterRef(uint32_t InInstanceIndex, uint32_t InClusterIndex)
        : InstanceIndex(InInstanceIndex)
        , ClusterIndex(InClusterIndex)
    {
    }

    bool IsInstance() const
    {
        return InstanceIndex != GClusterDAGInvalidIndex;
    }

    bool IsValid() const
    {
        return ClusterIndex != GClusterDAGInvalidIndex;
    }
};

struct FRuntimeClusterChildRef
{
    uint32_t InstanceIndex = GClusterDAGInvalidIndex;
    uint32_t ClusterIndex = GClusterDAGInvalidIndex;
};

struct FRuntimeClusterGroup
{
    FFloat3 BoundsCenter{ 0.0f, 0.0f, 0.0f };
    float BoundsRadius = 0.0f;
    FFloat3 LodBoundsCenter{ 0.0f, 0.0f, 0.0f };
    float LodBoundsRadius = 0.0f;
    float ParentLODError = 0.0f;
    uint32_t ChildRefStart = 0;
    uint32_t ChildRefCount = 0;
    uint32_t ParentRefStart = 0;
    uint32_t ParentRefCount = 0;
    uint32_t MipLevel = 0;
    uint32_t Flags = 0;
};

struct FRuntimeCluster
{
    FClusterBounds Bounds;
    float LODError = 0.0f;
    FFloat3 LodBoundsCenter{ 0.0f, 0.0f, 0.0f };
    float LodBoundsRadius = 0.0f;
    uint32_t GroupIndex = GClusterDAGInvalidIndex;
    uint32_t GeneratingGroupIndex = GClusterDAGInvalidIndex;
    uint32_t DrawDataStart = 0;
    uint32_t DrawDataCount = 0;
    uint32_t TriangleCount = 0;
    uint32_t MipLevel = 0;
};

struct FRuntimeClusterDrawData
{
    uint32_t IndexStart = 0;
    uint32_t IndexCount = 0;
    uint32_t Reserved0 = 0;
    uint32_t Reserved1 = 0;
};

struct FRuntimeClusterHierarchy
{
    std::vector<FRuntimeClusterGroup> Groups;
    std::vector<FRuntimeCluster> Clusters;
    std::vector<FRuntimeClusterChildRef> ChildRefs;
    std::vector<FRuntimeClusterDrawData> DrawDatas;
    std::vector<uint32_t> PackedIndices;
    uint32_t RootGroupIndex = GClusterDAGInvalidIndex;

    bool IsValid() const
    {
        return RootGroupIndex != GClusterDAGInvalidIndex
            && RootGroupIndex < Groups.size()
            && !Groups.empty()
            && !Clusters.empty()
            && !DrawDatas.empty();
    }
};

struct FCluster
{
    uint32_t VertexOffset = 0;
    uint32_t VertexCount = 0;
    uint32_t TriangleOffset = 0;
    uint32_t TriangleCount = 0;
    FClusterBounds Bounds;
    float LODError = 0.0f;
    FFloat3 LodBoundsCenter{ 0.0f, 0.0f, 0.0f };
    float LodBoundsRadius = 0.0f;
    uint32_t ExternalEdgeOffset = 0;
    uint32_t ExternalEdgeCount = 0;
    uint32_t GroupIndex = GClusterDAGInvalidIndex;
    uint32_t GeneratingGroupIndex = GClusterDAGInvalidIndex;
    uint32_t MipLevel = 0;
};

struct FClusterGroup
{
    std::vector<FClusterRef> ChildRefs;
    std::vector<FClusterRef> ParentRefs;
    FFloat3 BoundsCenter{ 0.0f, 0.0f, 0.0f };
    float BoundsRadius = 0.0f;
    FFloat3 LodBoundsCenter{ 0.0f, 0.0f, 0.0f };
    float LodBoundsRadius = 0.0f;
    float ParentLODError = 0.0f;
    uint32_t MipLevel = 0;
    bool bRoot = false;
};

struct FClusterCutParams
{
    uint32_t TargetNumTriangles = (std::numeric_limits<uint32_t>::max)();
    float TargetError = 0.0f;
    uint32_t TargetOvershoot = 0;
};

struct FClusterDagPackedPosition
{
    uint32_t XY = 0;
    uint32_t Z = 0;
};

struct FClusterDAGPackedVertexData
{
    std::vector<FClusterDagPackedPosition> Positions;
    std::vector<uint32_t> Normals;
    std::vector<uint32_t> UVs;
    std::vector<uint32_t> Tangents;
    std::vector<uint32_t> Colors;
    FFloat4 PositionOffset{ 0.0f, 0.0f, 0.0f, 0.0f };
    FFloat4 PositionScale{ 1.0f, 1.0f, 1.0f, 0.0f };
    FFloat2 ConstantUV{ 0.0f, 0.0f };
    FFloat2 PaddingUV{ 0.0f, 0.0f };
    FFloat4 ConstantColor{ 1.0f, 1.0f, 1.0f, 1.0f };

    bool IsValid() const
    {
        const size_t VertexCount = Positions.size();
        return VertexCount > 0
            && Normals.size() == VertexCount
            && (UVs.empty() || UVs.size() == VertexCount)
            && (Tangents.empty() || Tangents.size() == VertexCount)
            && (Colors.empty() || Colors.size() == VertexCount);
    }
};

struct FClusterDAG
{
    std::vector<FCluster> Clusters;
    std::vector<FClusterGroup> Groups;
    std::vector<FFloat3> Positions;
    std::vector<FFloat3> Normals;
    std::vector<FFloat2> UVs;
    std::vector<FFloat4> Tangents;
    std::vector<FFloat4> Colors;
    std::vector<uint8_t> TriangleIndices;
    std::vector<uint8_t> ExternalEdges;
    std::vector<uint32_t> ClusterVertices;
    FClusterDAGPackedVertexData PackedVertexData;
    FRuntimeClusterHierarchy RuntimeHierarchy;
    uint32_t RootGroupIndex = GClusterDAGInvalidIndex;

    bool IsValid() const
    {
        if (!HasRootGroup())
        {
            return false;
        }

        const FClusterGroup& RootGroup = Groups[RootGroupIndex];
        if (!RootGroup.bRoot || RootGroup.ChildRefs.size() != 1)
        {
            return false;
        }

        for (const FClusterRef& ChildRef : RootGroup.ChildRefs)
        {
            if (!ChildRef.IsValid() || ChildRef.ClusterIndex >= Clusters.size())
            {
                return false;
            }
        }

        return true;
    }

    bool HasRootGroup() const;
    const FClusterGroup* GetRootGroup() const;
    void FindCut(std::vector<FClusterRef>& OutSelectedClusters, const FClusterCutParams& Params) const;
    bool HasRuntimeHierarchy() const { return RuntimeHierarchy.IsValid(); }
    bool HasPackedVertexData() const { return PackedVertexData.IsValid(); }
};

struct FClusterDAGBuildParams
{
    uint32_t MaxClusterVertices = 128;
    uint32_t MaxClusterTriangles = 128;
    uint32_t TargetGroupSize = 16;
    float SimplifyRatio = 0.5f;
    float ConeWeight = 0.5f;
    uint32_t MaxLevels = 20;
    uint32_t ConvergeTriangleThreshold = 128;
    float AttributeNormalWeight = 0.5f;
    float AttributeUVWeight = 1.0f;
    EClusterGroupReducerBackend ReducerBackend = EClusterGroupReducerBackend::Meshopt;
};

uint64_t HashClusterDAGBuildParams(const FClusterDAGBuildParams& Params);
bool BuildClusterDAGPackedVertexData(const FClusterDAG& Dag, FClusterDAGPackedVertexData& OutPackedVertexData);
bool LoadClusterDAGCacheFile(
    const std::wstring& CacheFilePath,
    const std::wstring& SourceFilePath,
    const FClusterDAGBuildParams& Params,
    std::vector<std::vector<FClusterDAG>>& OutMeshClusterDAGs);
bool SaveClusterDAGCacheFile(
    const std::wstring& CacheFilePath,
    const std::wstring& SourceFilePath,
    const FClusterDAGBuildParams& Params,
    const std::vector<std::vector<FClusterDAG>>& MeshClusterDAGs);
