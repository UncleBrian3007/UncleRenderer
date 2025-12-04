#pragma once

#include <memory>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include "../Math/MathTypes.h"
#include "Renderer.h"
#include "RendererUtils.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FForwardRenderer : public FRenderer
{
public:
    FForwardRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const override { return DepthStencilHandle; }

private:
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipelineState(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateSceneGeometry(FDX12Device* Device);
    bool CreateCubeGeometry(FDX12Device* Device);
    bool CreateDefaultGridTexture(FDX12Device* Device);
    void UpdateSceneConstants(const FCamera& Camera, float DeltaTime);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;

    FMeshGeometryBuffers MeshBuffers{};
    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GridTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> TextureDescriptorHeap;

    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GridTextureGpuHandle{};
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};

    uint8_t* ConstantBufferMapped = nullptr;
    float RotationAngle = 0.0f;
};
