#pragma once

#include "DeferredPassContext.h"

class FDeferredPostProcessPasses
{
public:
    void AddTemporalAAPass(FDeferredPassContext& Context) const;
    void AddAutoExposurePass(FDeferredPassContext& Context) const;
    void AddTonemapPass(FDeferredPassContext& Context) const;
    void AddCasPass(FDeferredPassContext& Context) const;
    void AddDebugPrintPass(FDeferredPassContext& Context) const;
};
