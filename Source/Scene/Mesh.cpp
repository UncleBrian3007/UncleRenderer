#include "Mesh.h"
#include "../Core/Logger.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <vector>

#if __has_include("meshoptimizer.h")
#include "meshoptimizer.h"
#define WITH_MESHOPTIMIZER 1
#else
#define WITH_MESHOPTIMIZER 0
#endif

namespace
{
    bool GLogMeshOptimizationStats = false;

    bool IsNormalValid(const FFloat3& Normal)
    {
        using namespace DirectX;
        const XMVECTOR NormalVec = XMLoadFloat3(&Normal);
        const float LengthSq = XMVectorGetX(XMVector3LengthSq(NormalVec));
        return LengthSq > 1e-6f;
    }

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

#if WITH_MESHOPTIMIZER
    constexpr unsigned int GMeshoptAnalyzeCacheSize = 16;
    constexpr unsigned int GMeshoptAnalyzeWarpSize = 32;
    constexpr unsigned int GMeshoptAnalyzePrimitiveGroupSize = 128;

    struct FMeshOptimizationSnapshot
    {
        size_t VertexCount = 0;
        size_t IndexCount = 0;
        size_t VertexStride = 0;
        meshopt_VertexCacheStatistics VertexCache = {};
        meshopt_VertexFetchStatistics VertexFetch = {};
        bool bValid = false;
    };

    struct FMeshOptimizationStats
    {
        FMeshOptimizationSnapshot Before;
        FMeshOptimizationSnapshot After;
    };

    struct FMeshOptimizationTotals
    {
        size_t PrimitiveCount = 0;
        uint64_t VertexCountBefore = 0;
        uint64_t VertexCountAfter = 0;
        uint64_t TriangleCountBefore = 0;
        uint64_t TriangleCountAfter = 0;
        uint64_t VertexBufferBytesBefore = 0;
        uint64_t VertexBufferBytesAfter = 0;
        uint64_t VerticesTransformedBefore = 0;
        uint64_t VerticesTransformedAfter = 0;
        uint64_t BytesFetchedBefore = 0;
        uint64_t BytesFetchedAfter = 0;
    };

    size_t GetCombinedVertexStride(const FMesh::FVertexStreams& Streams)
    {
        size_t VertexStride = 0;
        VertexStride += Streams.Positions.empty() ? 0 : sizeof(FFloat3);
        VertexStride += Streams.Normals.empty() ? 0 : sizeof(FFloat3);
        VertexStride += Streams.UVs.empty() ? 0 : sizeof(FFloat2);
        VertexStride += Streams.Tangents.empty() ? 0 : sizeof(FFloat4);
        VertexStride += Streams.Colors.empty() ? 0 : sizeof(FFloat4);
        VertexStride += Streams.Joints.empty() ? 0 : sizeof(FUInt4);
        VertexStride += Streams.Weights.empty() ? 0 : sizeof(FFloat4);
        return VertexStride;
    }

    FMeshOptimizationSnapshot AnalyzeOptimizationSnapshot(const FMesh::FVertexStreams& Streams, const std::vector<uint32_t>& Indices)
    {
        FMeshOptimizationSnapshot Snapshot;
        Snapshot.VertexCount = Streams.Positions.size();
        Snapshot.IndexCount = Indices.size();
        Snapshot.VertexStride = GetCombinedVertexStride(Streams);

        if (Snapshot.VertexCount == 0 || Snapshot.IndexCount < 3 || Snapshot.VertexStride == 0 || (Snapshot.IndexCount % 3) != 0)
        {
            return Snapshot;
        }

        Snapshot.VertexCache = meshopt_analyzeVertexCache(
            Indices.data(),
            Indices.size(),
            Snapshot.VertexCount,
            GMeshoptAnalyzeCacheSize,
            GMeshoptAnalyzeWarpSize,
            GMeshoptAnalyzePrimitiveGroupSize);
        Snapshot.VertexFetch = meshopt_analyzeVertexFetch(
            Indices.data(),
            Indices.size(),
            Snapshot.VertexCount,
            Snapshot.VertexStride);
        Snapshot.bValid = true;
        return Snapshot;
    }

    double SafeRatio(uint64_t Numerator, uint64_t Denominator)
    {
        return Denominator == 0 ? 0.0 : static_cast<double>(Numerator) / static_cast<double>(Denominator);
    }

