#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "RendererUtils.h"

struct FSceneModelResource;
class FTextureLoader;

struct FRendererOptions
{
    std::wstring SceneFilePath = L"Assets/Scenes/Scene.json";
    bool bUseDepthPrepass = true;
    bool bEnableShadows = true;
    float ShadowBias = 0.0f;
    bool bEnableTonemap = true;
    float TonemapExposure = 0.5f;
    float TonemapWhitePoint = 4.0f;
    float TonemapGamma = 1.0f;
    bool bEnableAutoExposure = false;
    float AutoExposureKey = 0.18f;
    float AutoExposureMin = 0.1f;
    float AutoExposureMax = 5.0f;
    float AutoExposureSpeedUp = 3.0f;
    float AutoExposureSpeedDown = 1.0f;
    bool bEnableTAA = false;
    float TaaHistoryWeight = 0.9f;
    uint32_t FramesInFlight = 2;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bEnableHZB = true;
    bool bEnableIndirectDraw = false;
    bool bEnableGpuDebugPrint = false;
};

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FRenderer
{
public:
    struct FGpuDebugPrintEntry
    {
        uint32_t X = 0;
        uint32_t Y = 0;
        uint32_t Code = 0;
        uint32_t Color = 0;
    };

    static constexpr uint32_t GpuDebugPrintMaxEntries = 4096;
    static constexpr uint32_t GpuDebugPrintHeaderSize = sizeof(uint32_t);
    static constexpr uint32_t GpuDebugPrintEntryStride = sizeof(FGpuDebugPrintEntry);
    static constexpr uint64_t GpuDebugPrintBufferSize = GpuDebugPrintHeaderSize + static_cast<uint64_t>(GpuDebugPrintMaxEntries) * GpuDebugPrintEntryStride;

    virtual ~FRenderer();

    virtual bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options) = 0;
    virtual void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) = 0;
    virtual void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue) {}

    virtual void SetDepthPrepassEnabled(bool bEnabled) { bDepthPrepassEnabled = bEnabled; }
    virtual bool IsDepthPrepassEnabled() const { return bDepthPrepassEnabled; }

    void SetFrameIndex(uint32_t FrameIndex);
    uint32_t GetFrameIndex() const { return CurrentFrameIndex; }

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const;
    ID3D12Resource* GetDepthBuffer() const;
    D3D12_RESOURCE_STATES& GetDepthBufferState();
    ID3D12Resource* GetSceneConstantBuffer() const;
    D3D12_GPU_VIRTUAL_ADDRESS GetSceneConstantBufferAddress() const;
    uint8_t* GetSceneConstantBufferMapped() const;
    ID3D12Resource* GetIndirectCommandBuffer() const;
    D3D12_RESOURCE_STATES& GetIndirectCommandState();
    uint32_t GetFramesInFlight() const { return FramesInFlight; }

    DirectX::XMFLOAT3 GetSceneCenter() const { return SceneCenter; }
    float GetSceneRadius() const { return SceneRadius; }

    void SetLightDirection(const DirectX::XMFLOAT3& Direction) { LightDirection = Direction; }
    DirectX::XMFLOAT3 GetLightDirection() const { return LightDirection; }

    void SetLightIntensity(float Intensity) { LightIntensity = Intensity; }
    float GetLightIntensity() const { return LightIntensity; }

    void SetLightColor(const DirectX::XMFLOAT3& Color) { LightColor = Color; }
    DirectX::XMFLOAT3 GetLightColor() const { return LightColor; }

    void SetCullingCameraOverride(const FCamera* Camera) { CullingCameraOverride = Camera; }
    const FCamera* GetCullingCameraOverride() const { return CullingCameraOverride; }
    void SetIndirectDrawEnabled(bool bEnabled) { bEnableIndirectDraw = bEnabled; }
    bool IsIndirectDrawEnabled() const { return bEnableIndirectDraw; }
    virtual const std::vector<FSceneModelResource>* GetSceneModels() const { return &SceneModels; }
    virtual bool GetSceneModelStats(size_t& OutTotal, size_t& OutCulled) const;
    virtual void RequestObjectIdReadback(uint32_t X, uint32_t Y);
    virtual bool ConsumeObjectIdReadback(uint32_t& OutObjectId);

