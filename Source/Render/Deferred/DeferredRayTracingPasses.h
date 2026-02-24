#pragma once

#include "DeferredPassContext.h"

class FDeferredRayTracingPasses
{
public:
    void AddRayTracingShadowPass(FDeferredPassContext& Context) const;
    void AddPathTracingPass(FDeferredPassContext& Context) const;
    void AddPathTracingAccumulationPass(FDeferredPassContext& Context) const;
    void AddRestirGIPass(FDeferredPassContext& Context) const;
    void AddRestirGiDenoiserPasses(FDeferredPassContext& Context) const;
};