    std::string FormatBytes(uint64_t Bytes)
    {
        std::ostringstream Stream;
        Stream << std::fixed << std::setprecision(2);
        if (Bytes >= 1024ull * 1024ull)
        {
            Stream << (static_cast<double>(Bytes) / (1024.0 * 1024.0)) << " MiB";
        }
        else if (Bytes >= 1024ull)
        {
            Stream << (static_cast<double>(Bytes) / 1024.0) << " KiB";
        }
        else
        {
            Stream << Bytes << " B";
        }
        return Stream.str();
    }

    std::string FormatOptimizationStatsMessage(const char* Prefix, const FMeshOptimizationStats& Stats)
    {
        std::ostringstream Stream;
        Stream << std::fixed << std::setprecision(2);
        Stream
            << Prefix
            << " vertices " << Stats.Before.VertexCount << " -> " << Stats.After.VertexCount
            << ", cache ACMR " << Stats.Before.VertexCache.acmr << " -> " << Stats.After.VertexCache.acmr
            << ", ATVR " << Stats.Before.VertexCache.atvr << " -> " << Stats.After.VertexCache.atvr
            << ", transforms " << Stats.Before.VertexCache.vertices_transformed << " -> " << Stats.After.VertexCache.vertices_transformed
            << ", fetch overfetch " << Stats.Before.VertexFetch.overfetch << " -> " << Stats.After.VertexFetch.overfetch
            << ", fetched " << FormatBytes(Stats.Before.VertexFetch.bytes_fetched) << " -> " << FormatBytes(Stats.After.VertexFetch.bytes_fetched);
        return Stream.str();
    }

    std::string FormatOptimizationTotalsMessage(const FMeshOptimizationTotals& Totals)
    {
        std::ostringstream Stream;
        Stream << std::fixed << std::setprecision(2);
        Stream
            << "Mesh optimization summary: primitives=" << Totals.PrimitiveCount
            << ", vertices " << Totals.VertexCountBefore << " -> " << Totals.VertexCountAfter
            << ", cache ACMR " << SafeRatio(Totals.VerticesTransformedBefore, Totals.TriangleCountBefore) << " -> " << SafeRatio(Totals.VerticesTransformedAfter, Totals.TriangleCountAfter)
            << ", ATVR " << SafeRatio(Totals.VerticesTransformedBefore, Totals.VertexCountBefore) << " -> " << SafeRatio(Totals.VerticesTransformedAfter, Totals.VertexCountAfter)
            << ", transforms " << Totals.VerticesTransformedBefore << " -> " << Totals.VerticesTransformedAfter
            << ", fetch overfetch " << SafeRatio(Totals.BytesFetchedBefore, Totals.VertexBufferBytesBefore) << " -> " << SafeRatio(Totals.BytesFetchedAfter, Totals.VertexBufferBytesAfter)
            << ", fetched " << FormatBytes(Totals.BytesFetchedBefore) << " -> " << FormatBytes(Totals.BytesFetchedAfter);
        return Stream.str();
    }

    void AccumulateOptimizationTotals(FMeshOptimizationTotals& Totals, const FMeshOptimizationStats& Stats)
    {
        if (!Stats.Before.bValid || !Stats.After.bValid)
        {
            return;
        }

        ++Totals.PrimitiveCount;
        Totals.VertexCountBefore += Stats.Before.VertexCount;
        Totals.VertexCountAfter += Stats.After.VertexCount;
        Totals.TriangleCountBefore += Stats.Before.IndexCount / 3;
        Totals.TriangleCountAfter += Stats.After.IndexCount / 3;
        Totals.VertexBufferBytesBefore += Stats.Before.VertexCount * Stats.Before.VertexStride;
        Totals.VertexBufferBytesAfter += Stats.After.VertexCount * Stats.After.VertexStride;
        Totals.VerticesTransformedBefore += Stats.Before.VertexCache.vertices_transformed;
        Totals.VerticesTransformedAfter += Stats.After.VertexCache.vertices_transformed;
        Totals.BytesFetchedBefore += Stats.Before.VertexFetch.bytes_fetched;
        Totals.BytesFetchedAfter += Stats.After.VertexFetch.bytes_fetched;
    }

    void RemapVertexStreams(FMesh::FVertexStreams& Streams, const std::vector<unsigned int>& Remap)
    {
        auto RemapStream = [&Remap](auto& Stream)
        {
            if (!Stream.empty())
            {
                meshopt_remapVertexBuffer(Stream.data(), Stream.data(), Stream.size(), sizeof(Stream[0]), Remap.data());
            }
        };

        RemapStream(Streams.Positions);
        RemapStream(Streams.Normals);
        RemapStream(Streams.UVs);
        RemapStream(Streams.Tangents);
        RemapStream(Streams.Colors);
        RemapStream(Streams.Joints);
        RemapStream(Streams.Weights);
    }

