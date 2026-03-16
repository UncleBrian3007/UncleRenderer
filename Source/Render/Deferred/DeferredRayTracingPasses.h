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
};
