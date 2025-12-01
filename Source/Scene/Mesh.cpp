#include "Mesh.h"

FMesh FMesh::CreateCube(float Size)
{
    FMesh Mesh;

    const float HalfSize = Size * 0.5f;

    std::vector<FVertex> Vertices = {
        // +X
        { { HalfSize, -HalfSize, -HalfSize }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { { HalfSize, -HalfSize,  HalfSize }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        { { HalfSize,  HalfSize,  HalfSize }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { { HalfSize,  HalfSize, -HalfSize }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },

        // -X
        { { -HalfSize, -HalfSize,  HalfSize }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { { -HalfSize, -HalfSize, -HalfSize }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        { { -HalfSize,  HalfSize, -HalfSize }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -HalfSize,  HalfSize,  HalfSize }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },

        // +Y
        { { -HalfSize,  HalfSize, -HalfSize }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
        { {  HalfSize,  HalfSize, -HalfSize }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
        { {  HalfSize,  HalfSize,  HalfSize }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -HalfSize,  HalfSize,  HalfSize }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },

        // -Y
        { { -HalfSize, -HalfSize,  HalfSize }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
        { {  HalfSize, -HalfSize,  HalfSize }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
        { {  HalfSize, -HalfSize, -HalfSize }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -HalfSize, -HalfSize, -HalfSize }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },

        // +Z
        { { -HalfSize, -HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        { { -HalfSize,  HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { {  HalfSize,  HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
        { {  HalfSize, -HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },

        // -Z
        { {  HalfSize, -HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f } },
        { {  HalfSize,  HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f } },
        { { -HalfSize,  HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f } },
        { { -HalfSize, -HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f } },
    };

    std::vector<uint32_t> Indices = {
        // +X
        0, 1, 2, 0, 2, 3,
        // -X
        4, 5, 6, 4, 6, 7,
        // +Y
        8, 9, 10, 8, 10, 11,
        // -Y
        12, 13, 14, 12, 14, 15,
        // +Z
        16, 17, 18, 16, 18, 19,
        // -Z
        20, 21, 22, 20, 22, 23,
    };

    Mesh.SetVertices(Vertices);
    Mesh.SetIndices(Indices);

    return Mesh;
}
