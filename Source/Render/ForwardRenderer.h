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
    bool CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateObjectIdPipeline(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    bool CreateGpuDrivenResources(FDX12Device* Device);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset, const DirectX::XMMATRIX& LightViewProjection);
    void UpdateSkyConstants(const FCamera& Camera);
    void UpdateCullingVisibility(const FCamera& Camera);

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
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColorAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColorAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoEmissiveAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColorNoEmissiveAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoEmissiveAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColorNoEmissiveAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColorNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColorNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoEmissiveNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoBaseColorNoEmissiveNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoEmissiveNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineStateNoMrNoBaseColorNoEmissiveNoNormalAlphaMask;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ObjectIdPipeline;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> TextureDescriptorHeap;
    FMeshGeometryBuffers SkyGeometry;
    float SkySphereRadius = 1000.0f;

    D3D12_GPU_DESCRIPTOR_HANDLE SceneTextureGpuHandle{};
};
