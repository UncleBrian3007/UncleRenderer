#include "PositionQemReducer.h"
#include "../Core/Logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

#if __has_include("meshoptimizer.h")
#include "meshoptimizer.h"
#define WITH_MESHOPTIMIZER 1
#else
#define WITH_MESHOPTIMIZER 0
#endif

namespace
{
    std::string BuildPrimitiveLogPrefix(size_t PrimitiveIndex)
    {
        std::ostringstream Stream;
        Stream << "ClusterDAG Primitive[" << PrimitiveIndex << "]";
        return Stream.str();
    }

    void LogPrimitiveInfo(size_t PrimitiveIndex, const std::string& Message)
    {
        LogInfo(BuildPrimitiveLogPrefix(PrimitiveIndex) + ": " + Message);
    }

    void LogPrimitiveWarning(size_t PrimitiveIndex, const std::string& Message)
    {
        LogWarning(BuildPrimitiveLogPrefix(PrimitiveIndex) + ": " + Message);
    }

#ifndef CLUSTER_DAG_BUILD_LOGGING
#define CLUSTER_DAG_BUILD_LOGGING 1
#endif

#if CLUSTER_DAG_BUILD_LOGGING
#define POSITION_QEM_LOG_INFO(PrimitiveIndex, StreamExpression) do { std::ostringstream Stream; Stream << StreamExpression; LogPrimitiveInfo((PrimitiveIndex), Stream.str()); } while(false)
#define POSITION_QEM_LOG_WARNING(PrimitiveIndex, StreamExpression) do { std::ostringstream Stream; Stream << StreamExpression; LogPrimitiveWarning((PrimitiveIndex), Stream.str()); } while(false)
#else
#define POSITION_QEM_LOG_INFO(PrimitiveIndex, StreamExpression) do { (void)(PrimitiveIndex); } while(false)
#define POSITION_QEM_LOG_WARNING(PrimitiveIndex, StreamExpression) do { (void)(PrimitiveIndex); } while(false)
#endif

    constexpr double GDegenerateAreaEpsilon = 1e-16;
    constexpr double GFlipDotEpsilon = 1e-8;
    constexpr double GLockPenalty = 1e8;
    constexpr double GExternalPenalty = GLockPenalty;

    struct FPositionKey
    {
        uint32_t X = 0;
        uint32_t Y = 0;
        uint32_t Z = 0;

        bool operator==(const FPositionKey& Other) const
        {
            return X == Other.X && Y == Other.Y && Z == Other.Z;
        }
    };

    struct FPositionKeyHasher
    {
        size_t operator()(const FPositionKey& Key) const
        {
            size_t Hash = static_cast<size_t>(Key.X);
            Hash ^= static_cast<size_t>(Key.Y) + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
            Hash ^= static_cast<size_t>(Key.Z) + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
            return Hash;
        }
    };

    struct FNodeEdgeKey
    {
        uint32_t A = GClusterDAGInvalidIndex;
        uint32_t B = GClusterDAGInvalidIndex;

        bool operator==(const FNodeEdgeKey& Other) const
        {
            return A == Other.A && B == Other.B;
        }
    };

    struct FNodeEdgeKeyHasher
    {
        size_t operator()(const FNodeEdgeKey& Key) const
        {
            size_t Hash = static_cast<size_t>(Key.A);
            Hash ^= static_cast<size_t>(Key.B) + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
            return Hash;
        }
    };

    struct FOutputVertexKey
    {
        uint32_t Bits[16] = {};

        bool operator==(const FOutputVertexKey& Other) const
        {
            return std::memcmp(Bits, Other.Bits, sizeof(Bits)) == 0;
        }
    };

    struct FOutputVertexKeyHasher
    {
        size_t operator()(const FOutputVertexKey& Key) const
        {
            size_t Hash = 0;
            for (uint32_t Value : Key.Bits)
            {
                Hash ^= static_cast<size_t>(Value) + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
            }
            return Hash;
        }
    };

    struct FSymmetricQuadric
    {
        double M[10] = {};

        void AddPlane(const FFloat3& Normal, float D, double Weight)
        {
            const double A = static_cast<double>(Normal.x);
            const double B = static_cast<double>(Normal.y);
            const double C = static_cast<double>(Normal.z);
            const double PlaneD = static_cast<double>(D);
            const std::array<double, 4> Plane = { A, B, C, PlaneD };
            static constexpr int IndexLut[4][4] =
            {
                { 0, 1, 2, 3 },
                { 1, 4, 5, 6 },
                { 2, 5, 7, 8 },
                { 3, 6, 8, 9 }
            };

            for (int Row = 0; Row < 4; ++Row)
            {
                for (int Col = Row; Col < 4; ++Col)
                {
                    M[IndexLut[Row][Col]] += Plane[Row] * Plane[Col] * Weight;
                }
            }
        }

        FSymmetricQuadric& operator+=(const FSymmetricQuadric& Other)
        {
            for (size_t Index = 0; Index < 10; ++Index)
            {
                M[Index] += Other.M[Index];
            }
            return *this;
        }

        double Evaluate(const FFloat3& Position) const
        {
            const double X = static_cast<double>(Position.x);
            const double Y = static_cast<double>(Position.y);
            const double Z = static_cast<double>(Position.z);
            return
                M[0] * X * X +
                2.0 * M[1] * X * Y +
                2.0 * M[2] * X * Z +
                2.0 * M[3] * X +
                M[4] * Y * Y +
                2.0 * M[5] * Y * Z +
                2.0 * M[6] * Y +
                M[7] * Z * Z +
                2.0 * M[8] * Z +
                M[9];
        }
    };

    struct FQemCandidate
    {
        uint32_t NodeA = GClusterDAGInvalidIndex;
        uint32_t NodeB = GClusterDAGInvalidIndex;
        FFloat3 Position{ 0.0f, 0.0f, 0.0f };
        double Error = 0.0;
        double GeometricError = 0.0;
    };

    enum class EQemCollapseRejectReason
    {
        None,
        InvalidNode,
        Degenerate,
        NormalFlip
    };

    FFloat3 operator+(const FFloat3& A, const FFloat3& B)
    {
        return { A.x + B.x, A.y + B.y, A.z + B.z };
    }

    FFloat3 operator-(const FFloat3& A, const FFloat3& B)
    {
        return { A.x - B.x, A.y - B.y, A.z - B.z };
    }

    FFloat3 operator*(const FFloat3& A, float Scale)
    {
        return { A.x * Scale, A.y * Scale, A.z * Scale };
    }

    float Dot3(const FFloat3& A, const FFloat3& B)
    {
        return A.x * B.x + A.y * B.y + A.z * B.z;
    }

    FFloat3 Cross3(const FFloat3& A, const FFloat3& B)
    {
        return
        {
            A.y * B.z - A.z * B.y,
            A.z * B.x - A.x * B.z,
            A.x * B.y - A.y * B.x
        };
    }

    float LengthSquared3(const FFloat3& Value)
    {
        return Dot3(Value, Value);
    }

    float Length3(const FFloat3& Value)
    {
        return std::sqrt(LengthSquared3(Value));
    }

    FFloat3 Normalize3(const FFloat3& Value, const FFloat3& Fallback = FFloat3(0.0f, 0.0f, 1.0f))
    {
        const float Length = Length3(Value);
        if (Length <= 1e-20f)
        {
            return Fallback;
        }

        return Value * (1.0f / Length);
    }

    FPositionKey MakePositionKey(const FFloat3& Position)
    {
        FPositionKey Key;
        std::memcpy(&Key.X, &Position.x, sizeof(uint32_t));
        std::memcpy(&Key.Y, &Position.y, sizeof(uint32_t));
        std::memcpy(&Key.Z, &Position.z, sizeof(uint32_t));
        return Key;
    }

    FNodeEdgeKey MakeNodeEdgeKey(uint32_t A, uint32_t B)
    {
        if (B < A)
        {
            std::swap(A, B);
        }

        FNodeEdgeKey Key;
        Key.A = A;
        Key.B = B;
        return Key;
    }

    bool IsExternalScratchEdge(uint32_t IncidentTriangleCount)
    {
        return IncidentTriangleCount == 1u;
    }

    bool IsNonManifoldScratchEdge(uint32_t IncidentTriangleCount)
    {
        return IncidentTriangleCount > 2u;
    }

