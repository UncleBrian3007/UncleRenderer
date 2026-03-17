#pragma once

#include <array>
#include <algorithm>
#include <mutex>
#include <memory>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include "Renderer.h"
#include "RenderGraph.h"
#include "Deferred/Gtao.h"
#include "Deferred/RayTracingShadow.h"
#include "Deferred/Ssr.h"
#include "Deferred/RestirGI.h"
#include "Deferred/RestirGIDenoiser.h"
#include "Deferred/PathTracing.h"
#include "Deferred/AutoExposure.h"
#include "Deferred/Cas.h"
#include "Deferred/Taa.h"
#include "Deferred/Tonemap.h"
#include "../Core/RendererConfig.h"
#include "../Scene/GltfAnimation.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;
class FDeferredFrameOrchestrator;
class FDeferredVisibilityPasses;
class FDeferredGeometryPasses;
class FDeferredLightingPasses;
class FDeferredPostProcessPasses;
class FDeferredResourceImporter;
struct FModelTextureSet
{
    Microsoft::WRL::ComPtr<ID3D12Resource> BaseColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> MetallicRoughness;
    Microsoft::WRL::ComPtr<ID3D12Resource> Normal;
    Microsoft::WRL::ComPtr<ID3D12Resource> Emissive;
    Microsoft::WRL::ComPtr<ID3D12Resource> SheenColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> SheenRoughness;
    Microsoft::WRL::ComPtr<ID3D12Resource> Clearcoat;
    Microsoft::WRL::ComPtr<ID3D12Resource> ClearcoatRoughness;
    Microsoft::WRL::ComPtr<ID3D12Resource> ClearcoatNormal;
    Microsoft::WRL::ComPtr<ID3D12Resource> Anisotropy;
};

class FDeferredRenderer : public FRenderer
{
public:
    static constexpr DXGI_FORMAT LightingBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT PathTracingBufferFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    static const DXGI_FORMAT GBufferFormats[4];

    FDeferredRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;

    void SetShadowsEnabled(bool bEnabled) { bShadowsEnabled = bEnabled; }
    bool IsShadowsEnabled() const { return bShadowsEnabled; }

    void SetTonemapEnabled(bool bEnabled);
    bool IsTonemapEnabled() const;

    void SetTonemapExposure(float Exposure);
    float GetTonemapExposure() const;

    void SetTonemapWhitePoint(float WhitePoint);
    float GetTonemapWhitePoint() const;

    void SetTonemapGamma(float Gamma);
    float GetTonemapGamma() const;

    void SetCasEnabled(bool bEnabled);
    bool IsCasEnabled() const;
    void SetCasSharpness(float Sharpness);
    float GetCasSharpness() const;

    void SetAutoExposureEnabled(bool bEnabled);
    bool IsAutoExposureEnabled() const;

    void SetAutoExposureKey(float Key);
    float GetAutoExposureKey() const;

    void SetAutoExposureMin(float MinExposure);
    float GetAutoExposureMin() const;

    void SetAutoExposureMax(float MaxExposure);
    float GetAutoExposureMax() const;

    void SetAutoExposureSpeedUp(float Speed);
    float GetAutoExposureSpeedUp() const;

    void SetAutoExposureSpeedDown(float Speed);
    float GetAutoExposureSpeedDown() const;

    void SetTaaEnabled(bool bEnabled);
    bool IsTaaEnabled() const;

    void SetTaaHistoryWeight(float Weight);
    float GetTaaHistoryWeight() const;

    void SetShadowBias(float Bias) { ShadowBias = Bias; }
    float GetShadowBias() const { return ShadowBias; }

