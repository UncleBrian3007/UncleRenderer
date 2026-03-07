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
    void AddRestirGIPass(FDeferredPassContext& Context) const;
    void AddRestirGiDenoiserPasses(FDeferredPassContext& Context) const;

private:
    void AddRestirGIPassImpl(FDeferredPassContext& Context) const;
    void AddInitialSamplingPass(FDeferredPassContext& Context) const;
    void AddTemporalResamplingPass(FDeferredPassContext& Context) const;
    void AddReservoirBootstrapPass(FDeferredPassContext& Context) const;
    void AddSpatialResampling0Pass(FDeferredPassContext& Context) const;
    void AddSpatialResampling1Pass(FDeferredPassContext& Context) const;
    void AddResolvePass(FDeferredPassContext& Context) const;
    uint32_t GetDepthBindlessIndexForRestir(FDeferredRenderer& Owner) const;
    void DispatchRestirPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, const wchar_t* EventName, uint32_t SpatialPassIndex, const uint32_t BindlessIndices[30], uint32_t DispatchWidth, uint32_t DispatchHeight, bool bEnabled) const;
    void AddRestirGiDenoiserPreTemporalPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredRenderer::FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const;
    void AddRestirGiShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle DestinationHandle, FRGBufferHandle AtomicCounterHandle) const;
    void AddRestirGiLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle DestinationHandle, FRGBufferHandle AtomicCounterHandle) const;
    void AddRestirGiHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const;
    void AddRestirGiFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const;
};
