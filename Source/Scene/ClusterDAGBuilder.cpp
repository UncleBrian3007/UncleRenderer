#include "ClusterDAG.h"
#include "Mesh.h"
#include "MergedClusterSimplifier.h"
#include "../Core/Logger.h"
#include "../Core/StringUtils.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if __has_include("meshoptimizer.h")
#include "meshoptimizer.h"
#define WITH_MESHOPTIMIZER 1
#else
#define WITH_MESHOPTIMIZER 0
#endif

namespace
{
    constexpr uint32_t GVmeshVersion = 15;
    constexpr uint32_t GClusterDAGBuildSemanticVersion = 3;
    constexpr size_t GAttributeFloatCount = 5;
    constexpr uint32_t GClusterDAGMinGroupSize = 8;
    constexpr uint32_t GClusterDAGMaxGroupSize = 32;

    struct FVmeshCacheHeader
    {
        char Magic[4] = { 'V', 'M', 'E', 'S' };
        uint32_t Version = GVmeshVersion;
        uint64_t SourceWriteTime = 0;
        uint64_t SourceFileSize = 0;
        uint64_t ParamsHash = 0;
        uint32_t MeshCount = 0;
    };

    struct FVmeshDagHeader
    {
        uint32_t ClusterCount = 0;
        uint32_t GroupCount = 0;
        uint32_t PositionCount = 0;
        uint32_t NormalCount = 0;
        uint32_t UVCount = 0;
        uint32_t TangentCount = 0;
        uint32_t ColorCount = 0;
        uint32_t TriangleIndexCount = 0;
        uint32_t ExternalEdgeCount = 0;
        uint32_t ClusterVertexCount = 0;
        uint32_t RuntimeGroupCount = 0;
        uint32_t RuntimeClusterCount = 0;
        uint32_t RuntimeChildRefCount = 0;
        uint32_t RuntimeDrawDataCount = 0;
        uint32_t RuntimePackedIndexCount = 0;
        uint32_t PackedPositionCount = 0;
        uint32_t PackedNormalCount = 0;
        uint32_t PackedUVCount = 0;
        uint32_t PackedTangentCount = 0;
        uint32_t PackedColorCount = 0;
        uint32_t RuntimeRootGroupIndex = GClusterDAGInvalidIndex;
        uint32_t RootGroupIndex = GClusterDAGInvalidIndex;
        FFloat4 PackedPositionOffset{ 0.0f, 0.0f, 0.0f, 0.0f };
        FFloat4 PackedPositionScale{ 1.0f, 1.0f, 1.0f, 0.0f };
        FFloat4 PackedConstantUV{ 0.0f, 0.0f, 0.0f, 0.0f };
        FFloat4 PackedConstantColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    };

    struct FBuildState
    {
        FClusterDAG Dag;
        std::vector<float> ClusterInheritedErrors;
    };

#if WITH_MESHOPTIMIZER
    struct FMeshletBuildResult
    {
        std::vector<meshopt_Meshlet> Meshlets;
        std::vector<unsigned int> MeshletVertices;
        std::vector<unsigned char> MeshletTriangles;
        size_t MeshletCount = 0;
        size_t OutputTriangleCount = 0;
        size_t MaxMeshletVertices = 0;
        size_t MaxMeshletTriangles = 0;
    };
#endif

    struct FGroupSphere
    {
        FFloat3 Center{ 0.0f, 0.0f, 0.0f };
        float Radius = 0.0f;
    };

    struct FClusterEdgeOwnerInfo
    {
        uint32_t DistinctOwnerCount = 0;
    };

    std::string FormatFloat(float Value)
    {
        std::ostringstream Stream;
        Stream << std::fixed << std::setprecision(4) << Value;
        return Stream.str();
    }

    std::string SummarizeGroupSizes(const std::vector<std::vector<uint32_t>>& ClusterGroups)
    {
        size_t MinSize = std::numeric_limits<size_t>::max();
        size_t MaxSize = 0;
        size_t TotalSize = 0;
        size_t NonEmptyCount = 0;

        for (const std::vector<uint32_t>& Group : ClusterGroups)
        {
            if (Group.empty())
            {
                continue;
            }

            MinSize = (std::min)(MinSize, Group.size());
            MaxSize = (std::max)(MaxSize, Group.size());
            TotalSize += Group.size();
            ++NonEmptyCount;
        }

        if (NonEmptyCount == 0)
        {
            return "groups=0";
        }

        std::ostringstream Stream;
        Stream
            << "groups=" << NonEmptyCount
            << ", min=" << MinSize
            << ", max=" << MaxSize
            << ", avg=" << FormatFloat(static_cast<float>(TotalSize) / static_cast<float>(NonEmptyCount));
        return Stream.str();
    }

    using FAdjacencyList = std::vector<std::unordered_map<uint32_t, uint32_t>>;

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

    template <typename T>
    bool WriteValue(std::ofstream& Stream, const T& Value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        Stream.write(reinterpret_cast<const char*>(&Value), sizeof(T));
        return Stream.good();
    }

    template <typename T>
    bool ReadValue(std::ifstream& Stream, T& Value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        Stream.read(reinterpret_cast<char*>(&Value), sizeof(T));
        return Stream.good();
    }

    template <typename T>
    bool WritePodVector(std::ofstream& Stream, const std::vector<T>& Values)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!Values.empty())
        {
            Stream.write(reinterpret_cast<const char*>(Values.data()), static_cast<std::streamsize>(Values.size() * sizeof(T)));
        }
        return Stream.good();
    }

    template <typename T>
    bool ReadPodVector(std::ifstream& Stream, std::vector<T>& Values, uint32_t Count)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        Values.resize(Count);
        if (Count > 0)
        {
            Stream.read(reinterpret_cast<char*>(Values.data()), static_cast<std::streamsize>(Values.size() * sizeof(T)));
        }
        return Stream.good();
    }

    uint32_t PackUnorm16(float Value)
    {
        const float Clamped = (std::clamp)(Value, 0.0f, 1.0f);
        return static_cast<uint32_t>(std::lround(Clamped * 65535.0f));
    }

    uint32_t PackHalf2(const FFloat2& Value)
    {
        const uint32_t X = DirectX::PackedVector::XMConvertFloatToHalf(Value.x);
        const uint32_t Y = DirectX::PackedVector::XMConvertFloatToHalf(Value.y);
        return X | (Y << 16u);
    }

    FFloat2 UnpackHalf2(uint32_t PackedValue)
    {
        return FFloat2(
            DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t>(PackedValue & 0xFFFFu)),
            DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t>(PackedValue >> 16u)));
    }

    uint32_t PackRgba8(const FFloat4& Value)
    {
        auto Quantize = [](float Component)
        {
            const float Clamped = (std::clamp)(Component, 0.0f, 1.0f);
            return static_cast<uint32_t>(std::lround(Clamped * 255.0f));
        };

        const uint32_t R = Quantize(Value.x);
        const uint32_t G = Quantize(Value.y);
        const uint32_t B = Quantize(Value.z);
        const uint32_t A = Quantize(Value.w);
        return R | (G << 8u) | (B << 16u) | (A << 24u);
    }

    FFloat4 UnpackRgba8(uint32_t PackedValue)
    {
        constexpr float Inv255 = 1.0f / 255.0f;
        return FFloat4(
            static_cast<float>(PackedValue & 0xFFu) * Inv255,
            static_cast<float>((PackedValue >> 8u) & 0xFFu) * Inv255,
            static_cast<float>((PackedValue >> 16u) & 0xFFu) * Inv255,
            static_cast<float>((PackedValue >> 24u) & 0xFFu) * Inv255);
    }

    uint32_t EncodeOctahedral16x2(const FFloat3& Value)
    {
        const DirectX::XMVECTOR Vector = DirectX::XMVector3Normalize(DirectX::XMVectorSet(Value.x, Value.y, Value.z, 0.0f));
        DirectX::XMFLOAT3 Normal = {};
        DirectX::XMStoreFloat3(&Normal, Vector);

        const float Sum = std::fabs(Normal.x) + std::fabs(Normal.y) + std::fabs(Normal.z) + 1.0e-6f;
        float EncodedX = Normal.x / Sum;
        float EncodedY = Normal.y / Sum;
        if (Normal.z < 0.0f)
        {
            const float SignX = EncodedX >= 0.0f ? 1.0f : -1.0f;
            const float SignY = EncodedY >= 0.0f ? 1.0f : -1.0f;
            const float PrevX = EncodedX;
            EncodedX = (1.0f - std::fabs(EncodedY)) * SignX;
            EncodedY = (1.0f - std::fabs(PrevX)) * SignY;
        }

        const uint32_t PackedX = PackUnorm16(EncodedX * 0.5f + 0.5f);
        const uint32_t PackedY = PackUnorm16(EncodedY * 0.5f + 0.5f);
        return PackedX | (PackedY << 16u);
    }

    uint32_t PackSnorm10(float Value)
    {
        const float Clamped = (std::clamp)(Value, -1.0f, 1.0f);
        const int32_t Signed = static_cast<int32_t>(std::lround(Clamped * 511.0f));
        return static_cast<uint32_t>(Signed) & 0x3ffu;
    }

    FFloat3 BuildOrthogonalTangent(const FFloat3& Normal)
    {
        const FFloat3 Up = std::fabs(Normal.z) < 0.999f
            ? FFloat3(0.0f, 0.0f, 1.0f)
            : FFloat3(0.0f, 1.0f, 0.0f);
        const DirectX::XMVECTOR NormalVector = DirectX::XMVector3Normalize(DirectX::XMVectorSet(Normal.x, Normal.y, Normal.z, 0.0f));
        const DirectX::XMVECTOR UpVector = DirectX::XMVectorSet(Up.x, Up.y, Up.z, 0.0f);
        DirectX::XMFLOAT3 Tangent = {};
        DirectX::XMStoreFloat3(&Tangent, DirectX::XMVector3Normalize(DirectX::XMVector3Cross(UpVector, NormalVector)));
        return Tangent;
    }

    uint32_t PackTangent(const FFloat4& Value, const FFloat3& Normal)
    {
        FFloat3 Tangent = { Value.x, Value.y, Value.z };
        const float LengthSq = Tangent.x * Tangent.x + Tangent.y * Tangent.y + Tangent.z * Tangent.z;
        if (LengthSq <= 1.0e-8f)
        {
            Tangent = BuildOrthogonalTangent(Normal);
        }
        else
        {
            const float InvLength = 1.0f / std::sqrt(LengthSq);
            Tangent.x *= InvLength;
            Tangent.y *= InvLength;
            Tangent.z *= InvLength;
        }

        const uint32_t SignBits = Value.w < 0.0f ? 1u : 0u;
        return PackSnorm10(Tangent.x)
            | (PackSnorm10(Tangent.y) << 10u)
            | (PackSnorm10(Tangent.z) << 20u)
            | (SignBits << 30u);
    }

    bool AllPackedValuesEqual(const std::vector<uint32_t>& Values)
    {
        if (Values.empty())
        {
            return true;
        }

        const uint32_t FirstValue = Values.front();
        for (size_t Index = 1; Index < Values.size(); ++Index)
        {
            if (Values[Index] != FirstValue)
            {
                return false;
            }
        }

        return true;
    }

    uint64_t HashBytes(const void* Data, size_t Size)
    {
        const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Data);
        uint64_t Hash = 1469598103934665603ull;
        for (size_t Index = 0; Index < Size; ++Index)
        {
            Hash ^= Bytes[Index];
            Hash *= 1099511628211ull;
        }
        return Hash;
    }

    bool GetFileSignature(const std::wstring& FilePath, uint64_t& OutWriteTime, uint64_t& OutFileSize)
    {
        try
        {
            if (!std::filesystem::exists(FilePath))
            {
                return false;
            }

            OutFileSize = static_cast<uint64_t>(std::filesystem::file_size(FilePath));
            OutWriteTime = static_cast<uint64_t>(std::filesystem::last_write_time(FilePath).time_since_epoch().count());
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    uint32_t AppendVertexStreams(FClusterDAG& Dag, const FBuilderVertexStreams& Streams)
    {
        const uint32_t BaseVertex = static_cast<uint32_t>(Dag.Positions.size());
        Dag.Positions.insert(Dag.Positions.end(), Streams.Positions.begin(), Streams.Positions.end());
        Dag.Normals.insert(Dag.Normals.end(), Streams.Normals.begin(), Streams.Normals.end());
        Dag.UVs.insert(Dag.UVs.end(), Streams.UVs.begin(), Streams.UVs.end());
        Dag.Tangents.insert(Dag.Tangents.end(), Streams.Tangents.begin(), Streams.Tangents.end());
        Dag.Colors.insert(Dag.Colors.end(), Streams.Colors.begin(), Streams.Colors.end());
        return BaseVertex;
    }

    FClusterBounds ToClusterBounds(const meshopt_Bounds& Bounds)
    {
        FClusterBounds Result;
        Result.Center = { Bounds.center[0], Bounds.center[1], Bounds.center[2] };
        Result.Radius = Bounds.radius;
        Result.ConeApex = { Bounds.cone_apex[0], Bounds.cone_apex[1], Bounds.cone_apex[2] };
        Result.ConeAxis = { Bounds.cone_axis[0], Bounds.cone_axis[1], Bounds.cone_axis[2] };
        Result.ConeCutoff = Bounds.cone_cutoff;
        return Result;
    }

    bool IsClusterIndexDataValid(const FClusterDAG& Dag, const FCluster& Cluster)
    {
        const size_t TriangleStart = static_cast<size_t>(Cluster.TriangleOffset);
        const size_t TriangleEnd = TriangleStart + static_cast<size_t>(Cluster.TriangleCount) * 3u;
        const size_t VertexStart = static_cast<size_t>(Cluster.VertexOffset);
        const size_t VertexEnd = VertexStart + static_cast<size_t>(Cluster.VertexCount);
        if (TriangleEnd > Dag.TriangleIndices.size() || VertexEnd > Dag.ClusterVertices.size())
        {
            return false;
        }

        for (size_t TriangleOffset = TriangleStart; TriangleOffset < TriangleEnd; ++TriangleOffset)
        {
            if (Dag.TriangleIndices[TriangleOffset] >= Cluster.VertexCount)
            {
                return false;
            }
        }

        return true;
    }

    bool ValidateClusterIndexData(const FClusterDAG& Dag)
    {
        for (const FCluster& Cluster : Dag.Clusters)
        {
            if (!IsClusterIndexDataValid(Dag, Cluster))
            {
                return false;
            }
        }

        return true;
    }

    std::vector<uint32_t> ExtractAbsoluteClusterIndices(const FClusterDAG& Dag, const FCluster& Cluster)
    {
        std::vector<uint32_t> Indices;
        if (!IsClusterIndexDataValid(Dag, Cluster))
        {
            return Indices;
        }

        Indices.reserve(static_cast<size_t>(Cluster.TriangleCount) * 3);

        for (uint32_t Triangle = 0; Triangle < Cluster.TriangleCount; ++Triangle)
        {
            const uint32_t TriangleOffset = Cluster.TriangleOffset + Triangle * 3;
            const uint32_t VertexOffset = Cluster.VertexOffset;
            Indices.push_back(Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset]]);
            Indices.push_back(Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 1]]);
            Indices.push_back(Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 2]]);
        }

        return Indices;
    }

    float ComputeMaxEdgeLength(const std::vector<FFloat3>& Positions, const std::vector<uint32_t>& AbsoluteIndices)
    {
        float MaxEdgeLengthSq = 0.0f;
        for (size_t Index = 0; Index + 2 < AbsoluteIndices.size(); Index += 3)
        {
            const uint32_t V0 = AbsoluteIndices[Index];
            const uint32_t V1 = AbsoluteIndices[Index + 1];
            const uint32_t V2 = AbsoluteIndices[Index + 2];
            if (V0 >= Positions.size() || V1 >= Positions.size() || V2 >= Positions.size())
            {
                continue;
            }

            const FFloat3& P0 = Positions[V0];
            const FFloat3& P1 = Positions[V1];
            const FFloat3& P2 = Positions[V2];

            MaxEdgeLengthSq = (std::max)(MaxEdgeLengthSq, VectorMath::DistanceSquared3(P0, P1));
            MaxEdgeLengthSq = (std::max)(MaxEdgeLengthSq, VectorMath::DistanceSquared3(P1, P2));
            MaxEdgeLengthSq = (std::max)(MaxEdgeLengthSq, VectorMath::DistanceSquared3(P2, P0));
        }

        return std::sqrt(MaxEdgeLengthSq);
    }

    uint32_t AppendCluster(
        FBuildState& State,
        const std::vector<uint32_t>& AbsoluteIndices,
        uint32_t MipLevel,
        float LODError,
        const FFloat3& LodBoundsCenter,
        float LodBoundsRadius,
        uint32_t GroupIndex,
        uint32_t GeneratingGroupIndex,
        float InheritedMaxParentError)
    {
        assert((AbsoluteIndices.size() % 3) == 0);

        FCluster Cluster;
        Cluster.VertexOffset = static_cast<uint32_t>(State.Dag.ClusterVertices.size());
        Cluster.TriangleOffset = static_cast<uint32_t>(State.Dag.TriangleIndices.size());
        Cluster.TriangleCount = static_cast<uint32_t>(AbsoluteIndices.size() / 3);
        Cluster.LODError = LODError;
        Cluster.MaxEdgeLength = ComputeMaxEdgeLength(State.Dag.Positions, AbsoluteIndices);
        Cluster.LodBoundsCenter = LodBoundsCenter;
        Cluster.LodBoundsRadius = LodBoundsRadius;
        Cluster.ExternalEdgeOffset = static_cast<uint32_t>(State.Dag.ExternalEdges.size());
        Cluster.ExternalEdgeCount = static_cast<uint32_t>(AbsoluteIndices.size());
        Cluster.GroupIndex = GroupIndex;
        Cluster.GeneratingGroupIndex = GeneratingGroupIndex;
        Cluster.MipLevel = MipLevel;

        std::unordered_map<uint32_t, uint8_t> LocalRemap;
        LocalRemap.reserve(AbsoluteIndices.size());
        for (uint32_t AbsoluteIndex : AbsoluteIndices)
        {
            auto It = LocalRemap.find(AbsoluteIndex);
            if (It == LocalRemap.end())
            {
                const uint8_t LocalIndex = static_cast<uint8_t>(LocalRemap.size());
                LocalRemap.emplace(AbsoluteIndex, LocalIndex);
                State.Dag.ClusterVertices.push_back(AbsoluteIndex);
                State.Dag.TriangleIndices.push_back(LocalIndex);
            }
            else
            {
                State.Dag.TriangleIndices.push_back(It->second);
            }
        }

        Cluster.VertexCount = static_cast<uint32_t>(LocalRemap.size());
        State.Dag.ExternalEdges.insert(State.Dag.ExternalEdges.end(), AbsoluteIndices.size(), static_cast<uint8_t>(0));

#if WITH_MESHOPTIMIZER
        if (!State.Dag.Positions.empty())
        {
            const meshopt_Bounds Bounds = meshopt_computeClusterBounds(
                AbsoluteIndices.data(),
                AbsoluteIndices.size(),
                &State.Dag.Positions[0].x,
                State.Dag.Positions.size(),
                sizeof(FFloat3));
            Cluster.Bounds = ToClusterBounds(Bounds);
        }
#endif

        const uint32_t ClusterIndex = static_cast<uint32_t>(State.Dag.Clusters.size());
        State.Dag.Clusters.push_back(Cluster);
        State.ClusterInheritedErrors.push_back(InheritedMaxParentError);
        return ClusterIndex;
    }