    void ResizeVertexStreams(FMesh::FVertexStreams& Streams, size_t VertexCount)
    {
        auto ResizeStream = [VertexCount](auto& Stream)
        {
            Stream.resize(VertexCount);
        };

        ResizeStream(Streams.Positions);
        ResizeStream(Streams.Normals);
        ResizeStream(Streams.UVs);
        ResizeStream(Streams.Tangents);
        ResizeStream(Streams.Colors);
        ResizeStream(Streams.Joints);
        ResizeStream(Streams.Weights);
    }

    void OptimizeVertexCacheAndFetch(FMesh::FVertexStreams& Streams, std::vector<uint32_t>& Indices)
    {
        const size_t VertexCount = Streams.Positions.size();
        if (VertexCount == 0 || Indices.size() < 3)
        {
            return;
        }

        std::vector<uint32_t> CacheOptimizedIndices(Indices.size());
        meshopt_optimizeVertexCache(CacheOptimizedIndices.data(), Indices.data(), Indices.size(), VertexCount);
        Indices.swap(CacheOptimizedIndices);

        std::vector<unsigned int> FetchRemap(VertexCount);
        const size_t OptimizedVertexCount = meshopt_optimizeVertexFetchRemap(
            FetchRemap.data(),
            Indices.data(),
            Indices.size(),
            VertexCount);

        meshopt_remapIndexBuffer(Indices.data(), Indices.data(), Indices.size(), FetchRemap.data());
        RemapVertexStreams(Streams, FetchRemap);
        ResizeVertexStreams(Streams, OptimizedVertexCount);
    }

