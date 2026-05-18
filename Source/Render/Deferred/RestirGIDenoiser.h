#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "../GpuResource.h"
#include "../RenderGraph.h"
#include "GBufferLayout.h"

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
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPasses(FDeferredPassContext& Context) const;
    void FinalizeFrame(FDeferredRenderer& Owner);
    void InvalidateHistory();

    void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
    bool IsEnabled() const { return bEnabled; }

    void SetFreezeHistoryResetPeriod(uint32_t InPeriod) { FreezeHistoryResetPeriod = InPeriod; }
    uint32_t GetFreezeHistoryResetPeriod() const { return FreezeHistoryResetPeriod; }

    bool IsHistoryValid() const { return bHistoryValid; }
    ID3D12Resource* GetCurrentOutputTexture() const { return HistoryIrradiance.Get(); }
    uint32_t GetCurrentOutputSrvBindlessIndex() const { return HistoryIrradiance.SrvBindlessIndex; }
    uint32_t GetCurrentOutputUavBindlessIndex() const { return HistoryIrradiance.UavBindlessIndex; }
    uint32_t GetPrevLinearDepthSrvBindlessIndex() const { return PrevLinearDepth.SrvBindlessIndex; }

private:
    friend class FDeferredRenderer;

    bool ShouldResetHistoryForFreeze(const FDeferredRenderer& Owner) const;
    void RefreshPersistentInputValidation();
    bool IsReady() const;
    void AddFreezeResetPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle) const;
    void AddPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle& PreBlurSHHandle) const;
    void AddTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle& TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const;
    void AddShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const;
    void AddFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const;

private:
    bool bEnabled = true;
    uint32_t FreezeHistoryResetPeriod = 3;
    bool bHistoryValid = false;
    bool bPersistentInputsValid = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PreBlurPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TemporalAccumulationPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GenerateShMipsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GenerateLinearDepthMipsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> HistoryReconstructionPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> FinalBlurPipeline;

    FBindlessTexture HistoryIrradiance;
    FBindlessTexture HistorySH;
    FBindlessTexture HistoryCountA;
    FBindlessTexture HistoryCountB;
    FBindlessTexture PrevLinearDepth;
    FBindlessTexture PrevNormal;
};