    void EnsureVertexStreamSize(FBuilderVertexStreams& Streams)
    {
        const size_t VertexCount = Streams.Positions.size();
        if (Streams.Normals.size() != VertexCount)
        {
            Streams.Normals.resize(VertexCount, FFloat3(0.0f, 0.0f, 1.0f));
        }
        if (Streams.UVs.size() != VertexCount)
        {
            Streams.UVs.resize(VertexCount, FFloat2(0.0f, 0.0f));
        }
        if (Streams.Tangents.size() != VertexCount)
        {
            Streams.Tangents.resize(VertexCount, FFloat4(1.0f, 0.0f, 0.0f, 1.0f));
        }
        if (Streams.Colors.size() != VertexCount)
        {
            Streams.Colors.resize(VertexCount, FFloat4(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

#if WITH_MESHOPTIMIZER
    template <typename T>
    void RemapBuilderStream(std::vector<T>& Stream, const std::vector<unsigned int>& Remap)
    {
        if (!Stream.empty())
        {
            meshopt_remapVertexBuffer(Stream.data(), Stream.data(), Stream.size(), sizeof(T), Remap.data());
        }
    }

    void RemapByteStream(std::vector<unsigned char>& Stream, const std::vector<unsigned int>& Remap)
    {
        if (!Stream.empty())
        {
            meshopt_remapVertexBuffer(Stream.data(), Stream.data(), Stream.size(), sizeof(unsigned char), Remap.data());
        }
    }

    template <typename T>
    void ResizeBuilderStream(std::vector<T>& Stream, size_t VertexCount)
    {
        Stream.resize(VertexCount);
    }

    bool PredictMeshletCount(
        const FBuilderVertexStreams& Streams,
        const std::vector<uint32_t>& Indices,
        uint32_t MaxClusterVertices,
        uint32_t MaxClusterTriangles,
        float ConeWeight,
        uint32_t& OutMeshletCount)
    {
        OutMeshletCount = 0;
        if (Streams.Positions.empty() || Indices.size() < 3)
        {
            return false;
        }

        const size_t MaxMeshlets = meshopt_buildMeshletsBound(Indices.size(), MaxClusterVertices, MaxClusterTriangles);
        std::vector<meshopt_Meshlet> Meshlets(MaxMeshlets);
        std::vector<unsigned int> MeshletVertices(MaxMeshlets * MaxClusterVertices);
        std::vector<unsigned char> MeshletTriangles(MaxMeshlets * MaxClusterTriangles * 3);

        const size_t MeshletCount = meshopt_buildMeshlets(
            Meshlets.data(),
            MeshletVertices.data(),
            MeshletTriangles.data(),
            Indices.data(),
            Indices.size(),
            &Streams.Positions[0].x,
            Streams.Positions.size(),
            sizeof(FFloat3),
            MaxClusterVertices,
            MaxClusterTriangles,
            ConeWeight);

        if (MeshletCount == 0)
        {
            return false;
        }

        OutMeshletCount = static_cast<uint32_t>(MeshletCount);
        return true;
    }

    bool PredictMeshletCount(
        const FBuilderVertexStreams& Streams,
        const std::vector<uint32_t>& Indices,
        const FPositionQemReducerInput& Input,
        uint32_t& OutMeshletCount)
    {
        return PredictMeshletCount(Streams, Indices, Input.MaxClusterVertices, Input.MaxClusterTriangles, Input.ConeWeight, OutMeshletCount);
    }

    bool PredictMeshletCount(
        const FBuilderVertexStreams& Streams,
        const std::vector<uint32_t>& Indices,
        const FMeshoptScratchReducerInput& Input,
        uint32_t& OutMeshletCount)
    {
        return PredictMeshletCount(Streams, Indices, Input.MaxClusterVertices, Input.MaxClusterTriangles, Input.ConeWeight, OutMeshletCount);
    }
#endif

    void BuildOutputVertexKey(
        const FFloat3& Position,
        const FFloat3& Normal,
        const FFloat2& UV,
        const FFloat4& Tangent,
        const FFloat4& Color,
        FOutputVertexKey& OutKey)
    {
        std::memcpy(&OutKey.Bits[0], &Position.x, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[1], &Position.y, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[2], &Position.z, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[3], &Normal.x, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[4], &Normal.y, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[5], &Normal.z, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[6], &UV.x, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[7], &UV.y, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[8], &Tangent.x, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[9], &Tangent.y, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[10], &Tangent.z, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[11], &Tangent.w, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[12], &Color.x, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[13], &Color.y, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[14], &Color.z, sizeof(uint32_t));
        std::memcpy(&OutKey.Bits[15], &Color.w, sizeof(uint32_t));
    }

    void GetSourceVertexAttributes(
        const FClusterDAG& Dag,
        uint32_t SourceVertexIndex,
        FFloat3& OutNormal,
        FFloat2& OutUV,
        FFloat4& OutTangent,
        FFloat4& OutColor)
    {
        OutNormal = SourceVertexIndex < Dag.Normals.size() ? Dag.Normals[SourceVertexIndex] : FFloat3(0.0f, 0.0f, 1.0f);
        OutUV = SourceVertexIndex < Dag.UVs.size() ? Dag.UVs[SourceVertexIndex] : FFloat2(0.0f, 0.0f);
        OutTangent = SourceVertexIndex < Dag.Tangents.size() ? Dag.Tangents[SourceVertexIndex] : FFloat4(1.0f, 0.0f, 0.0f, 1.0f);
        OutColor = SourceVertexIndex < Dag.Colors.size() ? Dag.Colors[SourceVertexIndex] : FFloat4(1.0f, 1.0f, 1.0f, 1.0f);

        OutNormal = Normalize3(OutNormal);
        const FFloat3 TangentAxis = Normalize3(FFloat3(OutTangent.x, OutTangent.y, OutTangent.z), FFloat3(1.0f, 0.0f, 0.0f));
        OutTangent = FFloat4(TangentAxis.x, TangentAxis.y, TangentAxis.z, OutTangent.w >= 0.0f ? 1.0f : -1.0f);
    }
}

bool CompactAndOptimizeBuilderGeometry(
    FBuilderVertexStreams& Streams,
    std::vector<uint32_t>& Indices,
    std::vector<unsigned char>* InOutVertexLocks)
{
    EnsureVertexStreamSize(Streams);
    if (Streams.Positions.empty() || Indices.size() < 3)
    {
        return false;
    }

    if (InOutVertexLocks != nullptr && InOutVertexLocks->size() != Streams.Positions.size())
    {
        InOutVertexLocks->resize(Streams.Positions.size(), static_cast<unsigned char>(0));
    }

#if WITH_MESHOPTIMIZER
    std::vector<meshopt_Stream> MeshoptStreams;
    MeshoptStreams.push_back({ Streams.Positions.data(), sizeof(FFloat3), sizeof(FFloat3) });
    MeshoptStreams.push_back({ Streams.Normals.data(), sizeof(FFloat3), sizeof(FFloat3) });
    MeshoptStreams.push_back({ Streams.UVs.data(), sizeof(FFloat2), sizeof(FFloat2) });
    MeshoptStreams.push_back({ Streams.Tangents.data(), sizeof(FFloat4), sizeof(FFloat4) });
    MeshoptStreams.push_back({ Streams.Colors.data(), sizeof(FFloat4), sizeof(FFloat4) });

    std::vector<unsigned int> Remap(Streams.Positions.size());
    const size_t UniqueVertexCount = meshopt_generateVertexRemapMulti(
        Remap.data(),
        Indices.data(),
        Indices.size(),
        Streams.Positions.size(),
        MeshoptStreams.data(),
        MeshoptStreams.size());

    meshopt_remapIndexBuffer(Indices.data(), Indices.data(), Indices.size(), Remap.data());
    RemapBuilderStream(Streams.Positions, Remap);
    RemapBuilderStream(Streams.Normals, Remap);
    RemapBuilderStream(Streams.UVs, Remap);
    RemapBuilderStream(Streams.Tangents, Remap);
    RemapBuilderStream(Streams.Colors, Remap);
    if (InOutVertexLocks != nullptr)
    {
        RemapByteStream(*InOutVertexLocks, Remap);
        ResizeBuilderStream(*InOutVertexLocks, UniqueVertexCount);
    }
    ResizeBuilderStream(Streams.Positions, UniqueVertexCount);
    ResizeBuilderStream(Streams.Normals, UniqueVertexCount);
    ResizeBuilderStream(Streams.UVs, UniqueVertexCount);
    ResizeBuilderStream(Streams.Tangents, UniqueVertexCount);
    ResizeBuilderStream(Streams.Colors, UniqueVertexCount);

    std::vector<uint32_t> CacheOptimizedIndices(Indices.size());
    meshopt_optimizeVertexCache(CacheOptimizedIndices.data(), Indices.data(), Indices.size(), Streams.Positions.size());
    Indices.swap(CacheOptimizedIndices);

    std::vector<unsigned int> FetchRemap(Streams.Positions.size());
    const size_t OptimizedVertexCount = meshopt_optimizeVertexFetchRemap(
        FetchRemap.data(),
        Indices.data(),
        Indices.size(),
        Streams.Positions.size());

    meshopt_remapIndexBuffer(Indices.data(), Indices.data(), Indices.size(), FetchRemap.data());
    RemapBuilderStream(Streams.Positions, FetchRemap);
    RemapBuilderStream(Streams.Normals, FetchRemap);
    RemapBuilderStream(Streams.UVs, FetchRemap);
    RemapBuilderStream(Streams.Tangents, FetchRemap);
    RemapBuilderStream(Streams.Colors, FetchRemap);
    if (InOutVertexLocks != nullptr)
    {
        RemapByteStream(*InOutVertexLocks, FetchRemap);
        ResizeBuilderStream(*InOutVertexLocks, OptimizedVertexCount);
    }
    ResizeBuilderStream(Streams.Positions, OptimizedVertexCount);
    ResizeBuilderStream(Streams.Normals, OptimizedVertexCount);
    ResizeBuilderStream(Streams.UVs, OptimizedVertexCount);
    ResizeBuilderStream(Streams.Tangents, OptimizedVertexCount);
    ResizeBuilderStream(Streams.Colors, OptimizedVertexCount);
#else
    (void)InOutVertexLocks;
#endif

    return !Streams.Positions.empty() && Indices.size() >= 3;
}

namespace
{
    bool TriangleUsesNode(const FScratchTriangle& Triangle, uint32_t PositionNodeIndex)
    {
        return Triangle.PositionNodeIndices[0] == PositionNodeIndex
            || Triangle.PositionNodeIndices[1] == PositionNodeIndex
            || Triangle.PositionNodeIndices[2] == PositionNodeIndex;
    }

    bool TriangleUsesEitherNode(const FScratchTriangle& Triangle, uint32_t NodeA, uint32_t NodeB)
    {
        return TriangleUsesNode(Triangle, NodeA) || TriangleUsesNode(Triangle, NodeB);
    }
}

bool BuildMergedClusterScratch(
    size_t PrimitiveIndex,
    uint32_t Level,
    size_t GroupOrdinal,
    const FClusterDAG& Dag,
    const std::vector<uint32_t>& ChildClusters,
    FMergedClusterScratch& OutScratch)
{
    OutScratch = {};
    if (ChildClusters.empty())
    {
        POSITION_QEM_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch skipped" << ", reason=empty_child_clusters");
        return false;
    }

    uint32_t InputTriangleCount = 0;
    uint32_t InvalidChildClusterCount = 0;
    std::unordered_map<FPositionKey, uint32_t, FPositionKeyHasher> PositionNodeLookup;
    PositionNodeLookup.reserve(ChildClusters.size() * 64);
    std::unordered_map<FNodeEdgeKey, uint32_t, FNodeEdgeKeyHasher> EdgeCounts;
    EdgeCounts.reserve(ChildClusters.size() * 128);

    for (uint32_t ChildClusterIndex : ChildClusters)
    {
        if (ChildClusterIndex >= Dag.Clusters.size())
        {
            ++InvalidChildClusterCount;
            continue;
        }

        const FCluster& Cluster = Dag.Clusters[ChildClusterIndex];
        InputTriangleCount += Cluster.TriangleCount;
        for (uint32_t Triangle = 0; Triangle < Cluster.TriangleCount; ++Triangle)
        {
            const uint32_t TriangleOffset = Cluster.TriangleOffset + Triangle * 3;
            const uint32_t VertexOffset = Cluster.VertexOffset;
            if (TriangleOffset + 2 >= Dag.TriangleIndices.size())
            {
                continue;
            }

            FScratchTriangle ScratchTriangle;
            ScratchTriangle.SourceChildClusterIndex = ChildClusterIndex;

            for (uint32_t CornerOrdinal = 0; CornerOrdinal < 3; ++CornerOrdinal)
            {
                const uint32_t ClusterLocalVertexIndex = Dag.TriangleIndices[TriangleOffset + CornerOrdinal];
                const uint32_t ClusterVertexIndex = VertexOffset + ClusterLocalVertexIndex;
                if (ClusterVertexIndex >= Dag.ClusterVertices.size())
                {
                    POSITION_QEM_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", reason=cluster_vertex_oob" << ", childClusterIndex=" << ChildClusterIndex << ", triangle=" << Triangle << ", corner=" << CornerOrdinal << ", clusterVertexIndex=" << ClusterVertexIndex << ", clusterVertexCount=" << Dag.ClusterVertices.size());
                    return false;
                }

                const uint32_t SourceVertexIndex = Dag.ClusterVertices[ClusterVertexIndex];
                if (SourceVertexIndex >= Dag.Positions.size())
                {
                    POSITION_QEM_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", reason=source_vertex_oob" << ", childClusterIndex=" << ChildClusterIndex << ", triangle=" << Triangle << ", corner=" << CornerOrdinal << ", sourceVertexIndex=" << SourceVertexIndex << ", sourceVertexCount=" << Dag.Positions.size());
                    return false;
                }

                const FPositionKey PositionKey = MakePositionKey(Dag.Positions[SourceVertexIndex]);
                auto PositionIt = PositionNodeLookup.find(PositionKey);
                uint32_t PositionNodeIndex = GClusterDAGInvalidIndex;
                if (PositionIt == PositionNodeLookup.end())
                {
                    PositionNodeIndex = static_cast<uint32_t>(OutScratch.PositionNodes.size());
                    PositionNodeLookup.emplace(PositionKey, PositionNodeIndex);

                    FScratchPositionNode Node;
                    Node.Position = Dag.Positions[SourceVertexIndex];
                    OutScratch.PositionNodes.push_back(Node);
                }
                else
                {
                    PositionNodeIndex = PositionIt->second;
                }

                FScratchCorner Corner;
                Corner.SourceVertexIndex = SourceVertexIndex;
                Corner.PositionNodeIndex = PositionNodeIndex;
                Corner.SourceChildClusterIndex = ChildClusterIndex;

                ScratchTriangle.CornerIndices[CornerOrdinal] = static_cast<uint32_t>(OutScratch.Corners.size());
                ScratchTriangle.PositionNodeIndices[CornerOrdinal] = PositionNodeIndex;
                OutScratch.Corners.push_back(Corner);
            }

            if (ScratchTriangle.PositionNodeIndices[0] == ScratchTriangle.PositionNodeIndices[1]
                || ScratchTriangle.PositionNodeIndices[1] == ScratchTriangle.PositionNodeIndices[2]
                || ScratchTriangle.PositionNodeIndices[2] == ScratchTriangle.PositionNodeIndices[0])
            {
                continue;
            }

            OutScratch.Triangles.push_back(ScratchTriangle);
            ++OutScratch.ActiveTriangleCount;

            ++EdgeCounts[MakeNodeEdgeKey(ScratchTriangle.PositionNodeIndices[0], ScratchTriangle.PositionNodeIndices[1])];
            ++EdgeCounts[MakeNodeEdgeKey(ScratchTriangle.PositionNodeIndices[1], ScratchTriangle.PositionNodeIndices[2])];
            ++EdgeCounts[MakeNodeEdgeKey(ScratchTriangle.PositionNodeIndices[2], ScratchTriangle.PositionNodeIndices[0])];
        }
    }

    OutScratch.Edges.reserve(EdgeCounts.size());
    for (const auto& Pair : EdgeCounts)
    {
        FScratchEdge Edge;
        Edge.PositionNodeA = Pair.first.A;
        Edge.PositionNodeB = Pair.first.B;
        Edge.IncidentTriangleCount = Pair.second;
        Edge.bExternal = IsExternalScratchEdge(Pair.second);
        OutScratch.Edges.push_back(Edge);

        if (IsNonManifoldScratchEdge(Pair.second))
        {
            ++OutScratch.NonManifoldEdgeCount;
        }

        if (Edge.bExternal)
        {
            ++OutScratch.ExternalEdgeCount;
            if (Edge.PositionNodeA < OutScratch.PositionNodes.size())
            {
                OutScratch.PositionNodes[Edge.PositionNodeA].bLocked = true;
            }
            if (Edge.PositionNodeB < OutScratch.PositionNodes.size())
            {
                OutScratch.PositionNodes[Edge.PositionNodeB].bLocked = true;
            }
        }
    }

    for (const FScratchPositionNode& PositionNode : OutScratch.PositionNodes)
    {
        if (PositionNode.bLocked)
        {
            ++OutScratch.LockedPositionCount;
        }
    }

    const bool bValidScratch = OutScratch.IsValid();
    if (!bValidScratch)
    {
        POSITION_QEM_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", reason=invalid_output" << ", childClusters=" << ChildClusters.size() << ", invalidChildClusters=" << InvalidChildClusterCount << ", inputTriangles=" << InputTriangleCount << ", corners=" << OutScratch.Corners.size() << ", positions=" << OutScratch.PositionNodes.size() << ", triangles=" << OutScratch.Triangles.size() << ", activeTriangles=" << OutScratch.ActiveTriangleCount << ", edges=" << OutScratch.Edges.size() << ", externalEdges=" << OutScratch.ExternalEdgeCount << ", nonManifoldEdges=" << OutScratch.NonManifoldEdgeCount << ", lockedPositions=" << OutScratch.LockedPositionCount);
        return false;
    }

    POSITION_QEM_LOG_INFO(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch" << ", childClusters=" << ChildClusters.size() << ", invalidChildClusters=" << InvalidChildClusterCount << ", inputTriangles=" << InputTriangleCount << ", corners=" << OutScratch.Corners.size() << ", positions=" << OutScratch.PositionNodes.size() << ", triangles=" << OutScratch.Triangles.size() << ", activeTriangles=" << OutScratch.ActiveTriangleCount << ", edges=" << OutScratch.Edges.size() << ", externalEdges=" << OutScratch.ExternalEdgeCount << ", nonManifoldEdges=" << OutScratch.NonManifoldEdgeCount << ", lockedPositions=" << OutScratch.LockedPositionCount);
    return true;
}

bool EmitMergedClusterGeometry(
    const FClusterDAG& Dag,
    const FMergedClusterScratch& Scratch,
    FBuilderVertexStreams& OutStreams,
    std::vector<uint32_t>& OutIndices,
    std::vector<unsigned char>* OutVertexLocks)
{
    OutStreams = {};
    OutIndices.clear();
    if (OutVertexLocks != nullptr)
    {
        OutVertexLocks->clear();
    }

    if (!Scratch.IsValid())
    {
        return false;
    }

    std::unordered_map<FOutputVertexKey, uint32_t, FOutputVertexKeyHasher> VertexLookup;
    VertexLookup.reserve(Scratch.ActiveTriangleCount * 3);

    for (const FScratchTriangle& Triangle : Scratch.Triangles)
    {
        if (Triangle.bDeleted)
        {
            continue;
        }

        const uint32_t N0 = Triangle.PositionNodeIndices[0];
        const uint32_t N1 = Triangle.PositionNodeIndices[1];
        const uint32_t N2 = Triangle.PositionNodeIndices[2];
        if (N0 >= Scratch.PositionNodes.size() || N1 >= Scratch.PositionNodes.size() || N2 >= Scratch.PositionNodes.size())
        {
            continue;
        }

        if (N0 == N1 || N1 == N2 || N2 == N0)
        {
            continue;
        }

        const FFloat3& P0 = Scratch.PositionNodes[N0].Position;
        const FFloat3& P1 = Scratch.PositionNodes[N1].Position;
        const FFloat3& P2 = Scratch.PositionNodes[N2].Position;
        if (LengthSquared3(Cross3(P1 - P0, P2 - P0)) <= GDegenerateAreaEpsilon)
        {
            continue;
        }

        const size_t TriangleBaseIndex = OutIndices.size();
        for (uint32_t CornerOrdinal = 0; CornerOrdinal < 3; ++CornerOrdinal)
        {
            const uint32_t CornerIndex = Triangle.CornerIndices[CornerOrdinal];
            if (CornerIndex >= Scratch.Corners.size())
            {
                return false;
            }

            const FScratchCorner& Corner = Scratch.Corners[CornerIndex];
            const uint32_t PositionNodeIndex = Triangle.PositionNodeIndices[CornerOrdinal];
            if (Corner.SourceVertexIndex >= Dag.Positions.size() || PositionNodeIndex >= Scratch.PositionNodes.size())
            {
                return false;
            }

            const FFloat3 FinalPosition = Scratch.PositionNodes[PositionNodeIndex].Position;
            FFloat3 Normal;
            FFloat2 UV;
            FFloat4 Tangent;
            FFloat4 Color;
            GetSourceVertexAttributes(Dag, Corner.SourceVertexIndex, Normal, UV, Tangent, Color);

            FOutputVertexKey VertexKey;
            BuildOutputVertexKey(FinalPosition, Normal, UV, Tangent, Color, VertexKey);

            auto VertexIt = VertexLookup.find(VertexKey);
            uint32_t VertexIndex = GClusterDAGInvalidIndex;
            if (VertexIt == VertexLookup.end())
            {
                VertexIndex = static_cast<uint32_t>(OutStreams.Positions.size());
                VertexLookup.emplace(VertexKey, VertexIndex);
                OutStreams.Positions.push_back(FinalPosition);
                OutStreams.Normals.push_back(Normal);
                OutStreams.UVs.push_back(UV);
                OutStreams.Tangents.push_back(Tangent);
                OutStreams.Colors.push_back(Color);
                if (OutVertexLocks != nullptr)
                {
                    OutVertexLocks->push_back(Scratch.PositionNodes[PositionNodeIndex].bLocked ? static_cast<unsigned char>(1) : static_cast<unsigned char>(0));
                }
            }
            else
            {
                VertexIndex = VertexIt->second;
            }

            OutIndices.push_back(VertexIndex);
        }

        if (OutIndices[TriangleBaseIndex + 0] == OutIndices[TriangleBaseIndex + 1]
            || OutIndices[TriangleBaseIndex + 1] == OutIndices[TriangleBaseIndex + 2]
            || OutIndices[TriangleBaseIndex + 2] == OutIndices[TriangleBaseIndex + 0])
        {
            OutIndices.resize(TriangleBaseIndex);
        }
    }

    if (OutStreams.Positions.empty() || OutIndices.size() < 3)
    {
        OutStreams = {};
        OutIndices.clear();
        if (OutVertexLocks != nullptr)
        {
            OutVertexLocks->clear();
        }
        return false;
    }

    return CompactAndOptimizeBuilderGeometry(OutStreams, OutIndices, OutVertexLocks);
}

namespace
{
    bool SolveOptimalPosition(
        const FSymmetricQuadric& Quadric,
        const FFloat3& EndpointA,
        const FFloat3& EndpointB,
        FFloat3& OutPosition,
        double& OutError)
    {
        const double A00 = Quadric.M[0];
        const double A01 = Quadric.M[1];
        const double A02 = Quadric.M[2];
        const double A11 = Quadric.M[4];
        const double A12 = Quadric.M[5];
        const double A22 = Quadric.M[7];
        const double B0 = -Quadric.M[3];
        const double B1 = -Quadric.M[6];
        const double B2 = -Quadric.M[8];

        const double Determinant =
            A00 * (A11 * A22 - A12 * A12) -
            A01 * (A01 * A22 - A12 * A02) +
            A02 * (A01 * A12 - A11 * A02);

        FFloat3 BestPosition = EndpointA;
        double BestError = Quadric.Evaluate(EndpointA);

        const FFloat3 Midpoint = (EndpointA + EndpointB) * 0.5f;
        const double MidpointError = Quadric.Evaluate(Midpoint);
        if (MidpointError < BestError)
        {
            BestPosition = Midpoint;
            BestError = MidpointError;
        }

        const double EndpointBError = Quadric.Evaluate(EndpointB);
        if (EndpointBError < BestError)
        {
            BestPosition = EndpointB;
            BestError = EndpointBError;
        }

        if (std::abs(Determinant) > 1e-12)
        {
            const double InvDet = 1.0 / Determinant;

            const double I00 = (A11 * A22 - A12 * A12) * InvDet;
            const double I01 = (A02 * A12 - A01 * A22) * InvDet;
            const double I02 = (A01 * A12 - A02 * A11) * InvDet;
            const double I11 = (A00 * A22 - A02 * A02) * InvDet;
            const double I12 = (A02 * A01 - A00 * A12) * InvDet;
            const double I22 = (A00 * A11 - A01 * A01) * InvDet;

            const FFloat3 SolvedPosition =
            {
                static_cast<float>(I00 * B0 + I01 * B1 + I02 * B2),
                static_cast<float>(I01 * B0 + I11 * B1 + I12 * B2),
                static_cast<float>(I02 * B0 + I12 * B1 + I22 * B2)
            };

            if (std::isfinite(SolvedPosition.x) && std::isfinite(SolvedPosition.y) && std::isfinite(SolvedPosition.z))
            {
                const double SolvedError = Quadric.Evaluate(SolvedPosition);
                if (SolvedError < BestError)
                {
                    BestPosition = SolvedPosition;
                    BestError = SolvedError;
                }
            }
        }

        OutPosition = BestPosition;
        OutError = BestError;
        return std::isfinite(BestError);
    }

    void BuildDynamicQuadricsAndEdges(
        const FMergedClusterScratch& Scratch,
        std::vector<FSymmetricQuadric>& OutQuadrics,
        std::vector<FScratchEdge>& OutEdges)
    {
        OutQuadrics.assign(Scratch.PositionNodes.size(), {});
        std::unordered_map<FNodeEdgeKey, uint32_t, FNodeEdgeKeyHasher> EdgeCounts;
        EdgeCounts.reserve(Scratch.ActiveTriangleCount * 3);

        for (const FScratchTriangle& Triangle : Scratch.Triangles)
        {
            if (Triangle.bDeleted)
            {
                continue;
            }

            const uint32_t N0 = Triangle.PositionNodeIndices[0];
            const uint32_t N1 = Triangle.PositionNodeIndices[1];
            const uint32_t N2 = Triangle.PositionNodeIndices[2];
            if (N0 >= Scratch.PositionNodes.size() || N1 >= Scratch.PositionNodes.size() || N2 >= Scratch.PositionNodes.size())
            {
                continue;
            }

            const FFloat3& P0 = Scratch.PositionNodes[N0].Position;
            const FFloat3& P1 = Scratch.PositionNodes[N1].Position;
            const FFloat3& P2 = Scratch.PositionNodes[N2].Position;
            const FFloat3 NormalVector = Cross3(P1 - P0, P2 - P0);
            const double DoubleArea = static_cast<double>(Length3(NormalVector));
            if (DoubleArea > GDegenerateAreaEpsilon)
            {
                const FFloat3 PlaneNormal = Normalize3(NormalVector);
                const float PlaneD = -Dot3(PlaneNormal, P0);
                const double Weight = DoubleArea * 0.5;

                OutQuadrics[N0].AddPlane(PlaneNormal, PlaneD, Weight);
                OutQuadrics[N1].AddPlane(PlaneNormal, PlaneD, Weight);
                OutQuadrics[N2].AddPlane(PlaneNormal, PlaneD, Weight);
            }

            if (N0 != N1)
            {
                ++EdgeCounts[MakeNodeEdgeKey(N0, N1)];
            }
            if (N1 != N2)
            {
                ++EdgeCounts[MakeNodeEdgeKey(N1, N2)];
            }
            if (N2 != N0)
            {
                ++EdgeCounts[MakeNodeEdgeKey(N2, N0)];
            }
        }

        OutEdges.clear();
        OutEdges.reserve(EdgeCounts.size());
        for (const auto& Pair : EdgeCounts)
        {
            FScratchEdge Edge;
            Edge.PositionNodeA = Pair.first.A;
            Edge.PositionNodeB = Pair.first.B;
            Edge.IncidentTriangleCount = Pair.second;
            Edge.bExternal = IsExternalScratchEdge(Pair.second);
            OutEdges.push_back(Edge);
        }
    }

    bool TryCollapseEdge(
        FMergedClusterScratch& Scratch,
        uint32_t NodeA,
        uint32_t NodeB,
        const FFloat3& NewPosition,
        bool bAllowNormalFlip,
        EQemCollapseRejectReason* OutRejectReason = nullptr)
    {
        if (OutRejectReason != nullptr)
        {
            *OutRejectReason = EQemCollapseRejectReason::None;
        }

        if (NodeA == NodeB
            || NodeA >= Scratch.PositionNodes.size()
            || NodeB >= Scratch.PositionNodes.size()
            || Scratch.PositionNodes[NodeA].bDeleted
            || Scratch.PositionNodes[NodeB].bDeleted)
        {
            if (OutRejectReason != nullptr)
            {
                *OutRejectReason = EQemCollapseRejectReason::InvalidNode;
            }
            return false;
        }

        for (const FScratchTriangle& Triangle : Scratch.Triangles)
        {
            if (Triangle.bDeleted || !TriangleUsesEitherNode(Triangle, NodeA, NodeB))
            {
                continue;
            }

            const FFloat3 OldPositions[3] =
            {
                Scratch.PositionNodes[Triangle.PositionNodeIndices[0]].Position,
                Scratch.PositionNodes[Triangle.PositionNodeIndices[1]].Position,
                Scratch.PositionNodes[Triangle.PositionNodeIndices[2]].Position
            };

            uint32_t NewNodeIndices[3] =
            {
                Triangle.PositionNodeIndices[0],
                Triangle.PositionNodeIndices[1],
                Triangle.PositionNodeIndices[2]
            };

            for (uint32_t& NodeIndex : NewNodeIndices)
            {
                if (NodeIndex == NodeB)
                {
                    NodeIndex = NodeA;
                }
            }

            if (NewNodeIndices[0] == NewNodeIndices[1]
                || NewNodeIndices[1] == NewNodeIndices[2]
                || NewNodeIndices[2] == NewNodeIndices[0])
            {
                continue;
            }

            FFloat3 NewPositions[3] =
            {
                NewNodeIndices[0] == NodeA ? NewPosition : Scratch.PositionNodes[NewNodeIndices[0]].Position,
                NewNodeIndices[1] == NodeA ? NewPosition : Scratch.PositionNodes[NewNodeIndices[1]].Position,
                NewNodeIndices[2] == NodeA ? NewPosition : Scratch.PositionNodes[NewNodeIndices[2]].Position
            };

            const FFloat3 OldNormal = Cross3(OldPositions[1] - OldPositions[0], OldPositions[2] - OldPositions[0]);
            const FFloat3 NewNormal = Cross3(NewPositions[1] - NewPositions[0], NewPositions[2] - NewPositions[0]);
            const double OldAreaSquared = static_cast<double>(LengthSquared3(OldNormal));
            const double NewAreaSquared = static_cast<double>(LengthSquared3(NewNormal));

            if (NewAreaSquared <= GDegenerateAreaEpsilon)
            {
                if (OutRejectReason != nullptr)
                {
                    *OutRejectReason = EQemCollapseRejectReason::Degenerate;
                }
                return false;
            }

            if (OldAreaSquared > GDegenerateAreaEpsilon)
            {
                const double DotNormals = static_cast<double>(Dot3(OldNormal, NewNormal));
                const double RequiredDot = GFlipDotEpsilon * std::sqrt(OldAreaSquared * NewAreaSquared);
                if (DotNormals <= RequiredDot && !bAllowNormalFlip)
                {
                    if (OutRejectReason != nullptr)
                    {
                        *OutRejectReason = EQemCollapseRejectReason::NormalFlip;
                    }
                    return false;
                }
            }
        }

        Scratch.PositionNodes[NodeA].Position = NewPosition;
        Scratch.PositionNodes[NodeA].bLocked = Scratch.PositionNodes[NodeA].bLocked || Scratch.PositionNodes[NodeB].bLocked;
        Scratch.PositionNodes[NodeB].bDeleted = true;

        for (FScratchTriangle& Triangle : Scratch.Triangles)
        {
            if (Triangle.bDeleted || !TriangleUsesEitherNode(Triangle, NodeA, NodeB))
            {
                continue;
            }

            for (uint32_t& NodeIndex : Triangle.PositionNodeIndices)
            {
                if (NodeIndex == NodeB)
                {
                    NodeIndex = NodeA;
                }
            }

            if (Triangle.PositionNodeIndices[0] == Triangle.PositionNodeIndices[1]
                || Triangle.PositionNodeIndices[1] == Triangle.PositionNodeIndices[2]
                || Triangle.PositionNodeIndices[2] == Triangle.PositionNodeIndices[0])
            {
                Triangle.bDeleted = true;
                if (Scratch.ActiveTriangleCount > 0)
                {
                    --Scratch.ActiveTriangleCount;
                }
            }
        }

        return true;
    }

    bool IsValidScratchPositionTriangle(
        const std::vector<FScratchPositionNode>& PositionNodes,
        uint32_t NodeA,
        uint32_t NodeB,
        uint32_t NodeC)
    {
        if (NodeA == NodeB || NodeB == NodeC || NodeC == NodeA)
        {
            return false;
        }

        if (NodeA >= PositionNodes.size()
            || NodeB >= PositionNodes.size()
            || NodeC >= PositionNodes.size()
            || PositionNodes[NodeA].bDeleted
            || PositionNodes[NodeB].bDeleted
            || PositionNodes[NodeC].bDeleted)
        {
            return false;
        }

        const FFloat3& P0 = PositionNodes[NodeA].Position;
        const FFloat3& P1 = PositionNodes[NodeB].Position;
        const FFloat3& P2 = PositionNodes[NodeC].Position;
        return LengthSquared3(Cross3(P1 - P0, P2 - P0)) > GDegenerateAreaEpsilon;
    }

    bool BuildPositionIndexStream(
        const FMergedClusterScratch& Scratch,
        std::vector<uint32_t>& OutPositionIndices)
    {
        OutPositionIndices.clear();
        OutPositionIndices.reserve(Scratch.ActiveTriangleCount * 3);

        for (const FScratchTriangle& Triangle : Scratch.Triangles)
        {
            if (Triangle.bDeleted)
            {
                continue;
            }

            const uint32_t N0 = Triangle.PositionNodeIndices[0];
            const uint32_t N1 = Triangle.PositionNodeIndices[1];
            const uint32_t N2 = Triangle.PositionNodeIndices[2];
            if (!IsValidScratchPositionTriangle(Scratch.PositionNodes, N0, N1, N2))
            {
                continue;
            }

            OutPositionIndices.push_back(N0);
            OutPositionIndices.push_back(N1);
            OutPositionIndices.push_back(N2);
        }

        return OutPositionIndices.size() >= 3;
    }

    void RebuildScratchEdgesAndLocks(FMergedClusterScratch& Scratch)
    {
        Scratch.Edges.clear();
        Scratch.ExternalEdgeCount = 0;
        Scratch.NonManifoldEdgeCount = 0;
        Scratch.LockedPositionCount = 0;
        Scratch.ActiveTriangleCount = 0;

        std::unordered_map<FNodeEdgeKey, uint32_t, FNodeEdgeKeyHasher> EdgeCounts;
        EdgeCounts.reserve(Scratch.Triangles.size() * 3);

        for (const FScratchTriangle& Triangle : Scratch.Triangles)
        {
            if (Triangle.bDeleted)
            {
                continue;
            }

            const uint32_t N0 = Triangle.PositionNodeIndices[0];
            const uint32_t N1 = Triangle.PositionNodeIndices[1];
            const uint32_t N2 = Triangle.PositionNodeIndices[2];
            if (!IsValidScratchPositionTriangle(Scratch.PositionNodes, N0, N1, N2))
            {
                continue;
            }

            ++Scratch.ActiveTriangleCount;
            ++EdgeCounts[MakeNodeEdgeKey(N0, N1)];
            ++EdgeCounts[MakeNodeEdgeKey(N1, N2)];
            ++EdgeCounts[MakeNodeEdgeKey(N2, N0)];
        }

        Scratch.Edges.reserve(EdgeCounts.size());
        for (const auto& Pair : EdgeCounts)
        {
            FScratchEdge Edge;
            Edge.PositionNodeA = Pair.first.A;
            Edge.PositionNodeB = Pair.first.B;
            Edge.IncidentTriangleCount = Pair.second;
            Edge.bExternal = IsExternalScratchEdge(Pair.second);
            Scratch.Edges.push_back(Edge);

            if (IsNonManifoldScratchEdge(Pair.second))
            {
                ++Scratch.NonManifoldEdgeCount;
            }

            if (Edge.bExternal)
            {
                ++Scratch.ExternalEdgeCount;
                if (Edge.PositionNodeA < Scratch.PositionNodes.size())
                {
                    Scratch.PositionNodes[Edge.PositionNodeA].bLocked = true;
                }
                if (Edge.PositionNodeB < Scratch.PositionNodes.size())
                {
                    Scratch.PositionNodes[Edge.PositionNodeB].bLocked = true;
                }
            }
        }

        for (const FScratchPositionNode& PositionNode : Scratch.PositionNodes)
        {
            if (!PositionNode.bDeleted && PositionNode.bLocked)
            {
                ++Scratch.LockedPositionCount;
            }
        }
    }

    bool BuildReducedScratchFromPositionIndices(
        const FClusterDAG& Dag,
        const FMergedClusterScratch& SourceScratch,
        const std::vector<uint32_t>& PositionIndices,
        FMergedClusterScratch& OutScratch)
    {
        OutScratch = {};
        if (!SourceScratch.IsValid() || PositionIndices.size() < 3 || PositionIndices.size() % 3 != 0)
        {
            return false;
        }

        std::vector<uint32_t> FirstCornerForNode(SourceScratch.PositionNodes.size(), GClusterDAGInvalidIndex);
        for (const FScratchTriangle& Triangle : SourceScratch.Triangles)
        {
            if (Triangle.bDeleted)
            {
                continue;
            }

            for (uint32_t CornerOrdinal = 0; CornerOrdinal < 3; ++CornerOrdinal)
            {
                const uint32_t PositionNodeIndex = Triangle.PositionNodeIndices[CornerOrdinal];
                const uint32_t CornerIndex = Triangle.CornerIndices[CornerOrdinal];
                if (PositionNodeIndex >= SourceScratch.PositionNodes.size()
                    || CornerIndex >= SourceScratch.Corners.size()
                    || FirstCornerForNode[PositionNodeIndex] != GClusterDAGInvalidIndex)
                {
                    continue;
                }

                const FScratchCorner& Corner = SourceScratch.Corners[CornerIndex];
                if (Corner.SourceVertexIndex < Dag.Positions.size())
                {
                    FirstCornerForNode[PositionNodeIndex] = CornerIndex;
                }
            }
        }

        std::vector<uint32_t> PositionNodeRemap(SourceScratch.PositionNodes.size(), GClusterDAGInvalidIndex);
        auto GetOrAddPositionNode = [&](uint32_t SourceNodeIndex) -> uint32_t
        {
            if (SourceNodeIndex >= SourceScratch.PositionNodes.size() || SourceScratch.PositionNodes[SourceNodeIndex].bDeleted)
            {
                return GClusterDAGInvalidIndex;
            }

            uint32_t& RemappedIndex = PositionNodeRemap[SourceNodeIndex];
            if (RemappedIndex == GClusterDAGInvalidIndex)
            {
                RemappedIndex = static_cast<uint32_t>(OutScratch.PositionNodes.size());
                FScratchPositionNode Node = SourceScratch.PositionNodes[SourceNodeIndex];
                Node.bDeleted = false;
                OutScratch.PositionNodes.push_back(Node);
            }

            return RemappedIndex;
        };

        OutScratch.Corners.reserve(PositionIndices.size());
        OutScratch.Triangles.reserve(PositionIndices.size() / 3);

        for (size_t Index = 0; Index + 2 < PositionIndices.size(); Index += 3)
        {
            const uint32_t SourceNodes[3] =
            {
                PositionIndices[Index + 0],
                PositionIndices[Index + 1],
                PositionIndices[Index + 2]
            };

            if (!IsValidScratchPositionTriangle(SourceScratch.PositionNodes, SourceNodes[0], SourceNodes[1], SourceNodes[2]))
            {
                continue;
            }

            uint32_t SourceCorners[3] =
            {
                FirstCornerForNode[SourceNodes[0]],
                FirstCornerForNode[SourceNodes[1]],
                FirstCornerForNode[SourceNodes[2]]
            };

            if (SourceCorners[0] == GClusterDAGInvalidIndex
                || SourceCorners[1] == GClusterDAGInvalidIndex
                || SourceCorners[2] == GClusterDAGInvalidIndex)
            {
                continue;
            }

            uint32_t NewNodes[3] =
            {
                GetOrAddPositionNode(SourceNodes[0]),
                GetOrAddPositionNode(SourceNodes[1]),
                GetOrAddPositionNode(SourceNodes[2])
            };

            if (NewNodes[0] == GClusterDAGInvalidIndex
                || NewNodes[1] == GClusterDAGInvalidIndex
                || NewNodes[2] == GClusterDAGInvalidIndex
                || !IsValidScratchPositionTriangle(OutScratch.PositionNodes, NewNodes[0], NewNodes[1], NewNodes[2]))
            {
                continue;
            }

            FScratchTriangle Triangle;
            Triangle.PositionNodeIndices = { NewNodes[0], NewNodes[1], NewNodes[2] };
            Triangle.SourceChildClusterIndex = SourceScratch.Corners[SourceCorners[0]].SourceChildClusterIndex;

            for (uint32_t CornerOrdinal = 0; CornerOrdinal < 3; ++CornerOrdinal)
            {
                FScratchCorner Corner = SourceScratch.Corners[SourceCorners[CornerOrdinal]];
                Corner.PositionNodeIndex = NewNodes[CornerOrdinal];
                Triangle.CornerIndices[CornerOrdinal] = static_cast<uint32_t>(OutScratch.Corners.size());
                OutScratch.Corners.push_back(Corner);
            }

            OutScratch.Triangles.push_back(Triangle);
        }

        RebuildScratchEdgesAndLocks(OutScratch);
        return OutScratch.IsValid();
    }
}

bool ReduceMergedClusterWithMeshopt(
    const FClusterDAG& Dag,
    const FMergedClusterScratch& Scratch,
    const FMeshoptScratchReducerInput& Input,
    FMeshoptScratchReducerResult& OutResult)
{
    OutResult = {};
    if (!Scratch.IsValid())
    {
        OutResult.FailureReason = "invalid_scratch";
        return false;
    }

    const uint32_t MaxAllowedParentCount = Input.MaxAllowedParentCount > 0 ? Input.MaxAllowedParentCount : Input.DesiredParentCount;
    if (Input.DesiredParentCount == 0 || MaxAllowedParentCount == 0 || Input.TargetClusterTriangles == 0)
    {
        OutResult.FailureReason = "invalid_budget";
        return false;
    }

    const uint32_t SourceTriangleCount = Scratch.ActiveTriangleCount;
    const uint32_t TargetTriangleCount = (std::max)(1u, Input.DesiredParentCount * Input.TargetClusterTriangles);
    if (TargetTriangleCount >= SourceTriangleCount)
    {
        OutResult.FailureReason = "non_reducing_target";
        return false;
    }

    OutResult.PositionNodeCount = static_cast<uint32_t>(Scratch.PositionNodes.size());
    OutResult.LockedPositionCount = Scratch.LockedPositionCount;

#if WITH_MESHOPTIMIZER
    std::vector<uint32_t> PositionIndices;
    if (!BuildPositionIndexStream(Scratch, PositionIndices))
    {
        OutResult.FailureReason = "position_index_build_failed";
        return false;
    }

    OutResult.PositionTriangleCount = static_cast<uint32_t>(PositionIndices.size() / 3);
    if (OutResult.PositionTriangleCount >= SourceTriangleCount)
    {
        OutResult.PositionTriangleCount = SourceTriangleCount;
    }

    std::vector<FFloat3> Positions;
    Positions.reserve(Scratch.PositionNodes.size());
    std::vector<unsigned char> VertexLocks;
    VertexLocks.reserve(Scratch.PositionNodes.size());
    for (const FScratchPositionNode& Node : Scratch.PositionNodes)
    {
        Positions.push_back(Node.Position);
        VertexLocks.push_back(!Input.bRelaxLocks && Node.bLocked ? static_cast<unsigned char>(meshopt_SimplifyVertex_Lock) : static_cast<unsigned char>(0));
    }

    const size_t SourceIndexCount = PositionIndices.size();
    size_t TargetIndexCount = static_cast<size_t>(TargetTriangleCount) * 3ull;
    TargetIndexCount = (TargetIndexCount / 3) * 3;
    TargetIndexCount = (std::max)(TargetIndexCount, size_t(3));
    if (TargetIndexCount >= SourceIndexCount)
    {
        TargetIndexCount = SourceIndexCount > 3 ? SourceIndexCount - 3 : 0;
    }

    if (TargetIndexCount < 3)
    {
        OutResult.FailureReason = "invalid_target_index_count";
        return false;
    }

    std::vector<uint32_t> SimplifiedIndices(SourceIndexCount);
    const size_t SimplifiedIndexCount = meshopt_simplifyWithAttributes(
        SimplifiedIndices.data(),
        PositionIndices.data(),
        PositionIndices.size(),
        &Positions[0].x,
        Positions.size(),
        sizeof(FFloat3),
        nullptr,
        0,
        nullptr,
        0,
        VertexLocks.data(),
        TargetIndexCount,
        std::numeric_limits<float>::max(),
        Input.bRelaxLocks ? meshopt_SimplifyErrorAbsolute : (meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute),
        &OutResult.ResultError);

    OutResult.SimplifiedTriangleCount = static_cast<uint32_t>(SimplifiedIndexCount / 3);
    if (SimplifiedIndexCount < 3)
    {
        OutResult.FailureReason = "empty_simplified_output";
        return false;
    }

    if (SimplifiedIndexCount >= SourceIndexCount)
    {
        OutResult.FailureReason = "zero_progress";
        return false;
    }

    SimplifiedIndices.resize(SimplifiedIndexCount);

    FMergedClusterScratch ReducedScratch;
    if (!BuildReducedScratchFromPositionIndices(Dag, Scratch, SimplifiedIndices, ReducedScratch))
    {
        OutResult.FailureReason = "reduced_scratch_rebuild_failed";
        return false;
    }

    if (!EmitMergedClusterGeometry(Dag, ReducedScratch, OutResult.Streams, OutResult.Indices))
    {
        OutResult.FailureReason = "emit_failed";
        return false;
    }

    OutResult.OutputPositionCount = static_cast<uint32_t>(OutResult.Streams.Positions.size());
    OutResult.OutputTriangleCount = static_cast<uint32_t>(OutResult.Indices.size() / 3);

    if (!PredictMeshletCount(OutResult.Streams, OutResult.Indices, Input, OutResult.PredictedParentCount))
    {
        OutResult.FailureReason = "meshlet_prediction_failed";
        return false;
    }

    OutResult.bValidOutput =
        OutResult.OutputTriangleCount < SourceTriangleCount &&
        OutResult.PredictedParentCount > 0 &&
        OutResult.PredictedParentCount <= MaxAllowedParentCount;

    if (!OutResult.bValidOutput)
    {
        if (OutResult.OutputTriangleCount >= SourceTriangleCount)
        {
            OutResult.FailureReason = "target_triangle_budget_unmet";
        }
        else if (OutResult.PredictedParentCount == 0)
        {
            OutResult.FailureReason = "predicted_parents_zero";
        }
        else if (OutResult.PredictedParentCount > MaxAllowedParentCount)
        {
            OutResult.FailureReason = "predicted_parent_count_exceeded";
        }
    }

    return true;
#else
    (void)Dag;
    OutResult.FailureReason = "meshoptimizer_unavailable";
    return false;
#endif
}

bool ReduceMergedClusterWithPositionQem(
    const FClusterDAG& Dag,
    const FMergedClusterScratch& Scratch,
    const FPositionQemReducerInput& Input,
    FPositionQemReducerResult& OutResult)
{
    OutResult = {};
    if (!Scratch.IsValid())
    {
        OutResult.FailureReason = "invalid_scratch";
        return false;
    }

    const uint32_t MaxAllowedParentCount = Input.MaxAllowedParentCount > 0 ? Input.MaxAllowedParentCount : Input.DesiredParentCount;
    if (Input.DesiredParentCount == 0 || MaxAllowedParentCount == 0 || Input.TargetClusterTriangles == 0)
    {
        OutResult.FailureReason = "invalid_budget";
        return false;
    }

    const uint32_t SourceTriangleCount = Scratch.ActiveTriangleCount;
    const uint32_t TargetTriangleCount = (std::max)(1u, Input.DesiredParentCount * Input.TargetClusterTriangles);
    if (TargetTriangleCount >= SourceTriangleCount)
    {
        OutResult.FailureReason = "non_reducing_target";
        return false;
    }

    FMergedClusterScratch WorkingScratch = Scratch;
    double MaxAcceptedError = 0.0;

    while (WorkingScratch.ActiveTriangleCount > TargetTriangleCount)
    {
        std::vector<FSymmetricQuadric> Quadrics;
        std::vector<FScratchEdge> DynamicEdges;
        BuildDynamicQuadricsAndEdges(WorkingScratch, Quadrics, DynamicEdges);
        OutResult.LastActiveTriangleCount = WorkingScratch.ActiveTriangleCount;
        OutResult.LastTargetTriangleCount = TargetTriangleCount;
        OutResult.LastDynamicEdgeCount = static_cast<uint32_t>(DynamicEdges.size());
        OutResult.LastCandidateCount = 0;
        OutResult.LastFilteredExternalEdgeCount = 0;
        OutResult.LastFilteredInvalidEdgeCount = 0;
        OutResult.LastFilteredLockedEdgeCount = 0;
        OutResult.LastSolveFailedCount = 0;
        OutResult.LastLockedCandidateCount = 0;
        OutResult.LastPenaltyCandidateCount = 0;
        OutResult.LastRejectInvalidNodeCount = 0;
        OutResult.LastRejectDegenerateCount = 0;
        OutResult.LastRejectNormalFlipCount = 0;

        std::vector<FQemCandidate> Candidates;
        Candidates.reserve(DynamicEdges.size() * 2);
        for (const FScratchEdge& Edge : DynamicEdges)
        {
            if (Edge.bExternal && !Input.bAllowExternalPenaltyCollapses)
            {
                ++OutResult.LastFilteredExternalEdgeCount;
                continue;
            }

            if (Edge.PositionNodeA >= WorkingScratch.PositionNodes.size()
                || Edge.PositionNodeB >= WorkingScratch.PositionNodes.size()
                || WorkingScratch.PositionNodes[Edge.PositionNodeA].bDeleted
                || WorkingScratch.PositionNodes[Edge.PositionNodeB].bDeleted)
            {
                ++OutResult.LastFilteredInvalidEdgeCount;
                continue;
            }

            const FSymmetricQuadric CombinedQuadric = [&Quadrics, &Edge]()
            {
                FSymmetricQuadric Result = Quadrics[Edge.PositionNodeA];
                Result += Quadrics[Edge.PositionNodeB];
                return Result;
            }();

            const double EdgePenalty = Edge.bExternal ? GExternalPenalty : 0.0;

            auto AddCandidate = [&](
                uint32_t CandidateNodeA,
                uint32_t CandidateNodeB,
                const FFloat3& Position,
                double Penalty)
            {
                if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z))
                {
                    ++OutResult.LastSolveFailedCount;
                    return;
                }

                FQemCandidate Candidate;
                Candidate.NodeA = CandidateNodeA;
                Candidate.NodeB = CandidateNodeB;
                Candidate.Position = Position;
                Candidate.GeometricError = CombinedQuadric.Evaluate(Position);
                Candidate.Error = Candidate.GeometricError + Penalty;
                Candidates.push_back(Candidate);

                if (Penalty >= GLockPenalty)
                {
                    ++OutResult.LastLockedCandidateCount;
                }
            };

            const bool bLockedA = WorkingScratch.PositionNodes[Edge.PositionNodeA].bLocked;
            const bool bLockedB = WorkingScratch.PositionNodes[Edge.PositionNodeB].bLocked;
            const FFloat3 PositionA = WorkingScratch.PositionNodes[Edge.PositionNodeA].Position;
            const FFloat3 PositionB = WorkingScratch.PositionNodes[Edge.PositionNodeB].Position;

            if (bLockedA || bLockedB)
            {
                if (bLockedA && bLockedB)
                {
                    AddCandidate(Edge.PositionNodeA, Edge.PositionNodeB, PositionA, GLockPenalty + EdgePenalty);
                    AddCandidate(Edge.PositionNodeB, Edge.PositionNodeA, PositionB, GLockPenalty + EdgePenalty);
                }
                else
                {
                    const uint32_t LockedNode = bLockedA ? Edge.PositionNodeA : Edge.PositionNodeB;
                    const uint32_t UnlockedNode = bLockedA ? Edge.PositionNodeB : Edge.PositionNodeA;
                    const FFloat3 LockedPosition = WorkingScratch.PositionNodes[LockedNode].Position;
                    AddCandidate(LockedNode, UnlockedNode, LockedPosition, EdgePenalty);
                }
                continue;
            }

            std::vector<FFloat3> CandidatePositions;
            CandidatePositions.reserve(4);
            auto AddUniquePosition = [&CandidatePositions](const FFloat3& Position)
            {
                if (!std::isfinite(Position.x) || !std::isfinite(Position.y) || !std::isfinite(Position.z))
                {
                    return;
                }

                const FPositionKey PositionKey = MakePositionKey(Position);
                for (const FFloat3& ExistingPosition : CandidatePositions)
                {
                    if (MakePositionKey(ExistingPosition) == PositionKey)
                    {
                        return;
                    }
                }

                CandidatePositions.push_back(Position);
            };

            FFloat3 SolvedPosition;
            double SolvedError = 0.0;
            if (SolveOptimalPosition(CombinedQuadric, PositionA, PositionB, SolvedPosition, SolvedError))
            {
                AddUniquePosition(SolvedPosition);
            }
            else
            {
                ++OutResult.LastSolveFailedCount;
            }

            AddUniquePosition(PositionA);
            AddUniquePosition(PositionB);
            AddUniquePosition((PositionA + PositionB) * 0.5f);

            for (const FFloat3& Position : CandidatePositions)
            {
                AddCandidate(Edge.PositionNodeA, Edge.PositionNodeB, Position, EdgePenalty);
            }
        }

        OutResult.LastCandidateCount = static_cast<uint32_t>(Candidates.size());
        OutResult.CandidateEdgeCount += static_cast<uint32_t>(Candidates.size());
        if (Candidates.empty())
        {
            OutResult.FailureReason = "no_valid_candidates";
            break;
        }

        std::sort(
            Candidates.begin(),
            Candidates.end(),
            [](const FQemCandidate& A, const FQemCandidate& B)
            {
                return A.Error < B.Error;
            });

        auto TryCandidates = [&](bool bAllowNormalFlip) -> bool
        {
            for (const FQemCandidate& Candidate : Candidates)
            {
                EQemCollapseRejectReason RejectReason = EQemCollapseRejectReason::None;
                if (TryCollapseEdge(WorkingScratch, Candidate.NodeA, Candidate.NodeB, Candidate.Position, bAllowNormalFlip, &RejectReason))
                {
                    ++OutResult.AcceptedCollapseCount;
                    MaxAcceptedError = (std::max)(MaxAcceptedError, Candidate.GeometricError);
                    return true;
                }

                switch (RejectReason)
                {
                case EQemCollapseRejectReason::InvalidNode:
                    ++OutResult.LastRejectInvalidNodeCount;
                    break;
                case EQemCollapseRejectReason::Degenerate:
                    ++OutResult.LastRejectDegenerateCount;
                    break;
                case EQemCollapseRejectReason::NormalFlip:
                    ++OutResult.LastRejectNormalFlipCount;
                    break;
                case EQemCollapseRejectReason::None:
                default:
                    break;
                }
            }

            return false;
        };

        bool bCollapsed = TryCandidates(false);
        if (!bCollapsed && OutResult.LastRejectNormalFlipCount > 0)
        {
            OutResult.LastPenaltyCandidateCount = static_cast<uint32_t>(Candidates.size());
            bCollapsed = TryCandidates(true);
        }

        if (!bCollapsed)
        {
            OutResult.FailureReason = "all_candidates_rejected";
            break;
        }
    }