#if WITH_MESHOPTIMIZER
    bool GenerateMeshletsForGeometry(
        const FBuilderVertexStreams& Streams,
        const std::vector<uint32_t>& Indices,
        const FClusterDAGBuildParams& Params,
        FMeshletBuildResult& OutResult)
    {
        OutResult = {};
        if (Streams.Positions.empty() || Indices.size() < 3)
        {
            return false;
        }

        const size_t MaxMeshlets = meshopt_buildMeshletsBound(Indices.size(), Params.MaxClusterVertices, Params.MaxClusterTriangles);
        OutResult.Meshlets.resize(MaxMeshlets);
        OutResult.MeshletVertices.resize(MaxMeshlets * Params.MaxClusterVertices);
        OutResult.MeshletTriangles.resize(MaxMeshlets * Params.MaxClusterTriangles * 3);

        OutResult.MeshletCount = meshopt_buildMeshlets(
            OutResult.Meshlets.data(),
            OutResult.MeshletVertices.data(),
            OutResult.MeshletTriangles.data(),
            Indices.data(),
            Indices.size(),
            &Streams.Positions[0].x,
            Streams.Positions.size(),
            sizeof(FFloat3),
            Params.MaxClusterVertices,
            Params.MaxClusterTriangles,
            Params.ConeWeight);

        if (OutResult.MeshletCount == 0)
        {
            OutResult = {};
            return false;
        }

        for (size_t MeshletIndex = 0; MeshletIndex < OutResult.MeshletCount; ++MeshletIndex)
        {
            const meshopt_Meshlet& Meshlet = OutResult.Meshlets[MeshletIndex];
            OutResult.OutputTriangleCount += Meshlet.triangle_count;
            OutResult.MaxMeshletVertices = (std::max)(OutResult.MaxMeshletVertices, static_cast<size_t>(Meshlet.vertex_count));
            OutResult.MaxMeshletTriangles = (std::max)(OutResult.MaxMeshletTriangles, static_cast<size_t>(Meshlet.triangle_count));
        }

        return true;
    }

    bool BuildMeshletClustersForGeometry(
        size_t PrimitiveIndex,
        FBuildState& State,
        const FBuilderVertexStreams& Streams,
        const std::vector<uint32_t>& Indices,
        uint32_t BaseVertex,
        uint32_t MipLevel,
        int32_t GroupOrdinal,
        float LODError,
        const FFloat3& LodBoundsCenter,
        float LodBoundsRadius,
        uint32_t GroupIndex,
        uint32_t GeneratingGroupIndex,
        float InheritedMaxParentError,
        const FClusterDAGBuildParams& Params,
        std::vector<uint32_t>& OutClusterIndices)
    {
        OutClusterIndices.clear();
        if (Streams.Positions.empty() || Indices.size() < 3)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << MipLevel << " group " << GroupOrdinal << " BuildMeshletClustersForGeometry skipped" << ", reason=insufficient_input" << ", vertices=" << Streams.Positions.size() << ", indices=" << Indices.size());
            return false;
        }

        FMeshletBuildResult MeshletBuild;
        if (!GenerateMeshletsForGeometry(Streams, Indices, Params, MeshletBuild))
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << MipLevel << " group " << GroupOrdinal << " BuildMeshletClustersForGeometry failed" << ", meshopt_buildMeshlets returned 0" << ", maxVertices=" << Params.MaxClusterVertices << ", maxTriangles=" << Params.MaxClusterTriangles);
            return false;
        }

        OutClusterIndices.reserve(MeshletBuild.MeshletCount);
        for (size_t MeshletIndex = 0; MeshletIndex < MeshletBuild.MeshletCount; ++MeshletIndex)
        {
            const meshopt_Meshlet& Meshlet = MeshletBuild.Meshlets[MeshletIndex];
            std::vector<uint32_t> AbsoluteIndices;
            AbsoluteIndices.reserve(static_cast<size_t>(Meshlet.triangle_count) * 3);

            for (unsigned int Triangle = 0; Triangle < Meshlet.triangle_count; ++Triangle)
            {
                const unsigned int TriangleOffset = Meshlet.triangle_offset + Triangle * 3;
                const uint8_t Local0 = MeshletBuild.MeshletTriangles[TriangleOffset];
                const uint8_t Local1 = MeshletBuild.MeshletTriangles[TriangleOffset + 1];
                const uint8_t Local2 = MeshletBuild.MeshletTriangles[TriangleOffset + 2];
                const uint32_t VertexBase = Meshlet.vertex_offset;
                AbsoluteIndices.push_back(BaseVertex + MeshletBuild.MeshletVertices[VertexBase + Local0]);
                AbsoluteIndices.push_back(BaseVertex + MeshletBuild.MeshletVertices[VertexBase + Local1]);
                AbsoluteIndices.push_back(BaseVertex + MeshletBuild.MeshletVertices[VertexBase + Local2]);
            }

            OutClusterIndices.push_back(AppendCluster(
                State,
                AbsoluteIndices,
                MipLevel,
                LODError,
                LodBoundsCenter,
                LodBoundsRadius,
                GroupIndex,
                GeneratingGroupIndex,
                InheritedMaxParentError));
        }

        CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << MipLevel << " group " << GroupOrdinal << " BuildMeshletClustersForGeometry" << ", meshlets=" << MeshletBuild.MeshletCount << ", outputClusters=" << OutClusterIndices.size() << ", outputTriangles=" << MeshletBuild.OutputTriangleCount << ", maxMeshletVertices=" << MeshletBuild.MaxMeshletVertices << ", maxMeshletTriangles=" << MeshletBuild.MaxMeshletTriangles << ", dagClusters=" << State.Dag.Clusters.size());

        return !OutClusterIndices.empty();
    }
