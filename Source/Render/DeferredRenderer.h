#pragma once

#include <array>
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

struct FModelTextureSet
{
    Microsoft::WRL::ComPtr<ID3D12Resource> BaseColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> MetallicRoughness;
    Microsoft::WRL::ComPtr<ID3D12Resource> Normal;
    Microsoft::WRL::ComPtr<ID3D12Resource> Emissive;
};

class FDeferredRenderer : public FRenderer
{
public:
    FDeferredRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;

    void SetShadowsEnabled(bool bEnabled) { bShadowsEnabled = bEnabled; }
    bool IsShadowsEnabled() const { return bShadowsEnabled; }

    void SetTonemapEnabled(bool bEnabled) { bTonemapEnabled = bEnabled; }
    bool IsTonemapEnabled() const { return bTonemapEnabled; }

    void SetTonemapExposure(float Exposure) { TonemapExposure = Exposure; }
    float GetTonemapExposure() const { return TonemapExposure; }

    void SetTonemapWhitePoint(float WhitePoint) { TonemapWhitePoint = WhitePoint; }
    float GetTonemapWhitePoint() const { return TonemapWhitePoint; }

    void SetTonemapGamma(float Gamma) { TonemapGamma = Gamma; }
    float GetTonemapGamma() const { return TonemapGamma; }

    void SetShadowBias(float Bias) { ShadowBias = Bias; }
    float GetShadowBias() const { return ShadowBias; }

    void SetHZBEnabled(bool bEnabled) { bHZBEnabled = bEnabled; }
    bool IsHZBEnabled() const { return bHZBEnabled; }

private:
    bool CreateBasePassRootSignature(FDX12Device* Device);
    bool CreateLightingRootSignature(FDX12Device* Device);
    bool CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat);
    bool CreateDepthPrepassPipeline(FDX12Device* Device);
    bool CreateShadowPipeline(FDX12Device* Device);
    bool CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateHZBRootSignature(FDX12Device* Device);
    bool CreateHZBPipeline(FDX12Device* Device);
    bool CreateTonemapRootSignature(FDX12Device* Device);
    bool CreateTonemapPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateHZBResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateShadowResources(FDX12Device* Device);
    bool CreateDescriptorHeap(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset);
    void UpdateSkyConstants(const FCamera& Camera);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> BasePassRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> HZBRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BasePassPipelineWithNormalMap;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BasePassPipelineWithoutNormalMap;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LightingPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> HZBPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TonemapPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> TonemapRootSignature;

    std::vector<FSceneModelResource> SceneModels;
    std::vector<FModelTextureSet> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> SkyConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> ShadowMap;
    Microsoft::WRL::ComPtr<ID3D12Resource> LightingBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> HierarchicalZBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> HZBNullUavResource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DescriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShadowDSVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDSVHandle{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferA;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferB;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferC;
    std::unique_ptr<FTextureLoader> TextureLoader;
    float SkySphereRadius = 100.0f;

    DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_CPU_DESCRIPTOR_HANDLE GBufferRTVHandles[3]{};
    D3D12_CPU_DESCRIPTOR_HANDLE LightingRTVHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GBufferGpuHandles[3]{};
    D3D12_GPU_DESCRIPTOR_HANDLE LightingBufferHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE DepthBufferHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE HZBSrvHandle{};
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> HZBSrvMipHandles;
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> HZBUavHandles;
    D3D12_GPU_DESCRIPTOR_HANDLE HZBNullUavHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE ShadowMapHandle{};
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    D3D12_VIEWPORT ShadowViewport{};
    D3D12_RECT ShadowScissor{};

    D3D12_RESOURCE_STATES GBufferStates[3] =
    {
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
    };
    D3D12_RESOURCE_STATES DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES HZBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES ShadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES LightingBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    FMeshGeometryBuffers SkyGeometry;

    DirectX::XMFLOAT4X4 SceneWorldMatrix{};
    DirectX::XMFLOAT4X4 LightViewProjection{};
    uint8_t* ConstantBufferMapped = nullptr;
    uint8_t* SkyConstantBufferMapped = nullptr;
    uint64_t SceneConstantBufferStride = 0;
    float ShadowBias = 0.0005f;
    float ShadowStrength = 1.0f;
    bool bShadowsEnabled = true;
    bool bTonemapEnabled = true;
    float TonemapExposure = 0.9f;
    float TonemapWhitePoint = 6.0f;
    float TonemapGamma = 2.2f;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bHZBEnabled = true;

    FDX12Device* Device = nullptr;

    uint32_t HZBWidth = 0;
    uint32_t HZBHeight = 0;
    uint32_t HZBMipCount = 0;
};
