#pragma once

#include "DeferredPassContext.h"

class FDeferredPostProcessPasses
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat) const;
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount) const;
    void AddTemporalAAPass(FDeferredPassContext& Context) const;
    void AddAutoExposurePass(FDeferredPassContext& Context) const;
    void AddTonemapPass(FDeferredPassContext& Context) const;
    void AddCasPass(FDeferredPassContext& Context) const;
    void AddDebugPrintPass(FDeferredPassContext& Context) const;
};
