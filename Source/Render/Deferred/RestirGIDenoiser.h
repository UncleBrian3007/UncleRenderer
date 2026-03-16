#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FRestirGIDenoiserFrameResources
{
    FRGResourceHandle PreBlurSHHandle{};
    FRGResourceHandle TemporalSHHandle{};
    FRGResourceHandle HistorySHHandle{};
    FRGResourceHandle HistoryIrradianceHandle{};
    FRGResourceHandle HistoryCountAHandle{};
    FRGResourceHandle HistoryCountBHandle{};
    FRGResourceHandle PrevLinearDepthHandle{};
    FRGResourceHandle PrevNormalHandle{};
    FRGResourceHandle ShMipHandle{};
    FRGResourceHandle LinearDepthMipHandle{};
    FRGBufferHandle SpdAtomicCounterHandle{};
};

class FRestirGIDenoiser
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context) const;
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPasses(FDeferredPassContext& Context) const;
    void FinalizeFrame(FDeferredRenderer& Owner);
    void InvalidateHistory();

    void SetEnabled(bool bEnabled) { bEnabled_ = bEnabled; }
    bool IsEnabled() const { return bEnabled_; }

    void SetFreezeHistoryResetPeriod(uint32_t InPeriod) { FreezeHistoryResetPeriod = InPeriod; }
    uint32_t GetFreezeHistoryResetPeriod() const { return FreezeHistoryResetPeriod; }

    bool IsHistoryValid() const { return bHistoryValid; }
    Microsoft::WRL::ComPtr<ID3D12Resource> GetCurrentOutputTexture() const { return HistoryIrradianceTexture; }
    uint32_t GetCurrentOutputSrvBindlessIndex() const { return HistoryIrradianceSrvBindlessIndex; }
    uint32_t GetCurrentOutputUavBindlessIndex() const { return HistoryIrradianceUavBindlessIndex; }
    uint32_t GetPrevLinearDepthSrvBindlessIndex() const { return PrevLinearDepthSrvBindlessIndex; }

private:
    friend class FDeferredRenderer;

    bool ShouldResetHistoryForFreeze(const FDeferredRenderer& Owner) const;
    void AddFreezeResetPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle) const;
    void AddPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle& PreBlurSHHandle) const;
    void AddTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle& TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const;
    void AddShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const;
    void AddFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const;

private:
    bool bEnabled_ = true;
    uint32_t FreezeHistoryResetPeriod = 3;
    bool bHistoryValid = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PreBlurPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TemporalAccumulationPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GenerateShMipsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GenerateLinearDepthMipsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> HistoryReconstructionPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> FinalBlurPipeline;

    Microsoft::WRL::ComPtr<ID3D12Resource> HistorySHTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> HistoryIrradianceTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> HistoryCountATexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> HistoryCountBTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> PrevLinearDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> PrevNormalTexture;

    uint32_t HistoryIrradianceSrvBindlessIndex = UINT32_MAX;
    uint32_t HistoryIrradianceUavBindlessIndex = UINT32_MAX;
    uint32_t HistorySHSrvBindlessIndex = UINT32_MAX;
    uint32_t HistorySHUavBindlessIndex = UINT32_MAX;
    uint32_t HistoryCountASrvBindlessIndex = UINT32_MAX;
    uint32_t HistoryCountAUavBindlessIndex = UINT32_MAX;
    uint32_t HistoryCountBSrvBindlessIndex = UINT32_MAX;
    uint32_t HistoryCountBUavBindlessIndex = UINT32_MAX;
    uint32_t PrevLinearDepthSrvBindlessIndex = UINT32_MAX;
    uint32_t PrevLinearDepthUavBindlessIndex = UINT32_MAX;
    uint32_t PrevNormalSrvBindlessIndex = UINT32_MAX;
    uint32_t PrevNormalUavBindlessIndex = UINT32_MAX;

    D3D12_RESOURCE_STATES HistoryIrradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES HistorySHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES HistoryCountAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES HistoryCountBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES PrevLinearDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES PrevNormalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};