#endif

    FGroupSphere BuildGroupSphere(
        const FClusterDAG& Dag,
        const std::vector<uint32_t>& ChildClusters,
        bool bUseLodBounds)
    {
        FGroupSphere Sphere;
        if (ChildClusters.empty())
        {
            return Sphere;
        }

#if WITH_MESHOPTIMIZER
        std::vector<FFloat3> Centers;
        std::vector<float> Radii;
        Centers.reserve(ChildClusters.size());
        Radii.reserve(ChildClusters.size());

        for (uint32_t ClusterIndex : ChildClusters)
        {
            const FCluster& Cluster = Dag.Clusters[ClusterIndex];
            Centers.push_back(bUseLodBounds ? Cluster.LodBoundsCenter : Cluster.Bounds.Center);
            Radii.push_back(bUseLodBounds ? Cluster.LodBoundsRadius : Cluster.Bounds.Radius);
        }

        const meshopt_Bounds Bounds = meshopt_computeSphereBounds(
            &Centers[0].x,
            Centers.size(),
            sizeof(FFloat3),
            Radii.data(),
            sizeof(float));
        Sphere.Center = { Bounds.center[0], Bounds.center[1], Bounds.center[2] };
        Sphere.Radius = Bounds.radius;
#else
        Sphere.Center = bUseLodBounds ? Dag.Clusters[ChildClusters[0]].LodBoundsCenter : Dag.Clusters[ChildClusters[0]].Bounds.Center;
        Sphere.Radius = bUseLodBounds ? Dag.Clusters[ChildClusters[0]].LodBoundsRadius : Dag.Clusters[ChildClusters[0]].Bounds.Radius;
#endif
        return Sphere;
    }

    bool RebuildRuntimeClusterDrawData(FClusterDAG& Dag)
    {
        if (!ValidateClusterIndexData(Dag))
        {
            Dag.RuntimeHierarchy.PackedIndices.clear();
            Dag.RuntimeHierarchy.DrawDatas.clear();
            return false;
        }

        FRuntimeClusterHierarchy& Runtime = Dag.RuntimeHierarchy;
        Runtime.PackedIndices.clear();
        Runtime.DrawDatas.clear();
        Runtime.PackedIndices.reserve(Dag.TriangleIndices.size());
        Runtime.DrawDatas.reserve(Dag.Clusters.size());

        for (uint32_t ClusterIndex = 0; ClusterIndex < Dag.Clusters.size(); ++ClusterIndex)
        {
            const FCluster& Cluster = Dag.Clusters[ClusterIndex];
            const uint32_t IndexStart = static_cast<uint32_t>(Runtime.PackedIndices.size());
            const std::vector<uint32_t> AbsoluteIndices = ExtractAbsoluteClusterIndices(Dag, Cluster);
            Runtime.PackedIndices.insert(Runtime.PackedIndices.end(), AbsoluteIndices.begin(), AbsoluteIndices.end());

            FRuntimeClusterDrawData DrawData;
            DrawData.IndexStart = IndexStart;
            DrawData.IndexCount = static_cast<uint32_t>(AbsoluteIndices.size());
            Runtime.DrawDatas.push_back(DrawData);

            if (ClusterIndex < Runtime.Clusters.size())
            {
                FRuntimeCluster& RuntimeCluster = Runtime.Clusters[ClusterIndex];
                RuntimeCluster.DrawDataStart = static_cast<uint32_t>(Runtime.DrawDatas.size() - 1);
                RuntimeCluster.DrawDataCount = 1;
                RuntimeCluster.TriangleCount = Cluster.TriangleCount;
            }
        }

        return true;
    }

    void EncodeRuntimeClusterHierarchy(FClusterDAG& Dag)
    {
        Dag.RuntimeHierarchy = {};
        if (!Dag.IsValid())
        {
            return;
        }

        FRuntimeClusterHierarchy& Runtime = Dag.RuntimeHierarchy;
        Runtime.RootGroupIndex = Dag.RootGroupIndex;
        Runtime.Groups.reserve(Dag.Groups.size());
        Runtime.Clusters.reserve(Dag.Clusters.size());
        Runtime.ChildRefs.reserve(Dag.Groups.size() * 4);
        Runtime.DrawDatas.reserve(Dag.Clusters.size());

        for (const FClusterGroup& Group : Dag.Groups)
        {
            FRuntimeClusterGroup RuntimeGroup;
            RuntimeGroup.BoundsCenter = Group.BoundsCenter;
            RuntimeGroup.BoundsRadius = Group.BoundsRadius;
            RuntimeGroup.LodBoundsCenter = Group.LodBoundsCenter;
            RuntimeGroup.LodBoundsRadius = Group.LodBoundsRadius;
            RuntimeGroup.ParentLODError = Group.ParentLODError;
            RuntimeGroup.ChildRefStart = static_cast<uint32_t>(Runtime.ChildRefs.size());
            RuntimeGroup.ChildRefCount = static_cast<uint32_t>(Group.ChildRefs.size());

            for (const FClusterRef& ChildRef : Group.ChildRefs)
            {
                FRuntimeClusterChildRef RuntimeRef;
                RuntimeRef.InstanceIndex = ChildRef.InstanceIndex;
                RuntimeRef.ClusterIndex = ChildRef.ClusterIndex;
                Runtime.ChildRefs.push_back(RuntimeRef);
            }

            RuntimeGroup.ParentRefStart = static_cast<uint32_t>(Runtime.ChildRefs.size());
            RuntimeGroup.ParentRefCount = static_cast<uint32_t>(Group.ParentRefs.size());

            for (const FClusterRef& ParentRef : Group.ParentRefs)
            {
                FRuntimeClusterChildRef RuntimeRef;
                RuntimeRef.InstanceIndex = ParentRef.InstanceIndex;
                RuntimeRef.ClusterIndex = ParentRef.ClusterIndex;
                Runtime.ChildRefs.push_back(RuntimeRef);
            }

            RuntimeGroup.MipLevel = Group.MipLevel;
            RuntimeGroup.Flags = Group.bRoot ? 1u : 0u;
            Runtime.Groups.push_back(RuntimeGroup);
        }

        for (const FCluster& Cluster : Dag.Clusters)
        {
            FRuntimeCluster RuntimeCluster;
            RuntimeCluster.Bounds = Cluster.Bounds;
            RuntimeCluster.LODError = Cluster.LODError;
            RuntimeCluster.MaxEdgeLength = Cluster.MaxEdgeLength;
            RuntimeCluster.LodBoundsCenter = Cluster.LodBoundsCenter;
            RuntimeCluster.LodBoundsRadius = Cluster.LodBoundsRadius;
            RuntimeCluster.GroupIndex = Cluster.GroupIndex;
            RuntimeCluster.GeneratingGroupIndex = Cluster.GeneratingGroupIndex;
            RuntimeCluster.TriangleCount = Cluster.TriangleCount;
            RuntimeCluster.MipLevel = Cluster.MipLevel;
            Runtime.Clusters.push_back(RuntimeCluster);
        }

        if (!RebuildRuntimeClusterDrawData(Dag))
        {
            Dag.RuntimeHierarchy = {};
        }
    }

    uint64_t MakeUndirectedEdgeKey(uint32_t A, uint32_t B)
    {
        const uint32_t MinIndex = (std::min)(A, B);
        const uint32_t MaxIndex = (std::max)(A, B);
        return (static_cast<uint64_t>(MinIndex) << 32) | static_cast<uint64_t>(MaxIndex);
    }

    FAdjacencyList ComputeClusterAdjacency(const FClusterDAG& Dag, const std::vector<uint32_t>& ClusterIndices)
    {
        FAdjacencyList Adjacency(ClusterIndices.size());
        std::unordered_map<uint64_t, uint32_t> EdgeOwners;
        EdgeOwners.reserve(ClusterIndices.size() * 16);

        for (uint32_t LocalClusterIndex = 0; LocalClusterIndex < ClusterIndices.size(); ++LocalClusterIndex)
        {
            const FCluster& Cluster = Dag.Clusters[ClusterIndices[LocalClusterIndex]];
            for (uint32_t Triangle = 0; Triangle < Cluster.TriangleCount; ++Triangle)
            {
                const uint32_t TriangleOffset = Cluster.TriangleOffset + Triangle * 3;
                const uint32_t VertexOffset = Cluster.VertexOffset;
                const uint32_t V0 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset]];
                const uint32_t V1 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 1]];
                const uint32_t V2 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 2]];
                const std::array<uint64_t, 3> Edges = {
                    MakeUndirectedEdgeKey(V0, V1),
                    MakeUndirectedEdgeKey(V1, V2),
                    MakeUndirectedEdgeKey(V2, V0)
                };

                for (uint64_t Edge : Edges)
                {
                    auto OwnerIt = EdgeOwners.find(Edge);
                    if (OwnerIt == EdgeOwners.end())
                    {
                        EdgeOwners.emplace(Edge, LocalClusterIndex);
                    }
                    else if (OwnerIt->second != LocalClusterIndex)
                    {
                        ++Adjacency[LocalClusterIndex][OwnerIt->second];
                        ++Adjacency[OwnerIt->second][LocalClusterIndex];
                    }
                }
            }
        }

        return Adjacency;
    }

    FFloat3 AverageClusterCenter(const FClusterDAG& Dag, const std::vector<uint32_t>& ClusterIndices)
    {
        FFloat3 Result{ 0.0f, 0.0f, 0.0f };
        if (ClusterIndices.empty())
        {
            return Result;
        }

        for (uint32_t ClusterIndex : ClusterIndices)
        {
            const FFloat3& Center = Dag.Clusters[ClusterIndex].Bounds.Center;
            Result.x += Center.x;
            Result.y += Center.y;
            Result.z += Center.z;
        }

        const float Scale = 1.0f / static_cast<float>(ClusterIndices.size());
        Result.x *= Scale;
        Result.y *= Scale;
        Result.z *= Scale;
        return Result;
    }

    float DistanceSquared(const FFloat3& A, const FFloat3& B)
    {
        const float DX = A.x - B.x;
        const float DY = A.y - B.y;
        const float DZ = A.z - B.z;
        return DX * DX + DY * DY + DZ * DZ;
    }

    float ComputeRelativeError(float AbsoluteError, float SphereRadius)
    {
        const float Diameter = (std::max)(SphereRadius * 2.0f, 1e-6f);
        return AbsoluteError / Diameter;
    }

    uint32_t ComputeDesiredParentCount(uint32_t SourceTriangleCount, const FClusterDAGBuildParams& Params)
    {
        const uint32_t ParentTriangleBudget = (std::max)(1u, Params.MaxClusterTriangles * 2u);
        return (std::max)(1u, (SourceTriangleCount + ParentTriangleBudget - 1u) / ParentTriangleBudget);
    }

    uint32_t ComputeMaxAllowedParentCount(uint32_t DesiredParentCount, uint32_t ChildClusterCount)
    {
        if (DesiredParentCount == 0u || ChildClusterCount <= 1u)
        {
            return DesiredParentCount;
        }

        const uint32_t MaxReducingParentCount = ChildClusterCount - 1u;
        const uint32_t OvershootParentCount = (std::max)(1u, (DesiredParentCount + 1u) / 2u);
        return (std::min)(MaxReducingParentCount, DesiredParentCount + OvershootParentCount);
    }

    bool IsReducerOutputCandidate(
        bool bReduced,
        bool bValidOutput,
        uint32_t SourceTriangleCount,
        uint32_t OutputTriangleCount,
        uint32_t PredictedParentCount,
        uint32_t ChildClusterCount,
        const std::string& FailureReason)
    {
        const bool bMeaningfulTriangleReduction = OutputTriangleCount < SourceTriangleCount;
        const bool bMeaningfulParentReduction = PredictedParentCount > 0u && PredictedParentCount < ChildClusterCount;
        const bool bParentBudgetPendingActualSplit =
            !bValidOutput && FailureReason == "predicted_parent_count_exceeded";
        return bReduced
            && bMeaningfulTriangleReduction
            && bMeaningfulParentReduction
            && (bValidOutput || bParentBudgetPendingActualSplit);
    }

    std::vector<FClusterRef> MakeClusterRefs(const std::vector<uint32_t>& ClusterIndices)
    {
        std::vector<FClusterRef> ClusterRefs;
        ClusterRefs.reserve(ClusterIndices.size());
        for (uint32_t ClusterIndex : ClusterIndices)
        {
            ClusterRefs.emplace_back(ClusterIndex);
        }
        return ClusterRefs;
    }

    FPositionEdgeKey MakeUndirectedPositionEdgeKey(const FFloat3& A, const FFloat3& B)
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

    using FClusterEdgeOwnerMap = std::unordered_map<FPositionEdgeKey, FClusterEdgeOwnerInfo, FPositionEdgeKeyHasher>;

    FClusterEdgeOwnerMap BuildClusterExternalEdgeOwners(const FClusterDAG& Dag, const std::vector<uint32_t>& ClusterIndices)
    {
        FClusterEdgeOwnerMap EdgeOwners;
        EdgeOwners.reserve(ClusterIndices.size() * 64);

        for (uint32_t ClusterIndex : ClusterIndices)
        {
            if (ClusterIndex >= Dag.Clusters.size())
            {
                continue;
            }

            const FCluster& Cluster = Dag.Clusters[ClusterIndex];
            std::unordered_set<FPositionEdgeKey, FPositionEdgeKeyHasher> ClusterEdges;
            ClusterEdges.reserve(static_cast<size_t>(Cluster.TriangleCount) * 3);

            for (uint32_t Triangle = 0; Triangle < Cluster.TriangleCount; ++Triangle)
            {
                const uint32_t TriangleOffset = Cluster.TriangleOffset + Triangle * 3;
                const uint32_t VertexOffset = Cluster.VertexOffset;
                const uint32_t V0 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset]];
                const uint32_t V1 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 1]];
                const uint32_t V2 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 2]];

                ClusterEdges.insert(MakeUndirectedPositionEdgeKey(Dag.Positions[V0], Dag.Positions[V1]));
                ClusterEdges.insert(MakeUndirectedPositionEdgeKey(Dag.Positions[V1], Dag.Positions[V2]));
                ClusterEdges.insert(MakeUndirectedPositionEdgeKey(Dag.Positions[V2], Dag.Positions[V0]));
            }

            for (const FPositionEdgeKey& EdgeKey : ClusterEdges)
            {
                ++EdgeOwners[EdgeKey].DistinctOwnerCount;
            }
        }

        return EdgeOwners;
    }

    void UpdateClusterExternalEdges(FClusterDAG& Dag, const std::vector<uint32_t>& ClusterIndices)
    {
        const FClusterEdgeOwnerMap EdgeOwners = BuildClusterExternalEdgeOwners(Dag, ClusterIndices);

        for (uint32_t ClusterIndex : ClusterIndices)
        {
            if (ClusterIndex >= Dag.Clusters.size())
            {
                continue;
            }

            FCluster& Cluster = Dag.Clusters[ClusterIndex];
            if (Cluster.ExternalEdgeOffset > Dag.ExternalEdges.size()
                || Cluster.ExternalEdgeCount > Dag.ExternalEdges.size() - Cluster.ExternalEdgeOffset)
            {
                continue;
            }

            std::fill_n(Dag.ExternalEdges.begin() + Cluster.ExternalEdgeOffset, Cluster.ExternalEdgeCount, static_cast<uint8_t>(0));

            for (uint32_t Triangle = 0; Triangle < Cluster.TriangleCount; ++Triangle)
            {
                const uint32_t TriangleOffset = Cluster.TriangleOffset + Triangle * 3;
                const uint32_t VertexOffset = Cluster.VertexOffset;
                const uint32_t V0 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset]];
                const uint32_t V1 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 1]];
                const uint32_t V2 = Dag.ClusterVertices[VertexOffset + Dag.TriangleIndices[TriangleOffset + 2]];
                const std::array<FPositionEdgeKey, 3> Edges =
                {
                    MakeUndirectedPositionEdgeKey(Dag.Positions[V0], Dag.Positions[V1]),
                    MakeUndirectedPositionEdgeKey(Dag.Positions[V1], Dag.Positions[V2]),
                    MakeUndirectedPositionEdgeKey(Dag.Positions[V2], Dag.Positions[V0])
                };

                for (uint32_t EdgeIndex = 0; EdgeIndex < Edges.size(); ++EdgeIndex)
                {
                    const auto OwnerIt = EdgeOwners.find(Edges[EdgeIndex]);
                    const bool bExternal = OwnerIt == EdgeOwners.end() || OwnerIt->second.DistinctOwnerCount != 2u;
                    Dag.ExternalEdges[Cluster.ExternalEdgeOffset + Triangle * 3 + EdgeIndex] = bExternal ? 1u : 0u;
                }
            }
        }
    }

    void MergeSmallPartitions(
        size_t PrimitiveIndex,
        uint32_t Level,
        const FClusterDAG& Dag,
        const std::vector<uint32_t>& CurrentClusters,
        const FAdjacencyList& Adjacency,
        uint32_t TargetGroupSize,
        std::vector<std::vector<uint32_t>>& InOutGroups)
    {
        const uint32_t MinGroupSize = (std::min)(GClusterDAGMinGroupSize, (std::max)(2u, TargetGroupSize));
        if (InOutGroups.size() <= 1)
        {
            return;
        }

        std::unordered_map<uint32_t, size_t> CurrentClusterLookup;
        CurrentClusterLookup.reserve(CurrentClusters.size());
        for (size_t ClusterIndex = 0; ClusterIndex < CurrentClusters.size(); ++ClusterIndex)
        {
            CurrentClusterLookup.emplace(CurrentClusters[ClusterIndex], ClusterIndex);
        }

        bool bMerged = true;
        while (bMerged && InOutGroups.size() > 1)
        {
            bMerged = false;
            for (size_t GroupIndex = 0; GroupIndex < InOutGroups.size(); ++GroupIndex)
            {
                if (InOutGroups[GroupIndex].empty() || InOutGroups[GroupIndex].size() >= MinGroupSize)
                {
                    continue;
                }

                const FFloat3 GroupCenter = AverageClusterCenter(Dag, InOutGroups[GroupIndex]);
                size_t BestTargetIndex = static_cast<size_t>(-1);
                uint32_t BestAdjacency = 0;
                float BestDistance = std::numeric_limits<float>::max();

                for (size_t CandidateIndex = 0; CandidateIndex < InOutGroups.size(); ++CandidateIndex)
                {
                    if (CandidateIndex == GroupIndex || InOutGroups[CandidateIndex].empty())
                    {
                        continue;
                    }

                    if (InOutGroups[GroupIndex].size() + InOutGroups[CandidateIndex].size() > GClusterDAGMaxGroupSize)
                    {
                        continue;
                    }

                    uint32_t SharedEdges = 0;
                    for (uint32_t ClusterId : InOutGroups[GroupIndex])
                    {
                        const auto CurrentIt = CurrentClusterLookup.find(ClusterId);
                        if (CurrentIt == CurrentClusterLookup.end())
                        {
                            continue;
                        }

                        for (uint32_t CandidateClusterId : InOutGroups[CandidateIndex])
                        {
                            const auto CandidateIt = CurrentClusterLookup.find(CandidateClusterId);
                            if (CandidateIt == CurrentClusterLookup.end())
                            {
                                continue;
                            }

                            auto AdjIt = Adjacency[CurrentIt->second].find(static_cast<uint32_t>(CandidateIt->second));
                            if (AdjIt != Adjacency[CurrentIt->second].end())
                            {
                                SharedEdges += AdjIt->second;
                            }
                        }
                    }

                    const float CandidateDistance = DistanceSquared(GroupCenter, AverageClusterCenter(Dag, InOutGroups[CandidateIndex]));
                    if (BestTargetIndex == static_cast<size_t>(-1)
                        || SharedEdges > BestAdjacency
                        || (SharedEdges == BestAdjacency && CandidateDistance < BestDistance))
                    {
                        BestTargetIndex = CandidateIndex;
                        BestAdjacency = SharedEdges;
                        BestDistance = CandidateDistance;
                    }
                }

                if (BestTargetIndex != static_cast<size_t>(-1))
                {
                    const size_t SourceGroupSize = InOutGroups[GroupIndex].size();
                    const size_t TargetGroupSizeBeforeMerge = InOutGroups[BestTargetIndex].size();
                    InOutGroups[BestTargetIndex].insert(
                        InOutGroups[BestTargetIndex].end(),
                        InOutGroups[GroupIndex].begin(),
                        InOutGroups[GroupIndex].end());
                    const size_t TargetGroupSizeAfterMerge = InOutGroups[BestTargetIndex].size();
                    CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " MergeSmallPartitions merged" << ", sourceGroup=" << GroupIndex << ", sourceSize=" << SourceGroupSize << ", targetGroup=" << BestTargetIndex << ", targetSizeBefore=" << TargetGroupSizeBeforeMerge << ", targetSizeAfter=" << TargetGroupSizeAfterMerge << ", sharedEdges=" << BestAdjacency << ", centerDistanceSq=" << FormatFloat(BestDistance) << ", minGroupSize=" << MinGroupSize);
                    InOutGroups[GroupIndex].clear();
                    bMerged = true;
                }
            }
        }

        InOutGroups.erase(
            std::remove_if(
                InOutGroups.begin(),
                InOutGroups.end(),
                [](const std::vector<uint32_t>& Group) { return Group.empty(); }),
            InOutGroups.end());
    }