    bool BuildMeshletGroup(
        FMesh::FVertexStreams& Streams,
        std::vector<uint32_t>& Indices,
        uint32_t IndexStart,
        uint32_t IndexCount,
        uint32_t MaxVertices,
        uint32_t MaxTriangles,
        float ConeWeight,
        FMesh::FMeshletGroup& OutGroup,
        FMeshOptimizationStats* OutOptimizationStats)
    {
        OutGroup = {};
        if (OutOptimizationStats)
        {
            *OutOptimizationStats = {};
        }

        const size_t VertexCount = Streams.Positions.size();
        if (VertexCount == 0 || IndexCount < 3 || IndexStart + IndexCount > Indices.size())
        {
            return false;
        }

        std::vector<uint32_t> LocalIndices(Indices.begin() + IndexStart, Indices.begin() + IndexStart + IndexCount);
        FMesh::FVertexStreams LocalStreams = Streams;

        auto EnsureSize = [](auto& Vec, size_t Count, const auto& DefaultValue)
        {
            if (Vec.size() != Count)
            {
                Vec.resize(Count, DefaultValue);
            }
        };

        EnsureSize(LocalStreams.Normals, VertexCount, FFloat3(0.0f, 0.0f, 1.0f));
        EnsureSize(LocalStreams.UVs, VertexCount, FFloat2(0.0f, 0.0f));
        EnsureSize(LocalStreams.Tangents, VertexCount, FFloat4(0.0f, 0.0f, 0.0f, 1.0f));
        EnsureSize(LocalStreams.Colors, VertexCount, FFloat4(1.0f, 1.0f, 1.0f, 1.0f));
        EnsureSize(LocalStreams.Joints, VertexCount, FUInt4{});
        EnsureSize(LocalStreams.Weights, VertexCount, FFloat4(0.0f, 0.0f, 0.0f, 0.0f));

        if (OutOptimizationStats)
        {
            OutOptimizationStats->Before = AnalyzeOptimizationSnapshot(LocalStreams, LocalIndices);
        }

        std::vector<meshopt_Stream> VertexStreams;
        VertexStreams.push_back({ LocalStreams.Positions.data(), sizeof(FFloat3), sizeof(FFloat3) });
        VertexStreams.push_back({ LocalStreams.Normals.data(), sizeof(FFloat3), sizeof(FFloat3) });
        VertexStreams.push_back({ LocalStreams.UVs.data(), sizeof(FFloat2), sizeof(FFloat2) });
        VertexStreams.push_back({ LocalStreams.Tangents.data(), sizeof(FFloat4), sizeof(FFloat4) });
        VertexStreams.push_back({ LocalStreams.Colors.data(), sizeof(FFloat4), sizeof(FFloat4) });
        VertexStreams.push_back({ LocalStreams.Joints.data(), sizeof(FUInt4), sizeof(FUInt4) });
        VertexStreams.push_back({ LocalStreams.Weights.data(), sizeof(FFloat4), sizeof(FFloat4) });

        std::vector<unsigned int> Remap(LocalStreams.Positions.size());
        const size_t UniqueVertexCount = meshopt_generateVertexRemapMulti(
            Remap.data(),
            LocalIndices.data(),
            LocalIndices.size(),
            LocalStreams.Positions.size(),
            VertexStreams.data(),
            VertexStreams.size());

        meshopt_remapIndexBuffer(LocalIndices.data(), LocalIndices.data(), LocalIndices.size(), Remap.data());
        RemapVertexStreams(LocalStreams, Remap);
        ResizeVertexStreams(LocalStreams, UniqueVertexCount);

        OptimizeVertexCacheAndFetch(LocalStreams, LocalIndices);

        if (OutOptimizationStats)
        {
            OutOptimizationStats->After = AnalyzeOptimizationSnapshot(LocalStreams, LocalIndices);
        }

        const bool bCanReplaceSource = IndexStart == 0 && IndexCount == Indices.size();
        _ASSERT(bCanReplaceSource);
        if (bCanReplaceSource)
        {
            Indices.swap(LocalIndices);
            Streams = LocalStreams;
        }

        const std::vector<uint32_t>& BuildIndices = bCanReplaceSource ? Indices : LocalIndices;
        const std::vector<FFloat3>& BuildPositions = bCanReplaceSource ? Streams.Positions : LocalStreams.Positions;

        const size_t MaxMeshlets = meshopt_buildMeshletsBound(IndexCount, MaxVertices, MaxTriangles);
        std::vector<meshopt_Meshlet> TempMeshlets(MaxMeshlets);
        std::vector<unsigned int> TempMeshletVertices(MaxMeshlets * MaxVertices);
        std::vector<unsigned char> TempMeshletTriangles(MaxMeshlets * MaxTriangles * 3);

        const float* Positions = &BuildPositions[0].x;
        const size_t MeshletCount = meshopt_buildMeshlets(
            TempMeshlets.data(),
            TempMeshletVertices.data(),
            TempMeshletTriangles.data(),
            BuildIndices.data(),
            IndexCount,
            Positions,
            BuildPositions.size(),
            sizeof(FFloat3),
            MaxVertices,
            MaxTriangles,
            ConeWeight);

        if (MeshletCount == 0)
        {
            return false;
        }

        TempMeshlets.resize(MeshletCount);

        size_t TotalVertexCount = 0;
        size_t TotalTriangleIndexCount = 0;
        for (const meshopt_Meshlet& Meshlet : TempMeshlets)
        {
            TotalVertexCount = std::max(TotalVertexCount, static_cast<size_t>(Meshlet.vertex_offset + Meshlet.vertex_count));
            TotalTriangleIndexCount = std::max(TotalTriangleIndexCount, static_cast<size_t>(Meshlet.triangle_offset + Meshlet.triangle_count * 3));
        }

        OutGroup.MeshletVertices.assign(TempMeshletVertices.begin(), TempMeshletVertices.begin() + TotalVertexCount);
        OutGroup.MeshletTriangles.assign(TempMeshletTriangles.begin(), TempMeshletTriangles.begin() + TotalTriangleIndexCount);

        OutGroup.Meshlets.reserve(MeshletCount);
        OutGroup.MeshletBounds.reserve(MeshletCount);

        size_t TotalTriangleCount = 0;
        for (const meshopt_Meshlet& Meshlet : TempMeshlets)
        {
            FMesh::FMeshlet OutMeshlet;
            OutMeshlet.VertexOffset = static_cast<uint32_t>(Meshlet.vertex_offset);
            OutMeshlet.TriangleOffset = static_cast<uint32_t>(Meshlet.triangle_offset);
            OutMeshlet.VertexCount = static_cast<uint32_t>(Meshlet.vertex_count);
            OutMeshlet.TriangleCount = static_cast<uint32_t>(Meshlet.triangle_count);
            OutGroup.Meshlets.push_back(OutMeshlet);
            TotalTriangleCount += Meshlet.triangle_count;
        }

        OutGroup.MeshletIndices.reserve(TotalTriangleCount * 3);

        for (FMesh::FMeshlet& Meshlet : OutGroup.Meshlets)
        {
            const uint32_t MeshletIndexOffset = static_cast<uint32_t>(OutGroup.MeshletIndices.size());
            for (uint32_t Triangle = 0; Triangle < Meshlet.TriangleCount; ++Triangle)
            {
                const uint32_t TriangleOffset = Meshlet.TriangleOffset + Triangle * 3;
                const uint8_t Index0 = OutGroup.MeshletTriangles[TriangleOffset];
                const uint8_t Index1 = OutGroup.MeshletTriangles[TriangleOffset + 1];
                const uint8_t Index2 = OutGroup.MeshletTriangles[TriangleOffset + 2];

                const uint32_t VertexBase = Meshlet.VertexOffset;
                OutGroup.MeshletIndices.push_back(OutGroup.MeshletVertices[VertexBase + Index0]);
                OutGroup.MeshletIndices.push_back(OutGroup.MeshletVertices[VertexBase + Index1]);
                OutGroup.MeshletIndices.push_back(OutGroup.MeshletVertices[VertexBase + Index2]);
            }

            Meshlet.IndexOffset = MeshletIndexOffset;
            Meshlet.IndexCount = Meshlet.TriangleCount * 3;

            const meshopt_Bounds Bounds = meshopt_computeMeshletBounds(
                &OutGroup.MeshletVertices[Meshlet.VertexOffset],
                &OutGroup.MeshletTriangles[Meshlet.TriangleOffset],
                Meshlet.TriangleCount,
                Positions,
                BuildPositions.size(),
                sizeof(FFloat3));

            FMesh::FMeshletBounds OutBounds;
            OutBounds.Center = { Bounds.center[0], Bounds.center[1], Bounds.center[2] };
            OutBounds.Radius = Bounds.radius;
            OutBounds.ConeApex = { Bounds.cone_apex[0], Bounds.cone_apex[1], Bounds.cone_apex[2] };
            OutBounds.ConeCutoff = Bounds.cone_cutoff;
            OutBounds.ConeAxis = { Bounds.cone_axis[0], Bounds.cone_axis[1], Bounds.cone_axis[2] };
            OutGroup.MeshletBounds.push_back(OutBounds);
        }

        return true;
    }
#endif
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

