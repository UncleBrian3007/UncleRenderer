#pragma once

#include <utility>
#include <vector>
#include "../Math/MathTypes.h"

class FMesh
{
public:
    struct FVertex
    {
        FFloat3 Position;
        FFloat3 Normal;
        FFloat2 UV;
        FFloat4 Tangent;
        FFloat4 Color{ 1.0f, 1.0f, 1.0f, 1.0f };
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

    void SetVertices(const std::vector<FVertex>& InVertices) { Vertices = InVertices; }
    void SetIndices(const std::vector<uint32_t>& InIndices) { Indices = InIndices; }

    const std::vector<FVertex>& GetVertices() const { return Vertices; }
    const std::vector<uint32_t>& GetIndices() const { return Indices; }
    const std::vector<FMeshlet>& GetMeshlets() const { return Meshlets; }
    const std::vector<uint32_t>& GetMeshletVertices() const { return MeshletVertices; }
    const std::vector<uint8_t>& GetMeshletTriangles() const { return MeshletTriangles; }
    const std::vector<uint32_t>& GetMeshletIndices() const { return MeshletIndices; }
    const std::vector<FMeshletBounds>& GetMeshletBounds() const { return MeshletBounds; }
    const FMeshletGroup* GetMeshletGroup(size_t Index) const;
    size_t GetMeshletGroupCount() const { return MeshletGroups.size(); }
    bool HasMeshlets() const;

    void SetMeshletIndexingAllowed(bool bAllowed) { bAllowMeshletIndexing = bAllowed; }
    bool IsMeshletIndexingAllowed() const { return bAllowMeshletIndexing; }

    static FMesh CreateCube(float Size = 1.0f);
    static FMesh CreateSphere(float Radius = 1.0f, uint32_t SliceCount = 32, uint32_t StackCount = 16);

    // Generate per-vertex normals and tangents if missing or invalid.
    void GenerateNormalsIfMissing();
    void GenerateTangentsIfMissing();
    void BuildMeshlets(uint32_t MaxVertices = 64, uint32_t MaxTriangles = 124, float ConeWeight = 0.0f);
    void BuildMeshletGroups(const std::vector<std::pair<uint32_t, uint32_t>>& IndexRanges, uint32_t MaxVertices = 64, uint32_t MaxTriangles = 124, float ConeWeight = 0.0f);

private:
    std::vector<FVertex> Vertices;
    std::vector<uint32_t> Indices;
    std::vector<FMeshlet> Meshlets;
    std::vector<uint32_t> MeshletVertices;
    std::vector<uint8_t> MeshletTriangles;
    std::vector<uint32_t> MeshletIndices;
    std::vector<FMeshletBounds> MeshletBounds;
    std::vector<FMeshletGroup> MeshletGroups;
    bool bAllowMeshletIndexing = false;
};