    if (!EmitMergedClusterGeometry(Dag, WorkingScratch, OutResult.Streams, OutResult.Indices))
    {
        OutResult.FailureReason = "emit_failed";
        return false;
    }

    OutResult.OutputPositionCount = static_cast<uint32_t>(OutResult.Streams.Positions.size());
    OutResult.OutputTriangleCount = static_cast<uint32_t>(OutResult.Indices.size() / 3);
    OutResult.ResultError = static_cast<float>(std::sqrt((std::max)(0.0, MaxAcceptedError)));

#if WITH_MESHOPTIMIZER
    if (!PredictMeshletCount(OutResult.Streams, OutResult.Indices, Input, OutResult.PredictedParentCount))
    {
        OutResult.FailureReason = "meshlet_prediction_failed";
        return false;
    }
#else
    OutResult.PredictedParentCount = 1;
#endif

    OutResult.bValidOutput =
        OutResult.OutputTriangleCount < SourceTriangleCount &&
        OutResult.PredictedParentCount > 0 &&
        OutResult.PredictedParentCount <= MaxAllowedParentCount;

    if (!OutResult.bValidOutput)
    {
        if (OutResult.OutputTriangleCount >= SourceTriangleCount)
        {
            OutResult.FailureReason = "target_triangle_budget_unmet";
        }
        else if (OutResult.PredictedParentCount == 0)
        {
            OutResult.FailureReason = "predicted_parents_zero";
        }
        else if (OutResult.PredictedParentCount > MaxAllowedParentCount)
        {
            OutResult.FailureReason = "predicted_parent_count_exceeded";
        }
    }

    return true;
}
