#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "../RenderGraph.h"
#include "../../Core/RendererConfig.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12CommandContext;
class FDX12Device;

struct FRestirGIFrameResources
{
    FRGResourceHandle RestirGIHalfDepthNormalHandle{};
    FRGResourceHandle RestirGIHandle{};
    FRGResourceHandle RestirGIHistoryHandle{};
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
    FRGResourceHandle RestirGiInputSHHandle{};
    FRGResourceHandle RestirGiVarianceHandle{};
};

class FRestirGI
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    void AddPasses(FDeferredPassContext& Context) const;
    void ImportPersistentResources(FDeferredPassContext& Context) const;
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void FinalizeFrame(FDeferredRenderer& Owner);
    void InvalidateReservoirHistory();

    void SetEnabled(bool bEnabled) { bEnabled_ = bEnabled; }
    bool IsEnabled() const { return bEnabled_; }

    void SetSamplesPerPixel(uint32_t Samples) { SamplesPerPixel = Samples; }
    uint32_t GetSamplesPerPixel() const { return SamplesPerPixel; }

    void SetIntensity(float Intensity) { Intensity_ = Intensity; }
    float GetIntensity() const { return Intensity_; }

    void SetShowOnly(bool bEnabled) { bShowOnly = bEnabled; }
    bool IsShowOnly() const { return bShowOnly; }

    void SetRayLength(float Value) { RayLength = Value; }
    float GetRayLength() const { return RayLength; }

    void SetClampThreshold(float Value) { ClampThreshold = Value; }
    float GetClampThreshold() const { return ClampThreshold; }

    void SetTemporalReuseEnabled(bool bEnabled) { bTemporalReuse = bEnabled; }
    bool IsTemporalReuseEnabled() const { return bTemporalReuse; }

    void SetSpatialReuseEnabled(bool bEnabled) { bSpatialReuse = bEnabled; }
    bool IsSpatialReuseEnabled() const { return bSpatialReuse; }

    void SetTemporalAdditionalScale(float Value) { TemporalAdditionalScale = Value; }
    float GetTemporalAdditionalScale() const { return TemporalAdditionalScale; }

    void SetSpatialAdditionalScale(float Value) { SpatialAdditionalScale = Value; }
    float GetSpatialAdditionalScale() const { return SpatialAdditionalScale; }

    void SetResolveMinDenominator(float Value) { ResolveMinDenominator = Value; }
    float GetResolveMinDenominator() const { return ResolveMinDenominator; }

    void SetResolveMaxNormalization(float Value) { ResolveMaxNormalization = Value; }
    float GetResolveMaxNormalization() const { return ResolveMaxNormalization; }

    void SetResolveLowSampleBoostGuard(float Value) { ResolveLowSampleBoostGuard = Value; }
    float GetResolveLowSampleBoostGuard() const { return ResolveLowSampleBoostGuard; }

    void SetResolveUseConfidence(bool bEnabled) { bResolveUseConfidence = bEnabled; }
    bool IsResolveUseConfidence() const { return bResolveUseConfidence; }

    void SetUseVisibility(bool bEnabled) { bUseVisibility = bEnabled; }
    bool IsUseVisibility() const { return bUseVisibility; }

    void SetUseBrdf(bool bEnabled) { bUseBrdf = bEnabled; }
    bool IsUseBrdf() const { return bUseBrdf; }

    void SetUseHistoryIndirect(bool bEnabled) { bUseHistoryIndirect = bEnabled; }
    bool IsUseHistoryIndirect() const { return bUseHistoryIndirect; }

    void SetRandomMode(ERestirGIRandomMode Mode) { RandomMode = Mode; }
    ERestirGIRandomMode GetRandomMode() const { return RandomMode; }

    void SetDebugRayEnabled(bool bEnabled) { bDebugRayEnabled = bEnabled; }
    bool IsDebugRayEnabled() const { return bDebugRayEnabled; }

    void SetDebugPixel(uint32_t X, uint32_t Y)
    {
        DebugPixelX = X;
        DebugPixelY = Y;
    }

    uint32_t GetDebugPixelX() const { return DebugPixelX; }
    uint32_t GetDebugPixelY() const { return DebugPixelY; }

    void SetFreezeFrame(bool bEnabled, uint64_t FrameNumber);
    bool IsFreezeFrame() const { return bFreezeFrame; }
    uint32_t GetFrozenSequenceFrame() const { return FrozenSequenceFrame; }
    uint64_t GetFreezeStartFrameNumber() const { return FreezeStartFrameNumber; }
    void StepFreezeFrame() { ++FrozenSequenceFrame; }

    void SetMaxHistoryFrames(uint32_t Value) { MaxHistoryFrames = Value; }
    uint32_t GetMaxHistoryFrames() const { return MaxHistoryFrames; }

    Microsoft::WRL::ComPtr<ID3D12Resource> GetCurrentOutputTexture() const { return RestirGITexture; }
    uint32_t GetCurrentOutputSrvBindlessIndex() const { return RestirGIBindlessIndex; }
    uint32_t GetCurrentOutputUavBindlessIndex() const { return RestirGIUavBindlessIndex; }
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GetRootSignature() const { return RestirGIRootSignature; }
    bool IsReservoirHistoryValid() const { return bReservoirHistoryValid; }
    uint32_t GetReservoirHistoryFrameCount() const { return ReservoirHistoryFrameCount; }

