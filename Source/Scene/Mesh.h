#pragma once

#include <cstddef>
#include <utility>
#include <vector>
#include "../Math/MathTypes.h"
#include "ClusterDAG.h"

class FMesh
{
public:
    struct FVertexStreams
    {
        std::vector<FFloat3> Positions;
        std::vector<FFloat3> Normals;
        std::vector<FFloat2> UVs;
        std::vector<FFloat4> Tangents;
        std::vector<FFloat4> Colors;
        std::vector<FUInt4> Joints;
        std::vector<FFloat4> Weights;
    };

    struct FPrimitive
    {
        std::vector<uint32_t> Indices;
        FVertexStreams VertexStreams;
    };

    struct FMeshlet
    {
        uint32_t VertexOffset = 0;
        uint32_t TriangleOffset = 0;
        uint32_t VertexCount = 0;
        uint32_t TriangleCount = 0;
        uint32_t IndexOffset = 0;
        uint32_t IndexCount = 0;
    };

    struct FMeshletBounds
    {
        FFloat3 Center{ 0.0f, 0.0f, 0.0f };
        float Radius = 0.0f;
        FFloat3 ConeApex{ 0.0f, 0.0f, 0.0f };
        float ConeCutoff = -1.0f;
        FFloat3 ConeAxis{ 0.0f, 0.0f, 1.0f };
        float Padding = 0.0f;
    };

    struct FMeshletGroup
    {
        std::vector<FMeshlet> Meshlets;
        std::vector<uint32_t> MeshletVertices;
        std::vector<uint8_t> MeshletTriangles;
        std::vector<uint32_t> MeshletIndices;
        std::vector<FMeshletBounds> MeshletBounds;
    };

    void SetPrimitives(const std::vector<FPrimitive>& InPrimitives) { Primitives = InPrimitives; }
    void AddPrimitive(FPrimitive&& InPrimitive) { Primitives.push_back(std::move(InPrimitive)); }

    const std::vector<FPrimitive>& GetPrimitives() const { return Primitives; }
    const FMeshletGroup* GetMeshletGroup(size_t Index) const;
    size_t GetMeshletGroupCount() const { return MeshletGroups.size(); }
    const FClusterDAG* GetClusterDAG(size_t Index) const;
    const std::vector<FClusterDAG>& GetClusterDAGs() const { return ClusterDAGs; }
    size_t GetClusterDAGCount() const { return ClusterDAGs.size(); }
    bool HasMeshlets() const;
    bool HasClusterDAGs() const;
    static void SetOptimizationStatsLoggingEnabled(bool bEnabled);
    static bool IsOptimizationStatsLoggingEnabled();

    void SetMeshletIndexingAllowed(bool bAllowed) { bAllowMeshletIndexing = bAllowed; }
    bool IsMeshletIndexingAllowed() const { return bAllowMeshletIndexing; }
    void SetClusterDAGs(std::vector<FClusterDAG>&& InClusterDAGs) { ClusterDAGs = std::move(InClusterDAGs); }

    static FMesh CreateCube(float Size = 1.0f);
    static FMesh CreateSphere(float Radius = 1.0f, uint32_t SliceCount = 32, uint32_t StackCount = 16);

    // Generate per-vertex normals and tangents if missing or invalid.
    void GenerateNormalsIfMissing();
    void GenerateTangentsIfMissing();
    void BuildMeshlets(uint32_t MaxVertices = 64, uint32_t MaxTriangles = 124, float ConeWeight = 0.0f);
    void BuildMeshletGroups(const std::vector<size_t>& PrimitiveIndices, uint32_t MaxVertices = 64, uint32_t MaxTriangles = 124, float ConeWeight = 0.0f);
    void BuildClusterDAGs(const FClusterDAGBuildParams& Params = {});

private:
    std::vector<FPrimitive> Primitives;
    std::vector<FMeshletGroup> MeshletGroups;
    std::vector<FClusterDAG> ClusterDAGs;
    bool bAllowMeshletIndexing = false;
};
