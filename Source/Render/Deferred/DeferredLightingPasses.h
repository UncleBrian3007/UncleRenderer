#pragma once

#include "DeferredPassContext.h"

class FDX12Device;

class FDeferredLightingPasses
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat) const;
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const;

    void AddLinearDepthPass(FDeferredPassContext& Context) const;
    void AddGtaoPass(FDeferredPassContext& Context) const;
    void AddSsrRayCounterClearPass(FDeferredPassContext& Context) const;
    void AddSsrRayGatherPass(FDeferredPassContext& Context) const;
    void AddSsrBuildIndirectArgsPass(FDeferredPassContext& Context, bool bHwMiss) const;
    void AddSsrSwTracePass(FDeferredPassContext& Context) const;
    void AddSsrHwTracePass(FDeferredPassContext& Context) const;
    void AddSsrResolvePass(FDeferredPassContext& Context) const;
    void AddSsrPass(FDeferredPassContext& Context) const;
    void AddSsrFallbackPass(FDeferredPassContext& Context) const;
    void AddSsrDenoisePass(FDeferredPassContext& Context, FRGResourceHandle InputHandle) const;
    void AddDirectLightingPass(FDeferredPassContext& Context, FRGResourceHandle& OutDirectHandle) const;
    void AddCompositeLightPass(FDeferredPassContext& Context, FRGResourceHandle SsrHandle, FRGResourceHandle DirectHandle) const;
    void AddSkyPass(FDeferredPassContext& Context) const;
};
