#include "Mesh.h"

FMesh FMesh::CreateCube(float Size)
{
    FMesh Mesh;

    const float HalfSize = Size * 0.5f;

    const FFloat4 TangentPosX{ 0.0f, 0.0f, 1.0f, 1.0f };
    const FFloat4 TangentNegX{ 0.0f, 0.0f, -1.0f, 1.0f };
    const FFloat4 TangentPosY{ 1.0f, 0.0f, 0.0f, 1.0f };
    const FFloat4 TangentNegY{ 1.0f, 0.0f, 0.0f, 1.0f };
    const FFloat4 TangentPosZ{ 1.0f, 0.0f, 0.0f, 1.0f };
    const FFloat4 TangentNegZ{ -1.0f, 0.0f, 0.0f, 1.0f };

    std::vector<FVertex> Vertices = {
        // +X
        { { HalfSize, -HalfSize, -HalfSize }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f }, TangentPosX },
        { { HalfSize, -HalfSize,  HalfSize }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f }, TangentPosX },
        { { HalfSize,  HalfSize,  HalfSize }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, TangentPosX },
        { { HalfSize,  HalfSize, -HalfSize }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f }, TangentPosX },

        // -X
        { { -HalfSize, -HalfSize,  HalfSize }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f }, TangentNegX },
        { { -HalfSize, -HalfSize, -HalfSize }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f }, TangentNegX },
        { { -HalfSize,  HalfSize, -HalfSize }, { -1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, TangentNegX },
        { { -HalfSize,  HalfSize,  HalfSize }, { -1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f }, TangentNegX },

        // +Y
        { { -HalfSize,  HalfSize, -HalfSize }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f }, TangentPosY },
        { {  HalfSize,  HalfSize, -HalfSize }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f }, TangentPosY },
        { {  HalfSize,  HalfSize,  HalfSize }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f }, TangentPosY },
        { { -HalfSize,  HalfSize,  HalfSize }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f }, TangentPosY },

        // -Y
        { { -HalfSize, -HalfSize,  HalfSize }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 1.0f }, TangentNegY },
        { {  HalfSize, -HalfSize,  HalfSize }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 1.0f }, TangentNegY },
        { {  HalfSize, -HalfSize, -HalfSize }, { 0.0f, -1.0f, 0.0f }, { 1.0f, 0.0f }, TangentNegY },
        { { -HalfSize, -HalfSize, -HalfSize }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f }, TangentNegY },

        // +Z
        { { -HalfSize, -HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f }, TangentPosZ },
        { { -HalfSize,  HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }, TangentPosZ },
        { {  HalfSize,  HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }, TangentPosZ },
        { {  HalfSize, -HalfSize, HalfSize }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }, TangentPosZ },

        // -Z
        { {  HalfSize, -HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 1.0f }, TangentNegZ },
        { {  HalfSize,  HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f }, TangentNegZ },
        { { -HalfSize,  HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 0.0f }, TangentNegZ },
        { { -HalfSize, -HalfSize, -HalfSize }, { 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f }, TangentNegZ },
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
