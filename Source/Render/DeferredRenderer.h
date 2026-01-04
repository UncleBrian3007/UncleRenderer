#pragma once

#include <array>
#include <algorithm>
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

    void SetAutoExposureEnabled(bool bEnabled) { bAutoExposureEnabled = bEnabled; }
    bool IsAutoExposureEnabled() const { return bAutoExposureEnabled; }

    void SetAutoExposureKey(float Key) { AutoExposureKey = Key; }
    float GetAutoExposureKey() const { return AutoExposureKey; }

    void SetAutoExposureMin(float MinExposure) { AutoExposureMin = MinExposure; }
    float GetAutoExposureMin() const { return AutoExposureMin; }

    void SetAutoExposureMax(float MaxExposure) { AutoExposureMax = MaxExposure; }
    float GetAutoExposureMax() const { return AutoExposureMax; }

    void SetAutoExposureSpeedUp(float Speed) { AutoExposureSpeedUp = Speed; }
    float GetAutoExposureSpeedUp() const { return AutoExposureSpeedUp; }

    void SetAutoExposureSpeedDown(float Speed) { AutoExposureSpeedDown = Speed; }
    float GetAutoExposureSpeedDown() const { return AutoExposureSpeedDown; }

    void SetTaaEnabled(bool bEnabled)
    {
        bTaaEnabled = bEnabled;
        std::fill(TaaHistoryValid.begin(), TaaHistoryValid.end(), false);
        std::fill(TaaHistoryFenceValues.begin(), TaaHistoryFenceValues.end(), 0);
        TaaSampleIndex = 0;
    }
    bool IsTaaEnabled() const { return bTaaEnabled; }

    void SetTaaHistoryWeight(float Weight) { TaaHistoryWeight = Weight; }
    float GetTaaHistoryWeight() const { return TaaHistoryWeight; }

    void SetShadowBias(float Bias) { ShadowBias = Bias; }
    float GetShadowBias() const { return ShadowBias; }

    void SetHZBEnabled(bool bEnabled) { bHZBEnabled = bEnabled; }
    bool IsHZBEnabled() const { return bHZBEnabled; }

    void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue) override;

private:
    bool CreateBasePassRootSignature(FDX12Device* Device);
    bool CreateLightingRootSignature(FDX12Device* Device);
    bool CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat);
    bool CreateDepthPrepassPipeline(FDX12Device* Device);
    bool CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateHZBRootSignature(FDX12Device* Device);
    bool CreateHZBPipeline(FDX12Device* Device);
    bool CreateAutoExposureRootSignature(FDX12Device* Device);
    bool CreateAutoExposurePipeline(FDX12Device* Device);
    bool CreateTaaRootSignature(FDX12Device* Device);
    bool CreateTaaPipeline(FDX12Device* Device);
    bool CreateTonemapRootSignature(FDX12Device* Device);
    bool CreateTonemapPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateHZBResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateLuminanceResources(FDX12Device* Device);
    bool CreateTaaResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    bool CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateObjectIdPipeline(FDX12Device* Device);
    bool CreateDescriptorHeap(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    bool CreateGpuDrivenResources(FDX12Device* Device);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset);
    void UpdateSkyConstants(const FCamera& Camera);
    void UpdateCullingVisibility(const FCamera& Camera);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> BasePassRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> HZBRootSignature;
    // Base pass pipelines indexed by permutation key (bit 0: Normal, bit 1: MR, bit 2: BaseColor, bit 3: Emissive, bit 4: AlphaMask)
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 32> BasePassPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LightingPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> HZBPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> AutoExposurePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TaaPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TonemapPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ObjectIdPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> AutoExposureRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> TaaRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> TonemapRootSignature;
    std::vector<FModelTextureSet> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> LightingBuffer;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> LuminanceTextures;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> TaaHistoryTextures;
    std::vector<uint64_t> TaaHistoryFenceValues;
    Microsoft::WRL::ComPtr<ID3D12Resource> HierarchicalZBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> HZBNullUavResource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DescriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferA;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferB;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferC;
    float SkySphereRadius = 100.0f;

    DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_CPU_DESCRIPTOR_HANDLE GBufferRTVHandles[3]{};
    D3D12_CPU_DESCRIPTOR_HANDLE LightingRTVHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GBufferGpuHandles[3]{};
    D3D12_GPU_DESCRIPTOR_HANDLE LightingBufferHandle{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> LuminanceSrvHandles{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> LuminanceUavHandles{};
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> TaaSrvHandles;
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> TaaUavHandles;
    D3D12_GPU_DESCRIPTOR_HANDLE DepthBufferHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE HZBSrvHandle{};
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> HZBSrvMipHandles;
    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> HZBUavHandles;
    D3D12_GPU_DESCRIPTOR_HANDLE HZBNullUavHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE ShadowMapHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE EnvironmentCubeHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE BrdfLutHandle{};
    D3D12_RESOURCE_STATES GBufferStates[3] =
    {
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
    };
    D3D12_RESOURCE_STATES HZBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES LightingBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::array<D3D12_RESOURCE_STATES, 2> LuminanceStates = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    std::vector<D3D12_RESOURCE_STATES> TaaStates;
    FMeshGeometryBuffers SkyGeometry;

    DirectX::XMFLOAT4X4 SceneWorldMatrix{};
    DirectX::XMFLOAT4X4 LightViewProjection{};
    bool bTonemapEnabled = true;
    float TonemapExposure = 0.9f;
    float TonemapWhitePoint = 6.0f;
    float TonemapGamma = 2.2f;
    bool bAutoExposureEnabled = false;
    float AutoExposureKey = 0.18f;
    float AutoExposureMin = 0.1f;
    float AutoExposureMax = 5.0f;
    float AutoExposureSpeedUp = 3.0f;
    float AutoExposureSpeedDown = 1.0f;
    bool bTaaEnabled = false;
    float TaaHistoryWeight = 0.9f;
    uint32_t TaaFrameCount = 0;
    std::vector<bool> TaaHistoryValid;
    uint32_t TaaSampleIndex = 0;
    DirectX::XMFLOAT2 TaaJitter{ 0.0f, 0.0f };
    DirectX::XMMATRIX TaaProjection = DirectX::XMMatrixIdentity();
    bool bUseTaaJitter = false;
    uint32_t LuminanceWriteIndex = 0;
    bool bLuminanceHistoryValid = false;
    bool bHZBEnabled = true;
    bool bHZBReady = false;

    uint32_t HZBWidth = 0;
    uint32_t HZBHeight = 0;
    uint32_t HZBMipCount = 0;
};
