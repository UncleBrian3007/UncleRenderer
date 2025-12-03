#pragma once

#include <memory>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include "../Math/MathTypes.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FForwardRenderer
{
public:
    FForwardRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat);
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime);

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const { return DepthStencilHandle; }

private:
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipelineState(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateCubeGeometry(FDX12Device* Device);
    bool CreateConstantBuffer(FDX12Device* Device);
    bool CreateDefaultGridTexture(FDX12Device* Device);
    void UpdateSceneConstants(const FCamera& Camera, float DeltaTime);

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

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;

    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GridTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> TextureDescriptorHeap;

    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GridTextureGpuHandle{};
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};

    uint8_t* ConstantBufferMapped = nullptr;
    uint32_t IndexCount = 0;
    float RotationAngle = 0.0f;
};
