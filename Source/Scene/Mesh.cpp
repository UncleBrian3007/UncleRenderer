#include "Mesh.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    bool IsTangentValid(const FFloat4& Tangent)
    {
        using namespace DirectX;
        const XMVECTOR TangentVec = XMLoadFloat4(&Tangent);
        const float LengthSq = XMVectorGetX(XMVector3LengthSq(TangentVec));
        return LengthSq > 1e-6f;
    }

    DirectX::XMVECTOR BuildOrthonormalTangent(const DirectX::XMVECTOR& Normal)
    {
        using namespace DirectX;
        const XMVECTOR Up = std::abs(XMVectorGetX(Normal)) < 0.99f ? g_XMIdentityR1 : g_XMIdentityR0;
        return XMVector3Normalize(XMVector3Cross(Up, Normal));
    }
}

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

void FMesh::GenerateTangentsIfMissing()
{
    using namespace DirectX;

    if (Vertices.empty() || Indices.size() < 3)
    {
        return;
    }

    const bool bAllTangentsValid = std::all_of(Vertices.begin(), Vertices.end(), [](const FVertex& Vertex)
    {
        return IsTangentValid(Vertex.Tangent);
    });

    if (bAllTangentsValid)
    {
        return;
    }

    std::vector<XMVECTOR> TangentAccum(Vertices.size(), XMVectorZero());
    std::vector<XMVECTOR> BitangentAccum(Vertices.size(), XMVectorZero());

    for (size_t i = 0; i + 2 < Indices.size(); i += 3)
    {
        const uint32_t Index0 = Indices[i];
        const uint32_t Index1 = Indices[i + 1];
        const uint32_t Index2 = Indices[i + 2];

        const XMVECTOR P0 = XMLoadFloat3(&Vertices[Index0].Position);
        const XMVECTOR P1 = XMLoadFloat3(&Vertices[Index1].Position);
        const XMVECTOR P2 = XMLoadFloat3(&Vertices[Index2].Position);

        const XMVECTOR UV0 = XMLoadFloat2(&Vertices[Index0].UV);
        const XMVECTOR UV1 = XMLoadFloat2(&Vertices[Index1].UV);
        const XMVECTOR UV2 = XMLoadFloat2(&Vertices[Index2].UV);

        const XMVECTOR Edge1 = XMVectorSubtract(P1, P0);
        const XMVECTOR Edge2 = XMVectorSubtract(P2, P0);
        const XMVECTOR DeltaUV1 = XMVectorSubtract(UV1, UV0);
        const XMVECTOR DeltaUV2 = XMVectorSubtract(UV2, UV0);

        const float Determinant = XMVectorGetX(DeltaUV1) * XMVectorGetY(DeltaUV2) - XMVectorGetY(DeltaUV1) * XMVectorGetX(DeltaUV2);
        if (std::abs(Determinant) < 1e-8f)
        {
            continue;
        }

        const float InvDet = 1.0f / Determinant;
        const XMVECTOR Tangent = XMVectorScale(
            XMVectorSubtract(XMVectorScale(Edge1, XMVectorGetY(DeltaUV2)), XMVectorScale(Edge2, XMVectorGetY(DeltaUV1))), InvDet);
        const XMVECTOR Bitangent = XMVectorScale(
            XMVectorSubtract(XMVectorScale(Edge2, XMVectorGetX(DeltaUV1)), XMVectorScale(Edge1, XMVectorGetX(DeltaUV2))), InvDet);

        TangentAccum[Index0] = XMVectorAdd(TangentAccum[Index0], Tangent);
        TangentAccum[Index1] = XMVectorAdd(TangentAccum[Index1], Tangent);
        TangentAccum[Index2] = XMVectorAdd(TangentAccum[Index2], Tangent);

        BitangentAccum[Index0] = XMVectorAdd(BitangentAccum[Index0], Bitangent);
        BitangentAccum[Index1] = XMVectorAdd(BitangentAccum[Index1], Bitangent);
        BitangentAccum[Index2] = XMVectorAdd(BitangentAccum[Index2], Bitangent);
    }

    for (size_t i = 0; i < Vertices.size(); ++i)
    {
        XMVECTOR Normal = XMLoadFloat3(&Vertices[i].Normal);
        if (XMVector3LessOrEqual(XMVector3LengthSq(Normal), XMVectorReplicate(1e-8f)))
        {
            Normal = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        }
        Normal = XMVector3Normalize(Normal);

        XMVECTOR Tangent = TangentAccum[i];
        XMVECTOR Bitangent = BitangentAccum[i];

        if (XMVector3LessOrEqual(XMVector3LengthSq(Tangent), XMVectorReplicate(1e-8f)) ||
            XMVector3LessOrEqual(XMVector3LengthSq(Bitangent), XMVectorReplicate(1e-8f)))
        {
            Tangent = BuildOrthonormalTangent(Normal);
            Bitangent = XMVector3Cross(Normal, Tangent);
            XMStoreFloat4(&Vertices[i].Tangent, XMVectorSetW(Tangent, 1.0f));
            continue;
        }

        Tangent = XMVector3Normalize(XMVectorSubtract(Tangent, XMVectorScale(Normal, XMVectorGetX(XMVector3Dot(Normal, Tangent)))));
        Bitangent = XMVector3Normalize(Bitangent);

        const float Handedness = XMVectorGetX(XMVector3Dot(XMVector3Cross(Normal, Tangent), Bitangent)) < 0.0f ? -1.0f : 1.0f;
        XMStoreFloat4(&Vertices[i].Tangent, XMVectorSetW(Tangent, Handedness));
    }
}
