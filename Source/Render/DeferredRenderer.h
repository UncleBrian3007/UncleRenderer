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
#include "RenderGraph.h"
#include "../Scene/GltfAnimation.h"

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

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config) override;
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

    void SetCasEnabled(bool bEnabled) { bCasEnabled = bEnabled; }
    bool IsCasEnabled() const { return bCasEnabled; }
    void SetCasSharpness(float Sharpness) { CasSharpness = Sharpness; }
    float GetCasSharpness() const { return CasSharpness; }

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
        TaaSampleIndex = 0;
    }
    bool IsTaaEnabled() const { return bTaaEnabled; }

    void SetTaaHistoryWeight(float Weight) { TaaHistoryWeight = Weight; }
    float GetTaaHistoryWeight() const { return TaaHistoryWeight; }

    void SetShadowBias(float Bias) { ShadowBias = Bias; }
    float GetShadowBias() const { return ShadowBias; }

    void SetHZBEnabled(bool bEnabled) { bHZBEnabled = bEnabled; }
    bool IsHZBEnabled() const { return bHZBEnabled; }
    void SetHzbTwoPassEnabled(bool bEnabled) { bEnableHzbTwoPass = bEnabled; }
    bool IsHzbTwoPassEnabled() const { return bEnableHzbTwoPass; }
    void SetGtaoEnabled(bool bEnabled) { bGtaoEnabled = bEnabled; }
    bool IsGtaoEnabled() const { return bGtaoEnabled; }
    void SetPbrResearchEnabled(bool bEnabled) { bEnablePbrResearch = bEnabled; }

    void SetPathTracingAccumulationEnabled(bool bEnabled)
    {
        bPathTracingAccumulationEnabled = bEnabled;
        // Also update user preference so it's restored when exiting debug modes
        if (PathTracingDebugMode == 0)
        {
            bPathTracingAccumulationUserPreference = bEnabled;
        }
        std::fill(PathTracingAccumulationHistoryValid.begin(), PathTracingAccumulationHistoryValid.end(), false);
        PathTracingAccumulatedFrames = 0;
    }
    bool IsPathTracingAccumulationEnabled() const { return bPathTracingAccumulationEnabled; }

    void SetPathTracingMaxBounces(uint32_t MaxBounces) { PathTracingMaxBounces = MaxBounces; }
    uint32_t GetPathTracingMaxBounces() const { return PathTracingMaxBounces; }
    void SetPathTracingVndfEnabled(bool bEnabled) override
    {
        bPathTracingUseVndf = bEnabled;
        std::fill(PathTracingAccumulationHistoryValid.begin(), PathTracingAccumulationHistoryValid.end(), false);
        PathTracingAccumulatedFrames = 0;
    }

    void SetPathTracingDebugMode(int Mode) 
    { 
        if (PathTracingDebugMode != Mode)
        {
            // Save user preference when leaving mode 0
            if (PathTracingDebugMode == 0 && Mode >= 1 && Mode <= 12)
            {
                bPathTracingAccumulationUserPreference = bPathTracingAccumulationEnabled;
            }
            
            PathTracingDebugMode = Mode;
            std::fill(PathTracingAccumulationHistoryValid.begin(), PathTracingAccumulationHistoryValid.end(), false);
			PathTracingAccumulatedFrames = 0;

            // Disable accumulation for debug modes 1-12, restore for mode 0
            if (Mode >= 1 && Mode <= 12)
            {
                bPathTracingAccumulationEnabled = false;
            }
            else if (Mode == 0)
            {
                bPathTracingAccumulationEnabled = bPathTracingAccumulationUserPreference;
            }
        }
    }
    int GetPathTracingDebugMode() const { return PathTracingDebugMode; }

    void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue) override;