#if WITH_MESHOPTIMIZER
    bool FlattenGroupGeometry(
        const FClusterDAG& Dag,
        const std::vector<uint32_t>& ChildClusters,
        FBuilderVertexStreams& OutStreams,
        std::vector<uint32_t>& OutIndices)
    {
        OutStreams = {};
        OutIndices.clear();

        std::unordered_map<uint32_t, uint32_t> VertexRemap;
        VertexRemap.reserve(ChildClusters.size() * 64);

        for (uint32_t ClusterIndex : ChildClusters)
        {
            const std::vector<uint32_t> ClusterIndices = ExtractAbsoluteClusterIndices(Dag, Dag.Clusters[ClusterIndex]);
            for (uint32_t AbsoluteIndex : ClusterIndices)
            {
                auto It = VertexRemap.find(AbsoluteIndex);
                if (It == VertexRemap.end())
                {
                    const uint32_t LocalIndex = static_cast<uint32_t>(OutStreams.Positions.size());
                    VertexRemap.emplace(AbsoluteIndex, LocalIndex);
                    OutStreams.Positions.push_back(Dag.Positions[AbsoluteIndex]);
                    OutStreams.Normals.push_back(AbsoluteIndex < Dag.Normals.size() ? Dag.Normals[AbsoluteIndex] : FFloat3(0.0f, 0.0f, 1.0f));
                    OutStreams.UVs.push_back(AbsoluteIndex < Dag.UVs.size() ? Dag.UVs[AbsoluteIndex] : FFloat2(0.0f, 0.0f));
                    OutStreams.Tangents.push_back(AbsoluteIndex < Dag.Tangents.size() ? Dag.Tangents[AbsoluteIndex] : FFloat4(0.0f, 0.0f, 0.0f, 1.0f));
                    OutStreams.Colors.push_back(AbsoluteIndex < Dag.Colors.size() ? Dag.Colors[AbsoluteIndex] : FFloat4(1.0f, 1.0f, 1.0f, 1.0f));
                    OutIndices.push_back(LocalIndex);
                }
                else
                {
                    OutIndices.push_back(It->second);
                }
            }
        }

        if (OutStreams.Positions.empty() || OutIndices.size() < 3)
        {
            return false;
        }

        CompactAndOptimizeBuilderGeometry(OutStreams, OutIndices);
        return !OutStreams.Positions.empty() && OutIndices.size() >= 3;
    }

    std::vector<unsigned char> BuildExternalGroupLockMask(
        const FBuilderVertexStreams& Streams,
        const std::vector<uint32_t>& Indices)
    {
        std::vector<unsigned char> Locks(Streams.Positions.size(), 0);
        std::unordered_map<FPositionEdgeKey, uint32_t, FPositionEdgeKeyHasher> BoundaryEdgeCounts;
        BoundaryEdgeCounts.reserve(Indices.size());
        std::unordered_map<FPositionKey, std::vector<uint32_t>, FPositionKeyHasher> PositionToVertices;
        PositionToVertices.reserve(Streams.Positions.size());
        for (uint32_t VertexIndex = 0; VertexIndex < Streams.Positions.size(); ++VertexIndex)
        {
            PositionToVertices[MakePositionKey(Streams.Positions[VertexIndex])].push_back(VertexIndex);
        }

        for (size_t TriangleOffset = 0; TriangleOffset + 2 < Indices.size(); TriangleOffset += 3)
        {
            const uint32_t V0 = Indices[TriangleOffset + 0];
            const uint32_t V1 = Indices[TriangleOffset + 1];
            const uint32_t V2 = Indices[TriangleOffset + 2];
            if (V0 >= Streams.Positions.size()
                || V1 >= Streams.Positions.size()
                || V2 >= Streams.Positions.size())
            {
                continue;
            }

            const std::array<FPositionEdgeKey, 3> Edges =
            {
                MakeUndirectedPositionEdgeKey(Streams.Positions[V0], Streams.Positions[V1]),
                MakeUndirectedPositionEdgeKey(Streams.Positions[V1], Streams.Positions[V2]),
                MakeUndirectedPositionEdgeKey(Streams.Positions[V2], Streams.Positions[V0])
            };

            ++BoundaryEdgeCounts[Edges[0]];
            ++BoundaryEdgeCounts[Edges[1]];
            ++BoundaryEdgeCounts[Edges[2]];
        }

        for (const auto& Pair : BoundaryEdgeCounts)
        {
            if (Pair.second == 2u)
            {
                continue;
            }

            const auto EndpointAIt = PositionToVertices.find(Pair.first.A);
            if (EndpointAIt != PositionToVertices.end())
            {
                for (uint32_t VertexIndex : EndpointAIt->second)
                {
                    Locks[VertexIndex] = 1;
                }
            }

            const auto EndpointBIt = PositionToVertices.find(Pair.first.B);
            if (EndpointBIt != PositionToVertices.end())
            {
                for (uint32_t VertexIndex : EndpointBIt->second)
                {
                    Locks[VertexIndex] = 1;
                }
            }
        }

        return Locks;
    }

    void BuildAttributeStream(const FBuilderVertexStreams& Streams, std::vector<float>& OutAttributes)
    {
        OutAttributes.resize(Streams.Positions.size() * GAttributeFloatCount);
        for (size_t VertexIndex = 0; VertexIndex < Streams.Positions.size(); ++VertexIndex)
        {
            const size_t Base = VertexIndex * GAttributeFloatCount;
            const FFloat3& Normal = Streams.Normals[VertexIndex];
            const FFloat2& UV = Streams.UVs[VertexIndex];
            OutAttributes[Base + 0] = Normal.x;
            OutAttributes[Base + 1] = Normal.y;
            OutAttributes[Base + 2] = Normal.z;
            OutAttributes[Base + 3] = UV.x;
            OutAttributes[Base + 4] = UV.y;
        }
    }

    bool SimplifyGroupGeometryWithAttributes(
        size_t PrimitiveIndex,
        uint32_t Level,
        size_t GroupOrdinal,
        const FBuilderVertexStreams& SourceStreams,
        const std::vector<uint32_t>& SourceIndices,
        const std::vector<unsigned char>& SourceVertexLocks,
        const FClusterDAGBuildParams& Params,
        uint32_t DesiredParentCount,
        uint32_t ChildClusterCount,
        FBuilderVertexStreams& OutStreams,
        std::vector<uint32_t>& OutIndices,
        float& OutResultError,
        uint32_t& OutPredictedParentCount)
    {
        OutResultError = 0.0f;
        OutPredictedParentCount = 0;
        OutStreams = {};
        OutIndices.clear();
        if (SourceStreams.Positions.empty() || SourceIndices.size() < 6)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " SimplifyGroupGeometryWithAttributes skipped" << ", reason=insufficient_input" << ", vertices=" << SourceStreams.Positions.size() << ", indices=" << SourceIndices.size());
            return false;
        }

        const size_t SourceIndexCount = SourceIndices.size();
        const size_t SourceVertexCount = SourceStreams.Positions.size();
        auto ClampTargetIndexCount = [SourceIndexCount](size_t CandidateCount)
        {
            if (SourceIndexCount <= 3)
            {
                return size_t(0);
            }

            CandidateCount = (CandidateCount / 3) * 3;
            CandidateCount = (std::max)(CandidateCount, size_t(3));
            if (CandidateCount >= SourceIndexCount)
            {
                CandidateCount = SourceIndexCount - 3;
            }
            return CandidateCount;
        };

        const uint32_t SourceTriangleCount = static_cast<uint32_t>(SourceIndexCount / 3);
        const uint32_t InitialTargetClusterTriangles = Params.MaxClusterTriangles > 2u ? Params.MaxClusterTriangles - 2u : Params.MaxClusterTriangles;
        const uint32_t MinimumTargetClusterTriangles = (std::max)(1u, Params.MaxClusterTriangles / 2u);
        if (DesiredParentCount == 0u || InitialTargetClusterTriangles == 0u)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " SimplifyGroupGeometryWithAttributes skipped" << ", reason=invalid_parent_budget" << ", sourceTriangles=" << SourceTriangleCount << ", desiredParents=" << DesiredParentCount << ", targetClusterTriangles=" << InitialTargetClusterTriangles);
            return false;
        }

        std::vector<float> Attributes;
        BuildAttributeStream(SourceStreams, Attributes);
        const std::array<float, GAttributeFloatCount> AttributeWeights = {
            0.5f, 0.5f, 0.5f,
            1.0f, 1.0f
        };
        const std::vector<unsigned char> VertexLocks =
            SourceVertexLocks.size() == SourceStreams.Positions.size()
            ? SourceVertexLocks
            : std::vector<unsigned char>(SourceStreams.Positions.size(), static_cast<unsigned char>(0));
        const size_t LockedVertexCount = static_cast<size_t>(std::count(VertexLocks.begin(), VertexLocks.end(), static_cast<unsigned char>(1)));

        size_t AttemptCount = 0;
        uint32_t FinalTargetClusterTriangles = InitialTargetClusterTriangles;
        size_t FinalTargetIndexCount = 0;
        size_t LastSimplifiedIndexCount = 0;
        size_t LastPredictedParentCount = 0;
        uint32_t TargetClusterTriangles = InitialTargetClusterTriangles;
        bool bReduced = false;

        while (TargetClusterTriangles > 0u)
        {
            ++AttemptCount;
            FinalTargetClusterTriangles = TargetClusterTriangles;
            const size_t TargetIndexCount = ClampTargetIndexCount(static_cast<size_t>(DesiredParentCount) * static_cast<size_t>(TargetClusterTriangles) * 3ull);
            FinalTargetIndexCount = TargetIndexCount;
            if (TargetIndexCount < 3 || TargetIndexCount >= SourceIndexCount)
            {
                break;
            }

            std::vector<uint32_t> SimplifiedIndices(SourceIndexCount);
            OutResultError = 0.0f;
            const size_t SimplifiedIndexCount = meshopt_simplifyWithAttributes(
                SimplifiedIndices.data(),
                SourceIndices.data(),
                SourceIndices.size(),
                &SourceStreams.Positions[0].x,
                SourceStreams.Positions.size(),
                sizeof(FFloat3),
                Attributes.data(),
                sizeof(float) * GAttributeFloatCount,
                AttributeWeights.data(),
                GAttributeFloatCount,
                VertexLocks.data(),
                TargetIndexCount,
                std::numeric_limits<float>::max(),
                meshopt_SimplifyErrorAbsolute,
                &OutResultError);

            LastSimplifiedIndexCount = SimplifiedIndexCount;
            if (SimplifiedIndexCount >= 3 && SimplifiedIndexCount < SourceIndexCount)
            {
                SimplifiedIndices.resize(SimplifiedIndexCount);
                FBuilderVertexStreams CandidateStreams = SourceStreams;
                std::vector<uint32_t> CandidateIndices = std::move(SimplifiedIndices);
                CompactAndOptimizeBuilderGeometry(CandidateStreams, CandidateIndices);

                if (CandidateIndices.size() >= 3 && CandidateIndices.size() < SourceIndexCount)
                {
                    FMeshletBuildResult MeshletBuild;
                    if (GenerateMeshletsForGeometry(CandidateStreams, CandidateIndices, Params, MeshletBuild))
                    {
                        LastPredictedParentCount = MeshletBuild.MeshletCount;
                        const bool bMeaningfulTriangleReduction = CandidateIndices.size() + 3u <= SourceIndexCount;
                        const bool bMeaningfulClusterReduction = MeshletBuild.MeshletCount < ChildClusterCount;
                        const bool bSplitSuccess = MeshletBuild.MeshletCount > 0 && MeshletBuild.MeshletCount <= DesiredParentCount;
                        if (bMeaningfulTriangleReduction && bMeaningfulClusterReduction && bSplitSuccess)
                        {
                            OutStreams = std::move(CandidateStreams);
                            OutIndices = std::move(CandidateIndices);
                            OutPredictedParentCount = static_cast<uint32_t>(MeshletBuild.MeshletCount);
                            bReduced = true;
                            break;
                        }
                    }
                }
            }

            if (TargetClusterTriangles <= MinimumTargetClusterTriangles)
            {
                break;
            }

            TargetClusterTriangles -= 2u;
        }

        if (!bReduced)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " SimplifyGroupGeometry failed" << ", vertices=" << SourceStreams.Positions.size() << ", triangles=" << SourceTriangleCount << ", desiredParents=" << DesiredParentCount << ", targetClusterTriangles=" << FinalTargetClusterTriangles << ", targetTriangles=" << (FinalTargetIndexCount / 3) << ", lockedVertices=" << LockedVertexCount << ", simplifiedTriangles=" << (LastSimplifiedIndexCount / 3) << ", predictedParents=" << LastPredictedParentCount << ", attempts=" << AttemptCount << ", resultError=" << FormatFloat(OutResultError));
            return false;
        }

        const bool bValidOutput = OutIndices.size() >= 3 && OutIndices.size() < SourceIndexCount && OutPredictedParentCount > 0 && OutPredictedParentCount < ChildClusterCount;
        CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " SimplifyGroupGeometry" << ", srcVertices=" << SourceVertexCount << ", srcTriangles=" << SourceTriangleCount << ", outputVertices=" << OutStreams.Positions.size() << ", outputTriangles=" << (OutIndices.size() / 3) << ", desiredParents=" << DesiredParentCount << ", predictedParents=" << OutPredictedParentCount << ", targetClusterTriangles=" << FinalTargetClusterTriangles << ", targetTriangles=" << (FinalTargetIndexCount / 3) << ", lockedVertices=" << LockedVertexCount << ", attempts=" << AttemptCount << ", resultError=" << FormatFloat(OutResultError) << ", validOutput=" << (bValidOutput ? "true" : "false"));
        return bValidOutput;
    }

    bool SimplifyGroupGeometryPositionOnly(
        size_t PrimitiveIndex,
        uint32_t Level,
        size_t GroupOrdinal,
        const FClusterDAG& Dag,
        const FMergedClusterScratch& Scratch,
        const FClusterDAGBuildParams& Params,
        uint32_t DesiredParentCount,
        uint32_t MaxAllowedParentCount,
        uint32_t ChildClusterCount,
        bool bAllowExternalPenaltyCollapses,
        FBuilderVertexStreams& OutStreams,
        std::vector<uint32_t>& OutIndices,
        float& OutResultError,
        uint32_t& OutPredictedParentCount)
    {
        OutStreams = {};
        OutIndices.clear();
        OutResultError = 0.0f;
        OutPredictedParentCount = 0;

        if (!Scratch.IsValid())
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " SimplifyGroupGeometryPositionOnly skipped" << ", reason=invalid_scratch");
            return false;
        }

        const uint32_t SourceTriangleCount = Scratch.ActiveTriangleCount;
        const uint32_t SourcePositionCount = static_cast<uint32_t>(Scratch.PositionNodes.size());
        const uint32_t InitialTargetClusterTriangles = Params.MaxClusterTriangles > 2u ? Params.MaxClusterTriangles - 2u : Params.MaxClusterTriangles;
        const uint32_t FlatShadedVertexBoundTriangles = (std::max)(1u, Params.MaxClusterVertices / 3u);
        const uint32_t MinimumTargetClusterTriangles = (std::max)(1u, (std::min)(Params.MaxClusterTriangles / 2u, FlatShadedVertexBoundTriangles));
        if (DesiredParentCount == 0u || MaxAllowedParentCount == 0u || InitialTargetClusterTriangles == 0u)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " SimplifyGroupGeometryPositionOnly skipped" << ", reason=invalid_parent_budget" << ", sourceTriangles=" << SourceTriangleCount << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << MaxAllowedParentCount << ", targetClusterTriangles=" << InitialTargetClusterTriangles);
            return false;
        }

        uint32_t AttemptCount = 0;
        uint32_t RelaxedAttemptCount = 0;
        uint32_t FinalTargetClusterTriangles = InitialTargetClusterTriangles;
        uint32_t LastPositionTriangles = SourceTriangleCount;
        uint32_t LastSimplifiedTriangles = SourceTriangleCount;
        uint32_t LastOutputTriangles = SourceTriangleCount;
        uint32_t LastOutputPositions = SourcePositionCount;
        uint32_t LastPredictedParentCount = 0;
        std::string LastFailureReason = "no_attempt";

        auto TryReduce = [&](bool bRelaxLocks) -> bool
        {
            FBuilderVertexStreams PendingStreams;
            std::vector<uint32_t> PendingIndices;
            float PendingResultError = 0.0f;
            uint32_t PendingPredictedParentCount = 0;
            uint32_t PendingPositionNodeCount = 0;
            uint32_t PendingPositionTriangleCount = 0;
            uint32_t PendingOutputPositions = 0;
            uint32_t PendingOutputTriangles = 0;
            uint32_t PendingSimplifiedTriangles = 0;
            uint32_t PendingLockedPositionCount = 0;
            uint32_t PendingTargetClusterTriangles = 0;
            bool bHasPendingActualParentCheck = false;

            uint32_t TargetClusterTriangles = InitialTargetClusterTriangles;
            while (TargetClusterTriangles > 0u)
            {
                if (bRelaxLocks) ++RelaxedAttemptCount; else ++AttemptCount;
                FinalTargetClusterTriangles = TargetClusterTriangles;

                FMeshoptScratchReducerInput Input;
                Input.DesiredParentCount = DesiredParentCount;
                Input.MaxAllowedParentCount = MaxAllowedParentCount;
                Input.TargetClusterTriangles = TargetClusterTriangles;
                Input.MaxClusterVertices = Params.MaxClusterVertices;
                Input.MaxClusterTriangles = Params.MaxClusterTriangles;
                Input.ConeWeight = Params.ConeWeight;
                Input.bRelaxLocks = bRelaxLocks;

                FMeshoptScratchReducerResult Result;
                const bool bReduced = ReduceMergedClusterWithMeshopt(Dag, Scratch, Input, Result);
                OutResultError = Result.ResultError;
                LastPositionTriangles = Result.PositionTriangleCount;
                LastSimplifiedTriangles = Result.SimplifiedTriangleCount;
                LastOutputTriangles = Result.OutputTriangleCount;
                LastOutputPositions = Result.OutputPositionCount;
                LastPredictedParentCount = Result.PredictedParentCount;
                LastFailureReason = Result.FailureReason.empty() ? "unknown" : Result.FailureReason;

                if (IsReducerOutputCandidate(
                    bReduced,
                    Result.bValidOutput,
                    SourceTriangleCount,
                    Result.OutputTriangleCount,
                    Result.PredictedParentCount,
                    ChildClusterCount,
                    LastFailureReason))
                {
                    if (Result.bValidOutput)
                    {
                        OutStreams = std::move(Result.Streams);
                        OutIndices = std::move(Result.Indices);
                        OutPredictedParentCount = Result.PredictedParentCount;
                        CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " MeshoptScratchReducer" << ", backend=MeshoptScratch" << ", srcPositions=" << SourcePositionCount << ", srcTriangles=" << SourceTriangleCount << ", positionNodes=" << Result.PositionNodeCount << ", positionTriangles=" << Result.PositionTriangleCount << ", outputPositions=" << LastOutputPositions << ", outputTriangles=" << LastOutputTriangles << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << MaxAllowedParentCount << ", predictedParents=" << OutPredictedParentCount << ", targetClusterTriangles=" << FinalTargetClusterTriangles << ", meshoptSimplifiedTriangles=" << LastSimplifiedTriangles << ", lockedPositions=" << Result.LockedPositionCount << ", attempts=" << AttemptCount << ", relaxedAttempts=" << RelaxedAttemptCount << ", relaxedLocks=" << (bRelaxLocks ? "true" : "false") << ", resultError=" << FormatFloat(OutResultError) << ", validOutput=true");
                        return true;
                    }

                    PendingStreams = std::move(Result.Streams);
                    PendingIndices = std::move(Result.Indices);
                    PendingResultError = OutResultError;
                    PendingPredictedParentCount = Result.PredictedParentCount;
                    PendingPositionNodeCount = Result.PositionNodeCount;
                    PendingPositionTriangleCount = Result.PositionTriangleCount;
                    PendingOutputPositions = LastOutputPositions;
                    PendingOutputTriangles = LastOutputTriangles;
                    PendingSimplifiedTriangles = LastSimplifiedTriangles;
                    PendingLockedPositionCount = Result.LockedPositionCount;
                    PendingTargetClusterTriangles = FinalTargetClusterTriangles;
                    bHasPendingActualParentCheck = true;
                }

                if (TargetClusterTriangles <= MinimumTargetClusterTriangles)
                {
                    break;
                }

                TargetClusterTriangles -= 2u;
            }

            if (bHasPendingActualParentCheck)
            {
                OutStreams = std::move(PendingStreams);
                OutIndices = std::move(PendingIndices);
                OutResultError = PendingResultError;
                OutPredictedParentCount = PendingPredictedParentCount;
                CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " MeshoptScratchReducer" << ", backend=MeshoptScratch" << ", srcPositions=" << SourcePositionCount << ", srcTriangles=" << SourceTriangleCount << ", positionNodes=" << PendingPositionNodeCount << ", positionTriangles=" << PendingPositionTriangleCount << ", outputPositions=" << PendingOutputPositions << ", outputTriangles=" << PendingOutputTriangles << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << MaxAllowedParentCount << ", predictedParents=" << OutPredictedParentCount << ", targetClusterTriangles=" << PendingTargetClusterTriangles << ", meshoptSimplifiedTriangles=" << PendingSimplifiedTriangles << ", lockedPositions=" << PendingLockedPositionCount << ", attempts=" << AttemptCount << ", relaxedAttempts=" << RelaxedAttemptCount << ", relaxedLocks=" << (bRelaxLocks ? "true" : "false") << ", resultError=" << FormatFloat(OutResultError) << ", validOutput=pending_actual_parent_count");
                return true;
            }

            return false;
        };

        if (TryReduce(false))
        {
            return true;
        }

        if (bAllowExternalPenaltyCollapses && TryReduce(true))
        {
            return true;
        }

        OutPredictedParentCount = LastPredictedParentCount;
        CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " MeshoptScratchReducer failed" << ", fallback=MeshoptScratch" << ", positionNodes=" << SourcePositionCount << ", positionTriangles=" << LastPositionTriangles << ", sourceTriangles=" << SourceTriangleCount << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << MaxAllowedParentCount << ", targetClusterTriangles=" << FinalTargetClusterTriangles << ", lockedPositions=" << Scratch.LockedPositionCount << ", meshoptSimplifiedTriangles=" << LastSimplifiedTriangles << ", outputPositions=" << LastOutputPositions << ", outputTriangles=" << LastOutputTriangles << ", predictedParents=" << LastPredictedParentCount << ", attempts=" << AttemptCount << ", relaxedAttempts=" << RelaxedAttemptCount << ", relaxedRetry=" << (bAllowExternalPenaltyCollapses ? "true" : "false") << ", resultError=" << FormatFloat(OutResultError) << ", reason=" << LastFailureReason);
        return false;
    }

    bool SimplifyMergedClusterGroup(
        size_t PrimitiveIndex,
        uint32_t Level,
        size_t GroupOrdinal,
        const FClusterDAG& Dag,
        const std::vector<uint32_t>& ChildClusters,
        const FMergedClusterScratch& Scratch,
        const FClusterDAGBuildParams& Params,
        uint32_t DesiredParentCount,
        uint32_t MaxAllowedParentCount,
        bool bAllowExternalPenaltyCollapses,
        FBuilderVertexStreams& OutStreams,
        std::vector<uint32_t>& OutIndices,
        float& OutResultError,
        uint32_t& OutPredictedParentCount)
    {
        OutStreams = {};
        OutIndices.clear();
        OutResultError = 0.0f;
        OutPredictedParentCount = 0;

        FBuilderVertexStreams SourceStreams;
        std::vector<uint32_t> SourceIndices;
        std::vector<unsigned char> SourceVertexLocks;
        if (EmitMergedClusterGeometry(Dag, Scratch, SourceStreams, SourceIndices, &SourceVertexLocks))
        {
            if (SimplifyGroupGeometryWithAttributes(
                PrimitiveIndex,
                Level,
                GroupOrdinal,
                SourceStreams,
                SourceIndices,
                SourceVertexLocks,
                Params,
                DesiredParentCount,
                static_cast<uint32_t>(ChildClusters.size()),
                OutStreams,
                OutIndices,
                OutResultError,
                OutPredictedParentCount))
            {
                return true;
            }
        }
        else
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " meshopt source emission failed" << ", childClusters=" << ChildClusters.size() << ", fallback=MeshoptScratch");
        }

        OutStreams = {};
        OutIndices.clear();
        OutResultError = 0.0f;
        OutPredictedParentCount = 0;
        return SimplifyGroupGeometryPositionOnly(
            PrimitiveIndex,
            Level,
            GroupOrdinal,
            Dag,
            Scratch,
            Params,
            DesiredParentCount,
            MaxAllowedParentCount,
            static_cast<uint32_t>(ChildClusters.size()),
            bAllowExternalPenaltyCollapses,
            OutStreams,
            OutIndices,
            OutResultError,
            OutPredictedParentCount);
    }
