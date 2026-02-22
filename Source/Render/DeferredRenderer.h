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
#include "RendererUtils.h"
#include "TextureLoader.h"
#include "RenderGraph.h"
#include "../Core/RendererConfig.h"
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
    void SetSsrSwEnabled(bool bEnabled) { bSsrSwEnabled = bEnabled; }
    void SetSsrHwEnabled(bool bEnabled) { bSsrHwEnabled = bEnabled; }
    void SetSsrHzbEnabled(bool bEnabled) { bSsrHzbEnabled = bEnabled; }
    void SetSsrRefineEnabled(bool bEnabled) { bSsrRefineEnabled = bEnabled; }
    void SetSsrDenoiseEnabled(bool bEnabled) { bSsrDenoiseEnabled = bEnabled; }
    void SetSsrMode(ESSRMode Mode) { SsrMode = Mode; }
    void SetSsrSamplesPerQuad(uint32_t Samples) { SsrSamplesPerQuad = Samples; }
    bool IsSsrSwEnabled() const { return bSsrSwEnabled; }
    bool IsSsrHwEnabled() const { return bSsrHwEnabled; }
    void SetSsrMaxSteps(uint32_t Steps) { SsrMaxSteps = Steps; }
    uint32_t GetSsrMaxSteps() const { return SsrMaxSteps; }
    void SetSsrMaxDistance(float Distance) { SsrMaxDistance = Distance; }
    float GetSsrMaxDistance() const { return SsrMaxDistance; }
    void SetSsrThickness(float Thickness) { SsrThickness = Thickness; }
    float GetSsrThickness() const { return SsrThickness; }
    void SetSsrStride(float Stride) { SsrStride = Stride; }
    float GetSsrStride() const { return SsrStride; }
    void SetSsrRoughnessCutoff(float Cutoff) { SsrRoughnessCutoff = Cutoff; }
    float GetSsrRoughnessCutoff() const { return SsrRoughnessCutoff; }
    void SetSsrIntensity(float Intensity) { SsrIntensity = Intensity; }
    float GetSsrIntensity() const { return SsrIntensity; }
    void SetRestirGIEnabled(bool bEnabled) { bRestirGIEnabled = bEnabled; }
    bool IsRestirGIEnabled() const { return bRestirGIEnabled; }
    void SetRestirGISamplesPerPixel(uint32_t Samples) { RestirGISamplesPerPixel = Samples; }
    uint32_t GetRestirGISamplesPerPixel() const { return RestirGISamplesPerPixel; }
    void SetRestirGIIntensity(float Intensity) { RestirGIIntensity = Intensity; }
    float GetRestirGIIntensity() const { return RestirGIIntensity; }
    void SetRestirGIShowOnly(bool bEnabled) { bRestirGIShowOnly = bEnabled; }
    bool IsRestirGIShowOnly() const { return bRestirGIShowOnly; }
    void SetRestirGITemporalReuseEnabled(bool bEnabled) { bRestirGITemporalReuse = bEnabled; }
    bool IsRestirGITemporalReuseEnabled() const { return bRestirGITemporalReuse; }
    void SetRestirGISpatialReuseEnabled(bool bEnabled) { bRestirGISpatialReuse = bEnabled; }
    bool IsRestirGISpatialReuseEnabled() const { return bRestirGISpatialReuse; }
    void SetRestirGITemporalAdditionalScale(float Value) { RestirGITemporalAdditionalScale = Value; }
    float GetRestirGITemporalAdditionalScale() const { return RestirGITemporalAdditionalScale; }
    void SetRestirGISpatialAdditionalScale(float Value) { RestirGISpatialAdditionalScale = Value; }
    float GetRestirGISpatialAdditionalScale() const { return RestirGISpatialAdditionalScale; }
    void SetRestirGIResolveMinDenominator(float Value) { RestirGIResolveMinDenominator = Value; }
    float GetRestirGIResolveMinDenominator() const { return RestirGIResolveMinDenominator; }
    void SetRestirGIResolveMaxNormalization(float Value) { RestirGIResolveMaxNormalization = Value; }
    float GetRestirGIResolveMaxNormalization() const { return RestirGIResolveMaxNormalization; }
    void SetRestirGIResolveLowSampleBoostGuard(float Value) { RestirGIResolveLowSampleBoostGuard = Value; }
    float GetRestirGIResolveLowSampleBoostGuard() const { return RestirGIResolveLowSampleBoostGuard; }
    void SetRestirGIResolveUseConfidence(bool bEnabled) { bRestirGIResolveUseConfidence = bEnabled; }
    bool IsRestirGIResolveUseConfidence() const { return bRestirGIResolveUseConfidence; }

    void SetRestirGIUseVisibility(bool bEnabled) { bRestirGIUseVisibility = bEnabled; }
    bool IsRestirGIUseVisibility() const { return bRestirGIUseVisibility; }
    void SetRestirGIUseBrdf(bool bEnabled) { bRestirGIUseBrdf = bEnabled; }
    bool IsRestirGIUseBrdf() const { return bRestirGIUseBrdf; }
    void SetRestirGIUseHistoryIndirect(bool bEnabled) { bRestirGIUseHistoryIndirect = bEnabled; }
    bool IsRestirGIUseHistoryIndirect() const { return bRestirGIUseHistoryIndirect; }
    void SetRestirGIDebugRayEnabled(bool bEnabled) { bRestirGIDebugRayEnabled = bEnabled; }
    bool IsRestirGIDebugRayEnabled() const { return bRestirGIDebugRayEnabled; }
    void SetRestirGIDebugPixel(uint32_t X, uint32_t Y) { RestirGIDebugPixelX = X; RestirGIDebugPixelY = Y; }
    void SetRestirGIFreezeFrame(bool bEnabled)
    {
        if (bEnabled && !bRestirGIFreezeFrame)
        {
            RestirGIFrozenSequenceFrame = GetFrameIndex();
        }
        bRestirGIFreezeFrame = bEnabled;
    }
    bool IsRestirGIFreezeFrame() const { return bRestirGIFreezeFrame; }
    void StepRestirGIFreezeFrame() { ++RestirGIFrozenSequenceFrame; }

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
        bool bCameraMoved = false;
        bool bAnySkinningUpdated = false;
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
        FRGResourceHandle VelocityHandle{};
        std::array<FRGResourceHandle, 4> GBufferHandles{};
        FRGResourceHandle LinearDepthHandle{};
        FRGResourceHandle GtaoHandle{};
        FRGResourceHandle RestirGIHandle{};
        FRGResourceHandle RestirGIHistoryHandle{};
        FRGBufferHandle RestirGITemporalReservoirHandle{};
        FRGBufferHandle RestirGISpatialReservoirHandle{};
        FRGBufferHandle RestirGIReservoirHistoryHandle{};
        FRGResourceHandle RestirGIInitialRadianceHandle{};
        FRGResourceHandle RestirGIInitialRayDirectionHandle{};
        FRGResourceHandle RestirGIReservoirDepthNormalAHandle{};
        FRGResourceHandle RestirGIReservoirDepthNormalBHandle{};
        FRGResourceHandle RestirGIReservoirSampleRadianceAHandle{};
        FRGResourceHandle RestirGIReservoirSampleRadianceBHandle{};
        FRGResourceHandle RestirGIReservoirRayDirectionAHandle{};
        FRGResourceHandle RestirGIReservoirRayDirectionBHandle{};
        FRGResourceHandle RestirGIReservoirMWAHandle{};
        FRGResourceHandle RestirGIReservoirMWBHandle{};
        FRGResourceHandle SsrHandle{};
        FRGResourceHandle SsrDenoiseHandle{};
        FRGResourceHandle SsrFallbackHandle{};
        FRGResourceHandle SsrResolveHandle{};
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
    bool CreateRestirGIRootSignature(FDX12Device* Device);
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
    bool CreateGtaoRootSignature(FDX12Device* Device);
    bool CreateGtaoPipeline(FDX12Device* Device);
    bool CreateSsrRootSignature(FDX12Device* Device);
    bool CreateSsrPipeline(FDX12Device* Device);
    bool EnsureSsrGraphicsPipeline(uint32_t PipelineIndex);
    bool EnsureSsrGraphicsPipelineOrFail(uint32_t PipelineIndex, const char* PassContext);
    bool CompileSsrGraphicsPs(uint32_t PipelineIndex, std::vector<uint8_t>& OutPs);
    bool BuildSsrGraphicsPsoDesc(uint32_t PipelineIndex, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const;
    bool CreateSsrDenoiseRootSignature(FDX12Device* Device);
    bool CreateSsrDenoisePipeline(FDX12Device* Device);
    bool CreateSsrRayGatherRootSignature(FDX12Device* Device);
    bool CreateSsrRayGatherPipeline(FDX12Device* Device);
    bool CreateSsrSwTraceRootSignature(FDX12Device* Device);
    bool CreateSsrSwTracePipeline(FDX12Device* Device);
    bool EnsureSsrSwTracePipeline(uint32_t PipelineIndex);
    bool EnsureSsrSwTracePipelineOrFail(uint32_t PipelineIndex, const char* PassContext);
    bool CompileSsrSwTraceCs(uint32_t PipelineIndex, std::vector<uint8_t>& OutCs);
    bool CreateSsrBuildIndirectArgsRootSignature(FDX12Device* Device);
    bool CreateSsrBuildIndirectArgsPipeline(FDX12Device* Device);
    bool CreateSsrResolveRootSignature(FDX12Device* Device);
    bool CreateSsrResolvePipeline(FDX12Device* Device);
    bool CreateSsrDispatchCommandSignature(FDX12Device* Device);
    bool CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateRestirGIPipeline(FDX12Device* Device);
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
    bool CreateRestirGIResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateVelocityResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateSsrResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
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
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, size_t ModelIndex, uint64_t ConstantBufferOffset);
    void UpdateSkyConstants(const FCamera& Camera);
    void UpdateCullingVisibility(const FCamera& Camera);
    void PrepareFrameState(const FCamera& Camera, bool bAnySkinningUpdated, FDeferredFrameState& OutState);
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
        const std::array<FRGResourceHandle, 4>& GBufferHandles,
        FRGResourceHandle DepthHandle,
        FRGResourceHandle LightingHandle,
        bool bClearTargets,
        bool bClearDepth,
        const char* PassName,
        bool bAllowSkinningFallback);
    void AddObjectIdPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle ObjectIdHandle, FRGResourceHandle DepthHandle);
    void AddVelocityPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const FCamera& Camera, FRGResourceHandle VelocityHandle, FRGResourceHandle DepthHandle);
    void AddHZBPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle HZBHandle);
    void AddLinearDepthPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle LinearDepthHandle);
    void AddGtaoPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle GtaoHandle);
    void AddRestirGIPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle RestirGIHandle, FRGResourceHandle RestirGIHistoryHandle, FRGResourceHandle RestirGIInitialRadianceHandle, FRGResourceHandle RestirGIInitialRayDirectionHandle, FRGResourceHandle RestirGIReservoirDepthNormalAHandle, FRGResourceHandle RestirGIReservoirDepthNormalBHandle, FRGResourceHandle RestirGIReservoirSampleRadianceAHandle, FRGResourceHandle RestirGIReservoirSampleRadianceBHandle, FRGResourceHandle RestirGIReservoirRayDirectionAHandle, FRGResourceHandle RestirGIReservoirRayDirectionBHandle, FRGResourceHandle RestirGIReservoirMWAHandle, FRGResourceHandle RestirGIReservoirMWBHandle);
    void AddSsrRayCounterClearPass(FRenderGraph& Graph, uint32_t FrameIndex);
    void AddSsrRayGatherPass(FRenderGraph& Graph, uint32_t FrameIndex, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle);
    void AddSsrBuildIndirectArgsPass(FRenderGraph& Graph, uint32_t FrameIndex, bool bHwMiss);
    void AddSsrSwTracePass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle HZBHandle, FRGResourceHandle SsrHandle);
    void AddSsrHwTracePass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const FCamera& Camera, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle SsrHandle);
    void AddSsrResolvePass(FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle SsrInputHandle, FRGResourceHandle SsrResolveHandle);
    void AddSsrPass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle HZBHandle, FRGResourceHandle SsrHandle);
    void AddSsrFallbackPass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const FCamera& Camera, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle SsrFallbackHandle);
    void AddSsrDenoisePass(FRenderGraph& Graph, FRGResourceHandle SsrHandle, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle SsrDenoiseHandle);
    void AddLightingPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle GtaoHandle, FRGResourceHandle RestirGIHandle, FRGResourceHandle SsrHandle, FRGResourceHandle SsrFallbackHandle, FRGResourceHandle ShadowHandle, FRGResourceHandle LightingHandle);
    void AddPathTracingPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferAHandle, FRGResourceHandle GBufferBHandle, FRGResourceHandle GBufferCHandle, FRGResourceHandle OutputHandle);
    void AddPathTracingAccumulationPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle PathTracingTempHandle, FRGResourceHandle LightingHandle, const std::vector<FRGResourceHandle>& AccumulationHandles);
    void AddSkyPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle LightingHandle);
    void AddTemporalAAPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle LightingHandle, const std::vector<FRGResourceHandle>& TaaHandles);
    void AddAutoExposurePass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle LightingHandle, const std::array<FRGResourceHandle, 2>& LuminanceHandles, float DeltaTime);
    void AddTonemapPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LightingHandle, FRGResourceHandle TonemapOutputResource, const std::array<FRGResourceHandle, 2>& LuminanceHandles, const std::vector<FRGResourceHandle>& TaaHandles, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddCasPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle TonemapOutputResource, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddDebugPrintPass(FRenderGraph& Graph, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void FinalizeFrameState(const FDeferredFrameState& FrameState);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> BasePassRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LightingRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RestirGIRootSignature;
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
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GtaoRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> GtaoPipelines;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrDenoiseRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8> SsrPipelines;
    std::vector<uint8_t> SsrGraphicsVsBytecode;
    std::array<std::vector<uint8_t>, 8> SsrGraphicsPsBytecodes;
    std::array<bool, 8> SsrGraphicsPsCompiled{};
    std::array<bool, 8> SsrGraphicsFailureLogged{};
    std::mutex SsrGraphicsPipelineMutex;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrDenoisePipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrRayGatherRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrRayGatherPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrSwTraceRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8> SsrSwTracePipelines;
    std::array<std::vector<uint8_t>, 8> SsrSwTraceCsBytecodes;
    std::array<bool, 8> SsrSwTraceCsCompiled{};
    std::array<bool, 8> SsrSwTraceFailureLogged{};
    std::mutex SsrSwTracePipelineMutex;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrBuildIndirectArgsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrBuildIndirectArgsPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrResolveRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrResolvePipeline;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> SsrDispatchCommandSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> LightingPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> VelocityPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> VelocityPipelinesSkinned;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGIInitialPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGITemporalPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGISpatialPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGIResolvePipeline;
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
    Microsoft::WRL::ComPtr<ID3D12Resource> VelocityTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> LinearDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> GtaoTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGITexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIHistoryTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGITemporalReservoirBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGISpatialReservoirBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirHistoryBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIInitialRadianceTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIInitialRayDirectionTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirDepthNormalATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirDepthNormalBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirSampleRadianceATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirSampleRadianceBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirRayDirectionATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirRayDirectionBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirMWATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirMWBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> SsrTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> SsrDenoiseTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> SsrFallbackTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> SsrResolveTexture;
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
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> VelocityRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GtaoRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> SsrRtvHeap;
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
    D3D12_CPU_DESCRIPTOR_HANDLE GtaoRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE SsrRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE SsrDenoiseRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE TonemapOutputRtvHandle{};
    std::array<uint32_t, 4> GBufferBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX } };
    uint32_t ShadowMapBindlessIndex = UINT32_MAX;
    uint32_t EnvironmentCubeBindlessIndex = UINT32_MAX;
    uint32_t BrdfLutBindlessIndex = UINT32_MAX;
    uint32_t LinearDepthBindlessIndex = UINT32_MAX;
    uint32_t VelocityBindlessIndex = UINT32_MAX;
    uint32_t HilbertLutBindlessIndex = UINT32_MAX;
    uint32_t GtaoBindlessIndex = UINT32_MAX;
    uint32_t RestirGIBindlessIndex = UINT32_MAX;
    uint32_t RestirGIUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIHistorySrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIHistoryUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGITemporalReservoirSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGITemporalReservoirUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGISpatialReservoirSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGISpatialReservoirUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirHistorySrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirHistoryUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIInitialRadianceSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIInitialRadianceUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIInitialRayDirectionSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIInitialRayDirectionUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirDepthNormalASrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirDepthNormalAUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirDepthNormalBSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirDepthNormalBUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirSampleRadianceASrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirSampleRadianceAUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirSampleRadianceBSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirSampleRadianceBUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirRayDirectionASrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirRayDirectionAUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirRayDirectionBSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirRayDirectionBUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirMWASrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirMWAUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirMWBSrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIReservoirMWBUavBindlessIndex = UINT32_MAX;
    uint32_t SsrBindlessIndex = UINT32_MAX;
    uint32_t SsrDenoiseBindlessIndex = UINT32_MAX;
    uint32_t SsrFallbackBindlessIndex = UINT32_MAX;
    uint32_t SsrFallbackUavBindlessIndex = UINT32_MAX;
    uint32_t SsrUavBindlessIndex = UINT32_MAX;
    uint32_t SsrResolveBindlessIndex = UINT32_MAX;
    uint32_t SsrResolveUavBindlessIndex = UINT32_MAX;
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
    D3D12_RESOURCE_STATES GtaoState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES RestirGIState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIHistoryState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGITemporalReservoirState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGISpatialReservoirState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirHistoryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES RestirGIInitialRadianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIInitialRayDirectionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirDepthNormalAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirDepthNormalBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirSampleRadianceAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirSampleRadianceBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirRayDirectionAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirRayDirectionBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirMWAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirMWBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES SsrState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES SsrDenoiseState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES SsrFallbackState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES SsrResolveState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayListBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayCounterBuffers;
    std::vector<uint32_t> SsrRayListSrvBindlessIndices;
    std::vector<uint32_t> SsrRayListUavBindlessIndices;
    std::vector<uint32_t> SsrRayCounterSrvBindlessIndices;
    std::vector<uint32_t> SsrRayCounterUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SsrRayListStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayCounterStates;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayListPrimaryBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayCounterPrimaryBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayListHwMissBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayCounterHwMissBuffers;
    std::vector<uint32_t> SsrRayListPrimarySrvBindlessIndices;
    std::vector<uint32_t> SsrRayListPrimaryUavBindlessIndices;
    std::vector<uint32_t> SsrRayCounterPrimarySrvBindlessIndices;
    std::vector<uint32_t> SsrRayCounterPrimaryUavBindlessIndices;
    std::vector<uint32_t> SsrRayListHwMissSrvBindlessIndices;
    std::vector<uint32_t> SsrRayListHwMissUavBindlessIndices;
    std::vector<uint32_t> SsrRayCounterHwMissSrvBindlessIndices;
    std::vector<uint32_t> SsrRayCounterHwMissUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SsrRayListPrimaryStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayCounterPrimaryStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayListHwMissStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayCounterHwMissStates;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrIndirectArgsPrimaryBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrIndirectArgsHwMissBuffers;
    std::vector<uint32_t> SsrIndirectArgsPrimaryUavBindlessIndices;
    std::vector<uint32_t> SsrIndirectArgsHwMissUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SsrIndirectArgsPrimaryStates;
    std::vector<D3D12_RESOURCE_STATES> SsrIndirectArgsHwMissStates;
    uint32_t SsrMaxRayCount = 0;
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
    DirectX::XMFLOAT4X4 PreviousViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 CurrentViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 PreviousUnjitteredViewProjectionMatrix{};
    DirectX::XMFLOAT4X4 CurrentUnjitteredViewProjectionMatrix{};
    bool bHasPreviousViewProjection = false;
    bool bHasPreviousUnjitteredViewProjection = false;
    bool bFirstFrame = true;
    uint32_t LuminanceWriteIndex = 0;
    bool bLuminanceHistoryValid = false;
    bool bHZBEnabled = true;
    bool bHZBReady = false;
    bool bEnableHzbTwoPass = true;
    bool bEnablePbrResearch = false;
    bool bSsrSwEnabled = true;
    bool bSsrHwEnabled = true;
    bool bSsrHzbEnabled = false;
    bool bSsrRefineEnabled = false;
    bool bSsrDenoiseEnabled = false;
    bool bRestirGIEnabled = false;
	uint32_t SsrMaxSteps = 32;
	float SsrMaxDistance = 50.0f;
	float SsrThickness = 1.00f;
	float SsrStride = 1.0f;
	float SsrRoughnessCutoff = 0.6f;
	float SsrIntensity = 0.3f;
    uint32_t RestirGISamplesPerPixel = 2;
    float RestirGIIntensity = 1.0f;
    bool bRestirGIShowOnly = false;
    float RestirGIRayLength = 100.0f;
    float RestirGIClamp = 10.0f;
    bool bRestirGITemporalReuse = true;
    bool bRestirGISpatialReuse = false;
    float RestirGITemporalAdditionalScale = 0.2f;
    float RestirGISpatialAdditionalScale = 0.15f;
    float RestirGIResolveMinDenominator = 1e-5f;
    float RestirGIResolveMaxNormalization = 32.0f;
    float RestirGIResolveLowSampleBoostGuard = 0.6f;
    bool bRestirGIResolveUseConfidence = true;
    uint32_t RestirGIMaxHistoryFrames = 1;
    bool bRestirGIUseVisibility = true;
    bool bRestirGIUseBrdf = true;
    bool bRestirGIUseHistoryIndirect = true;
    bool bRestirGIDebugRayEnabled = false;
    uint32_t RestirGIDebugPixelX = 0;
    uint32_t RestirGIDebugPixelY = 0;
    bool bRestirGIFreezeFrame = false;
    uint32_t RestirGIFrozenSequenceFrame = 0;
    uint32_t RestirGIHistoryFrameCount = 0;
    bool bRestirGIHistoryValid = false;
    ESSRMode SsrMode = ESSRMode::PS;
    uint32_t SsrSamplesPerQuad = 1;

    uint32_t HZBWidth = 0;
    uint32_t HZBHeight = 0;
    uint32_t HZBMipCount = 0;
};
