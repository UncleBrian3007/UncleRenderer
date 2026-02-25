#pragma once

#include "DeferredPassContext.h"

class FDX12Device;

class FDeferredGeometryPasses
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT LightingBufferFormat) const;
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const;

    void AddShadowPass(FDeferredPassContext& Context) const;
    void AddDepthPrepass(FDeferredPassContext& Context) const;
    void AddBasePass(FDeferredPassContext& Context, bool bClearTargets, bool bClearDepth, const char* PassName, bool bAllowSkinningFallback) const;
    void AddObjectIdPass(FDeferredPassContext& Context) const;
    void AddVelocityPass(FDeferredPassContext& Context) const;
};
