#pragma once

#include "ClusterDAG.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct FBuilderVertexStreams
{
    std::vector<FFloat3> Positions;
    std::vector<FFloat3> Normals;
    std::vector<FFloat2> UVs;
    std::vector<FFloat4> Tangents;
    std::vector<FFloat4> Colors;
};

struct FScratchCorner
{
    uint32_t SourceVertexIndex = GClusterDAGInvalidIndex;
    uint32_t PositionNodeIndex = GClusterDAGInvalidIndex;
    uint32_t SourceChildClusterIndex = GClusterDAGInvalidIndex;
};

struct FScratchTriangle
{
    std::array<uint32_t, 3> CornerIndices{ GClusterDAGInvalidIndex, GClusterDAGInvalidIndex, GClusterDAGInvalidIndex };
    std::array<uint32_t, 3> PositionNodeIndices{ GClusterDAGInvalidIndex, GClusterDAGInvalidIndex, GClusterDAGInvalidIndex };
    uint32_t SourceChildClusterIndex = GClusterDAGInvalidIndex;
    bool bDeleted = false;
};

struct FScratchPositionNode
{
    FFloat3 Position{ 0.0f, 0.0f, 0.0f };
    bool bLocked = false;
    bool bDeleted = false;
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
    uint32_t ExternalEdgeCount = 0;
    uint32_t NonManifoldEdgeCount = 0;
    uint32_t ActiveTriangleCount = 0;

    bool IsValid() const
    {
        return !Corners.empty() && !Triangles.empty() && !PositionNodes.empty() && ActiveTriangleCount > 0;
    }
};

struct FPositionQemReducerInput
{
    uint32_t DesiredParentCount = 0;
    uint32_t MaxAllowedParentCount = 0;
    uint32_t TargetClusterTriangles = 0;
    uint32_t MaxClusterVertices = 128;
    uint32_t MaxClusterTriangles = 128;
    float ConeWeight = 0.5f;
    bool bAllowExternalPenaltyCollapses = false;
};

struct FPositionQemReducerResult
{
    FBuilderVertexStreams Streams;
    std::vector<uint32_t> Indices;
    float ResultError = 0.0f;
    uint32_t PredictedParentCount = 0;
    uint32_t CandidateEdgeCount = 0;
    uint32_t AcceptedCollapseCount = 0;
    uint32_t OutputPositionCount = 0;
    uint32_t OutputTriangleCount = 0;
    uint32_t LastActiveTriangleCount = 0;
    uint32_t LastTargetTriangleCount = 0;
    uint32_t LastDynamicEdgeCount = 0;
    uint32_t LastCandidateCount = 0;
    uint32_t LastFilteredExternalEdgeCount = 0;
    uint32_t LastFilteredInvalidEdgeCount = 0;
    uint32_t LastFilteredLockedEdgeCount = 0;
    uint32_t LastSolveFailedCount = 0;
    uint32_t LastLockedCandidateCount = 0;
    uint32_t LastPenaltyCandidateCount = 0;
    uint32_t LastRejectInvalidNodeCount = 0;
    uint32_t LastRejectDegenerateCount = 0;
    uint32_t LastRejectNormalFlipCount = 0;
    bool bValidOutput = false;
    std::string FailureReason;
};

struct FMeshoptScratchReducerInput
{
    uint32_t DesiredParentCount = 0;
    uint32_t MaxAllowedParentCount = 0;
    uint32_t TargetClusterTriangles = 0;
    uint32_t MaxClusterVertices = 128;
    uint32_t MaxClusterTriangles = 128;
    float ConeWeight = 0.5f;
    bool bRelaxLocks = false;
};

struct FMeshoptScratchReducerResult
{
    FBuilderVertexStreams Streams;
    std::vector<uint32_t> Indices;
    float ResultError = 0.0f;
    uint32_t PredictedParentCount = 0;
    uint32_t PositionNodeCount = 0;
    uint32_t PositionTriangleCount = 0;
    uint32_t LockedPositionCount = 0;
    uint32_t SimplifiedTriangleCount = 0;
    uint32_t OutputPositionCount = 0;
    uint32_t OutputTriangleCount = 0;
    bool bValidOutput = false;
    std::string FailureReason;
};

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
    FMergedClusterScratch& OutScratch);

bool EmitMergedClusterGeometry(
    const FClusterDAG& Dag,
    const FMergedClusterScratch& Scratch,
    FBuilderVertexStreams& OutStreams,
    std::vector<uint32_t>& OutIndices,
    std::vector<unsigned char>* OutVertexLocks = nullptr);

bool ReduceMergedClusterWithPositionQem(
    const FClusterDAG& Dag,
    const FMergedClusterScratch& Scratch,
    const FPositionQemReducerInput& Input,
    FPositionQemReducerResult& OutResult);

bool ReduceMergedClusterWithMeshopt(
    const FClusterDAG& Dag,
    const FMergedClusterScratch& Scratch,
    const FMeshoptScratchReducerInput& Input,
    FMeshoptScratchReducerResult& OutResult);