protected:
    void InitializeCommonSettings(uint32_t Width, uint32_t Height, const FRendererOptions& Options);
    bool CreateShadowPipeline(FDX12Device* Device, ID3D12RootSignature* RootSignature, Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState);
    bool CreateShadowResources(
        FDX12Device* Device,
        uint32_t& InOutWidth,
        uint32_t& InOutHeight,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutShadowMap,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& OutShadowDsvHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE& OutShadowDsvHandle,
        D3D12_RESOURCE_STATES& OutShadowState);
    void DispatchGpuCulling(FDX12CommandContext& CmdContext, const FCamera& Camera);
    void ConfigureHZBOcclusion(bool bEnabled, ID3D12DescriptorHeap* DescriptorHeap, D3D12_GPU_DESCRIPTOR_HANDLE Handle, uint32_t Width, uint32_t Height, uint32_t MipCount);
    void PrepareGpuDebugPrint(FDX12CommandContext& CmdContext);
    void DispatchGpuDebugPrintStats(FDX12CommandContext& CmdContext);
    bool CreateGpuDebugPrintResources(FDX12Device* Device);
    bool CreateGpuDebugPrintPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateGpuDebugPrintStatsPipeline(FDX12Device* Device);
    void RenderGpuDebugPrint(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle);
    bool CreateDepthResourcesPerFrame(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format);
    bool CreateSceneConstantBuffersPerFrame(FDX12Device* Device, uint64_t BufferSize);

    std::vector<FDepthResources> DepthResourcesPerFrame;
    std::vector<D3D12_RESOURCE_STATES> DepthBufferStates;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SceneConstantBuffers;
    std::vector<uint8_t*> SceneConstantBufferMapped;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE ShadowDSVHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE ObjectIdRtvHandle{};
    DirectX::XMFLOAT3 SceneCenter{ 0.0f, 0.0f, 0.0f };
    float SceneRadius = 1.0f;

    DirectX::XMFLOAT3 LightDirection{ -0.5f, -1.0f, 0.2f };
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };

    bool bDepthPrepassEnabled = false;
    const FCamera* CullingCameraOverride = nullptr;

    std::vector<FSceneModelResource> SceneModels;
    std::vector<bool> SceneModelVisibility;
    struct FIndirectDrawRange
    {
        uint32_t Start = 0;
        uint32_t Count = 0;
        uint32_t PipelineKey = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE TextureHandle{};
        std::wstring Name;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> SkyConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> ShadowMap;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> IndirectCommandBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> ModelBoundsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> ModelBoundsUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintStatsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintStatsUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> ObjectIdTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> ObjectIdReadback;
    Microsoft::WRL::ComPtr<ID3D12Resource> NullTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> EnvironmentCubeTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> BrdfLutTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShadowDSVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ObjectIdRtvHeap;
    std::unique_ptr<FTextureLoader> TextureLoader;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CullingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CullingPipeline;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> IndirectCommandSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GpuDebugPrintRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GpuDebugPrintPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GpuDebugPrintStatsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GpuDebugPrintStatsPipeline;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GpuDebugPrintDescriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintFontTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> GpuDebugPrintGlyphBuffer;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuDebugPrintGlyphHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GpuDebugPrintFontHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GpuDebugPrintBufferHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE GpuDebugPrintStatsHandle{};
    uint32_t GpuDebugPrintAtlasWidth = 0;
    uint32_t GpuDebugPrintAtlasHeight = 0;
    uint32_t GpuDebugPrintFirstChar = 32;
    uint32_t GpuDebugPrintCharCount = 96;
    float GpuDebugPrintFontSize = 16.0f;

    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
    D3D12_VIEWPORT ShadowViewport{};
    D3D12_RECT ShadowScissor{};

    D3D12_RESOURCE_STATES ShadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES ObjectIdState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::vector<D3D12_RESOURCE_STATES> IndirectCommandStates;
    D3D12_RESOURCE_STATES GpuDebugPrintState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_COMMON;

    uint8_t* SkyConstantBufferMapped = nullptr;
    uint64_t SceneConstantBufferStride = 0;
    float ShadowBias = 0.0f;
    float ShadowStrength = 1.0f;
    uint32_t ShadowMapWidth = 0;
    uint32_t ShadowMapHeight = 0;
    bool bShadowsEnabled = true;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bEnableIndirectDraw = false;
    bool bEnableGpuDebugPrint = false;
    float EnvironmentMipCount = 1.0f;
    bool bObjectIdReadbackRequested = false;
    bool bObjectIdReadbackRecorded = false;
    uint32_t ObjectIdReadbackX = 0;
    uint32_t ObjectIdReadbackY = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT ObjectIdFootprint{};
    uint32_t ObjectIdRowPitch = 0;
    uint32_t IndirectCommandCount = 0;
    std::vector<FIndirectDrawRange> IndirectDrawRanges;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> CullingDescriptorHeap;
    D3D12_GPU_DESCRIPTOR_HANDLE HZBCullingHandle{};
    uint32_t HZBCullingWidth = 0;
    uint32_t HZBCullingHeight = 0;
    uint32_t HZBCullingMipCount = 0;
    bool bHZBOcclusionEnabled = false;

    FDX12Device* Device = nullptr;
    uint32_t FramesInFlight = 1;
    uint32_t CurrentFrameIndex = 0;
};
