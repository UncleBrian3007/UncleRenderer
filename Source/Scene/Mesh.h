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
    };

    void SetVertices(const std::vector<FVertex>& InVertices) { Vertices = InVertices; }
    void SetIndices(const std::vector<uint32_t>& InIndices) { Indices = InIndices; }

    const std::vector<FVertex>& GetVertices() const { return Vertices; }
    const std::vector<uint32_t>& GetIndices() const { return Indices; }

private:
    std::vector<FVertex> Vertices;
    std::vector<uint32_t> Indices;
};
