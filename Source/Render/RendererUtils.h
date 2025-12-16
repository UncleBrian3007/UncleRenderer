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
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    float Padding3 = 0.0f;
    DirectX::XMFLOAT3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    float Padding4 = 0.0f;
    DirectX::XMFLOAT4 BaseColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 BaseColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 NormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 NormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 EmissiveTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 EmissiveTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
};

struct FSkyAtmosphereConstants
{
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4X4 View;
    DirectX::XMFLOAT4X4 Projection;
    DirectX::XMFLOAT3 CameraPosition;
    float Padding0 = 0.0f;
    DirectX::XMFLOAT3 LightDirection{ 0.0f, -1.0f, 0.0f };
    float Padding1 = 0.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    float Padding2 = 0.0f;
};

struct FSkyPipelineConfig
{
    bool DepthEnable = false;
    D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_DEPTH_WRITE_MASK DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DXGI_FORMAT DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
};

struct FSceneModelResource
{
    FMeshGeometryBuffers Geometry;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    DirectX::XMFLOAT3 Center{ 0.0f, 0.0f, 0.0f };
    float Radius = 1.0f;
    DirectX::XMFLOAT3 BaseColorFactor{ 1.0f, 1.0f, 1.0f };
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    DirectX::XMFLOAT3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    std::wstring BaseColorTexturePath;
    std::wstring MetallicRoughnessTexturePath;
    std::wstring NormalTexturePath;
    std::wstring EmissiveTexturePath;
    bool bHasNormalMap = true;
    D3D12_GPU_DESCRIPTOR_HANDLE TextureHandle{};
    DirectX::XMFLOAT4 BaseColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 BaseColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 NormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 NormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 EmissiveTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 EmissiveTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
};

namespace RendererUtils
{
    bool CreateMeshGeometry(FDX12Device* Device, const FMesh& Mesh, FMeshGeometryBuffers& OutGeometry);
    bool CreateCubeGeometry(FDX12Device* Device, FCubeGeometryBuffers& OutGeometry, float Size = 1.0f);
    bool CreateSphereGeometry(
        FDX12Device* Device,
        FMeshGeometryBuffers& OutGeometry,
        float Radius = 1.0f,
        uint32_t SliceCount = 32,
        uint32_t StackCount = 16);
    bool CreateDefaultSceneGeometry(FDX12Device* Device, FMeshGeometryBuffers& OutGeometry, FFloat3& OutCenter, float& OutRadius, FGltfMaterialTextures* OutTexturePaths = nullptr);
    bool CreateSceneModelsFromJson(
        FDX12Device* Device,
        const std::wstring& SceneFilePath,
        std::vector<FSceneModelResource>& OutModels,
        DirectX::XMFLOAT3& OutSceneCenter,
        float& OutSceneRadius);
    bool CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format, FDepthResources& OutDepthResources);
    bool CreateMappedConstantBuffer(FDX12Device* Device, uint64_t BufferSize, FMappedConstantBuffer& OutConstantBuffer);
    bool CreateSkyAtmosphereResources(
        FDX12Device* Device,
        float SkySphereRadius,
        FMeshGeometryBuffers& OutGeometry,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutConstantBuffer,
        uint8_t*& OutConstantBufferMapped);
    bool CreateSkyAtmospherePipeline(
        FDX12Device* Device,
        DXGI_FORMAT BackBufferFormat,
        const FSkyPipelineConfig& Config,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& OutRootSignature,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState);
    void UpdateSceneConstants(
        const FCamera& Camera,
        const FSceneModelResource& Model,
        float LightIntensity,
        const DirectX::XMVECTOR& LightDirection,
        const DirectX::XMFLOAT3& LightColor,
        uint8_t* ConstantBufferMapped,
        uint64_t ConstantBufferOffset = 0);
    void UpdateSkyConstants(
        const FCamera& Camera,
        const DirectX::XMMATRIX& WorldMatrix,
        const DirectX::XMVECTOR& LightDirection,
        const DirectX::XMFLOAT3& LightColor,
        uint8_t* ConstantBufferMapped);
}