    void SetHZBEnabled(bool bEnabled) { bHZBEnabled = bEnabled; }
    bool IsHZBEnabled() const { return bHZBEnabled; }
    void SetHzbTwoPassEnabled(bool bEnabled) { bEnableHzbTwoPass = bEnabled; }
    bool IsHzbTwoPassEnabled() const { return bEnableHzbTwoPass; }
    void SetGtaoEnabled(bool bEnabled) { bGtaoEnabled = bEnabled; }
    bool IsGtaoEnabled() const { return bGtaoEnabled; }
    void SetPbrResearchEnabled(bool bEnabled) { bEnablePbrResearch = bEnabled; }
    void SetSsrSwEnabled(bool bEnabled);
    void SetSsrHwEnabled(bool bEnabled);
    void SetSsrHzbEnabled(bool bEnabled);
    void SetSsrRefineEnabled(bool bEnabled);
    void SetSsrDenoiseEnabled(bool bEnabled);
    void SetSsrMode(ESSRMode Mode);
    void SetSsrSamplesPerQuad(uint32_t Samples);
    bool IsSsrSwEnabled() const;
    bool IsSsrHwEnabled() const;
    void SetSsrMaxSteps(uint32_t Steps);
    uint32_t GetSsrMaxSteps() const;
    void SetSsrMaxDistance(float Distance);
    float GetSsrMaxDistance() const;
    void SetSsrThickness(float Thickness);
    float GetSsrThickness() const;
    void SetSsrStride(float Stride);
    float GetSsrStride() const;
    void SetSsrRoughnessCutoff(float Cutoff);
    float GetSsrRoughnessCutoff() const;
    void SetSsrIntensity(float Intensity);
    float GetSsrIntensity() const;
    void SetRestirGIEnabled(bool bEnabled);
    bool IsRestirGIEnabled() const;
    void SetRestirGIDenoiserEnabled(bool bEnabled);
    bool IsRestirGIDenoiserEnabled() const;
    void SetRestirGISamplesPerPixel(uint32_t Samples);
    uint32_t GetRestirGISamplesPerPixel() const;
    void SetRestirGIIntensity(float Intensity);
    float GetRestirGIIntensity() const;
    void SetRestirGIShowOnly(bool bEnabled);
    bool IsRestirGIShowOnly() const;
    void SetRestirGITemporalReuseEnabled(bool bEnabled);
    bool IsRestirGITemporalReuseEnabled() const;
    void SetRestirGISpatialReuseEnabled(bool bEnabled);
    bool IsRestirGISpatialReuseEnabled() const;
    void SetRestirGITemporalAdditionalScale(float Value);
    float GetRestirGITemporalAdditionalScale() const;
    void SetRestirGISpatialAdditionalScale(float Value);
    float GetRestirGISpatialAdditionalScale() const;
    void SetRestirGIResolveMinDenominator(float Value);
    float GetRestirGIResolveMinDenominator() const;
    void SetRestirGIResolveMaxNormalization(float Value);
    float GetRestirGIResolveMaxNormalization() const;
    void SetRestirGIResolveLowSampleBoostGuard(float Value);
    float GetRestirGIResolveLowSampleBoostGuard() const;
    void SetRestirGIResolveUseConfidence(bool bEnabled);
    bool IsRestirGIResolveUseConfidence() const;
    void SetRestirGIUseVisibility(bool bEnabled);
    bool IsRestirGIUseVisibility() const;
    void SetRestirGIUseBrdf(bool bEnabled);
    bool IsRestirGIUseBrdf() const;
    void SetRestirGIUseHistoryIndirect(bool bEnabled);
    bool IsRestirGIUseHistoryIndirect() const;
    void SetRestirGIRandomMode(ERestirGIRandomMode Mode);
    ERestirGIRandomMode GetRestirGIRandomMode() const;
    void SetRestirGIDebugRayEnabled(bool bEnabled);
    bool IsRestirGIDebugRayEnabled() const;
    void SetRestirGIDebugPixel(uint32_t X, uint32_t Y);
    void SetRestirGIFreezeFrame(bool bEnabled);
    bool IsRestirGIFreezeFrame() const;
    uint32_t GetRestirGIFrozenSequenceFrame() const;
    uint64_t GetRestirGIFreezeStartFrameNumber() const;
    void StepRestirGIFreezeFrame();
    void SetRestirGIFreezeDenoiserHistoryResetPeriod(uint32_t InPeriod);
    uint32_t GetRestirGIFreezeDenoiserHistoryResetPeriod() const;

    void SetPathTracingAccumulationEnabled(bool bEnabled) override;
    bool IsPathTracingAccumulationEnabled() const;

    void SetPathTracingMaxBounces(uint32_t MaxBounces);
    uint32_t GetPathTracingMaxBounces() const;
    void SetPathTracingVndfEnabled(bool bEnabled) override;

    void SetPathTracingDebugMode(int Mode);
    int GetPathTracingDebugMode() const;

    void OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue) override;

public:
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
        bool bCameraMoved = false;
        bool bAnySkinningUpdated = false;
        uint32_t PathTracingAccumulationReadIndex = 0;
        uint32_t PathTracingAccumulationWriteIndex = 0;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    struct FDeferredFrameResources
    {
        FRGResourceHandle ShadowHandle{};
        FRayTracingShadowFrameResources RayTracingShadow;
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle ObjectIdHandle{};
        FRGResourceHandle VelocityHandle{};
        std::array<FRGResourceHandle, 4> GBufferHandles{};
        FRGResourceHandle LinearDepthHandle{};
        FGtaoFrameResources Gtao;
        FRestirGIFrameResources RestirGI;
        FRestirGIDenoiserFrameResources RestirGIDenoiser;
        FSsrFrameResources Ssr;
        FRGResourceHandle LightingHandle{};
        FAutoExposureFrameResources AutoExposure;
        FCasFrameResources Cas;
        FTonemapFrameResources Tonemap;
        FTaaFrameResources Taa;
        FRGResourceHandle HZBHandle{};
        FPathTracingFrameResources PathTracing;
    };

