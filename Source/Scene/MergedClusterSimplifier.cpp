#include "MergedClusterSimplifier.h"
#include "../Core/Logger.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
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

static std::string FormatFloat(float Value)
{
    std::ostringstream Stream;
    Stream << std::fixed << std::setprecision(4) << Value;
    return Stream.str();
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

    struct FNodeEdgeAccum
    {
        uint32_t IncidentTriangleCount = 0;
        uint32_t FirstSectionIndex = GClusterDAGInvalidIndex;
        bool bMixedSection = false;
    };

    struct FOutputVertexKey
    {
        uint32_t Bits[17] = {};

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

    void RemapUintStream(std::vector<uint32_t>& Stream, const std::vector<unsigned int>& Remap)
    {
        if (!Stream.empty())
        {
            meshopt_remapVertexBuffer(Stream.data(), Stream.data(), Stream.size(), sizeof(uint32_t), Remap.data());
        }
    }

    template <typename T>
    void ResizeBuilderStream(std::vector<T>& Stream, size_t VertexCount)
    {
        Stream.resize(VertexCount);
    }

#endif

    void BuildOutputVertexKey(
        const FFloat3& Position,
        const FFloat3& Normal,
        const FFloat2& UV,
        const FFloat4& Tangent,
        const FFloat4& Color,
        uint32_t SectionIndex,
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
        OutKey.Bits[16] = SectionIndex;
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
    MeshoptStreams.push_back({ Streams.SectionIndices.data(), sizeof(uint32_t), sizeof(uint32_t) });

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
    RemapUintStream(Streams.SectionIndices, Remap);
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
    ResizeBuilderStream(Streams.SectionIndices, UniqueVertexCount);

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
    RemapUintStream(Streams.SectionIndices, FetchRemap);
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
    ResizeBuilderStream(Streams.SectionIndices, OptimizedVertexCount);
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
    const FClusterEdgeOwnerMap& LevelEdgeOwners,
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
    std::unordered_map<FNodeEdgeKey, FNodeEdgeAccum, FNodeEdgeKeyHasher> EdgeAccums;
    EdgeAccums.reserve(ChildClusters.size() * 128);

    auto AccumulateEdge = [&EdgeAccums](uint32_t NodeA, uint32_t NodeB, uint32_t SectionIndex)
    {
        FNodeEdgeAccum& Accum = EdgeAccums[MakeNodeEdgeKey(NodeA, NodeB)];
        ++Accum.IncidentTriangleCount;
        if (Accum.FirstSectionIndex == GClusterDAGInvalidIndex)
        {
            Accum.FirstSectionIndex = SectionIndex;
        }
        else if (Accum.FirstSectionIndex != SectionIndex)
        {
            Accum.bMixedSection = true;
        }
    };

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
                const uint32_t SectionIndex = SourceVertexIndex < Dag.VertexSectionIndices.size()
                    ? Dag.VertexSectionIndices[SourceVertexIndex]
                    : 0u;
                auto PositionIt = PositionNodeLookup.find(PositionKey);
                uint32_t PositionNodeIndex = GClusterDAGInvalidIndex;
                if (PositionIt == PositionNodeLookup.end())
                {
                    PositionNodeIndex = static_cast<uint32_t>(OutScratch.PositionNodes.size());
                    PositionNodeLookup.emplace(PositionKey, PositionNodeIndex);

                    FScratchPositionNode Node;
                    Node.Position = Dag.Positions[SourceVertexIndex];
                    Node.FirstSectionIndex = SectionIndex;
                    OutScratch.PositionNodes.push_back(Node);
                }
                else
                {
                    PositionNodeIndex = PositionIt->second;
                    FScratchPositionNode& Node = OutScratch.PositionNodes[PositionNodeIndex];
                    if (Node.FirstSectionIndex != SectionIndex)
                    {
                        Node.bMixedSection = true;
                        Node.bLocked = true;
                    }
                }

                FScratchCorner Corner;
                Corner.SourceVertexIndex = SourceVertexIndex;
                Corner.PositionNodeIndex = PositionNodeIndex;
                Corner.SectionIndex = SectionIndex;

                ScratchTriangle.CornerIndices[CornerOrdinal] = static_cast<uint32_t>(OutScratch.Corners.size());
                ScratchTriangle.PositionNodeIndices[CornerOrdinal] = PositionNodeIndex;
                OutScratch.Corners.push_back(Corner);
            }

            const uint32_t Corner0 = ScratchTriangle.CornerIndices[0];
            if (Corner0 >= OutScratch.Corners.size())
            {
                continue;
            }
            ScratchTriangle.SectionIndex = OutScratch.Corners[Corner0].SectionIndex;

            if (ScratchTriangle.PositionNodeIndices[0] == ScratchTriangle.PositionNodeIndices[1]
                || ScratchTriangle.PositionNodeIndices[1] == ScratchTriangle.PositionNodeIndices[2]
                || ScratchTriangle.PositionNodeIndices[2] == ScratchTriangle.PositionNodeIndices[0])
            {
                continue;
            }

            OutScratch.Triangles.push_back(ScratchTriangle);
            ++OutScratch.ActiveTriangleCount;

            AccumulateEdge(ScratchTriangle.PositionNodeIndices[0], ScratchTriangle.PositionNodeIndices[1], ScratchTriangle.SectionIndex);
            AccumulateEdge(ScratchTriangle.PositionNodeIndices[1], ScratchTriangle.PositionNodeIndices[2], ScratchTriangle.SectionIndex);
            AccumulateEdge(ScratchTriangle.PositionNodeIndices[2], ScratchTriangle.PositionNodeIndices[0], ScratchTriangle.SectionIndex);
        }
    }

    OutScratch.Edges.reserve(EdgeAccums.size());
    uint32_t OpenBoundaryEdgeCount = 0;
    uint32_t LockedBoundaryEdgeCount = 0;
    uint32_t MissingBoundaryEdgeCount = 0;
    auto LockEdgeEndpoints = [&OutScratch](const FScratchEdge& Edge)
    {
        if (Edge.PositionNodeA < OutScratch.PositionNodes.size())
        {
            OutScratch.PositionNodes[Edge.PositionNodeA].bLocked = true;
        }
        if (Edge.PositionNodeB < OutScratch.PositionNodes.size())
        {
            OutScratch.PositionNodes[Edge.PositionNodeB].bLocked = true;
        }
    };

    for (const auto& Pair : EdgeAccums)
    {
        FScratchEdge Edge;
        Edge.PositionNodeA = Pair.first.A;
        Edge.PositionNodeB = Pair.first.B;
        Edge.IncidentTriangleCount = Pair.second.IncidentTriangleCount;
        Edge.bExternal = IsExternalScratchEdge(Pair.second.IncidentTriangleCount);
        OutScratch.Edges.push_back(Edge);

        if (Pair.second.bMixedSection)
        {
            ++OutScratch.SectionBoundaryEdgeCount;
            LockEdgeEndpoints(Edge);
        }

        if (IsNonManifoldScratchEdge(Pair.second.IncidentTriangleCount))
        {
            ++OutScratch.NonManifoldEdgeCount;
        }

        if (Edge.bExternal)
        {
            ++OutScratch.ExternalEdgeCount;
            if (Edge.PositionNodeA >= OutScratch.PositionNodes.size()
                || Edge.PositionNodeB >= OutScratch.PositionNodes.size())
            {
                ++MissingBoundaryEdgeCount;
                LockEdgeEndpoints(Edge);
                continue;
            }

            const FPositionEdgeKey PositionEdgeKey = MakeUndirectedPositionEdgeKey(
                OutScratch.PositionNodes[Edge.PositionNodeA].Position,
                OutScratch.PositionNodes[Edge.PositionNodeB].Position);
            const auto OwnerIt = LevelEdgeOwners.find(PositionEdgeKey);
            if (OwnerIt == LevelEdgeOwners.end())
            {
                ++MissingBoundaryEdgeCount;
                LockEdgeEndpoints(Edge);
            }
            else if (OwnerIt->second.DistinctOwnerCount >= 2u)
            {
                ++LockedBoundaryEdgeCount;
                LockEdgeEndpoints(Edge);
            }
            else
            {
                ++OpenBoundaryEdgeCount;
            }
        }
    }

    for (const FScratchPositionNode& PositionNode : OutScratch.PositionNodes)
    {
        if (PositionNode.bMixedSection)
        {
            ++OutScratch.SectionSeamPositionCount;
        }
        if (PositionNode.bLocked)
        {
            ++OutScratch.LockedPositionCount;
        }
    }
    const float LockedPositionRatio = !OutScratch.PositionNodes.empty()
        ? static_cast<float>(OutScratch.LockedPositionCount) / static_cast<float>(OutScratch.PositionNodes.size())
        : 0.0f;

    const bool bValidScratch = OutScratch.IsValid();
    if (!bValidScratch)
    {
        CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", reason=invalid_output" << ", childClusters=" << ChildClusters.size() << ", invalidChildClusters=" << InvalidChildClusterCount << ", inputTriangles=" << InputTriangleCount << ", corners=" << OutScratch.Corners.size() << ", positions=" << OutScratch.PositionNodes.size() << ", triangles=" << OutScratch.Triangles.size() << ", activeTriangles=" << OutScratch.ActiveTriangleCount << ", edges=" << OutScratch.Edges.size() << ", externalEdges=" << OutScratch.ExternalEdgeCount << ", openBoundaryEdges=" << OpenBoundaryEdgeCount << ", lockedBoundaryEdges=" << LockedBoundaryEdgeCount << ", missingBoundaryEdges=" << MissingBoundaryEdgeCount << ", sectionBoundaryEdges=" << OutScratch.SectionBoundaryEdgeCount << ", sectionSeamPositions=" << OutScratch.SectionSeamPositionCount << ", nonManifoldEdges=" << OutScratch.NonManifoldEdgeCount << ", lockedPositions=" << OutScratch.LockedPositionCount << ", lockedPositionRatio=" << FormatFloat(LockedPositionRatio));
        return false;
    }

    CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch" << ", childClusters=" << ChildClusters.size() << ", invalidChildClusters=" << InvalidChildClusterCount << ", inputTriangles=" << InputTriangleCount << ", corners=" << OutScratch.Corners.size() << ", positions=" << OutScratch.PositionNodes.size() << ", triangles=" << OutScratch.Triangles.size() << ", activeTriangles=" << OutScratch.ActiveTriangleCount << ", edges=" << OutScratch.Edges.size() << ", externalEdges=" << OutScratch.ExternalEdgeCount << ", openBoundaryEdges=" << OpenBoundaryEdgeCount << ", lockedBoundaryEdges=" << LockedBoundaryEdgeCount << ", missingBoundaryEdges=" << MissingBoundaryEdgeCount << ", sectionBoundaryEdges=" << OutScratch.SectionBoundaryEdgeCount << ", sectionSeamPositions=" << OutScratch.SectionSeamPositionCount << ", nonManifoldEdges=" << OutScratch.NonManifoldEdgeCount << ", lockedPositions=" << OutScratch.LockedPositionCount << ", lockedPositionRatio=" << FormatFloat(LockedPositionRatio));
    return true;
}

bool BuildMergedClusterGeometry(
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
            BuildOutputVertexKey(FinalPosition, Normal, UV, Tangent, Color, Corner.SectionIndex, VertexKey);

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
                OutStreams.SectionIndices.push_back(Corner.SectionIndex);
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
