#pragma once

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

    void SetVertices(const std::vector<FVertex>& InVertices) { Vertices = InVertices; }
    void SetIndices(const std::vector<uint32_t>& InIndices) { Indices = InIndices; }

    const std::vector<FVertex>& GetVertices() const { return Vertices; }
    const std::vector<uint32_t>& GetIndices() const { return Indices; }

    static FMesh CreateCube(float Size = 1.0f);
    static FMesh CreateSphere(float Radius = 1.0f, uint32_t SliceCount = 32, uint32_t StackCount = 16);

    // Generate per-vertex normals and tangents if missing or invalid.
    void GenerateNormalsIfMissing();
    void GenerateTangentsIfMissing();

private:
    std::vector<FVertex> Vertices;
    std::vector<uint32_t> Indices;
};
