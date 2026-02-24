#pragma once

#include "DeferredPassContext.h"

class FDeferredLightingPasses
{
public:
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
    void AddLightingPass(FDeferredPassContext& Context, FRGResourceHandle SsrHandle) const;
    void AddSkyPass(FDeferredPassContext& Context) const;
};
