#pragma once

#include "../Render/GpuResource.h"
#include "../Math/MathTypes.h"
#include "../Scene/Mesh.h"
#include "../Scene/ClusterDAG.h"
#include "MeshMaterial.h"

#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <wrl.h>
#include <d3d12.h>

constexpr uint32_t kMeshVertexStreamCount = 7;
constexpr uint32_t kMeshVertexStreamPosition = 0;
constexpr uint32_t kMeshVertexStreamNormal = 1;
constexpr uint32_t kMeshVertexStreamUv = 2;
constexpr uint32_t kMeshVertexStreamTangent = 3;
constexpr uint32_t kMeshVertexStreamColor = 4;
constexpr uint32_t kMeshVertexStreamJoints = 5;
constexpr uint32_t kMeshVertexStreamWeights = 6;
constexpr uint32_t kClusterDagVertexStreamCount = 4;
constexpr uint32_t kClusterDagVertexStreamPosition = 0;
constexpr uint32_t kClusterDagVertexStreamNormal = 1;
constexpr uint32_t kClusterDagVertexStreamUv = 2;
constexpr uint32_t kClusterDagVertexStreamTangent = 3;

struct FMeshGeometryBuffers
{
    std::array<FBindlessBuffer, kMeshVertexStreamCount> VertexBuffers;
    FBindlessBuffer IndexBuffer;
    std::array<D3D12_VERTEX_BUFFER_VIEW, kMeshVertexStreamCount> VertexBufferViews{};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
    uint32_t VertexBufferCount = 0;
    uint32_t IndexCount = 0;
};

using FCubeGeometryBuffers = FMeshGeometryBuffers;

struct FMeshletDrawData
{
    uint32_t StartIndex;
    uint32_t IndexCount;
    uint32_t RangeIndex;
    uint32_t GroupIndex;

    static FMeshletDrawData Make(uint32_t InStartIndex, uint32_t InIndexCount, uint32_t InRangeIndex, uint32_t InGroupIndex)
    {
        return { InStartIndex, InIndexCount, InRangeIndex, InGroupIndex };
    }
};

struct FSectionRenderData
{
    FMeshGeometryBuffers Geometry;
    uint32_t DrawIndexStart = 0;
    uint32_t DrawIndexCount = 0;
    uint32_t BaseIndexCount = 0;
    FMeshMaterial Material;
    uint32_t PipelineKey = 0;
    bool bUseMeshletCulling = false;
    DirectX::XMFLOAT4X4 ModelTransform{};
    std::vector<FMesh::FMeshlet> Meshlets;
    std::vector<FMesh::FMeshletBounds> MeshletBounds;
    std::vector<uint32_t> MeshletIndices;
};

struct FSectionSkinningData
{
    bool bUseSkinning = false;
    bool bSkinningUpdatedThisFrame = false;
    bool bSkinningVisible = false;
    uint32_t LastSkinnedSlot = UINT32_MAX;
    uint32_t BoneMatrixCount = 0;
    FBindlessBuffer BoneMatrixBuffer;
    uint8_t* BoneMatrixBufferMapped = nullptr;
    std::vector<FBindlessBuffer> SkinnedPositionBuffers;
    FBindlessBuffer SkinnedPositionBuffer;
};

struct FSectionClusterDagData
{
    uint32_t ClusterDagMeshIndex = GClusterDAGInvalidIndex;
    std::wstring ClusterDagSourceFilePath;
    std::wstring ClusterDagCacheFilePath;
    uint32_t ClusterDagIndexCount = 0;
    bool bUseClusterDagRuntime = false;
    bool bCoveredByClusterDagRuntime = false;
    uint32_t ClusterDagVertexPackingMode = 0;
    FRuntimeClusterHierarchy ClusterDagRuntimeHierarchy;
    FClusterDAGPackedVertexData ClusterDagPackedVertexData;
    DirectX::XMFLOAT4 ClusterDagPackedPositionOffset{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedPositionScale{ 1.0f, 1.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantUV{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<FBindlessBuffer, kClusterDagVertexStreamCount> ClusterDagVertexBuffers;
    FBindlessBuffer ClusterDagIndexBuffer;
    FBindlessBuffer ClusterDagColorBuffer;
    FBindlessBuffer ClusterDagDebugColorBuffer;
};

struct FSectionRaytracingData
{
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasResultBuffer;
    D3D12_RAYTRACING_GEOMETRY_DESC BlasGeometryDesc{};
    bool bHasRayTracingBlas = false;
};

struct FMeshSection : public FSectionRenderData, public FSectionSkinningData, public FSectionClusterDagData, public FSectionRaytracingData
{
    DirectX::XMFLOAT3 Center{ 0.0f, 0.0f, 0.0f };
    float Radius = 1.0f;
    std::string Name;
    DirectX::XMFLOAT3 BoundsMin{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 BoundsMax{ 0.0f, 0.0f, 0.0f };
    uint32_t ObjectId = 0;
    bool bVisible = true;

    FSectionRenderData& GetRenderData() { return *this; }
    const FSectionRenderData& GetRenderData() const { return *this; }

    bool IsStaticRegularMeshCandidate() const
    {
        return !bUseSkinning
            && Geometry.VertexBuffers[kMeshVertexStreamPosition].IsValid()
            && Geometry.VertexBuffers[kMeshVertexStreamPosition].HasSrv()
            && Geometry.IndexBuffer.IsValid()
            && Geometry.IndexBuffer.HasSrv()
            && Geometry.IndexCount >= 3u
            && DrawIndexCount >= 3u
            && Geometry.VertexBuffers[kMeshVertexStreamPosition].Desc.NumElements > 0u
            && Geometry.IndexBuffer.Desc.NumElements >= Geometry.IndexCount
            && DrawIndexStart <= Geometry.IndexCount
            && DrawIndexCount <= Geometry.IndexCount - DrawIndexStart
            && Material.AlphaMode == static_cast<uint32_t>(EAlphaMode::Opaque);
    }

    bool IsClusterDagRuntimeAlphaModeAllowed() const
    {
        return Material.AlphaMode == static_cast<uint32_t>(EAlphaMode::Opaque)
            || Material.AlphaMode == static_cast<uint32_t>(EAlphaMode::Mask);
    }

    bool IsRuntimeDagSection() const
    {
        return bUseClusterDagRuntime
            && BoneMatrixBuffer.SrvBindlessIndex == UINT32_MAX
            && IsClusterDagRuntimeAlphaModeAllowed()
            && ClusterDagRuntimeHierarchy.IsValid()
            && !ClusterDagRuntimeHierarchy.Clusters.empty()
            && !ClusterDagRuntimeHierarchy.DrawDatas.empty();
    }
};
