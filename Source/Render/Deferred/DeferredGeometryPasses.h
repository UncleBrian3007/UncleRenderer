#pragma once

#include "DeferredPassContext.h"

class FDeferredGeometryPasses
{
public:
    void AddShadowPass(FDeferredPassContext& Context) const;
    void AddDepthPrepass(FDeferredPassContext& Context) const;
    void AddBasePass(FDeferredPassContext& Context, bool bClearTargets, bool bClearDepth, const char* PassName, bool bAllowSkinningFallback) const;
    void AddObjectIdPass(FDeferredPassContext& Context) const;
    void AddVelocityPass(FDeferredPassContext& Context) const;
};