    FVertexStreams Streams;
    Streams.Positions = {
        { HalfSize, -HalfSize, -HalfSize }, { HalfSize, -HalfSize,  HalfSize }, { HalfSize,  HalfSize,  HalfSize }, { HalfSize,  HalfSize, -HalfSize },
        { -HalfSize, -HalfSize,  HalfSize }, { -HalfSize, -HalfSize, -HalfSize }, { -HalfSize,  HalfSize, -HalfSize }, { -HalfSize,  HalfSize,  HalfSize },
        { -HalfSize,  HalfSize, -HalfSize }, {  HalfSize,  HalfSize, -HalfSize }, {  HalfSize,  HalfSize,  HalfSize }, { -HalfSize,  HalfSize,  HalfSize },
        { -HalfSize, -HalfSize,  HalfSize }, {  HalfSize, -HalfSize,  HalfSize }, {  HalfSize, -HalfSize, -HalfSize }, { -HalfSize, -HalfSize, -HalfSize },
        { -HalfSize, -HalfSize, HalfSize }, { -HalfSize,  HalfSize, HalfSize }, {  HalfSize,  HalfSize, HalfSize }, {  HalfSize, -HalfSize, HalfSize },
        {  HalfSize, -HalfSize, -HalfSize }, {  HalfSize,  HalfSize, -HalfSize }, { -HalfSize,  HalfSize, -HalfSize }, { -HalfSize, -HalfSize, -HalfSize },
    };

    Streams.Normals = {
        { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f },
        { -1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, -1.0f }, { 0.0f, 0.0f, -1.0f },
    };

    Streams.UVs = {
        { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f },
        { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f },
        { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f },
        { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f },
        { 0.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f },
        { 0.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f },
    };

    Streams.Tangents = {
        TangentPosX, TangentPosX, TangentPosX, TangentPosX,
        TangentNegX, TangentNegX, TangentNegX, TangentNegX,
        TangentPosY, TangentPosY, TangentPosY, TangentPosY,
        TangentNegY, TangentNegY, TangentNegY, TangentNegY,
        TangentPosZ, TangentPosZ, TangentPosZ, TangentPosZ,
        TangentNegZ, TangentNegZ, TangentNegZ, TangentNegZ,
    };

    Streams.Colors.assign(24, FFloat4(1.0f, 1.0f, 1.0f, 1.0f));

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