#endif

    bool SphereContainsSphere(const FGroupSphere& Sphere, const FFloat3& ChildCenter, float ChildRadius)
    {
        const float DX = ChildCenter.x - Sphere.Center.x;
        const float DY = ChildCenter.y - Sphere.Center.y;
        const float DZ = ChildCenter.z - Sphere.Center.z;
        return std::sqrt(DX * DX + DY * DY + DZ * DZ) + ChildRadius <= Sphere.Radius + 1e-3f;
    }

    bool ValidateMonotonicErrors(const FClusterDAG& Dag)
    {
        for (const FClusterGroup& Group : Dag.Groups)
        {
            for (const FClusterRef& ChildRef : Group.ChildRefs)
            {
                if (!ChildRef.IsValid() || ChildRef.ClusterIndex >= Dag.Clusters.size() || Dag.Clusters[ChildRef.ClusterIndex].LODError > Group.ParentLODError + 1e-5f)
                {
                    return false;
                }
            }

            for (const FClusterRef& ParentRef : Group.ParentRefs)
            {
                if (!ParentRef.IsValid() || ParentRef.ClusterIndex >= Dag.Clusters.size() || std::abs(Dag.Clusters[ParentRef.ClusterIndex].LODError - Group.ParentLODError) > 1e-5f)
                {
                    return false;
                }
            }
        }

        return true;
    }

    bool ValidateGroupSphereCoverage(size_t PrimitiveIndex, const FClusterDAG& Dag)
    {
        for (size_t GroupIndex = 0; GroupIndex < Dag.Groups.size(); ++GroupIndex)
        {
            const FClusterGroup& Group = Dag.Groups[GroupIndex];
            const FGroupSphere GroupBoundsSphere{ Group.BoundsCenter, Group.BoundsRadius };
            const FGroupSphere GroupSphere{ Group.LodBoundsCenter, Group.LodBoundsRadius };
            for (size_t ChildOrdinal = 0; ChildOrdinal < Group.ChildRefs.size(); ++ChildOrdinal)
            {
                const FClusterRef& ChildRef = Group.ChildRefs[ChildOrdinal];
                if (!ChildRef.IsValid() || ChildRef.ClusterIndex >= Dag.Clusters.size())
                {
                    CLUSTER_DAG_LOG_WARNING(
                        PrimitiveIndex,
                        "ValidateGroupSphereCoverage invalid child ref"
                        << ", groupIndex=" << GroupIndex
                        << ", childOrdinal=" << ChildOrdinal
                        << ", childClusterIndex=" << ChildRef.ClusterIndex);
                    return false;
                }

                const FCluster& ChildCluster = Dag.Clusters[ChildRef.ClusterIndex];
                if (!SphereContainsSphere(GroupBoundsSphere, ChildCluster.Bounds.Center, ChildCluster.Bounds.Radius))
                {
                    const float DX = ChildCluster.Bounds.Center.x - GroupBoundsSphere.Center.x;
                    const float DY = ChildCluster.Bounds.Center.y - GroupBoundsSphere.Center.y;
                    const float DZ = ChildCluster.Bounds.Center.z - GroupBoundsSphere.Center.z;
                    const float ChildDistance = std::sqrt(DX * DX + DY * DY + DZ * DZ);
                    CLUSTER_DAG_LOG_WARNING(
                        PrimitiveIndex,
                        "ValidateGroupSphereCoverage child bounds outside group"
                        << ", groupIndex=" << GroupIndex
                        << ", childOrdinal=" << ChildOrdinal
                        << ", childClusterIndex=" << ChildRef.ClusterIndex
                        << ", groupMip=" << Group.MipLevel
                        << ", groupBoundsRadius=" << FormatFloat(Group.BoundsRadius)
                        << ", childMip=" << ChildCluster.MipLevel
                        << ", childBoundsRadius=" << FormatFloat(ChildCluster.Bounds.Radius)
                        << ", childDistance=" << FormatFloat(ChildDistance)
                        << ", groupBoundsCenter=(" << FormatFloat(Group.BoundsCenter.x) << "," << FormatFloat(Group.BoundsCenter.y) << "," << FormatFloat(Group.BoundsCenter.z) << ")"
                        << ", childBoundsCenter=(" << FormatFloat(ChildCluster.Bounds.Center.x) << "," << FormatFloat(ChildCluster.Bounds.Center.y) << "," << FormatFloat(ChildCluster.Bounds.Center.z) << ")");
                    return false;
                }

                if (!SphereContainsSphere(GroupSphere, ChildCluster.LodBoundsCenter, ChildCluster.LodBoundsRadius))
                {
                    const float DX = ChildCluster.LodBoundsCenter.x - GroupSphere.Center.x;
                    const float DY = ChildCluster.LodBoundsCenter.y - GroupSphere.Center.y;
                    const float DZ = ChildCluster.LodBoundsCenter.z - GroupSphere.Center.z;
                    const float ChildDistance = std::sqrt(DX * DX + DY * DY + DZ * DZ);
                    CLUSTER_DAG_LOG_WARNING(
                        PrimitiveIndex,
                        "ValidateGroupSphereCoverage child outside group"
                        << ", groupIndex=" << GroupIndex
                        << ", childOrdinal=" << ChildOrdinal
                        << ", childClusterIndex=" << ChildRef.ClusterIndex
                        << ", groupMip=" << Group.MipLevel
                        << ", groupRadius=" << FormatFloat(Group.LodBoundsRadius)
                        << ", childMip=" << ChildCluster.MipLevel
                        << ", childRadius=" << FormatFloat(ChildCluster.LodBoundsRadius)
                        << ", childDistance=" << FormatFloat(ChildDistance)
                        << ", groupCenter=(" << FormatFloat(Group.LodBoundsCenter.x) << "," << FormatFloat(Group.LodBoundsCenter.y) << "," << FormatFloat(Group.LodBoundsCenter.z) << ")"
                        << ", childCenter=(" << FormatFloat(ChildCluster.LodBoundsCenter.x) << "," << FormatFloat(ChildCluster.LodBoundsCenter.y) << "," << FormatFloat(ChildCluster.LodBoundsCenter.z) << ")");
                    return false;
                }
            }
        }

        return true;
    }

    float ComputeMaxDistanceToCenter(const FClusterDAG& Dag, const std::vector<uint32_t>& AbsoluteIndices, const FFloat3& Center)
    {
        float MaxDistance = 0.0f;
        for (uint32_t AbsoluteIndex : AbsoluteIndices)
        {
            if (AbsoluteIndex >= Dag.Positions.size())
            {
                continue;
            }

            const FFloat3& Position = Dag.Positions[AbsoluteIndex];
            const float DX = Position.x - Center.x;
            const float DY = Position.y - Center.y;
            const float DZ = Position.z - Center.z;
            MaxDistance = (std::max)(MaxDistance, std::sqrt(DX * DX + DY * DY + DZ * DZ));
        }

        return MaxDistance;
    }

    void LogLeafLodBoundsDiagnostics(size_t PrimitiveIndex, const FClusterDAG& Dag)
    {
        size_t LeafCount = 0;
        size_t CoverageFailureCount = 0;
        size_t BoundsMismatchCount = 0;
        float MinRadius = (std::numeric_limits<float>::max)();
        float MaxRadius = 0.0f;
        float MaxOverflow = 0.0f;
        float MaxCenterDelta = 0.0f;
        float MaxRadiusDelta = 0.0f;
        uint32_t WorstClusterIndex = GClusterDAGInvalidIndex;
        uint32_t MaxDetailedLogs = 0u;

        for (uint32_t ClusterIndex = 0; ClusterIndex < Dag.Clusters.size(); ++ClusterIndex)
        {
            const FCluster& Cluster = Dag.Clusters[ClusterIndex];
            if (Cluster.GeneratingGroupIndex != GClusterDAGInvalidIndex)
            {
                continue;
            }

            ++LeafCount;
            MinRadius = (std::min)(MinRadius, Cluster.LodBoundsRadius);
            MaxRadius = (std::max)(MaxRadius, Cluster.LodBoundsRadius);

            const float CenterDX = Cluster.LodBoundsCenter.x - Cluster.Bounds.Center.x;
            const float CenterDY = Cluster.LodBoundsCenter.y - Cluster.Bounds.Center.y;
            const float CenterDZ = Cluster.LodBoundsCenter.z - Cluster.Bounds.Center.z;
            const float CenterDelta = std::sqrt(CenterDX * CenterDX + CenterDY * CenterDY + CenterDZ * CenterDZ);
            const float RadiusDelta = std::abs(Cluster.LodBoundsRadius - Cluster.Bounds.Radius);
            MaxCenterDelta = (std::max)(MaxCenterDelta, CenterDelta);
            MaxRadiusDelta = (std::max)(MaxRadiusDelta, RadiusDelta);
            if (CenterDelta > 1e-4f || RadiusDelta > 1e-4f)
            {
                ++BoundsMismatchCount;
            }

            const std::vector<uint32_t> AbsoluteIndices = ExtractAbsoluteClusterIndices(Dag, Cluster);
            const float MaxDistance = ComputeMaxDistanceToCenter(Dag, AbsoluteIndices, Cluster.LodBoundsCenter);
            const float Overflow = MaxDistance - Cluster.LodBoundsRadius;
            if (Overflow > MaxOverflow)
            {
                MaxOverflow = Overflow;
                WorstClusterIndex = ClusterIndex;
            }

            if (Overflow > 1e-3f)
            {
                ++CoverageFailureCount;
                if (MaxDetailedLogs < 8u)
                {
                    CLUSTER_DAG_LOG_WARNING(
                        PrimitiveIndex,
                        "Leaf LodBounds coverage mismatch"
                        << ", clusterIndex=" << ClusterIndex
                        << ", mip=" << Cluster.MipLevel
                        << ", triangles=" << Cluster.TriangleCount
                        << ", radius=" << FormatFloat(Cluster.LodBoundsRadius)
                        << ", maxVertexDistance=" << FormatFloat(MaxDistance)
                        << ", overflow=" << FormatFloat(Overflow)
                        << ", centerDelta=" << FormatFloat(CenterDelta)
                        << ", radiusDelta=" << FormatFloat(RadiusDelta)
                        << ", lodCenter=(" << FormatFloat(Cluster.LodBoundsCenter.x) << "," << FormatFloat(Cluster.LodBoundsCenter.y) << "," << FormatFloat(Cluster.LodBoundsCenter.z) << ")"
                        << ", boundsCenter=(" << FormatFloat(Cluster.Bounds.Center.x) << "," << FormatFloat(Cluster.Bounds.Center.y) << "," << FormatFloat(Cluster.Bounds.Center.z) << ")");
                    ++MaxDetailedLogs;
                }
            }
        }

        if (LeafCount == 0)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Leaf LodBounds diagnostics skipped, no leaf clusters were found.");
            return;
        }

        CLUSTER_DAG_LOG_INFO(
            PrimitiveIndex,
            "Leaf LodBounds diagnostics"
            << ", leafCount=" << LeafCount
            << ", minRadius=" << FormatFloat(MinRadius)
            << ", maxRadius=" << FormatFloat(MaxRadius)
            << ", coverageFailures=" << CoverageFailureCount
            << ", boundsMismatches=" << BoundsMismatchCount
            << ", maxOverflow=" << FormatFloat(MaxOverflow)
            << ", maxCenterDelta=" << FormatFloat(MaxCenterDelta)
            << ", maxRadiusDelta=" << FormatFloat(MaxRadiusDelta)
            << ", worstClusterIndex=" << WorstClusterIndex);
    }

    bool BuildClusterDAGForPrimitive(size_t PrimitiveIndex, const FMesh::FPrimitive& Primitive, const FClusterDAGBuildParams& Params, FClusterDAG& OutDag)
    {

// 1. Build initial leaf clusters with meshopt_buildMeshlets.
// 2. Partition clusters into groups with meshopt_partitionClusters.
// 3. Simplify each group with meshopt_simplifyWithAttributes.
// 4. Rebuild meshlet clusters for each simplified group.
// 5. Feed the new parent clusters back into the next reduction level.

        OutDag = {};

#if WITH_MESHOPTIMIZER
        CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "start" << ", vertices=" << Primitive.VertexStreams.Positions.size() << ", indices=" << Primitive.Indices.size() << ", triangles=" << (Primitive.Indices.size() / 3) << ", params{maxVerts=" << Params.MaxClusterVertices << ", maxTris=" << Params.MaxClusterTriangles << ", targetGroupSize=" << Params.TargetGroupSize << ", simplifyRatio=" << FormatFloat(Params.SimplifyRatio) << ", maxLevels=" << Params.MaxLevels << "}");

        if (Primitive.VertexStreams.Positions.empty() || Primitive.Indices.size() < 3 || (Primitive.Indices.size() % 3) != 0)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "invalid primitive input; positions or triangle index count is not valid.");
            return false;
        }

        FBuilderVertexStreams BaseStreams;
        BaseStreams.Positions = Primitive.VertexStreams.Positions;
        BaseStreams.Normals = Primitive.VertexStreams.Normals;
        BaseStreams.UVs = Primitive.VertexStreams.UVs;
        BaseStreams.Tangents = Primitive.VertexStreams.Tangents;
        BaseStreams.Colors = Primitive.VertexStreams.Colors;
        std::vector<uint32_t> BaseIndices = Primitive.Indices;
        CompactAndOptimizeBuilderGeometry(BaseStreams, BaseIndices);
        CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "CompactAndOptimizeGeometry" << ", vertices=" << BaseStreams.Positions.size() << ", indices=" << BaseIndices.size() << ", triangles=" << (BaseIndices.size() / 3));

        if (BaseStreams.Positions.empty() || BaseIndices.size() < 3)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "optimized geometry became empty.");
            return false;
        }

        FBuildState State;
        const uint32_t BaseVertex = AppendVertexStreams(State.Dag, BaseStreams);

        FGroupSphere PrimitiveSphere;
        const meshopt_Bounds PrimitiveBounds = meshopt_computeSphereBounds(
            &BaseStreams.Positions[0].x,
            BaseStreams.Positions.size(),
            sizeof(FFloat3),
            nullptr,
            0);
        PrimitiveSphere.Center = { PrimitiveBounds.center[0], PrimitiveBounds.center[1], PrimitiveBounds.center[2] };
        PrimitiveSphere.Radius = PrimitiveBounds.radius;

        std::vector<uint32_t> CurrentClusters;
        if (!BuildMeshletClustersForGeometry(
            PrimitiveIndex,
            State,
            BaseStreams,
            BaseIndices,
            BaseVertex,
            0,
            -1,
            0.0f,
            PrimitiveSphere.Center,
            PrimitiveSphere.Radius,
            GClusterDAGInvalidIndex,
            GClusterDAGInvalidIndex,
            0.0f,
            Params,
            CurrentClusters))
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "BuildMeshletClustersForGeometry failed for leaf level.");
            return false;
        }

        UpdateClusterExternalEdges(State.Dag, CurrentClusters);

        for (uint32_t ClusterIndex : CurrentClusters)
        {
            FClusterGroup LeafGroup;
            LeafGroup.ChildRefs.emplace_back(ClusterIndex);
            LeafGroup.BoundsCenter = State.Dag.Clusters[ClusterIndex].Bounds.Center;
            LeafGroup.BoundsRadius = State.Dag.Clusters[ClusterIndex].Bounds.Radius;
            LeafGroup.LodBoundsCenter = State.Dag.Clusters[ClusterIndex].Bounds.Center;
            LeafGroup.LodBoundsRadius = State.Dag.Clusters[ClusterIndex].Bounds.Radius;
            LeafGroup.ParentLODError = 0.0f;
            LeafGroup.MipLevel = 0;

            const uint32_t GroupIndex = static_cast<uint32_t>(State.Dag.Groups.size());
            State.Dag.Groups.push_back(std::move(LeafGroup));
            State.Dag.Clusters[ClusterIndex].GroupIndex = GroupIndex;
            State.Dag.Clusters[ClusterIndex].LodBoundsCenter = State.Dag.Groups[GroupIndex].LodBoundsCenter;
            State.Dag.Clusters[ClusterIndex].LodBoundsRadius = State.Dag.Groups[GroupIndex].LodBoundsRadius;
        }

        size_t CurrentTriangleCount = BaseIndices.size() / 3;
        for (uint32_t Level = 1; Level < Params.MaxLevels && CurrentClusters.size() > 1; ++Level)
        {
            CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " begin" << ", inputClusters=" << CurrentClusters.size() << ", inputTriangles=" << CurrentTriangleCount);

            std::vector<std::vector<uint32_t>> ClusterGroups;
            if (CurrentClusters.size() <= GClusterDAGMaxGroupSize)
            {
                ClusterGroups.push_back(CurrentClusters);
                CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " grouping skipped" << ", currentClusters=" << CurrentClusters.size() << ", maxGroupSize=" << GClusterDAGMaxGroupSize << ", " << SummarizeGroupSizes(ClusterGroups));
            }
            else
            {
                std::vector<uint32_t> PartitionClusterIndices;
                std::vector<uint32_t> PartitionClusterIndexCounts;
                PartitionClusterIndexCounts.reserve(CurrentClusters.size());

                for (uint32_t ClusterIndex : CurrentClusters)
                {
                    std::vector<uint32_t> ClusterIndices = ExtractAbsoluteClusterIndices(State.Dag, State.Dag.Clusters[ClusterIndex]);
                    PartitionClusterIndexCounts.push_back(static_cast<uint32_t>(ClusterIndices.size()));
                    PartitionClusterIndices.insert(PartitionClusterIndices.end(), ClusterIndices.begin(), ClusterIndices.end());
                }

                std::vector<unsigned int> Partitions(CurrentClusters.size(), 0);
                const size_t PartitionCount = meshopt_partitionClusters(
                    Partitions.data(),
                    PartitionClusterIndices.data(),
                    PartitionClusterIndices.size(),
                    PartitionClusterIndexCounts.data(),
                    CurrentClusters.size(),
                    &State.Dag.Positions[0].x,
                    State.Dag.Positions.size(),
                    sizeof(FFloat3),
                    Params.TargetGroupSize);

                ClusterGroups.resize((std::max)(PartitionCount, size_t(1)));
                for (size_t ClusterIndex = 0; ClusterIndex < CurrentClusters.size(); ++ClusterIndex)
                {
                    const size_t PartitionIndex = PartitionCount > 0 ? Partitions[ClusterIndex] : 0;
                    ClusterGroups[PartitionIndex].push_back(CurrentClusters[ClusterIndex]);
                }

                CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " meshopt_partitionClusters" << ", inputClusters=" << CurrentClusters.size() << ", targetGroupSize=" << Params.TargetGroupSize << ", partitionCount=" << PartitionCount << ", " << SummarizeGroupSizes(ClusterGroups));

                const uint32_t MinGroupSize = (std::min)(GClusterDAGMinGroupSize, (std::max)(2u, Params.TargetGroupSize));
                const bool bNeedsMerge = ClusterGroups.size() > 1 &&
                    std::any_of(ClusterGroups.begin(), ClusterGroups.end(),
                        [MinGroupSize](const std::vector<uint32_t>& Group)
                        {
                            return !Group.empty() && Group.size() < MinGroupSize;
                        });
                if (bNeedsMerge)
                {
                    MergeSmallPartitions(PrimitiveIndex, Level, State.Dag, CurrentClusters,
                        ComputeClusterAdjacency(State.Dag, CurrentClusters), Params.TargetGroupSize, ClusterGroups);
                }
            }

            std::vector<uint32_t> NextClusters;
            size_t NextTriangleCount = 0;
            bool bReducedAnyGroup = false;

            size_t GroupOrdinal = 0;
            for (const std::vector<uint32_t>& ChildClusters : ClusterGroups)
            {
                if (ChildClusters.empty())
                {
                    continue;
                }

                FMergedClusterScratch Scratch;
                if (!BuildMergedClusterScratch(PrimitiveIndex, Level, GroupOrdinal, State.Dag, ChildClusters, Scratch))
                {
                    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMergedClusterScratch failed" << ", childClusters=" << ChildClusters.size());
                    return false;
                }

                const uint32_t SourceTriangleCount = Scratch.ActiveTriangleCount;
                const uint32_t DesiredParentCount = ComputeDesiredParentCount(SourceTriangleCount, Params);
                const uint32_t MaxAllowedParentCount = ComputeMaxAllowedParentCount(DesiredParentCount, static_cast<uint32_t>(ChildClusters.size()));
                const bool bAllowExternalPenaltyCollapses =
                    ClusterGroups.size() == 1u &&
                    ChildClusters.size() == CurrentClusters.size();
                FBuilderVertexStreams ReducedGroupStreams;
                std::vector<uint32_t> ReducedGroupIndices;
                float SimplifyError = 0.0f;
                uint32_t PredictedParentCount = 0;
                const bool bSimplified = SimplifyMergedClusterGroup(
                    PrimitiveIndex,
                    Level,
                    GroupOrdinal,
                    State.Dag,
                    ChildClusters,
                    Scratch,
                    Params,
                    DesiredParentCount,
                    MaxAllowedParentCount,
                    bAllowExternalPenaltyCollapses,
                    ReducedGroupStreams,
                    ReducedGroupIndices,
                    SimplifyError,
                    PredictedParentCount);
                const bool bGroupReduced = bSimplified && (ReducedGroupIndices.size() / 3) < SourceTriangleCount;
                bReducedAnyGroup = bReducedAnyGroup || bGroupReduced;

                if (!bGroupReduced)
                {
                    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " reduction failed after retries" << ", childClusters=" << ChildClusters.size() << ", sourceTriangles=" << SourceTriangleCount << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << MaxAllowedParentCount << ", predictedParents=" << PredictedParentCount << ", simplifyError=" << FormatFloat(SimplifyError));
                    return false;
                }

                FBuilderVertexStreams GroupStreams = std::move(ReducedGroupStreams);
                std::vector<uint32_t> GroupIndices = std::move(ReducedGroupIndices);

                const FGroupSphere GroupBoundsSphere = BuildGroupSphere(State.Dag, ChildClusters, false);
                const FGroupSphere GroupSphere = BuildGroupSphere(State.Dag, ChildClusters, true);
                float ChildMaxError = 0.0f;
                for (uint32_t ClusterIndex : ChildClusters)
                {
                    ChildMaxError = (std::max)(ChildMaxError, State.Dag.Clusters[ClusterIndex].LODError);
                    ChildMaxError = (std::max)(ChildMaxError, State.ClusterInheritedErrors[ClusterIndex]);
                }

                FClusterGroup Group;
                Group.ChildRefs = MakeClusterRefs(ChildClusters);
                Group.BoundsCenter = GroupBoundsSphere.Center;
                Group.BoundsRadius = GroupBoundsSphere.Radius;
                Group.LodBoundsCenter = GroupSphere.Center;
                Group.LodBoundsRadius = GroupSphere.Radius;
                Group.ParentLODError = (std::max)(ChildMaxError, SimplifyError);
                Group.MipLevel = Level - 1;

                const uint32_t GroupIndex = static_cast<uint32_t>(State.Dag.Groups.size());
                State.Dag.Groups.push_back(Group);

                for (uint32_t ClusterIndex : ChildClusters)
                {
                    State.Dag.Clusters[ClusterIndex].GroupIndex = GroupIndex;
                    State.ClusterInheritedErrors[ClusterIndex] = Group.ParentLODError;
                }

                const uint32_t GroupBaseVertex = AppendVertexStreams(State.Dag, GroupStreams);
                std::vector<uint32_t> ParentClusters;
                if (!BuildMeshletClustersForGeometry(
                    PrimitiveIndex,
                    State,
                    GroupStreams,
                    GroupIndices,
                    GroupBaseVertex,
                    Level,
                    static_cast<int32_t>(GroupOrdinal),
                    Group.ParentLODError,
                    GroupSphere.Center,
                    GroupSphere.Radius,
                    GClusterDAGInvalidIndex,
                    GroupIndex,
                    0.0f,
                    Params,
                    ParentClusters))
                {
                    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " BuildMeshletClustersForGeometry failed" << ", groupIndex=" << GroupIndex << ", childClusters=" << ChildClusters.size() << ", simplified=" << (bSimplified ? "true" : "false") << ", simplifyError=" << FormatFloat(SimplifyError));
                    return false;
                }

                const uint32_t ParentBudgetForValidation = MaxAllowedParentCount;
                const bool bWithinParentBudget = ParentClusters.size() <= ParentBudgetForValidation;
                const bool bActualParentReduction = ParentClusters.size() < ChildClusters.size();
                if (ParentClusters.empty() || !bActualParentReduction)
                {
                    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " produced invalid parent reduction" << ", childClusters=" << ChildClusters.size() << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << ParentBudgetForValidation << ", predictedParents=" << PredictedParentCount << ", actualParents=" << ParentClusters.size() << ", outputTriangles=" << (GroupIndices.size() / 3));
                    return false;
                }

                if (!bWithinParentBudget)
                {
                    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " accepted over-budget parent reduction" << ", childClusters=" << ChildClusters.size() << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << ParentBudgetForValidation << ", predictedParents=" << PredictedParentCount << ", actualParents=" << ParentClusters.size() << ", outputTriangles=" << (GroupIndices.size() / 3));
                }

                State.Dag.Groups[GroupIndex].ParentRefs = MakeClusterRefs(ParentClusters);
                for (uint32_t ParentClusterIndex : ParentClusters)
                {
                    State.Dag.Clusters[ParentClusterIndex].LodBoundsCenter = GroupSphere.Center;
                    State.Dag.Clusters[ParentClusterIndex].LodBoundsRadius = GroupSphere.Radius;
                    NextTriangleCount += State.Dag.Clusters[ParentClusterIndex].TriangleCount;
                }

                NextClusters.insert(NextClusters.end(), ParentClusters.begin(), ParentClusters.end());
                CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " group " << GroupOrdinal << " result" << ", childClusters=" << ChildClusters.size() << ", desiredParents=" << DesiredParentCount << ", maxAllowedParents=" << ParentBudgetForValidation << ", predictedParents=" << PredictedParentCount << ", parentClusters=" << ParentClusters.size() << ", outputTriangles=" << (GroupIndices.size() / 3) << ", simplifyError=" << FormatFloat(SimplifyError) << ", relativeError=" << FormatFloat(ComputeRelativeError(SimplifyError, GroupSphere.Radius)) << ", childMaxError=" << FormatFloat(ChildMaxError) << ", parentError=" << FormatFloat(Group.ParentLODError) << ", lodRadius=" << FormatFloat(GroupSphere.Radius));
                ++GroupOrdinal;
            }

            if (NextClusters.empty())
            {
                CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " produced no parent clusters.");
                return false;
            }

            if (NextClusters.size() >= CurrentClusters.size() && NextTriangleCount >= CurrentTriangleCount)
            {
                CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " made no convergence progress" << ", nextClusters=" << NextClusters.size() << ", currentClusters=" << CurrentClusters.size() << ", nextTriangles=" << NextTriangleCount << ", currentTriangles=" << CurrentTriangleCount << ", reducedAnyGroup=" << (bReducedAnyGroup ? "true" : "false"));
                return false;
            }

            CurrentClusters = std::move(NextClusters);
            CurrentTriangleCount = NextTriangleCount;
            UpdateClusterExternalEdges(State.Dag, CurrentClusters);
            CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " complete" << ", outputClusters=" << CurrentClusters.size() << ", outputTriangles=" << CurrentTriangleCount << ", totalDagClusters=" << State.Dag.Clusters.size() << ", totalDagGroups=" << State.Dag.Groups.size());
        }

        if (CurrentClusters.empty())
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "no root clusters remained after reduction.");
            return false;
        }

        if (CurrentClusters.size() != 1)
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "failed to converge to a single root cluster" << ", remainingClusters=" << CurrentClusters.size() << ", maxLevels=" << Params.MaxLevels);
            return false;
        }

        FClusterGroup RootGroup;
        const FGroupSphere RootBoundsSphere = BuildGroupSphere(State.Dag, CurrentClusters, false);
        const FGroupSphere RootSphere = BuildGroupSphere(State.Dag, CurrentClusters, true);
        RootGroup.ChildRefs = MakeClusterRefs(CurrentClusters);
        RootGroup.BoundsCenter = RootBoundsSphere.Center;
        RootGroup.BoundsRadius = RootBoundsSphere.Radius;
        RootGroup.LodBoundsCenter = RootSphere.Center;
        RootGroup.LodBoundsRadius = RootSphere.Radius;
        RootGroup.ParentLODError = 1e10f;
        RootGroup.MipLevel = 0;
        RootGroup.bRoot = true;

        for (uint32_t ClusterIndex : CurrentClusters)
        {
            RootGroup.MipLevel = (std::max)(RootGroup.MipLevel, State.Dag.Clusters[ClusterIndex].MipLevel);
        }

        const uint32_t RootGroupIndex = static_cast<uint32_t>(State.Dag.Groups.size());
        State.Dag.Groups.push_back(std::move(RootGroup));
        for (uint32_t ClusterIndex : CurrentClusters)
        {
            State.Dag.Clusters[ClusterIndex].GroupIndex = RootGroupIndex;
        }

        OutDag = std::move(State.Dag);
        OutDag.RootGroupIndex = RootGroupIndex;

        if (!ValidateMonotonicErrors(OutDag))
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "ValidateMonotonicErrors failed; discarding DAG.");
            OutDag = {};
            return false;
        }

        if (!ValidateGroupSphereCoverage(PrimitiveIndex, OutDag))
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "ValidateGroupSphereCoverage failed; discarding DAG.");
            OutDag = {};
            return false;
        }

        LogLeafLodBoundsDiagnostics(PrimitiveIndex, OutDag);

        EncodeRuntimeClusterHierarchy(OutDag);
        if (!OutDag.HasRuntimeHierarchy())
        {
            CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "EncodeRuntimeClusterHierarchy failed; discarding DAG.");
            OutDag = {};
            return false;
        }

        CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "completed" << ", rootGroupIndex=" << OutDag.RootGroupIndex << ", rootChildren=" << OutDag.Groups[OutDag.RootGroupIndex].ChildRefs.size() << ", clusters=" << OutDag.Clusters.size() << ", groups=" << OutDag.Groups.size() << ", runtimeClusters=" << OutDag.RuntimeHierarchy.Clusters.size() << ", runtimeGroups=" << OutDag.RuntimeHierarchy.Groups.size() << ", sharedVertices=" << OutDag.Positions.size() << ", sharedTriangles=" << (OutDag.TriangleIndices.size() / 3));

        return OutDag.IsValid();
