#include "DeferredRayTracingPasses.h"
#include "../DeferredRenderer.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"

bool FDeferredRayTracingPasses::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device) const
{
    if (!Owner.CreateRestirGiDenoiserPipelines(Device))
    {
        LogWarning("Deferred renderer: ReSTIR GI denoiser pipeline creation failed (passes will be skipped).");
    }

    return Owner.CreatePathTracingAccumulationRootSignature(Device) && Owner.CreatePathTracingAccumulationPipeline(Device);
}

bool FDeferredRayTracingPasses::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount) const
{
    return Owner.CreateRestirGiDenoiserResources(Device, Width, Height)
        && Owner.CreatePathTracingAccumulationResources(Device, Width, Height, FrameCount);
}