private:

    bool CreateBasePassRootSignature(FDX12Device* Device);
    bool CreateLightingRootSignature(FDX12Device* Device);
    bool CreateVelocityRootSignature(FDX12Device* Device);
    bool CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat);
    bool EnsureBasePassPipeline(uint32_t PipelineKey, bool bUseSkinning);
    bool EnsureBasePassPipelineOrFail(uint32_t PipelineKey, bool bUseSkinning, const char* PassContext);
    bool CompileDeferredBasePassPs(uint32_t PipelineKey, std::vector<uint8_t>& OutPs);
    bool BuildDeferredBasePassPsoDesc(uint32_t PipelineKey, bool bUseSkinning, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const;
    bool CreateVelocityPipeline(FDX12Device* Device);
    bool CreateDepthPrepassPipeline(FDX12Device* Device);
    bool CreateLinearDepthRootSignature(FDX12Device* Device);
    bool CreateLinearDepthPipeline(FDX12Device* Device);
    bool CreateExtractHalfDepthNormalRootSignature(FDX12Device* Device);
    bool CreateExtractHalfDepthNormalPipeline(FDX12Device* Device);
    bool CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    DXGI_FORMAT ResolveRestirGiRadianceFormat(FDX12Device* Device) const;
    bool CreateHZBRootSignature(FDX12Device* Device);
    bool CreateHZBPipeline(FDX12Device* Device);
    bool CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateLinearDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateVelocityResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateHZBResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateObjectIdPipeline(FDX12Device* Device);
    bool CreateDescriptorHeap(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    bool CreateGpuDrivenResources(FDX12Device* Device);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, size_t ModelIndex, uint64_t ConstantBufferOffset);
    void UpdateSkyConstants(const FCamera& Camera);
    void UpdateCullingVisibility(const FCamera& Camera);
    void PrepareFrameState(const FCamera& Camera, bool bAnySkinningUpdated, FDeferredFrameState& OutState);
    void ConfigureFrameGraph(FRenderGraph& Graph) const;
    void FinalizeFrameState(const FDeferredFrameState& FrameState);
    void InvalidateRestirGiDenoiserHistory();
    void ApplyRendererConfig(const FRendererConfig& Config);
    bool InitializePipelineDomains(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool InitializeFrameResources(FDX12Device* Device, uint32_t Width, uint32_t Height, const FRendererConfig& Config);
    bool InitializeSceneResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config);
    bool InitializeSceneModelResources(FDX12Device* Device, const FRendererConfig& Config);
    bool InitializeEnvironmentAndDescriptorResources(FDX12Device* Device, const FRendererConfig& Config);
    bool InitializeSkyResources(FDX12Device* Device);
    bool InitializeGpuDebugResources(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);

private:
    friend class FDeferredFrameOrchestrator;
    friend class FDeferredVisibilityPasses;
    friend class FDeferredGeometryPasses;
    friend class FDeferredLightingPasses;
    friend class FDeferredPostProcessPasses;
    friend class FDeferredResourceImporter;
    friend class FGtao;
    friend class FRayTracingShadow;
    friend class FSsr;
    friend class FRestirGI;
    friend class FRestirGIDenoiser;
    friend class FPathTracing;
    friend class FAutoExposure;
    friend class FCas;
    friend class FTaa;
    friend class FTonemap;

    std::unique_ptr<FDeferredFrameOrchestrator> FrameOrchestrator;
    std::unique_ptr<FDeferredVisibilityPasses> VisibilityPasses;
    std::unique_ptr<FDeferredGeometryPasses> GeometryPasses;
    std::unique_ptr<FDeferredLightingPasses> LightingPasses;
    std::unique_ptr<FGtao> Gtao;
    std::unique_ptr<FRayTracingShadow> RayTracingShadow;
    std::unique_ptr<FSsr> Ssr;
    std::unique_ptr<FRestirGI> RestirGI;
    std::unique_ptr<FRestirGIDenoiser> RestirGIDenoiser;
    std::unique_ptr<FPathTracing> PathTracing;
    std::unique_ptr<FAutoExposure> AutoExposure;
    std::unique_ptr<FCas> Cas;
    std::unique_ptr<FTaa> Taa;
    std::unique_ptr<FTonemap> Tonemap;
    std::unique_ptr<FDeferredPostProcessPasses> PostProcessPasses;
    std::unique_ptr<FDeferredResourceImporter> ResourceImporter;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> BasePassRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> VelocityRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> HZBRootSignature;
    // Base pass pipelines indexed by permutation key (bit 0: Normal, bit 1: MR, bit 2: BaseColor, bit 3: Emissive, bit 4: AlphaMask, bit 5: SheenModel, bit 6: ClearcoatModel, bit 7: AnisotropyModel, bit 8: DoubleSided)
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 512> BasePassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 512> BasePassPipelinesSkinned;
    std::array<std::vector<uint8_t>, 2> DeferredBasePassVsBytecodes;
    std::array<std::vector<uint8_t>, 512> DeferredBasePassPsBytecodes;
    std::array<bool, 512> DeferredBasePassPsCompiled{};
    std::array<bool, 512> DeferredBasePassFailureLogged{};
    std::mutex DeferredBasePassPipelineMutex;
    DXGI_FORMAT DeferredBasePassLightingFormat = DXGI_FORMAT_UNKNOWN;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> DepthPrepassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> DepthPrepassPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> ShadowPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> ShadowPipelinesSkinned;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LinearDepthRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LinearDepthPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> ExtractHalfDepthNormalRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ExtractHalfDepthNormalPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> DirectLightingPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CompositeLightingPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> VelocityPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> VelocityPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> HZBPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ObjectIdPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSignature;
    std::vector<FModelTextureSet> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> LightingBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> VelocityTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> LinearDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlueNoiseSobolTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlueNoiseScramblingRanking1SPPTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> TonemapOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> HierarchicalZBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> HZBNullUavResource;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GBufferRTVHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> LinearDepthRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> VelocityRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferA;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferB;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferC;
    Microsoft::WRL::ComPtr<ID3D12Resource> GBufferD;
    float SkySphereRadius = 100.0f;

    DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_UNKNOWN;

    D3D12_CPU_DESCRIPTOR_HANDLE GBufferRTVHandles[4]{};
    D3D12_CPU_DESCRIPTOR_HANDLE LightingRTVHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE VelocityRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE LinearDepthRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE TonemapOutputRtvHandle{};
    std::array<uint32_t, 4> GBufferBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX } };
    uint32_t ShadowMapBindlessIndex = UINT32_MAX;
    uint32_t EnvironmentCubeBindlessIndex = UINT32_MAX;
    uint32_t BrdfLutBindlessIndex = UINT32_MAX;
    uint32_t LinearDepthBindlessIndex = UINT32_MAX;
    uint32_t VelocityBindlessIndex = UINT32_MAX;
    uint32_t BlueNoiseSobolSrvBindlessIndex = UINT32_MAX;
    uint32_t BlueNoiseScramblingRanking1SPPSrvBindlessIndex = UINT32_MAX;
    uint32_t DirectLightingBindlessIndex = UINT32_MAX;
    uint32_t LightingBufferBindlessIndex = UINT32_MAX;
    std::vector<uint32_t> DepthBindlessIndices;
    uint32_t HZBSrvBindlessIndex = UINT32_MAX;
    std::vector<uint32_t> HZBSrvMipBindlessIndices;
    std::vector<uint32_t> HZBUavBindlessIndices;
    uint32_t HZBNullUavBindlessIndex = UINT32_MAX;
    D3D12_RESOURCE_STATES GBufferStates[4] =
    {
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
    };
    D3D12_RESOURCE_STATES HZBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES LightingBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES VelocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES LinearDepthState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12Resource* DirectLightingResource = nullptr;
    std::vector<FGltfScene> GltfScenes;
    std::vector<FGltfAnimationPose> GltfScenePoses;
    std::vector<float> GltfSceneTimes;
    D3D12_RESOURCE_STATES TonemapOutputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    FMeshGeometryBuffers SkyGeometry;

    DirectX::XMFLOAT4X4 SceneWorldMatrix{};
    DirectX::XMFLOAT4X4 LightViewProjection{};
    DirectX::XMFLOAT3 PreviousCameraPosition{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4X4 PreviousCameraViewMatrix{};
    DirectX::XMFLOAT4X4 PreviousViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 CurrentViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 PreviousUnjitteredViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 CurrentUnjitteredViewProjectionMatrix{};
    bool bHasPreviousViewProjection = false;
    bool bHasPreviousUnjitteredViewProjection = false;
    bool bFirstFrame = true;
    bool bHZBEnabled = true;
    bool bHZBReady = false;
    bool bEnableHzbTwoPass = true;
    bool bEnablePbrResearch = false;
    uint32_t HZBWidth = 0;
    uint32_t HZBHeight = 0;
    uint32_t HZBMipCount = 0;
};