#else
        CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "meshoptimizer is unavailable; builder is disabled.");
        (void)Primitive;
        (void)Params;
        return false;
#endif
    }
}

uint64_t HashClusterDAGBuildParams(const FClusterDAGBuildParams& Params)
{
    static_assert(std::is_trivially_copyable_v<FClusterDAGBuildParams>);
    struct FClusterDAGBuildHashPayload
    {
        FClusterDAGBuildParams Params;
        uint32_t SemanticVersion = GClusterDAGBuildSemanticVersion;
    };
    static_assert(std::is_trivially_copyable_v<FClusterDAGBuildHashPayload>);

    FClusterDAGBuildHashPayload Payload;
    Payload.Params = Params;
    return HashBytes(&Payload, sizeof(Payload));
}

bool FClusterDAG::HasRootGroup() const
{
    return RootGroupIndex != GClusterDAGInvalidIndex && RootGroupIndex < Groups.size();
}

const FClusterGroup* FClusterDAG::GetRootGroup() const
{
    return HasRootGroup() ? &Groups[RootGroupIndex] : nullptr;
}

void FClusterDAG::FindCut(std::vector<FClusterRef>& OutSelectedClusters, const FClusterCutParams& Params) const
{
    OutSelectedClusters.clear();

    const FClusterGroup* RootGroup = GetRootGroup();
    if (RootGroup == nullptr || RootGroup->ChildRefs.empty())
    {
        return;
    }

    auto LargestError = [this](const FClusterRef& A, const FClusterRef& B)
    {
        return Clusters[A.ClusterIndex].LODError > Clusters[B.ClusterIndex].LODError;
    };

    std::unordered_set<uint32_t> ExpandedGroups;
    ExpandedGroups.reserve(Groups.size());

    uint32_t NumTriangles = 0;
    float MinError = 0.0f;

    for (const FClusterRef& ChildRef : RootGroup->ChildRefs)
    {
        if (!ChildRef.IsValid() || ChildRef.ClusterIndex >= Clusters.size())
        {
            continue;
        }

        OutSelectedClusters.push_back(ChildRef);
        const FCluster& RootCluster = Clusters[ChildRef.ClusterIndex];
        NumTriangles += RootCluster.TriangleCount;
        MinError = RootCluster.LODError;
    }

    if (OutSelectedClusters.empty())
    {
        return;
    }

    ExpandedGroups.insert(RootGroupIndex);
    std::make_heap(OutSelectedClusters.begin(), OutSelectedClusters.end(), LargestError);

    const uint32_t MaxTriangleBudget = (std::numeric_limits<uint32_t>::max)();
    if (Params.TargetNumTriangles == MaxTriangleBudget && Params.TargetError <= 0.0f)
    {
        std::sort_heap(OutSelectedClusters.begin(), OutSelectedClusters.end(), LargestError);
        return;
    }

    while (!OutSelectedClusters.empty())
    {
        const FClusterRef ClusterRef = OutSelectedClusters.front();
        if (!ClusterRef.IsValid() || ClusterRef.ClusterIndex >= Clusters.size())
        {
            std::pop_heap(OutSelectedClusters.begin(), OutSelectedClusters.end(), LargestError);
            OutSelectedClusters.pop_back();
            continue;
        }

        const FCluster& Cluster = Clusters[ClusterRef.ClusterIndex];
        if (Cluster.MipLevel == 0 || Cluster.GeneratingGroupIndex == GClusterDAGInvalidIndex)
        {
            break;
        }

        const float LODError = Cluster.LODError;
        const uint32_t GeneratingGroupIndex = Cluster.GeneratingGroupIndex;
        if (GeneratingGroupIndex >= Groups.size())
        {
            break;
        }

        const FClusterGroup& GeneratingGroup = Groups[GeneratingGroupIndex];
        uint32_t ChildTriangleCount = 0;
        for (const FClusterRef& ChildRef : GeneratingGroup.ChildRefs)
        {
            if (!ChildRef.IsValid() || ChildRef.ClusterIndex >= Clusters.size())
            {
                continue;
            }

            ChildTriangleCount += Clusters[ChildRef.ClusterIndex].TriangleCount;
        }

        const uint32_t AllowedTriangles =
            Params.TargetNumTriangles == MaxTriangleBudget
            ? MaxTriangleBudget
            : Params.TargetNumTriangles + Params.TargetOvershoot;
        const bool bNeedsRefineForError = Params.TargetError > 0.0f && LODError > Params.TargetError;
        const bool bNeedsRefineForTriangles =
            Params.TargetNumTriangles != MaxTriangleBudget &&
            ChildTriangleCount > Cluster.TriangleCount &&
            NumTriangles < Params.TargetNumTriangles &&
            NumTriangles + (ChildTriangleCount - Cluster.TriangleCount) <= AllowedTriangles;

        if (!bNeedsRefineForError && !bNeedsRefineForTriangles)
        {
            break;
        }

        std::pop_heap(OutSelectedClusters.begin(), OutSelectedClusters.end(), LargestError);
        OutSelectedClusters.pop_back();
        NumTriangles -= Cluster.TriangleCount;
        MinError = LODError;

        const auto [_, bInserted] = ExpandedGroups.insert(GeneratingGroupIndex);
        if (!bInserted)
        {
            continue;
        }

        for (const FClusterRef& ChildRef : GeneratingGroup.ChildRefs)
        {
            if (!ChildRef.IsValid() || ChildRef.ClusterIndex >= Clusters.size())
            {
                continue;
            }

            const FCluster& ChildCluster = Clusters[ChildRef.ClusterIndex];
            assert(ChildCluster.MipLevel < Cluster.MipLevel);
            assert(ChildCluster.LODError <= MinError + 1e-5f);
            OutSelectedClusters.push_back(ChildRef);
            std::push_heap(OutSelectedClusters.begin(), OutSelectedClusters.end(), LargestError);
            NumTriangles += ChildCluster.TriangleCount;
        }
    }

    std::sort_heap(OutSelectedClusters.begin(), OutSelectedClusters.end(), LargestError);
}

