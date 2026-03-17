#pragma once

#include "DeferredPassContext.h"

class FDX12Device;

class FDeferredLightingPasses
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat) const;
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const;

    void AddLinearDepthPass(FDeferredPassContext& Context) const;
    void AddExtractHalfDepthNormalPass(FDeferredPassContext& Context) const;
    void AddDirectLightingPass(FDeferredPassContext& Context, FRGResourceHandle& OutDirectHandle) const;
    void AddCompositeLightPass(FDeferredPassContext& Context, FRGResourceHandle DirectHandle) const;
    void AddSkyPass(FDeferredPassContext& Context) const;
};
