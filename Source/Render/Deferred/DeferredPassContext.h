#pragma once

#include <cstdint>
#include <d3d12.h>

#include "../DeferredRenderer.h"

struct FDeferredPassContext
{
    // Contract:
    // - Modules may register passes and perform local calculations only.
    // - Global renderer state mutation remains in FDeferredRenderer::PrepareFrameState/FinalizeFrameState.
    FDeferredRenderer& Owner;
    FRenderGraph& Graph;
    FDeferredRenderer::FDeferredFrameState& FrameState;
    FDeferredRenderer::FDeferredFrameResources& Resources;
    const FCamera& Camera;
    uint32_t FrameIndex = 0;
    float DeltaTime = 0.0f;
    bool bUsePathTracing = false;
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle{};
};
