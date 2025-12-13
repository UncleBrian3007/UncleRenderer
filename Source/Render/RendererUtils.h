#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../Math/MathTypes.h"

class FDX12Device;
class FCamera;
class FMesh;
struct FGltfMaterialTextures;

struct FMeshGeometryBuffers
{
    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
    uint32_t IndexCount = 0;
};

using FCubeGeometryBuffers = FMeshGeometryBuffers;

struct FDepthResources
{
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
};

struct FMappedConstantBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    uint8_t* MappedData = nullptr;
};

struct FSceneConstants
{
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4X4 View;
    DirectX::XMFLOAT4X4 Projection;
    DirectX::XMFLOAT3 BaseColor;
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightDirection;
    float Padding1 = 0.0f;
    DirectX::XMFLOAT3 CameraPosition;
    float Padding2 = 0.0f;
};

struct FSceneModelResource
{
    FMeshGeometryBuffers Geometry;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    DirectX::XMFLOAT3 Center{ 0.0f, 0.0f, 0.0f };
    float Radius = 1.0f;
    std::wstring BaseColorTexturePath;
    std::wstring MetallicRoughnessTexturePath;
    std::wstring NormalTexturePath;
    D3D12_GPU_DESCRIPTOR_HANDLE TextureHandle{};
};

namespace RendererUtils
{
    bool CreateMeshGeometry(FDX12Device* Device, const FMesh& Mesh, FMeshGeometryBuffers& OutGeometry);
    bool CreateCubeGeometry(FDX12Device* Device, FCubeGeometryBuffers& OutGeometry, float Size = 1.0f);
    bool CreateDefaultSceneGeometry(FDX12Device* Device, FMeshGeometryBuffers& OutGeometry, FFloat3& OutCenter, float& OutRadius, FGltfMaterialTextures* OutTexturePaths = nullptr);
    bool CreateSceneModelsFromJson(
        FDX12Device* Device,
        const std::wstring& SceneFilePath,
        std::vector<FSceneModelResource>& OutModels,
        DirectX::XMFLOAT3& OutSceneCenter,
        float& OutSceneRadius);
    bool CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format, FDepthResources& OutDepthResources);
    bool CreateMappedConstantBuffer(FDX12Device* Device, uint64_t BufferSize, FMappedConstantBuffer& OutConstantBuffer);
    void UpdateSceneConstants(const FCamera& Camera, const DirectX::XMFLOAT3& BaseColor, float LightIntensity, const DirectX::XMVECTOR& LightDirection, const DirectX::XMMATRIX& WorldMatrix, uint8_t* ConstantBufferMapped);
}

