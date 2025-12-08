#pragma once

#include <memory>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include "Renderer.h"
#include "RendererUtils.h"
#include "TextureLoader.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FDeferredRenderer : public FRenderer
{
public:
    FDeferredRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;

private:
    bool CreateBasePassRootSignature(FDX12Device* Device);
    bool CreateLightingRootSignature(FDX12Device* Device);
    bool CreateBasePassPipeline(FDX12Device* Device);
    bool CreateDepthPrepassPipeline(FDX12Device* Device);
    bool CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateDescriptorHeap(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    void UpdateSceneConstants(const FCamera& Camera, const DirectX::XMFLOAT4X4& WorldMatrix);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> BasePassRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BasePassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LightingPipeline;

    std::vector<FSceneModelResource> SceneModels;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DescriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferA;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferB;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferC;
    std::unique_ptr<FTextureLoader> TextureLoader;
    bool bUseDepthPrepass = true;

    D3D12_CPU_DESCRIPTOR_HANDLE GBufferRTVHandles[3]{};
    D3D12_GPU_DESCRIPTOR_HANDLE GBufferGpuHandles[3]{};
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};

    D3D12_RESOURCE_STATES DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    DirectX::XMFLOAT4X4 SceneWorldMatrix{};
    uint8_t* ConstantBufferMapped = nullptr;
};

