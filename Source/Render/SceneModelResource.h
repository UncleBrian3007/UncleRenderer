#pragma once

#include "GpuResource.h"
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <limits>

#include "../Math/MathTypes.h"
#include "../Scene/Mesh.h"
#include "../Scene/ClusterDAG.h"

constexpr uint32_t kMeshVertexStreamCount = 7;
constexpr uint32_t kClusterDagVertexStreamCount = 4;

struct FTextureTransform
{
    DirectX::XMFLOAT4 OffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT2 Rotation{ 1.0f, 0.0f };
};

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

struct FSceneModelResource
{
    FMeshGeometryBuffers Geometry;
    uint32_t DrawIndexStart = 0;
    uint32_t DrawIndexCount = 0;
    uint32_t BaseIndexCount = 0;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    DirectX::XMFLOAT4X4 PreviousWorldMatrix{};
    bool bHasPreviousWorldMatrix = false;
    DirectX::XMFLOAT3 Center{ 0.0f, 0.0f, 0.0f };
    float Radius = 1.0f;
    DirectX::XMFLOAT3 BaseColorFactor{ 1.0f, 1.0f, 1.0f };
    float BaseColorAlpha = 1.0f;
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    DirectX::XMFLOAT3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 SheenColorFactor{ 0.0f, 0.0f, 0.0f };
    float SheenRoughnessFactor = 0.0f;
    float ClearcoatFactor = 0.0f;
    float ClearcoatRoughnessFactor = 0.0f;
    float AnisotropyStrength = 0.0f;
    float AnisotropyRotation = 0.0f;
    float AlphaCutoff = 0.5f;
    uint32_t AlphaMode = 0;
    bool bDoubleSided = false;
    uint32_t ShadingModelId = 0;
    std::wstring BaseColorTexturePath;
    std::wstring MetallicRoughnessTexturePath;
    std::wstring NormalTexturePath;
    std::wstring EmissiveTexturePath;
    std::wstring SheenColorTexturePath;
    std::wstring SheenRoughnessTexturePath;
    std::wstring ClearcoatTexturePath;
    std::wstring ClearcoatRoughnessTexturePath;
    std::wstring ClearcoatNormalTexturePath;
    std::wstring AnisotropyTexturePath;
    bool bHasNormalMap = true;
    FBindlessTexture BaseColor;
    FBindlessTexture MetallicRoughness;
    FBindlessTexture Normal;
    FBindlessTexture Emissive;
    FBindlessTexture SheenColor;
    FBindlessTexture SheenRoughness;
    FBindlessTexture Clearcoat;
    FBindlessTexture ClearcoatRoughness;
    FBindlessTexture ClearcoatNormal;
    FBindlessTexture Anisotropy;
    FTextureTransform BaseColorTransform;
    FTextureTransform MetallicRoughnessTransform;
    FTextureTransform NormalTransform;
    FTextureTransform EmissiveTransform;
    FTextureTransform SheenColorTransform;
    FTextureTransform SheenRoughnessTransform;
    FTextureTransform ClearcoatTransform;
    FTextureTransform ClearcoatRoughnessTransform;
    FTextureTransform ClearcoatNormalTransform;
    FTextureTransform AnisotropyTransform;
    std::string Name;
    DirectX::XMFLOAT3 BoundsMin{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 BoundsMax{ 0.0f, 0.0f, 0.0f };
    uint32_t ObjectId = 0;
    uint32_t PipelineKey = 0;
    bool bUseSkinning = false;
    bool bSkinningUpdatedThisFrame = false;
    uint32_t LastSkinnedSlot = UINT32_MAX;
    bool bUseMeshletCulling = false;
    DirectX::XMFLOAT4X4 ModelTransform{};
        int GltfSceneIndex = -1;
    int GltfNodeIndex = -1;
    int GltfMeshIndex = -1;
    int GltfSkinIndex = -1;
    uint32_t ClusterDagMeshIndex = GClusterDAGInvalidIndex;
    uint32_t ClusterDagPrimitiveIndex = GClusterDAGInvalidIndex;
    std::wstring ClusterDagSourceFilePath;
    std::wstring ClusterDagCacheFilePath;
    uint32_t ClusterDagRuntimeClusterOffset = 0;
    uint32_t ClusterDagRuntimeClusterCount = 0;
    uint32_t ClusterDagIndexCount = 0;
    bool bUseClusterDagRuntime = false;
    uint32_t ClusterDagVertexPackingMode = 0;
    FRuntimeClusterHierarchy ClusterDagRuntimeHierarchy;
    FClusterDAGPackedVertexData ClusterDagPackedVertexData;
    DirectX::XMFLOAT4 ClusterDagPackedPositionOffset{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedPositionScale{ 1.0f, 1.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantUV{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t BoneMatrixCount = 0;
    FBindlessBuffer BoneMatrixBuffer;
    uint8_t* BoneMatrixBufferMapped = nullptr;
    std::array<FBindlessBuffer, kClusterDagVertexStreamCount> ClusterDagVertexBuffers;
    FBindlessBuffer ClusterDagIndexBuffer;
    FBindlessBuffer ClusterDagColorBuffer;
    FBindlessBuffer ClusterDagDebugColorBuffer;
    std::vector<FBindlessBuffer> SkinnedPositionBuffers;
    FBindlessBuffer SkinnedPositionBuffer;
    std::vector<FMesh::FMeshlet> Meshlets;
    std::vector<FMesh::FMeshletBounds> MeshletBounds;
    std::vector<uint32_t> MeshletIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasResultBuffer;
    D3D12_RAYTRACING_GEOMETRY_DESC BlasGeometryDesc{};
    bool bHasRayTracingBlas = false;

    bool IsStaticRegularMeshCandidate() const
    {
        return !bUseSkinning
            && Geometry.VertexBuffers[0].HasSrv()
            && Geometry.IndexBuffer.HasSrv()
            && Geometry.IndexCount >= 3u
            && DrawIndexCount >= 3u
            && AlphaMode == 0u;
    }
};

inline bool ComputeSceneModelStats(
    const std::vector<FSceneModelResource>& Models,
    const std::vector<bool>& Visibility,
    size_t& OutTotal,
    size_t& OutCulled)
{
    OutTotal = Models.size();
    if (Visibility.empty())
    {
        OutCulled = 0;
        return true;
    }

    const size_t VisibleCountMax = (std::min)(Models.size(), Visibility.size());
    size_t VisibleCount = 0;
    for (size_t Index = 0; Index < VisibleCountMax; ++Index)
    {
        if (Visibility[Index])
        {
            ++VisibleCount;
        }
    }

    OutCulled = OutTotal > VisibleCount ? (OutTotal - VisibleCount) : 0;
    return true;
}
