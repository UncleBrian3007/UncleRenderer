#include "MergedClusterSimplifier.h"
#include "../Core/Logger.h"

#include <algorithm>
#include <array>
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

namespace
{
    constexpr double GDegenerateAreaEpsilon = 1e-16;
    constexpr double GFlipDotEpsilon = 1e-8;
    constexpr double GLockPenalty = 1e8;
    constexpr double GExternalPenalty = GLockPenalty;

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

        OutNormal = VectorMath::Normalize3(OutNormal, FFloat3(0.0f, 0.0f, 1.0f));
        const FFloat3 TangentAxis = VectorMath::Normalize3(FFloat3(OutTangent.x, OutTangent.y, OutTangent.z), FFloat3(1.0f, 0.0f, 0.0f));
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
        CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch skipped" << ", reason=empty_child_clusters");
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

            for (uint32_t CornerOrdinal = 0; CornerOrdinal < 3; ++CornerOrdinal)
            {
                const uint32_t ClusterLocalVertexIndex = Dag.TriangleIndices[TriangleOffset + CornerOrdinal];
                const uint32_t ClusterVertexIndex = VertexOffset + ClusterLocalVertexIndex;
                if (ClusterVertexIndex >= Dag.ClusterVertices.size())
                {
                    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", reason=cluster_vertex_oob" << ", childClusterIndex=" << ChildClusterIndex << ", triangle=" << Triangle << ", corner=" << CornerOrdinal << ", clusterVertexIndex=" << ClusterVertexIndex << ", clusterVertexCount=" << Dag.ClusterVertices.size());
                    return false;
                }

                const uint32_t SourceVertexIndex = Dag.ClusterVertices[ClusterVertexIndex];
                if (SourceVertexIndex >= Dag.Positions.size())
                {
                    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", reason=source_vertex_oob" << ", childClusterIndex=" << ChildClusterIndex << ", triangle=" << Triangle << ", corner=" << CornerOrdinal << ", sourceVertexIndex=" << SourceVertexIndex << ", sourceVertexCount=" << Dag.Positions.size());
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
        CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", reason=invalid_output" << ", childClusters=" << ChildClusters.size() << ", invalidChildClusters=" << InvalidChildClusterCount << ", inputTriangles=" << InputTriangleCount << ", corners=" << OutScratch.Corners.size() << ", positions=" << OutScratch.PositionNodes.size() << ", triangles=" << OutScratch.Triangles.size() << ", activeTriangles=" << OutScratch.ActiveTriangleCount << ", edges=" << OutScratch.Edges.size() << ", externalEdges=" << OutScratch.ExternalEdgeCount << ", nonManifoldEdges=" << OutScratch.NonManifoldEdgeCount << ", lockedPositions=" << OutScratch.LockedPositionCount);
        return false;
    }

    CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch" << ", childClusters=" << ChildClusters.size() << ", invalidChildClusters=" << InvalidChildClusterCount << ", inputTriangles=" << InputTriangleCount << ", corners=" << OutScratch.Corners.size() << ", positions=" << OutScratch.PositionNodes.size() << ", triangles=" << OutScratch.Triangles.size() << ", activeTriangles=" << OutScratch.ActiveTriangleCount << ", edges=" << OutScratch.Edges.size() << ", externalEdges=" << OutScratch.ExternalEdgeCount << ", nonManifoldEdges=" << OutScratch.NonManifoldEdgeCount << ", lockedPositions=" << OutScratch.LockedPositionCount);
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
        const FFloat3 Edge01 = VectorMath::Sub3(P1, P0);
        const FFloat3 Edge02 = VectorMath::Sub3(P2, P0);
        if (VectorMath::LengthSquared3(VectorMath::Cross3(Edge01, Edge02)) <= GDegenerateAreaEpsilon)
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
            || NodeC >= PositionNodes.size())
        {
            return false;
        }

        const FFloat3& P0 = PositionNodes[NodeA].Position;
        const FFloat3& P1 = PositionNodes[NodeB].Position;
        const FFloat3& P2 = PositionNodes[NodeC].Position;
        const FFloat3 Edge01 = VectorMath::Sub3(P1, P0);
        const FFloat3 Edge02 = VectorMath::Sub3(P2, P0);
        return VectorMath::LengthSquared3(VectorMath::Cross3(Edge01, Edge02)) > GDegenerateAreaEpsilon;
    }

    bool BuildPositionIndexStream(
        const FMergedClusterScratch& Scratch,
        std::vector<uint32_t>& OutPositionIndices)
    {
        OutPositionIndices.clear();
        OutPositionIndices.reserve(Scratch.ActiveTriangleCount * 3);

        for (const FScratchTriangle& Triangle : Scratch.Triangles)
        {
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
            if (PositionNode.bLocked)
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
            if (SourceNodeIndex >= SourceScratch.PositionNodes.size())
            {
                return GClusterDAGInvalidIndex;
            }

            uint32_t& RemappedIndex = PositionNodeRemap[SourceNodeIndex];
            if (RemappedIndex == GClusterDAGInvalidIndex)
            {
                RemappedIndex = static_cast<uint32_t>(OutScratch.PositionNodes.size());
                OutScratch.PositionNodes.push_back(SourceScratch.PositionNodes[SourceNodeIndex]);
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
