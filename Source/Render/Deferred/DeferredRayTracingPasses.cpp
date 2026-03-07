#include "DeferredRayTracingPasses.h"
#include "../DeferredRenderer.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"

bool FDeferredRayTracingPasses::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device) const
{
    if (!Device->IsRayTracingSupported())
    {
        if (Owner.bRestirGIEnabled)
        {
            LogWarning("Deferred renderer: ReSTIR GI disabled because DXR is not supported on this device.");
        }
        Owner.bRestirGIEnabled = false;
    }
    else
    {
        if (!Owner.CreateRestirGIRootSignature(Device) || !Owner.CreateRestirGIPipeline(Device))
        {
            LogWarning("Deferred renderer: ReSTIR GI pipeline creation failed.");
            Owner.RestirGIRootSignature.Reset();
            for (Microsoft::WRL::ComPtr<ID3D12PipelineState>& Pipeline : Owner.RestirGIInitialPipelines)
            {
                Pipeline.Reset();
            }
            Owner.RestirGIReservoirBootstrapPipeline.Reset();
            Owner.RestirGITemporalPipeline.Reset();
            Owner.RestirGISpatialPipeline.Reset();
            Owner.RestirGIResolvePipeline.Reset();
        }

        if (!Owner.CreateRestirGiDenoiserPipelines(Device))
        {
            LogWarning("Deferred renderer: ReSTIR GI denoiser pipeline creation failed (passes will be skipped).");
        }
    }

    return Owner.CreatePathTracingAccumulationRootSignature(Device) && Owner.CreatePathTracingAccumulationPipeline(Device);
}

bool FDeferredRayTracingPasses::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount) const
{
    return Owner.CreateRestirGIResources(Device, Width, Height)
        && Owner.CreateRestirGiDenoiserResources(Device, Width, Height)
        && Owner.CreatePathTracingAccumulationResources(Device, Width, Height, FrameCount);
}