bool BuildClusterDAGPackedVertexData(const FClusterDAG& Dag, FClusterDAGPackedVertexData& OutPackedVertexData)
{
    OutPackedVertexData = {};

    const size_t VertexCount = Dag.Positions.size();
    if (VertexCount == 0)
    {
        return false;
    }

    std::vector<FFloat3> Normals(VertexCount, FFloat3(0.0f, 0.0f, 1.0f));
    if (Dag.Normals.size() == VertexCount)
    {
        Normals = Dag.Normals;
    }

    FFloat3 Min = Dag.Positions[0];
    FFloat3 Max = Dag.Positions[0];
    for (const FFloat3& Position : Dag.Positions)
    {
        Min.x = (std::min)(Min.x, Position.x);
        Min.y = (std::min)(Min.y, Position.y);
        Min.z = (std::min)(Min.z, Position.z);
        Max.x = (std::max)(Max.x, Position.x);
        Max.y = (std::max)(Max.y, Position.y);
        Max.z = (std::max)(Max.z, Position.z);
    }

    const FFloat3 Extent =
    {
        Max.x - Min.x,
        Max.y - Min.y,
        Max.z - Min.z
    };
    const FFloat3 Scale =
    {
        Extent.x > 0.0f ? Extent.x / 65535.0f : 0.0f,
        Extent.y > 0.0f ? Extent.y / 65535.0f : 0.0f,
        Extent.z > 0.0f ? Extent.z / 65535.0f : 0.0f
    };

    OutPackedVertexData.PositionOffset = FFloat4(Min.x, Min.y, Min.z, 0.0f);
    OutPackedVertexData.PositionScale = FFloat4(Scale.x, Scale.y, Scale.z, 0.0f);
    OutPackedVertexData.Positions.resize(VertexCount);
    OutPackedVertexData.Normals.resize(VertexCount);

    auto QuantizePosition = [](float Position, float Offset, float ScaleValue)
    {
        if (ScaleValue <= 0.0f)
        {
            return 0u;
        }

        const float Quantized = (Position - Offset) / ScaleValue;
        const long QuantizedInt = std::lround(Quantized);
        const long Clamped = (std::clamp)(QuantizedInt, 0l, 65535l);
        return static_cast<uint32_t>(Clamped);
    };

    std::vector<uint32_t> PackedUVs;
    PackedUVs.reserve(VertexCount);
    if (Dag.UVs.size() == VertexCount)
    {
        for (const FFloat2& UV : Dag.UVs)
        {
            PackedUVs.push_back(PackHalf2(UV));
        }

        if (AllPackedValuesEqual(PackedUVs))
        {
            OutPackedVertexData.ConstantUV = UnpackHalf2(PackedUVs.front());
        }
        else
        {
            OutPackedVertexData.UVs = std::move(PackedUVs);
        }
    }

    std::vector<uint32_t> PackedColors;
    PackedColors.reserve(VertexCount);
    if (Dag.Colors.size() == VertexCount)
    {
        for (const FFloat4& Color : Dag.Colors)
        {
            PackedColors.push_back(PackRgba8(Color));
        }

        if (AllPackedValuesEqual(PackedColors))
        {
            OutPackedVertexData.ConstantColor = UnpackRgba8(PackedColors.front());
        }
        else
        {
            OutPackedVertexData.Colors = std::move(PackedColors);
        }
    }

    bool bStoreTangents = false;
    if (Dag.Tangents.size() == VertexCount)
    {
        for (size_t VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
        {
            const uint32_t PackedTangent = PackTangent(Dag.Tangents[VertexIndex], Normals[VertexIndex]);
            const uint32_t FallbackPackedTangent = PackTangent(FFloat4(0.0f, 0.0f, 0.0f, 1.0f), Normals[VertexIndex]);
            if (PackedTangent != FallbackPackedTangent)
            {
                bStoreTangents = true;
                break;
            }
        }
    }

    if (bStoreTangents)
    {
        OutPackedVertexData.Tangents.resize(VertexCount);
    }

    for (size_t VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
    {
        const FFloat3& Position = Dag.Positions[VertexIndex];
        const uint32_t X = QuantizePosition(Position.x, Min.x, Scale.x);
        const uint32_t Y = QuantizePosition(Position.y, Min.y, Scale.y);
        const uint32_t Z = QuantizePosition(Position.z, Min.z, Scale.z);
        OutPackedVertexData.Positions[VertexIndex].XY = X | (Y << 16u);
        OutPackedVertexData.Positions[VertexIndex].Z = Z;
        OutPackedVertexData.Normals[VertexIndex] = EncodeOctahedral16x2(Normals[VertexIndex]);
        if (!OutPackedVertexData.Tangents.empty())
        {
            OutPackedVertexData.Tangents[VertexIndex] = PackTangent(Dag.Tangents[VertexIndex], Normals[VertexIndex]);
        }
    }

    return true;
}

bool LoadClusterDAGCacheFile(
    const std::wstring& CacheFilePath,
    const std::wstring& SourceFilePath,
    const FClusterDAGBuildParams& Params,
    std::vector<std::vector<FClusterDAG>>& OutMeshClusterDAGs)
{
    OutMeshClusterDAGs.clear();

    uint64_t SourceWriteTime = 0;
    uint64_t SourceFileSize = 0;
    if (!GetFileSignature(SourceFilePath, SourceWriteTime, SourceFileSize))
    {
        return false;
    }

    std::ifstream Stream(std::filesystem::path(CacheFilePath), std::ios::binary);
    if (!Stream.is_open())
    {
        return false;
    }

    FVmeshCacheHeader Header;
    if (!ReadValue(Stream, Header))
    {
        return false;
    }

    if (Header.Magic[0] != 'V' || Header.Magic[1] != 'M' || Header.Magic[2] != 'E' || Header.Magic[3] != 'S')
    {
        return false;
    }

    if (Header.Version != GVmeshVersion
        || Header.SourceWriteTime != SourceWriteTime
        || Header.SourceFileSize != SourceFileSize
        || Header.ParamsHash != HashClusterDAGBuildParams(Params))
    {
        return false;
    }

    OutMeshClusterDAGs.resize(Header.MeshCount);
    for (uint32_t MeshIndex = 0; MeshIndex < Header.MeshCount; ++MeshIndex)
    {
        uint32_t DagCount = 0;
        if (!ReadValue(Stream, DagCount))
        {
            OutMeshClusterDAGs.clear();
            return false;
        }

        std::vector<FClusterDAG>& MeshDAGs = OutMeshClusterDAGs[MeshIndex];
        MeshDAGs.resize(DagCount);
        for (uint32_t DagIndex = 0; DagIndex < DagCount; ++DagIndex)
        {
            FVmeshDagHeader DagHeader;
            if (!ReadValue(Stream, DagHeader))
            {
                OutMeshClusterDAGs.clear();
                return false;
            }

            FClusterDAG& Dag = MeshDAGs[DagIndex];
            Dag.RootGroupIndex = DagHeader.RootGroupIndex;
            if (!ReadPodVector(Stream, Dag.Clusters, DagHeader.ClusterCount)
                || !ReadPodVector(Stream, Dag.Positions, DagHeader.PositionCount)
                || !ReadPodVector(Stream, Dag.Normals, DagHeader.NormalCount)
                || !ReadPodVector(Stream, Dag.UVs, DagHeader.UVCount)
                || !ReadPodVector(Stream, Dag.Tangents, DagHeader.TangentCount)
                || !ReadPodVector(Stream, Dag.Colors, DagHeader.ColorCount)
                || !ReadPodVector(Stream, Dag.TriangleIndices, DagHeader.TriangleIndexCount)
                || !ReadPodVector(Stream, Dag.ExternalEdges, DagHeader.ExternalEdgeCount)
                || !ReadPodVector(Stream, Dag.ClusterVertices, DagHeader.ClusterVertexCount)
                || !ReadPodVector(Stream, Dag.RuntimeHierarchy.Groups, DagHeader.RuntimeGroupCount)
                || !ReadPodVector(Stream, Dag.RuntimeHierarchy.Clusters, DagHeader.RuntimeClusterCount)
                || !ReadPodVector(Stream, Dag.RuntimeHierarchy.ChildRefs, DagHeader.RuntimeChildRefCount)
                || !ReadPodVector(Stream, Dag.RuntimeHierarchy.DrawDatas, DagHeader.RuntimeDrawDataCount)
                || !ReadPodVector(Stream, Dag.PackedVertexData.Positions, DagHeader.PackedPositionCount)
                || !ReadPodVector(Stream, Dag.PackedVertexData.Normals, DagHeader.PackedNormalCount)
                || !ReadPodVector(Stream, Dag.PackedVertexData.UVs, DagHeader.PackedUVCount)
                || !ReadPodVector(Stream, Dag.PackedVertexData.Tangents, DagHeader.PackedTangentCount)
                || !ReadPodVector(Stream, Dag.PackedVertexData.Colors, DagHeader.PackedColorCount))
            {
                OutMeshClusterDAGs.clear();
                return false;
            }

            Dag.RuntimeHierarchy.RootGroupIndex = DagHeader.RuntimeRootGroupIndex;
            Dag.PackedVertexData.PositionOffset = DagHeader.PackedPositionOffset;
            Dag.PackedVertexData.PositionScale = DagHeader.PackedPositionScale;
            Dag.PackedVertexData.ConstantUV = FFloat2(DagHeader.PackedConstantUV.x, DagHeader.PackedConstantUV.y);
            Dag.PackedVertexData.ConstantColor = DagHeader.PackedConstantColor;
            if (!Dag.PackedVertexData.IsValid() && !Dag.Positions.empty())
            {
                BuildClusterDAGPackedVertexData(Dag, Dag.PackedVertexData);
            }
            if (!RebuildRuntimeClusterDrawData(Dag))
            {
                OutMeshClusterDAGs.clear();
                return false;
            }

            Dag.Groups.resize(DagHeader.GroupCount);
            for (uint32_t GroupIndex = 0; GroupIndex < DagHeader.GroupCount; ++GroupIndex)
            {
                FClusterGroup& Group = Dag.Groups[GroupIndex];
                if (!ReadValue(Stream, Group.BoundsCenter)
                    || !ReadValue(Stream, Group.BoundsRadius)
                    || !ReadValue(Stream, Group.LodBoundsCenter)
                    || !ReadValue(Stream, Group.LodBoundsRadius)
                    || !ReadValue(Stream, Group.ParentLODError)
                    || !ReadValue(Stream, Group.MipLevel)
                    || !ReadValue(Stream, Group.bRoot))
                {
                    OutMeshClusterDAGs.clear();
                    return false;
                }

                uint32_t ChildCount = 0;
                uint32_t ParentCount = 0;
                if (!ReadValue(Stream, ChildCount)
                    || !ReadValue(Stream, ParentCount)
                    || !ReadPodVector(Stream, Group.ChildRefs, ChildCount)
                    || !ReadPodVector(Stream, Group.ParentRefs, ParentCount))
                {
                    OutMeshClusterDAGs.clear();
                    return false;
                }
            }

            const bool bEmptyDag =
                Dag.Clusters.empty() &&
                Dag.Groups.empty() &&
                Dag.Positions.empty() &&
                Dag.Normals.empty() &&
                Dag.UVs.empty() &&
                Dag.Tangents.empty() &&
                Dag.Colors.empty() &&
                Dag.TriangleIndices.empty() &&
                Dag.ExternalEdges.empty() &&
                Dag.ClusterVertices.empty() &&
                Dag.RootGroupIndex == GClusterDAGInvalidIndex;
            if (!bEmptyDag && (!Dag.IsValid() || !Dag.HasRuntimeHierarchy()))
            {
                OutMeshClusterDAGs.clear();
                return false;
            }
        }
    }

    LogInfo("Cluster DAG cache metadata loaded: path="
        + StringUtils::WideToUtf8(CacheFilePath)
        + ", meshes="
        + std::to_string(Header.MeshCount));

    return true;
}

bool SaveClusterDAGCacheFile(
    const std::wstring& CacheFilePath,
    const std::wstring& SourceFilePath,
    const FClusterDAGBuildParams& Params,
    const std::vector<std::vector<FClusterDAG>>& MeshClusterDAGs)
{
    uint64_t SourceWriteTime = 0;
    uint64_t SourceFileSize = 0;
    if (!GetFileSignature(SourceFilePath, SourceWriteTime, SourceFileSize))
    {
        return false;
    }

    FVmeshCacheHeader Header;
    Header.SourceWriteTime = SourceWriteTime;
    Header.SourceFileSize = SourceFileSize;
    Header.ParamsHash = HashClusterDAGBuildParams(Params);
    Header.MeshCount = static_cast<uint32_t>(MeshClusterDAGs.size());

    const std::filesystem::path TargetPath(CacheFilePath);
    const std::filesystem::path TempPath = TargetPath.wstring() + L".tmp";
    std::error_code ErrorCode;
    if (!TargetPath.parent_path().empty())
    {
        std::filesystem::create_directories(TargetPath.parent_path(), ErrorCode);
    }

    std::ofstream Stream(TempPath, std::ios::binary | std::ios::trunc);
    if (!Stream.is_open())
    {
        return false;
    }

    if (!WriteValue(Stream, Header))
    {
        return false;
    }

    for (const std::vector<FClusterDAG>& MeshDAGs : MeshClusterDAGs)
    {
        const uint32_t DagCount = static_cast<uint32_t>(MeshDAGs.size());
        if (!WriteValue(Stream, DagCount))
        {
            return false;
        }

        for (const FClusterDAG& Dag : MeshDAGs)
        {
            FClusterDAGPackedVertexData PackedVertexData = Dag.PackedVertexData;
            if (!PackedVertexData.IsValid())
            {
                BuildClusterDAGPackedVertexData(Dag, PackedVertexData);
            }

            FVmeshDagHeader DagHeader;
            DagHeader.ClusterCount = static_cast<uint32_t>(Dag.Clusters.size());
            DagHeader.GroupCount = static_cast<uint32_t>(Dag.Groups.size());
            DagHeader.PositionCount = 0;
            DagHeader.NormalCount = 0;
            DagHeader.UVCount = 0;
            DagHeader.TangentCount = 0;
            DagHeader.ColorCount = 0;
            DagHeader.TriangleIndexCount = static_cast<uint32_t>(Dag.TriangleIndices.size());
            DagHeader.ExternalEdgeCount = static_cast<uint32_t>(Dag.ExternalEdges.size());
            DagHeader.ClusterVertexCount = static_cast<uint32_t>(Dag.ClusterVertices.size());
            DagHeader.RuntimeGroupCount = static_cast<uint32_t>(Dag.RuntimeHierarchy.Groups.size());
            DagHeader.RuntimeClusterCount = static_cast<uint32_t>(Dag.RuntimeHierarchy.Clusters.size());
            DagHeader.RuntimeChildRefCount = static_cast<uint32_t>(Dag.RuntimeHierarchy.ChildRefs.size());
            DagHeader.RuntimeDrawDataCount = static_cast<uint32_t>(Dag.RuntimeHierarchy.DrawDatas.size());
            DagHeader.RuntimePackedIndexCount = 0;
            DagHeader.PackedPositionCount = static_cast<uint32_t>(PackedVertexData.Positions.size());
            DagHeader.PackedNormalCount = static_cast<uint32_t>(PackedVertexData.Normals.size());
            DagHeader.PackedUVCount = static_cast<uint32_t>(PackedVertexData.UVs.size());
            DagHeader.PackedTangentCount = static_cast<uint32_t>(PackedVertexData.Tangents.size());
            DagHeader.PackedColorCount = static_cast<uint32_t>(PackedVertexData.Colors.size());
            DagHeader.RuntimeRootGroupIndex = Dag.RuntimeHierarchy.RootGroupIndex;
            DagHeader.RootGroupIndex = Dag.RootGroupIndex;
            DagHeader.PackedPositionOffset = PackedVertexData.PositionOffset;
            DagHeader.PackedPositionScale = PackedVertexData.PositionScale;
            DagHeader.PackedConstantUV = FFloat4(PackedVertexData.ConstantUV.x, PackedVertexData.ConstantUV.y, 0.0f, 0.0f);
            DagHeader.PackedConstantColor = PackedVertexData.ConstantColor;

            static const std::vector<FFloat3> EmptyFloat3s;
            static const std::vector<FFloat2> EmptyFloat2s;
            static const std::vector<FFloat4> EmptyFloat4s;
            static const std::vector<uint32_t> EmptyUint32s;

            if (!WriteValue(Stream, DagHeader)
                || !WritePodVector(Stream, Dag.Clusters)
                || !WritePodVector(Stream, EmptyFloat3s)
                || !WritePodVector(Stream, EmptyFloat3s)
                || !WritePodVector(Stream, EmptyFloat2s)
                || !WritePodVector(Stream, EmptyFloat4s)
                || !WritePodVector(Stream, EmptyFloat4s)
                || !WritePodVector(Stream, Dag.TriangleIndices)
                || !WritePodVector(Stream, Dag.ExternalEdges)
                || !WritePodVector(Stream, Dag.ClusterVertices)
                || !WritePodVector(Stream, Dag.RuntimeHierarchy.Groups)
                || !WritePodVector(Stream, Dag.RuntimeHierarchy.Clusters)
                || !WritePodVector(Stream, Dag.RuntimeHierarchy.ChildRefs)
                || !WritePodVector(Stream, Dag.RuntimeHierarchy.DrawDatas)
                || !WritePodVector(Stream, PackedVertexData.Positions)
                || !WritePodVector(Stream, PackedVertexData.Normals)
                || !WritePodVector(Stream, PackedVertexData.UVs)
                || !WritePodVector(Stream, PackedVertexData.Tangents)
                || !WritePodVector(Stream, PackedVertexData.Colors))
            {
                return false;
            }

            for (const FClusterGroup& Group : Dag.Groups)
            {
                const uint32_t ChildCount = static_cast<uint32_t>(Group.ChildRefs.size());
                const uint32_t ParentCount = static_cast<uint32_t>(Group.ParentRefs.size());
                if (!WriteValue(Stream, Group.BoundsCenter)
                    || !WriteValue(Stream, Group.BoundsRadius)
                    || !WriteValue(Stream, Group.LodBoundsCenter)
                    || !WriteValue(Stream, Group.LodBoundsRadius)
                    || !WriteValue(Stream, Group.ParentLODError)
                    || !WriteValue(Stream, Group.MipLevel)
                    || !WriteValue(Stream, Group.bRoot)
                    || !WriteValue(Stream, ChildCount)
                    || !WriteValue(Stream, ParentCount)
                    || !WritePodVector(Stream, Group.ChildRefs)
                    || !WritePodVector(Stream, Group.ParentRefs))
                {
                    return false;
                }
            }
        }
    }

    Stream.close();
    if (!Stream.good())
    {
        std::filesystem::remove(TempPath, ErrorCode);
        return false;
    }

    std::filesystem::rename(TempPath, TargetPath, ErrorCode);
    if (ErrorCode)
    {
        std::filesystem::remove(TargetPath, ErrorCode);
        ErrorCode.clear();
        std::filesystem::rename(TempPath, TargetPath, ErrorCode);
    }

    if (ErrorCode)
    {
        std::filesystem::remove(TempPath, ErrorCode);
        return false;
    }

    LogInfo("Cluster DAG cache metadata saved: path="
        + StringUtils::WideToUtf8(CacheFilePath)
        + ", meshes="
        + std::to_string(Header.MeshCount));

    return true;
}

void FMesh::BuildClusterDAGs(const FClusterDAGBuildParams& Params)
{
    ClusterDAGs.clear();
    ClusterDAGs.reserve(Primitives.size());

    for (size_t PrimitiveIndex = 0; PrimitiveIndex < Primitives.size(); ++PrimitiveIndex)
    {
        FClusterDAG Dag;
        if (!BuildClusterDAGForPrimitive(PrimitiveIndex, Primitives[PrimitiveIndex], Params, Dag) && !Primitives[PrimitiveIndex].Indices.empty())
        {
            std::ostringstream Stream;
            Stream << "Cluster DAG build skipped for primitive[" << PrimitiveIndex << "]; keeping legacy meshlet path only.";
            LogWarning(Stream.str());
        }

        ClusterDAGs.push_back(std::move(Dag));
    }
}

const FClusterDAG* FMesh::GetClusterDAG(size_t Index) const
{
    if (Index >= ClusterDAGs.size())
    {
        return nullptr;
    }
    return &ClusterDAGs[Index];
}

bool FMesh::HasClusterDAGs() const
{
    for (const FClusterDAG& Dag : ClusterDAGs)
    {
        if (Dag.IsValid())
        {
            return true;
        }
    }

    return false;
}
