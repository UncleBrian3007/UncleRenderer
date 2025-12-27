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

    void SetShadowsEnabled(bool bEnabled) { bShadowsEnabled = bEnabled; }
    bool IsShadowsEnabled() const { return bShadowsEnabled; }

    void SetShadowBias(float Bias) { ShadowBias = Bias; }
    float GetShadowBias() const { return ShadowBias; }

private:
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipelineState(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateShadowPipeline(FDX12Device* Device);
    bool CreateShadowResources(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset, const DirectX::XMMATRIX& LightViewProjection);
    void UpdateSkyConstants(const FCamera& Camera);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColor;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColor;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoEmissive;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColorNoEmissive;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoEmissive;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColorNoEmissive;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColorNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColorNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoEmissiveNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColorNoEmissiveNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoEmissiveNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColorNoEmissiveNoNormal;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;

    std::vector<FSceneModelResource> SceneModels;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> SkyConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> ShadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShadowDSVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDSVHandle{};
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> TextureDescriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> NullTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> EnvironmentCubeTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> BrdfLutTexture;
    std::unique_ptr<FTextureLoader> TextureLoader;
    FMeshGeometryBuffers SkyGeometry;
    float SkySphereRadius = 1000.0f;

    D3D12_GPU_DESCRIPTOR_HANDLE SceneTextureGpuHandle{};
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    D3D12_VIEWPORT ShadowViewport{};
    D3D12_RECT ShadowScissor{};

    D3D12_RESOURCE_STATES DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES ShadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    uint8_t* ConstantBufferMapped = nullptr;
    uint8_t* SkyConstantBufferMapped = nullptr;
    uint64_t SceneConstantBufferStride = 0;
    float ShadowBias = 0.0005f;
    float ShadowStrength = 1.0f;
    bool bShadowsEnabled = true;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    float EnvironmentMipCount = 1.0f;

    FDX12Device* Device = nullptr;
};
