#include "DeferredPostProcessPasses.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"

bool FDeferredPostProcessPasses::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat) const
{
    (void)Owner;
    (void)Device;
    (void)BackBufferFormat;
    return true;
}

bool FDeferredPostProcessPasses::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount) const
{
    (void)Owner;
    (void)Device;
    (void)Width;
    (void)Height;
    (void)FrameCount;
    return true;
}

void FDeferredPostProcessPasses::AddDebugPrintPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FDebugPrintPassData
    {
        bool bEnabled = false;
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
    };

    Context.Graph.AddPass<FDebugPrintPassData>("GpuDebugPrint", [&Owner, &Context](FDebugPrintPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.bEnableGpuDebugPrint && Owner.GpuDebugPrintPipeline && Owner.GpuDebugPrintRootSignature
            && Owner.GpuDebugLinePipeline && Owner.GpuDebugLineRootSignature
            && Owner.Device && Owner.Device->GetBindlessDescriptorHeap();
        Data.OutputHandle = Context.RtvHandle;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [&Owner](const FDebugPrintPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        Owner.DispatchGpuDebugPrintStats(Cmd);
        Owner.RenderGpuDebugPrint(Cmd, Data.OutputHandle);
        Owner.RenderGpuDebugLine(Cmd, Data.OutputHandle);
    });
}