private:
    friend class FDeferredRenderer;

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device);
    DXGI_FORMAT ResolveRadianceFormat(FDX12Device* Device) const;
    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    uint32_t GetDepthBindlessIndexForRestir(FDeferredRenderer& Owner) const;
    void DispatchRestirPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, const wchar_t* EventName, uint32_t SpatialPassIndex, const uint32_t BindlessIndices[30], uint32_t DispatchWidth, uint32_t DispatchHeight, bool bEnabled) const;
    void AddInitialSamplingPass(FDeferredPassContext& Context) const;
    void AddTemporalResamplingPass(FDeferredPassContext& Context) const;
    void AddReservoirBootstrapPass(FDeferredPassContext& Context) const;
    void AddSpatialResampling0Pass(FDeferredPassContext& Context) const;
    void AddSpatialResampling1Pass(FDeferredPassContext& Context) const;
    void AddResolvePass(FDeferredPassContext& Context) const;

private:
    bool bEnabled_ = false;
    uint32_t SamplesPerPixel = 2;
    float Intensity_ = 1.0f;
    bool bShowOnly = false;
    float RayLength = 1000.0f;
    float ClampThreshold = 10.0f;
    bool bTemporalReuse = true;
    bool bSpatialReuse = true;
    float TemporalAdditionalScale = 0.2f;
    float SpatialAdditionalScale = 0.15f;
    float ResolveMinDenominator = 1e-5f;
    float ResolveMaxNormalization = 32.0f;
    float ResolveLowSampleBoostGuard = 0.6f;
    bool bResolveUseConfidence = true;
    uint32_t MaxHistoryFrames = 1;
    bool bUseVisibility = true;
    bool bUseBrdf = true;
    bool bUseHistoryIndirect = true;
    ERestirGIRandomMode RandomMode = ERestirGIRandomMode::BlueNoiseSobol;
    bool bDebugRayEnabled = false;
    uint32_t DebugPixelX = 0;
    uint32_t DebugPixelY = 0;
    bool bFreezeFrame = false;
    uint32_t FrozenSequenceFrame = 0;
    uint64_t FreezeStartFrameNumber = 0;
    uint32_t ReservoirHistoryFrameCount = 0;
    bool bReservoirHistoryValid = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RestirGIRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> RestirGIInitialPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGIReservoirBootstrapPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGITemporalPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGISpatialPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RestirGIResolvePipeline;

    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGITexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIHistoryTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirDepthNormalATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirDepthNormalBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirSampleRadianceATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirSampleRadianceBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirRayDirectionATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirRayDirectionBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirMWATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> RestirGIReservoirMWBTexture;

    uint32_t RestirGIBindlessIndex = UINT32_MAX;
    uint32_t RestirGIUavBindlessIndex = UINT32_MAX;
    uint32_t RestirGIHistorySrvBindlessIndex = UINT32_MAX;
    uint32_t RestirGIHistoryUavBindlessIndex = UINT32_MAX;
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

    D3D12_RESOURCE_STATES RestirGIState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIHistoryState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirDepthNormalAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirDepthNormalBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirSampleRadianceAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirSampleRadianceBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirRayDirectionAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirRayDirectionBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirMWAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES RestirGIReservoirMWBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
