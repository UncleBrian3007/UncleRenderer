#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

class FDX12Device;
class FCamera;

struct FCubeGeometryBuffers
{
    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
    uint32_t IndexCount = 0;
};

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
    float Padding0 = 0.0f;
    DirectX::XMFLOAT3 LightDirection;
    float Padding1 = 0.0f;
};

namespace RendererUtils
{
    bool CreateCubeGeometry(FDX12Device* Device, FCubeGeometryBuffers& OutGeometry, float Size = 1.0f);
    bool CreateDefaultGridTexture(FDX12Device* Device, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);
    bool CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format, FDepthResources& OutDepthResources);
    bool CreateMappedConstantBuffer(FDX12Device* Device, uint64_t BufferSize, FMappedConstantBuffer& OutConstantBuffer);
    void UpdateSceneConstants(const FCamera& Camera, float DeltaTime, float& RotationAngle, const DirectX::XMFLOAT3& BaseColor, const DirectX::XMVECTOR& LightDirection, uint8_t* ConstantBufferMapped);
}

