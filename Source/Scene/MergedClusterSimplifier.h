#pragma once

#include "ClusterDAG.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct FPositionKey
{
    uint32_t X = 0;
    uint32_t Y = 0;
    uint32_t Z = 0;

    bool operator==(const FPositionKey& Other) const
    {
        return X == Other.X && Y == Other.Y && Z == Other.Z;
    }

    bool operator<(const FPositionKey& Other) const
    {
        if (X != Other.X) return X < Other.X;
        if (Y != Other.Y) return Y < Other.Y;
        return Z < Other.Z;
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

inline FPositionKey MakePositionKey(const FFloat3& Position)
{
    static_assert(sizeof(float) == sizeof(uint32_t));
    FPositionKey Key;
    std::memcpy(&Key.X, &Position.x, sizeof(uint32_t));
    std::memcpy(&Key.Y, &Position.y, sizeof(uint32_t));
    std::memcpy(&Key.Z, &Position.z, sizeof(uint32_t));
    return Key;
}

struct FPositionEdgeKey
{
    FPositionKey A;
    FPositionKey B;

    bool operator==(const FPositionEdgeKey& Other) const
    {
        return A == Other.A && B == Other.B;
    }
};

struct FPositionEdgeKeyHasher
{
    size_t operator()(const FPositionEdgeKey& Key) const
    {
        const size_t HashA = FPositionKeyHasher{}(Key.A);
        const size_t HashB = FPositionKeyHasher{}(Key.B);
        size_t Hash = HashA;
        Hash ^= HashB + 0x9e3779b9u + (Hash << 6) + (Hash >> 2);
        return Hash;
    }
};

inline FPositionEdgeKey MakeUndirectedPositionEdgeKey(const FFloat3& A, const FFloat3& B)
{
    FPositionKey KeyA = MakePositionKey(A);
    FPositionKey KeyB = MakePositionKey(B);
    if (KeyB < KeyA)
    {
        std::swap(KeyA, KeyB);
    }

    FPositionEdgeKey EdgeKey;
    EdgeKey.A = KeyA;
    EdgeKey.B = KeyB;
    return EdgeKey;
}

struct FClusterEdgeOwnerInfo
{
    uint32_t DistinctOwnerCount = 0;
};

using FClusterEdgeOwnerMap = std::unordered_map<FPositionEdgeKey, FClusterEdgeOwnerInfo, FPositionEdgeKeyHasher>;

struct FBuilderVertexStreams
{
    std::vector<FFloat3> Positions;
    std::vector<FFloat3> Normals;
    std::vector<FFloat2> UVs;
    std::vector<FFloat4> Tangents;
    std::vector<FFloat4> Colors;
    std::vector<uint32_t> SectionIndices;
};

inline void EnsureVertexStreamSize(FBuilderVertexStreams& Streams)
{
    const size_t VertexCount = Streams.Positions.size();
    if (Streams.Normals.size() != VertexCount)
        Streams.Normals.resize(VertexCount, FFloat3(0.0f, 0.0f, 1.0f));
    if (Streams.UVs.size() != VertexCount)
        Streams.UVs.resize(VertexCount, FFloat2(0.0f, 0.0f));
    if (Streams.Tangents.size() != VertexCount)
        Streams.Tangents.resize(VertexCount, FFloat4(1.0f, 0.0f, 0.0f, 1.0f));
    if (Streams.Colors.size() != VertexCount)
        Streams.Colors.resize(VertexCount, FFloat4(1.0f, 1.0f, 1.0f, 1.0f));
    if (Streams.SectionIndices.size() != VertexCount)
        Streams.SectionIndices.resize(VertexCount, 0u);
}

struct FScratchCorner
{
    uint32_t SourceVertexIndex = GClusterDAGInvalidIndex;
    uint32_t PositionNodeIndex = GClusterDAGInvalidIndex;
    uint32_t SectionIndex = 0;
};

struct FScratchTriangle
{
    std::array<uint32_t, 3> CornerIndices{ GClusterDAGInvalidIndex, GClusterDAGInvalidIndex, GClusterDAGInvalidIndex };
    std::array<uint32_t, 3> PositionNodeIndices{ GClusterDAGInvalidIndex, GClusterDAGInvalidIndex, GClusterDAGInvalidIndex };
    uint32_t SectionIndex = 0;
};

struct FScratchPositionNode
{
    FFloat3 Position{ 0.0f, 0.0f, 0.0f };
    uint32_t FirstSectionIndex = GClusterDAGInvalidIndex;
    bool bMixedSection = false;
    bool bLocked = false;
};

struct FScratchEdge
{
    uint32_t PositionNodeA = GClusterDAGInvalidIndex;
    uint32_t PositionNodeB = GClusterDAGInvalidIndex;
    uint32_t IncidentTriangleCount = 0;
    bool bExternal = false;
};

struct FMergedClusterScratch
{
    std::vector<FScratchCorner> Corners;
    std::vector<FScratchTriangle> Triangles;
    std::vector<FScratchPositionNode> PositionNodes;
    std::vector<FScratchEdge> Edges;
    uint32_t LockedPositionCount = 0;
    uint32_t SectionBoundaryEdgeCount = 0;
    uint32_t SectionSeamPositionCount = 0;
    uint32_t ExternalEdgeCount = 0;
    uint32_t NonManifoldEdgeCount = 0;
    uint32_t ActiveTriangleCount = 0;

    bool IsValid() const
    {
        return !Corners.empty() && !Triangles.empty() && !PositionNodes.empty() && ActiveTriangleCount > 0;
    }
};

std::string BuildPrimitiveLogPrefix(size_t PrimitiveIndex);
void LogPrimitiveInfo(size_t PrimitiveIndex, const std::string& Message);
void LogPrimitiveWarning(size_t PrimitiveIndex, const std::string& Message);

#ifndef CLUSTER_DAG_BUILD_LOGGING
#define CLUSTER_DAG_BUILD_LOGGING 1
#endif

#if CLUSTER_DAG_BUILD_LOGGING
#define CLUSTER_DAG_LOG_INFO(PrimitiveIndex, StreamExpression) do { std::ostringstream Stream; Stream << StreamExpression; LogPrimitiveInfo((PrimitiveIndex), Stream.str()); } while(false)
#define CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, StreamExpression) do { std::ostringstream Stream; Stream << StreamExpression; LogPrimitiveWarning((PrimitiveIndex), Stream.str()); } while(false)
#else
#define CLUSTER_DAG_LOG_INFO(PrimitiveIndex, StreamExpression) do { (void)(PrimitiveIndex); } while(false)
#define CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, StreamExpression) do { (void)(PrimitiveIndex); } while(false)
#endif

bool CompactAndOptimizeBuilderGeometry(
    FBuilderVertexStreams& Streams,
    std::vector<uint32_t>& Indices,
    std::vector<unsigned char>* InOutVertexLocks = nullptr);

bool BuildMergedClusterScratch(
    size_t PrimitiveIndex,
    uint32_t Level,
    size_t GroupOrdinal,
    const FClusterDAG& Dag,
    const std::vector<uint32_t>& ChildClusters,
    const FClusterEdgeOwnerMap& LevelEdgeOwners,
    FMergedClusterScratch& OutScratch);

bool BuildMergedClusterGeometry(
    const FClusterDAG& Dag,
    const FMergedClusterScratch& Scratch,
    FBuilderVertexStreams& OutStreams,
    std::vector<uint32_t>& OutIndices,
    std::vector<unsigned char>* OutVertexLocks = nullptr);
