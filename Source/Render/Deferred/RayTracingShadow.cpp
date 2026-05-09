#include "RayTracingShadow.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include <cassert>

void FRayTracingShadow::AddPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FDeferredRenderer* OwnerPtr = &Context.Owner;
    FRenderGraph* GraphPtr = &Context.Graph;
    const FCamera& Camera = Context.Camera;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GBufferHandle = Context.Resources.GBufferHandles[0];
    FRGResourceHandle& ShadowMaskHandle = Context.Resources.RayTracingShadow.ShadowMaskHandle;

    struct FRayTracingShadowPassData
    {
        FRGResourceHandle ShadowMaskHandle{};
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle GBufferHandle{};
        const FCamera* Camera = nullptr;
    };

    const FRGTextureDesc ShadowMaskDesc =
    {
        static_cast<uint32>(Owner.Viewport.Width),
        static_cast<uint32>(Owner.Viewport.Height),
        DXGI_FORMAT_R8_UNORM
    };

    Graph.AddPass<FRayTracingShadowPassData>("RTShadowMask", [&, ShadowMaskDesc, DepthHandle, GBufferHandle](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        if (!Owner.bRayTracedShadowsEnabled || !Owner.GetRayTracingRuntime().bRayTracingPipelineReady || !GBufferHandle)
        {
            return;
        }

        Data.ShadowMaskHandle = Builder.CreateTexture("ShadowMask", ShadowMaskDesc);
        Data.DepthHandle = DepthHandle;
        Data.GBufferHandle = GBufferHandle;
        Data.Camera = &Camera;
        Builder.WriteTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadTexture(Data.DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.KeepAlive();
        ShadowMaskHandle = Data.ShadowMaskHandle;
    }, [OwnerPtr, GraphPtr](const FRayTracingShadowPassData& Data, FDX12CommandContext& CmdContext)
    {
        FDeferredRenderer& Owner = *OwnerPtr;
        FRenderGraph& Graph = *GraphPtr;
        if (!Owner.GetRayTracingRuntime().bRayTracingPipelineReady || !Owner.GetRayTracingRuntime().RayQueryShadowPipeline || !Owner.GetRayTracingRuntime().RayQueryRootSignature)
        {
            return;
        }

        if (Owner.SceneModels.empty() || Data.Camera == nullptr)
        {
            return;
        }

        ID3D12Resource* ShadowMask = Graph.GetTextureResource(Data.ShadowMaskHandle);
        if (!ShadowMask)
        {
            return;
        }

        ID3D12Resource* DepthBuffer = Graph.GetTextureResource(Data.DepthHandle);
        if (!DepthBuffer)
        {
            return;
        }

        ID3D12Resource* GBufferA = Graph.GetTextureResource(Data.GBufferHandle);
        if (!GBufferA)
        {
            return;
        }

        const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
        if (FrameIndex >= Owner.GetRayTracingRuntime().TlasResultBuffers.size() || !Owner.GetRayTracingRuntime().TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }


        FRayTracingRuntime& RayTracing = Owner.GetRayTracingRuntime();
        const uint32_t DepthBindlessIndex = RayTracing.UpdateDepthSrv(Owner, FrameIndex, DepthBuffer);
        const uint32_t GBufferABindlessIndex = RayTracing.UpdateGBufferSrv(Owner, FRayTracingRuntime::EGBufferSlot::A, GBufferA);
        const uint32_t ShadowMaskUavBindlessIndex = RayTracing.UpdateShadowMaskUav(Owner, ShadowMask);
        const uint32_t ShadowMaskSrvBindlessIndex = RayTracing.UpdateShadowMaskSrv(Owner, ShadowMask);

        if (DepthBindlessIndex == UINT32_MAX || GBufferABindlessIndex == UINT32_MAX || ShadowMaskUavBindlessIndex == UINT32_MAX || ShadowMaskSrvBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t DispatchWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t DispatchHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        if (DispatchWidth == 0 || DispatchHeight == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 8;
        const uint32_t GroupCountX = (DispatchWidth + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;
        const uint32_t GroupCountY = (DispatchHeight + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        CommandList4->SetPipelineState(Owner.GetRayTracingRuntime().RayQueryShadowPipeline.Get());
        CommandList4->SetComputeRootSignature(Owner.GetRayTracingRuntime().RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, Owner.GetRayTracingRuntime().TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        const uint32_t BindlessIndices[] =
        {
            DepthBindlessIndex,
            GBufferABindlessIndex,
            ShadowMaskUavBindlessIndex,
            0u,
            DispatchWidth,
            DispatchHeight
        };
        static_assert(_countof(BindlessIndices) <= FRayTracingRuntime::RayQueryRootConstantDwordCount, "Ray query root constants exceed root signature capacity.");
        assert(_countof(BindlessIndices) <= FRayTracingRuntime::RayQueryRootConstantDwordCount);
        CommandList4->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        CommandList4->Dispatch(GroupCountX, GroupCountY, 1);
    });

    Graph.AddPass<FRayTracingShadowPassData>("ShadowMaskSRV", [&, ShadowMaskDesc, ShadowMaskHandle](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.ShadowMaskHandle = ShadowMaskHandle;
        if (Data.ShadowMaskHandle)
        {
            Builder.ReadTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Builder.KeepAlive();
        }
    }, [](const FRayTracingShadowPassData&, FDX12CommandContext&)
    {
    });
}
