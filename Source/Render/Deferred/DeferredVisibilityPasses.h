#pragma once

#include "DeferredPassContext.h"

class FDX12Device;

class FDeferredVisibilityPasses
{
public:
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device) const;

    void AddVisibilityListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex, uint32_t VisibilityFrameIndex) const;
    void AddGpuCullingPass(
        FDeferredPassContext& Context,
        FRenderer::ECullingMode Mode,
        uint32_t VisibilityInputIndex,
        uint32_t VisibilityInputFrameIndex,
        uint32_t CullingListIndex,
        uint32_t CullingListCountIndex,
        const char* PassName) const;
    void AddEarlyRejectListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex) const;
    void AddLateListMergePass(FDeferredPassContext& Context) const;
    void AddHZBPass(FDeferredPassContext& Context) const;
};