private:
    struct FDeferredFrameState
    {
        bool bTaaActive = false;
        bool bTaaHistoryReady = false;
        uint32_t TaaFrameIndex = 0;
        uint32_t TaaReadIndex = 0;
        uint32_t TaaWriteIndex = 0;
        bool bRenderShadows = false;
        bool bDoDepthPrepass = false;
        bool bUseHZBOcclusion = false;
        bool bUseHzbTwoPass = false;
        bool bBuildHZB = false;
        bool bCasActive = false;
        bool bGtaoJitterActive = false;
        bool bPathTracingAccumulationActive = false;
        bool bPathTracingAccumulationHistoryReady = false;
        uint32_t PathTracingAccumulationReadIndex = 0;
        uint32_t PathTracingAccumulationWriteIndex = 0;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    struct FDeferredFrameResources
    {
        FRGResourceHandle ShadowHandle{};
        FRGResourceHandle ShadowMaskHandle{};
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle ObjectIdHandle{};
        std::array<FRGResourceHandle, 3> GBufferHandles{};
        FRGResourceHandle LinearDepthHandle{};
        FRGResourceHandle GtaoHandle{};
        FRGResourceHandle LightingHandle{};
        FRGResourceHandle TonemapOutputResource{};
        std::array<FRGResourceHandle, 2> LuminanceHandles{};
        std::vector<FRGResourceHandle> TaaHandles{};
        FRGResourceHandle HZBHandle{};
        FRGResourceHandle PathTracingTempHandle{};
        std::vector<FRGResourceHandle> PathTracingAccumulationHandles{};
    };

    bool CreateBasePassRootSignature(FDX12Device* Device);
    bool CreateLightingRootSignature(FDX12Device* Device);
    bool CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat);
    bool CreateDepthPrepassPipeline(FDX12Device* Device);
    bool CreateLinearDepthRootSignature(FDX12Device* Device);
    bool CreateLinearDepthPipeline(FDX12Device* Device);
    bool CreateGtaoRootSignature(FDX12Device* Device);
    bool CreateGtaoPipeline(FDX12Device* Device);
    bool CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateHZBRootSignature(FDX12Device* Device);
    bool CreateHZBPipeline(FDX12Device* Device);
    bool CreateAutoExposureRootSignature(FDX12Device* Device);
    bool CreateAutoExposurePipeline(FDX12Device* Device);
    bool CreateTaaRootSignature(FDX12Device* Device);
    bool CreateTaaPipeline(FDX12Device* Device);
    bool CreateTonemapRootSignature(FDX12Device* Device);
    bool CreateTonemapPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateCasRootSignature(FDX12Device* Device);
    bool CreateCasPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateLinearDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateGtaoResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateHilbertLutResources(FDX12Device* Device);
    bool CreateHZBResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateLuminanceResources(FDX12Device* Device);
    bool CreateTaaResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    bool CreatePathTracingAccumulationResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    bool CreatePathTracingAccumulationRootSignature(FDX12Device* Device);
    bool CreatePathTracingAccumulationPipeline(FDX12Device* Device);
    bool CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateObjectIdPipeline(FDX12Device* Device);
    bool CreateDescriptorHeap(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    bool CreateGpuDrivenResources(FDX12Device* Device);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset);
    void UpdateSkyConstants(const FCamera& Camera);
    void UpdateCullingVisibility(const FCamera& Camera);
    void PrepareFrameState(const FCamera& Camera, FDeferredFrameState& OutState);
    void ConfigureFrameGraph(FRenderGraph& Graph) const;
    void ImportFrameResources(FRenderGraph& Graph, FDeferredFrameResources& OutResources);
    void AddGpuCullingPass(
        FRenderGraph& Graph,
        const FCamera& Camera,
        const FDeferredFrameState& FrameState,
        FRGResourceHandle HZBHandle,
        FRenderer::ECullingMode Mode,
        uint32_t VisibilityInputIndex,
        uint32_t VisibilityInputFrameIndex,
        uint32_t CullingListIndex,
        uint32_t CullingListCountIndex,
        const char* PassName);
    void AddVisibilityListPass(
        FRenderGraph& Graph,
        const FDeferredFrameState& FrameState,
        uint32_t VisibilityIndex,
        uint32_t VisibilityFrameIndex,
        uint32_t FrameIndex);
    void AddEarlyRejectListPass(
        FRenderGraph& Graph,
        const FDeferredFrameState& FrameState,
        uint32_t VisibilityIndex,
        uint32_t FrameIndex);
    void AddLateListMergePass(
        FRenderGraph& Graph,
        const FDeferredFrameState& FrameState,
        uint32_t FrameIndex);
    void AddShadowPass(FRenderGraph& Graph, const FCamera& Camera, const FDeferredFrameState& FrameState, FRGResourceHandle ShadowHandle);
    void AddRayTracingShadowPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferHandle, FRGResourceHandle& ShadowMaskHandle);
    void AddDepthPrepass(FRenderGraph& Graph, const FCamera& Camera, const FDeferredFrameState& FrameState, FRGResourceHandle DepthHandle);
    void AddBasePass(
        FRenderGraph& Graph,
        const FCamera& Camera,
        const FDeferredFrameState& FrameState,
        const std::array<FRGResourceHandle, 3>& GBufferHandles,
        FRGResourceHandle DepthHandle,
        FRGResourceHandle LightingHandle,
        bool bClearTargets,
        bool bClearDepth,
        const char* PassName,
        bool bAllowSkinningFallback);
    void AddObjectIdPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle ObjectIdHandle, FRGResourceHandle DepthHandle);
    void AddHZBPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle HZBHandle);
    void AddLinearDepthPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle LinearDepthHandle);
    void AddGtaoPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 3>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle GtaoHandle);
    void AddLightingPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 3>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle GtaoHandle, FRGResourceHandle ShadowHandle, FRGResourceHandle LightingHandle);
    void AddPathTracingPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferAHandle, FRGResourceHandle GBufferBHandle, FRGResourceHandle GBufferCHandle, FRGResourceHandle OutputHandle);
    void AddPathTracingAccumulationPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle PathTracingTempHandle, FRGResourceHandle LightingHandle, const std::vector<FRGResourceHandle>& AccumulationHandles);
    void AddSkyPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle LightingHandle);
    void AddTemporalAAPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle LightingHandle, const std::vector<FRGResourceHandle>& TaaHandles);
    void AddAutoExposurePass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle LightingHandle, const std::array<FRGResourceHandle, 2>& LuminanceHandles, float DeltaTime);
    void AddTonemapPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 3>& GBufferHandles, FRGResourceHandle LightingHandle, FRGResourceHandle TonemapOutputResource, const std::array<FRGResourceHandle, 2>& LuminanceHandles, const std::vector<FRGResourceHandle>& TaaHandles, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddCasPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle TonemapOutputResource, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddDebugPrintPass(FRenderGraph& Graph, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void FinalizeFrameState(const FDeferredFrameState& FrameState);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> BasePassRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> HZBRootSignature;
    // Base pass pipelines indexed by permutation key (bit 0: Normal, bit 1: MR, bit 2: BaseColor, bit 3: Emissive, bit 4: AlphaMask)
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 32> BasePassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 32> BasePassPipelinesSkinned;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthPrepassPipelineSkinned;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ShadowPipelineSkinned;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LinearDepthRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LinearDepthPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GtaoRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> GtaoPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> LightingPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> HZBPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> AutoExposurePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TaaPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PathTracingAccumulationPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TonemapPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CasPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ObjectIdPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> AutoExposureRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> TaaRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> PathTracingAccumulationRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> TonemapRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CasRootSignature;
    std::vector<FModelTextureSet> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> LightingBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> LinearDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> GtaoTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> HilbertLutTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> TonemapOutput;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> LuminanceTextures;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> TaaHistoryTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> PathTracingTempTexture;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> PathTracingAccumulationTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> HierarchicalZBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> HZBNullUavResource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> LinearDepthRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GtaoRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferA;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferB;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferC;
    float SkySphereRadius = 100.0f;

    DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_CPU_DESCRIPTOR_HANDLE GBufferRTVHandles[3]{};
    D3D12_CPU_DESCRIPTOR_HANDLE LightingRTVHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE LinearDepthRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE GtaoRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE TonemapOutputRtvHandle{};
    std::array<uint32_t, 3> GBufferBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX } };
    uint32_t ShadowMapBindlessIndex = UINT32_MAX;
    uint32_t EnvironmentCubeBindlessIndex = UINT32_MAX;
    uint32_t BrdfLutBindlessIndex = UINT32_MAX;
    uint32_t LinearDepthBindlessIndex = UINT32_MAX;
    uint32_t HilbertLutBindlessIndex = UINT32_MAX;
    uint32_t GtaoBindlessIndex = UINT32_MAX;
    uint32_t LightingBufferBindlessIndex = UINT32_MAX;
    uint32_t TonemapOutputBindlessIndex = UINT32_MAX;
    std::array<uint32_t, 2> LuminanceSrvBindlessIndices{ { UINT32_MAX, UINT32_MAX } };
    std::array<uint32_t, 2> LuminanceUavBindlessIndices{ { UINT32_MAX, UINT32_MAX } };
    std::vector<uint32_t> TaaSrvBindlessIndices;
    std::vector<uint32_t> TaaUavBindlessIndices;
    uint32_t PathTracingTempBindlessIndex = UINT32_MAX;
    std::vector<uint32_t> PathTracingAccumulationSrvBindlessIndices;
    std::vector<uint32_t> PathTracingAccumulationUavBindlessIndices;
    std::vector<uint32_t> DepthBindlessIndices;
    uint32_t HZBSrvBindlessIndex = UINT32_MAX;
    std::vector<uint32_t> HZBSrvMipBindlessIndices;
    std::vector<uint32_t> HZBUavBindlessIndices;
    uint32_t HZBNullUavBindlessIndex = UINT32_MAX;
    D3D12_RESOURCE_STATES GBufferStates[3] =
    {
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
    };
    D3D12_RESOURCE_STATES HZBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES LightingBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES LinearDepthState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES GtaoState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::vector<FGltfScene> GltfScenes;
    std::vector<FGltfAnimationPose> GltfScenePoses;
    std::vector<float> GltfSceneTimes;
    D3D12_RESOURCE_STATES TonemapOutputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    std::array<D3D12_RESOURCE_STATES, 2> LuminanceStates = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    std::vector<D3D12_RESOURCE_STATES> TaaStates;
    D3D12_RESOURCE_STATES PathTracingTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::vector<D3D12_RESOURCE_STATES> PathTracingAccumulationStates;
    FMeshGeometryBuffers SkyGeometry;

    DirectX::XMFLOAT4X4 SceneWorldMatrix{};
    DirectX::XMFLOAT4X4 LightViewProjection{};
    bool bTonemapEnabled = true;
    float TonemapExposure = 0.9f;
    float TonemapWhitePoint = 6.0f;
    float TonemapGamma = 2.2f;
    bool bCasEnabled = true;
    float CasSharpness = 0.2f;
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
    bool bPathTracingAccumulationEnabled = false;
    bool bPathTracingAccumulationUserPreference = false; // User's preference when not in debug mode
    uint32_t PathTracingAccumulationFrameCount = 0;
    std::vector<bool> PathTracingAccumulationHistoryValid;
    uint32_t PathTracingAccumulatedFrames = 0;
    uint32_t PathTracingMaxBounces = 8;
    int PathTracingDebugMode = 0; // 0=Normal PT, 1=GBuffer Albedo, 2=First Hit Albedo, 3=Texture Index Hash, 4=Direct Light, 5=Diffuse Probability, 6=Hit/Miss Mask, 7=Throughput Over Pdf, 8=Firefly Metric, 9=First Hit Distance, 10=Sky Miss Contribution, 11=First Hit NdotV, 12=Bounce1 NdotV
    DirectX::XMFLOAT3 PreviousCameraPosition{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4X4 PreviousCameraViewMatrix{};
    bool bFirstFrame = true;
    uint32_t LuminanceWriteIndex = 0;
    bool bLuminanceHistoryValid = false;
    bool bHZBEnabled = true;
    bool bHZBReady = false;
    bool bEnableHzbTwoPass = true;
    bool bEnablePbrResearch = false;

    uint32_t HZBWidth = 0;
    uint32_t HZBHeight = 0;
    uint32_t HZBMipCount = 0;
};
