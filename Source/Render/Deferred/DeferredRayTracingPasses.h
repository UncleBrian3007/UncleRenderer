#pragma once

#include "DeferredPassContext.h"

class FDX12Device;

class FDeferredRayTracingPasses
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device) const;
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount) const;

    void AddRayTracingShadowPass(FDeferredPassContext& Context) const;
    void AddPathTracingPass(FDeferredPassContext& Context) const;
    void AddPathTracingAccumulationPass(FDeferredPassContext& Context) const;
    void AddRestirGiDenoiserPasses(FDeferredPassContext& Context) const;

private:
    void AddRestirGiDenoiserFreezeResetPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle) const;
    void AddRestirGiDenoiserPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle& PreBlurSHHandle) const;
    void AddRestirGiDenoiserTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredRenderer::FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle& TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const;
    void AddRestirGiShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddRestirGiLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddRestirGiHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const;
    void AddRestirGiFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const;
};