    FPrimitive Primitive;
    Primitive.VertexStreams = std::move(Streams);
    Primitive.Indices = std::move(Indices);
    Mesh.AddPrimitive(std::move(Primitive));
    Mesh.SetMeshletIndexingAllowed(true);
    Mesh.BuildMeshlets();

    return Mesh;
}

FMesh FMesh::CreateSphere(float Radius, uint32_t SliceCount, uint32_t StackCount)
{
    FMesh Mesh;

    SliceCount = std::max(3u, SliceCount);
    StackCount = std::max(2u, StackCount);

    FVertexStreams Streams;
    const size_t VertexCapacity = static_cast<size_t>((SliceCount + 1) * (StackCount + 1));
    Streams.Positions.reserve(VertexCapacity);
    Streams.Normals.reserve(VertexCapacity);
    Streams.UVs.reserve(VertexCapacity);
    Streams.Tangents.reserve(VertexCapacity);
    Streams.Colors.reserve(VertexCapacity);

    const float TwoPi = DirectX::XM_2PI;
    const float Pi = DirectX::XM_PI;

    for (uint32_t Stack = 0; Stack <= StackCount; ++Stack)
    {
        const float V = static_cast<float>(Stack) / static_cast<float>(StackCount);
        const float Phi = V * Pi;
        const float SinPhi = std::sin(Phi);
        const float CosPhi = std::cos(Phi);

        for (uint32_t Slice = 0; Slice <= SliceCount; ++Slice)
        {
            const float U = static_cast<float>(Slice) / static_cast<float>(SliceCount);
            const float Theta = U * TwoPi;
            const float SinTheta = std::sin(Theta);
            const float CosTheta = std::cos(Theta);

            const float X = Radius * SinPhi * CosTheta;
            const float Y = Radius * CosPhi;
            const float Z = Radius * SinPhi * SinTheta;

            Streams.Positions.push_back({ X, Y, Z });

            const FFloat3 Normal = { SinPhi * CosTheta, CosPhi, SinPhi * SinTheta };
            const DirectX::XMVECTOR NormalVec = DirectX::XMLoadFloat3(&Normal);
            FFloat3 NormalNormalized;
            DirectX::XMStoreFloat3(&NormalNormalized, DirectX::XMVector3Normalize(NormalVec));
            Streams.Normals.push_back(NormalNormalized);

            Streams.UVs.push_back({ U, V });

            FFloat3 Tangent = { -SinTheta, 0.0f, CosTheta };
            if (std::abs(SinPhi) > 1e-4f)
            {
                Tangent.x *= SinPhi;
                Tangent.z *= SinPhi;
            }
            else
            {
                Tangent = { 1.0f, 0.0f, 0.0f };
            }

            const DirectX::XMVECTOR TangentVec = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&Tangent));
            FFloat4 TangentPacked;
            DirectX::XMStoreFloat4(&TangentPacked, DirectX::XMVectorSetW(TangentVec, 1.0f));
            Streams.Tangents.push_back(TangentPacked);

            Streams.Colors.push_back(FFloat4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

    std::vector<uint32_t> Indices;
    Indices.reserve(SliceCount * StackCount * 6);

    for (uint32_t Stack = 0; Stack < StackCount; ++Stack)
    {
        for (uint32_t Slice = 0; Slice < SliceCount; ++Slice)
        {
            const uint32_t A = Stack * (SliceCount + 1) + Slice;
            const uint32_t B = A + SliceCount + 1;

            Indices.push_back(A);
            Indices.push_back(B);
            Indices.push_back(A + 1);

            Indices.push_back(A + 1);
            Indices.push_back(B);
            Indices.push_back(B + 1);
        }
    }

    FPrimitive Primitive;
    Primitive.VertexStreams = std::move(Streams);
    Primitive.Indices = std::move(Indices);
    Mesh.AddPrimitive(std::move(Primitive));
    Mesh.SetMeshletIndexingAllowed(true);
    Mesh.BuildMeshlets();

    return Mesh;
}

void FMesh::GenerateNormalsIfMissing()
{
    using namespace DirectX;

    for (FPrimitive& Primitive : Primitives)
    {
        const size_t VertexCount = Primitive.VertexStreams.Positions.size();
        if (VertexCount == 0 || Primitive.Indices.size() < 3)
        {
            continue;
        }

        if (Primitive.VertexStreams.Normals.size() != VertexCount)
        {
            Primitive.VertexStreams.Normals.resize(VertexCount, FFloat3(0.0f, 0.0f, 1.0f));
        }

        const bool bAllNormalsValid = std::all_of(
            Primitive.VertexStreams.Normals.begin(),
            Primitive.VertexStreams.Normals.end(),
            [](const FFloat3& Normal) { return IsNormalValid(Normal); });

        if (bAllNormalsValid)
        {
            continue;
        }

        std::vector<XMVECTOR> NormalAccum(VertexCount, XMVectorZero());

        for (size_t i = 0; i + 2 < Primitive.Indices.size(); i += 3)
        {
            const uint32_t Index0 = Primitive.Indices[i];
            const uint32_t Index1 = Primitive.Indices[i + 1];
            const uint32_t Index2 = Primitive.Indices[i + 2];

            if (Index0 >= VertexCount || Index1 >= VertexCount || Index2 >= VertexCount)
            {
                continue;
            }

            const XMVECTOR P0 = XMLoadFloat3(&Primitive.VertexStreams.Positions[Index0]);
            const XMVECTOR P1 = XMLoadFloat3(&Primitive.VertexStreams.Positions[Index1]);
            const XMVECTOR P2 = XMLoadFloat3(&Primitive.VertexStreams.Positions[Index2]);

            const XMVECTOR Edge1 = XMVectorSubtract(P1, P0);
            const XMVECTOR Edge2 = XMVectorSubtract(P2, P0);
            const XMVECTOR FaceNormal = XMVector3Cross(Edge1, Edge2);

            NormalAccum[Index0] = XMVectorAdd(NormalAccum[Index0], FaceNormal);
            NormalAccum[Index1] = XMVectorAdd(NormalAccum[Index1], FaceNormal);
            NormalAccum[Index2] = XMVectorAdd(NormalAccum[Index2], FaceNormal);
        }

        for (size_t i = 0; i < VertexCount; ++i)
        {
            XMVECTOR Normal = NormalAccum[i];
            if (XMVector3LessOrEqual(XMVector3LengthSq(Normal), XMVectorReplicate(1e-8f)))
            {
                Normal = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            }
            Normal = XMVector3Normalize(Normal);
            XMStoreFloat3(&Primitive.VertexStreams.Normals[i], Normal);
        }
    }
}

void FMesh::GenerateTangentsIfMissing()
{
    using namespace DirectX;

    for (FPrimitive& Primitive : Primitives)
    {
        const size_t VertexCount = Primitive.VertexStreams.Positions.size();
        if (VertexCount == 0 || Primitive.Indices.size() < 3)
        {
            continue;
        }

        if (Primitive.VertexStreams.Normals.size() != VertexCount)
        {
            Primitive.VertexStreams.Normals.resize(VertexCount, FFloat3(0.0f, 0.0f, 1.0f));
        }
        if (Primitive.VertexStreams.UVs.size() != VertexCount)
        {
            Primitive.VertexStreams.UVs.resize(VertexCount, FFloat2(0.0f, 0.0f));
        }
        if (Primitive.VertexStreams.Tangents.size() != VertexCount)
        {
            Primitive.VertexStreams.Tangents.resize(VertexCount, FFloat4(0.0f, 0.0f, 0.0f, 1.0f));
        }

        const bool bAllTangentsValid = std::all_of(
            Primitive.VertexStreams.Tangents.begin(),
            Primitive.VertexStreams.Tangents.end(),
            [](const FFloat4& Tangent) { return IsTangentValid(Tangent); });

        if (bAllTangentsValid)
        {
            continue;
        }

        std::vector<XMVECTOR> TangentAccum(VertexCount, XMVectorZero());
        std::vector<XMVECTOR> BitangentAccum(VertexCount, XMVectorZero());

        for (size_t i = 0; i + 2 < Primitive.Indices.size(); i += 3)
        {
            const uint32_t Index0 = Primitive.Indices[i];
            const uint32_t Index1 = Primitive.Indices[i + 1];
            const uint32_t Index2 = Primitive.Indices[i + 2];

            if (Index0 >= VertexCount || Index1 >= VertexCount || Index2 >= VertexCount)
            {
                continue;
            }

            const XMVECTOR P0 = XMLoadFloat3(&Primitive.VertexStreams.Positions[Index0]);
            const XMVECTOR P1 = XMLoadFloat3(&Primitive.VertexStreams.Positions[Index1]);
            const XMVECTOR P2 = XMLoadFloat3(&Primitive.VertexStreams.Positions[Index2]);

            const XMVECTOR UV0 = XMLoadFloat2(&Primitive.VertexStreams.UVs[Index0]);
            const XMVECTOR UV1 = XMLoadFloat2(&Primitive.VertexStreams.UVs[Index1]);
            const XMVECTOR UV2 = XMLoadFloat2(&Primitive.VertexStreams.UVs[Index2]);

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

        for (size_t i = 0; i < VertexCount; ++i)
        {
            XMVECTOR Normal = XMLoadFloat3(&Primitive.VertexStreams.Normals[i]);
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
                XMStoreFloat4(&Primitive.VertexStreams.Tangents[i], XMVectorSetW(Tangent, 1.0f));
                continue;
            }

            Tangent = XMVector3Normalize(XMVectorSubtract(Tangent, XMVectorScale(Normal, XMVectorGetX(XMVector3Dot(Normal, Tangent)))));
            Bitangent = XMVector3Normalize(Bitangent);

            const float Handedness = XMVectorGetX(XMVector3Dot(XMVector3Cross(Normal, Tangent), Bitangent)) < 0.0f ? -1.0f : 1.0f;
            XMStoreFloat4(&Primitive.VertexStreams.Tangents[i], XMVectorSetW(Tangent, Handedness));
        }
    }
}

void FMesh::BuildMeshlets(uint32_t MaxVertices, uint32_t MaxTriangles, float ConeWeight)
{
    std::vector<size_t> PrimitiveIndices(Primitives.size());
    for (size_t Index = 0; Index < Primitives.size(); ++Index)
    {
        PrimitiveIndices[Index] = Index;
    }
    BuildMeshletGroups(PrimitiveIndices, MaxVertices, MaxTriangles, ConeWeight);
}

void FMesh::BuildMeshletGroups(const std::vector<size_t>& PrimitiveIndices, uint32_t MaxVertices, uint32_t MaxTriangles, float ConeWeight)
{
    MeshletGroups.clear();

    if (Primitives.empty() || PrimitiveIndices.empty())
    {
        return;
    }

#if WITH_MESHOPTIMIZER
    MeshletGroups.reserve(PrimitiveIndices.size());
    const bool bLogOptimizationStats = IsOptimizationStatsLoggingEnabled();
    FMeshOptimizationTotals OptimizationTotals;
    for (size_t PrimitiveIndex : PrimitiveIndices)
    {
        FMeshletGroup Group;
        if (PrimitiveIndex < Primitives.size())
        {
            FPrimitive& Primitive = Primitives[PrimitiveIndex];
            FMeshOptimizationStats OptimizationStats;
            if (!Primitive.Indices.empty() && !Primitive.VertexStreams.Positions.empty()
                && BuildMeshletGroup(Primitive.VertexStreams, Primitive.Indices, 0, static_cast<uint32_t>(Primitive.Indices.size()),
                    MaxVertices, MaxTriangles, ConeWeight, Group, bLogOptimizationStats ? &OptimizationStats : nullptr))
            {
                MeshletGroups.push_back(std::move(Group));
                if (bLogOptimizationStats)
                {
                    AccumulateOptimizationTotals(OptimizationTotals, OptimizationStats);
                    if (OptimizationStats.Before.bValid && OptimizationStats.After.bValid)
                    {
                        std::ostringstream Prefix;
                        Prefix << "Primitive[" << PrimitiveIndex << "] optimization:";
                        LogVerbose(FormatOptimizationStatsMessage(Prefix.str().c_str(), OptimizationStats));
                    }
                }
            }
            else
            {
                MeshletGroups.emplace_back();
            }
        }
        else
        {
            MeshletGroups.emplace_back();
        }
    }

    if (bLogOptimizationStats && OptimizationTotals.PrimitiveCount > 0)
    {
        LogInfo(FormatOptimizationTotalsMessage(OptimizationTotals));
    }

#endif
}

void FMesh::SetOptimizationStatsLoggingEnabled(bool bEnabled)
{
    GLogMeshOptimizationStats = bEnabled;
}

bool FMesh::IsOptimizationStatsLoggingEnabled()
{
    return GLogMeshOptimizationStats;
}

const FMesh::FMeshletGroup* FMesh::GetMeshletGroup(size_t Index) const
{
    if (Index >= MeshletGroups.size())
    {
        return nullptr;
    }
    return &MeshletGroups[Index];
}

bool FMesh::HasMeshlets() const
{
    if (!MeshletGroups.empty())
    {
        for (const FMeshletGroup& Group : MeshletGroups)
        {
            if (!Group.MeshletIndices.empty())
            {
                return true;
            }
        }
        return false;
    }
    return false;
}
