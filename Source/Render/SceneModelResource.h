#pragma once

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

struct FMeshGeometryBuffers
{
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 7> VertexBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
    std::array<D3D12_VERTEX_BUFFER_VIEW, 7> VertexBufferViews{};
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
    std::array<uint32_t, 7> VertexBufferBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX } };
    uint32_t IndexBufferBindlessIndex = UINT32_MAX;
    uint32_t BaseColorBindlessIndex = UINT32_MAX;
    uint32_t MetallicRoughnessBindlessIndex = UINT32_MAX;
    uint32_t NormalBindlessIndex = UINT32_MAX;
    uint32_t EmissiveBindlessIndex = UINT32_MAX;
    uint32_t SheenColorBindlessIndex = UINT32_MAX;
    uint32_t SheenRoughnessBindlessIndex = UINT32_MAX;
    uint32_t ClearcoatBindlessIndex = UINT32_MAX;
    uint32_t ClearcoatRoughnessBindlessIndex = UINT32_MAX;
    uint32_t ClearcoatNormalBindlessIndex = UINT32_MAX;
    uint32_t AnisotropyBindlessIndex = UINT32_MAX;
    DirectX::XMFLOAT4 BaseColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 BaseColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 NormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 NormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 EmissiveTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 EmissiveTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 SheenColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 SheenRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatNormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatNormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 AnisotropyTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 AnisotropyTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    std::string Name;
    DirectX::XMFLOAT3 BoundsMin{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 BoundsMax{ 0.0f, 0.0f, 0.0f };
    uint32_t ObjectId = 0;
    bool bUseSkinning = false;
    bool bSkinningUpdatedThisFrame = false;
    bool bUseMeshletCulling = false;
    DirectX::XMFLOAT4X4 ModelTransform{};
        int GltfSceneIndex = -1;
    int GltfNodeIndex = -1;
    int GltfMeshIndex = -1;
    int GltfSkinIndex = -1;
    uint32_t ClusterDagMeshIndex = GClusterDAGInvalidIndex;
    uint32_t ClusterDagPrimitiveIndex = GClusterDAGInvalidIndex;
    uint32_t ClusterDagRuntimeClusterOffset = 0;
    uint32_t ClusterDagRuntimeClusterCount = 0;
    uint32_t ClusterDagIndexCount = 0;
    bool bUseClusterDagRuntime = false;
    uint32_t ClusterDagVertexPackingMode = 0;
    FRuntimeClusterHierarchy ClusterDagRuntimeHierarchy;
    DirectX::XMFLOAT4 ClusterDagPackedPositionOffset{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedPositionScale{ 1.0f, 1.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantUV{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t BoneMatrixBindlessIndex = UINT32_MAX;
    uint32_t BoneMatrixCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> BoneMatrixBuffer;
    uint8_t* BoneMatrixBufferMapped = nullptr;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 4> ClusterDagVertexBuffers;
    std::array<uint32_t, 4> ClusterDagVertexBufferBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX } };
    Microsoft::WRL::ComPtr<ID3D12Resource> ClusterDagIndexBuffer;
    uint32_t ClusterDagIndexBufferBindlessIndex = UINT32_MAX;
    Microsoft::WRL::ComPtr<ID3D12Resource> ClusterDagColorBuffer;
    uint32_t ClusterDagColorBufferBindlessIndex = UINT32_MAX;
    Microsoft::WRL::ComPtr<ID3D12Resource> ClusterDagDebugColorBuffer;
    uint32_t ClusterDagDebugColorBufferBindlessIndex = UINT32_MAX;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SkinnedPositionBuffers;
    std::vector<uint32_t> SkinnedPositionSrvBindlessIndices;
    std::vector<uint32_t> SkinnedPositionUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SkinnedPositionStates;
    Microsoft::WRL::ComPtr<ID3D12Resource> SkinnedPositionBuffer;
    uint32_t SkinnedPositionSrvBindlessIndex = UINT32_MAX;
    uint32_t SkinnedPositionUavBindlessIndex = UINT32_MAX;
    std::vector<FMesh::FMeshlet> Meshlets;
    std::vector<FMesh::FMeshletBounds> MeshletBounds;
    std::vector<uint32_t> MeshletIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasResultBuffer;
    D3D12_RAYTRACING_GEOMETRY_DESC BlasGeometryDesc{};
    bool bHasRayTracingBlas = false;
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
