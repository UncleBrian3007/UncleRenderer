#pragma once

#include <memory>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include "../Math/MathTypes.h"
#include "Renderer.h"
#include "RendererUtils.h"
#include "TextureLoader.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FForwardRenderer : public FRenderer
{
public:
    FForwardRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;

private:
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipelineState(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    void UpdateSceneConstants(const FCamera& Camera, const DirectX::XMFLOAT4X4& WorldMatrix);
    void UpdateSkyConstants(const FCamera& Camera);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;

    std::vector<FSceneModelResource> SceneModels;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> SkyConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> TextureDescriptorHeap;
    std::unique_ptr<FTextureLoader> TextureLoader;
    FMeshGeometryBuffers SkyGeometry;
    float SkySphereRadius = 1000.0f;
    bool bUseDepthPrepass = true;

    D3D12_GPU_DESCRIPTOR_HANDLE SceneTextureGpuHandle{};
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};

    D3D12_RESOURCE_STATES DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    uint8_t* ConstantBufferMapped = nullptr;
    uint8_t* SkyConstantBufferMapped = nullptr;
};
