#include "DeferredLightingPasses.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include <algorithm>
#include <d3dx12.h>
namespace
{
    constexpr uint32_t SsrRayItemStride = 48u;
}

void FDeferredLightingPasses::AddLinearDepthPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;

    struct FLinearDepthPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FLinearDepthPassData>("LinearDepth", [&Owner, DepthHandle, LinearDepthHandle](FLinearDepthPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.LinearDepthPipeline && Owner.LinearDepthRootSignature;
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner](const FLinearDepthPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent LinearDepthEvent(LocalCommandList, L"LinearDepth");

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.LinearDepthRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(Owner.LinearDepthRtvHandle, ClearColor, 0, nullptr);

        LocalCommandList->SetPipelineState(Owner.LinearDepthPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.LinearDepthRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const uint32_t DepthIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[DepthIndex];
        LocalCommandList->SetGraphicsRoot32BitConstant(1, DepthBindlessIndex, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredLightingPasses::AddGtaoPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle GtaoHandle = Context.Resources.GtaoHandle;

    struct FGtaoPassData
    {
        bool bEnabled = false;
        uint32_t PipelineIndex = 0;
    };

    Graph.AddPass<FGtaoPassData>("GTAO", [&Owner, GBufferHandles, LinearDepthHandle, GtaoHandle](FGtaoPassData& Data, FRGPassBuilder& Builder)
    {
        Data.PipelineIndex = Owner.bGtaoJitterEnabled ? 1u : 0u;
        Data.bEnabled = Owner.bGtaoEnabled && Owner.GtaoRootSignature && Owner.GtaoPipelines[Data.PipelineIndex];
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(GtaoHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner](const FGtaoPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent GtaoEvent(LocalCommandList, L"GTAO");

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.GtaoRtvHandle, nullptr);

        const float ClearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        LocalCommandList->ClearRenderTargetView(Owner.GtaoRtvHandle, ClearColor, 0, nullptr);

        LocalCommandList->SetPipelineState(Owner.GtaoPipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.GtaoRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const uint32_t GtaoBindlessIndices[] =
        {
            Owner.GBufferBindlessIndices[0],
            Owner.LinearDepthBindlessIndex,
            Owner.HilbertLutBindlessIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(GtaoBindlessIndices), GtaoBindlessIndices, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredLightingPasses::AddSsrRayCounterClearPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;

    struct FSsrRayCounterClearPassData
    {
    };

    Graph.AddPass<FSsrRayCounterClearPassData>("SSR RayCounter Clear", [&Owner, FrameIndex, &Graph](FSsrRayCounterClearPassData& Data, FRGPassBuilder& Builder)
    {
        if (FrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || FrameIndex >= Owner.SsrRayCounterHwMissBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle PrimaryHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            Owner.SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle HwMissHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            Owner.SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);

        Builder.WriteBuffer(PrimaryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FSsrRayCounterClearPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrClearEvent(LocalCommandList, L"SSR RayCounter Clear");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= Owner.SsrRayCounterHwMissBuffers.size())
        {
            return;
        }

        ID3D12Resource* PrimaryCounterBuffer = Owner.SsrRayCounterPrimaryBuffers[LocalFrameIndex].Get();
        ID3D12Resource* HwMissCounterBuffer = Owner.SsrRayCounterHwMissBuffers[LocalFrameIndex].Get();
        const uint32_t PrimaryCounterUavIndex = Owner.SsrRayCounterPrimaryUavBindlessIndices[LocalFrameIndex];
        const uint32_t HwMissCounterUavIndex = Owner.SsrRayCounterHwMissUavBindlessIndices[LocalFrameIndex];

        if (!PrimaryCounterBuffer || !HwMissCounterBuffer || PrimaryCounterUavIndex == UINT32_MAX || HwMissCounterUavIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE PrimaryGpuHandle = Owner.GetBindlessGpuHandle(PrimaryCounterUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE PrimaryCpuHandle = Owner.GetBindlessCpuClearHandle(PrimaryCounterUavIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(PrimaryGpuHandle, PrimaryCpuHandle, PrimaryCounterBuffer, ClearValues, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HwMissGpuHandle = Owner.GetBindlessGpuHandle(HwMissCounterUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HwMissCpuHandle = Owner.GetBindlessCpuClearHandle(HwMissCounterUavIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HwMissGpuHandle, HwMissCpuHandle, HwMissCounterBuffer, ClearValues, 0, nullptr);
    });
}

void FDeferredLightingPasses::AddSsrRayGatherPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;

    struct FSsrRayGatherPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrRayGatherPassData>("SSR Ray Gather", [&Owner, FrameIndex, GBufferHandles, LinearDepthHandle, &Graph](FSsrRayGatherPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.SsrRayGatherPipeline && Owner.SsrRayGatherRootSignature;
        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || FrameIndex >= Owner.SsrRayListPrimaryBuffers.size())
        {
            return;
        }

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(Owner.SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            Owner.SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListPrimary",
            Owner.SsrRayListPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayListPrimaryStates[FrameIndex],
            RayListDesc);

        Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FSsrRayGatherPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= Owner.SsrRayListPrimaryBuffers.size())
        {
            return;
        }

        const uint32_t RayCounterUavIndex = Owner.SsrRayCounterPrimaryUavBindlessIndices[LocalFrameIndex];
        const uint32_t RayListUavIndex = Owner.SsrRayListPrimaryUavBindlessIndices[LocalFrameIndex];
        if (RayCounterUavIndex == UINT32_MAX || RayListUavIndex == UINT32_MAX)
        {
            return;
        }

        if (Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrGatherEvent(LocalCommandList, L"SSR Ray Gather");

        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetPipelineState(Owner.SsrRayGatherPipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.SsrRayGatherRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        struct FSsrRayGatherConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            uint32_t FrameIndex = 0;
            uint32_t SamplesPerQuad = 0;
            uint32_t MaxRayCount = 0;
            float MaxDistance = 0.0f;
            float RoughnessCutoff = 0.0f;
            float NormalBiasScale = 0.0f;
            float TMinBias = 0.0f;
            uint32_t PatternRotate = 0;
            uint32_t Padding = 0;
        };

        const FSsrRayGatherConstants Constants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            LocalFrameIndex,
            Owner.SsrSamplesPerQuad,
            Owner.SsrMaxRayCount,
            Owner.SsrMaxDistance,
            Owner.SsrRoughnessCutoff,
            0.001f,
            0.01f,
            LocalFrameIndex & 3u,
            0u
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, sizeof(FSsrRayGatherConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t BindlessIndices[] =
        {
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.LinearDepthBindlessIndex,
            RayCounterUavIndex,
            RayListUavIndex,
            Owner.Device->GetPointClampSamplerIndex()
        };
        LocalCommandList->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        const uint32_t DispatchX = (Constants.OutputWidth + 7u) / 8u;
        const uint32_t DispatchY = (Constants.OutputHeight + 7u) / 8u;
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FDeferredLightingPasses::AddSsrBuildIndirectArgsPass(FDeferredPassContext& Context, bool bHwMiss) const
{
    Context.Owner.AddSsrBuildIndirectArgsPass(Context.Graph, Context.FrameIndex, bHwMiss);
}

void FDeferredLightingPasses::AddSsrSwTracePass(FDeferredPassContext& Context) const
{
    Context.Owner.AddSsrSwTracePass(Context.Graph, Context.FrameIndex, Context.FrameState, Context.Resources.TaaHandles, Context.Resources.LinearDepthHandle, Context.Resources.HZBHandle, Context.Resources.SsrHandle);
}

void FDeferredLightingPasses::AddSsrHwTracePass(FDeferredPassContext& Context) const
{
    Context.Owner.AddSsrHwTracePass(Context.Graph, Context.FrameIndex, Context.FrameState, Context.Camera, Context.Resources.TaaHandles, Context.Resources.SsrHandle);
}

void FDeferredLightingPasses::AddSsrResolvePass(FDeferredPassContext& Context) const
{
    Context.Owner.AddSsrResolvePass(Context.Graph, Context.Resources.GBufferHandles, Context.Resources.LinearDepthHandle, Context.Resources.SsrHandle, Context.Resources.SsrResolveHandle);
}

void FDeferredLightingPasses::AddSsrPass(FDeferredPassContext& Context) const
{
    Context.Owner.AddSsrPass(Context.Graph, Context.FrameIndex, Context.FrameState, Context.Resources.GBufferHandles, Context.Resources.LinearDepthHandle, Context.Resources.TaaHandles, Context.Resources.HZBHandle, Context.Resources.SsrHandle);
}

void FDeferredLightingPasses::AddSsrFallbackPass(FDeferredPassContext& Context) const
{
    Context.Owner.AddSsrFallbackPass(Context.Graph, Context.FrameIndex, Context.FrameState, Context.Camera, Context.Resources.TaaHandles, Context.Resources.SsrFallbackHandle);
}

void FDeferredLightingPasses::AddSsrDenoisePass(FDeferredPassContext& Context, FRGResourceHandle InputHandle) const
{
    Context.Owner.AddSsrDenoisePass(Context.Graph, InputHandle, Context.Resources.GBufferHandles, Context.Resources.LinearDepthHandle, Context.Resources.SsrDenoiseHandle);
}

void FDeferredLightingPasses::AddLightingPass(FDeferredPassContext& Context, FRGResourceHandle SsrHandle) const
{
    Context.Owner.AddLightingPass(
        Context.Graph,
        Context.FrameState,
        Context.Resources.GBufferHandles,
        Context.Resources.DepthHandle,
        Context.Resources.GtaoHandle,
        Context.Resources.RestirGiHistoryIrradianceHandle,
        SsrHandle,
        Context.Resources.SsrFallbackHandle,
        Context.Resources.ShadowHandle,
        Context.Resources.LightingHandle);
}

void FDeferredLightingPasses::AddSkyPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FCamera& Camera = Context.Camera;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;

    struct FSkyPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSkyPassData>("Sky", [&Owner, &Camera, DepthHandle, LightingHandle](FSkyPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.SkyPipelineState && Owner.SkyRootSignature && Owner.SkyGeometry.IndexCount > 0;
        Data.Camera = &Camera;

        if (Data.bEnabled)
        {
            Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
            Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [&Owner](const FSkyPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent SkyEvent(LocalCommandList, L"SkyAtmosphere");
        LocalCommandList->SetPipelineState(Owner.SkyPipelineState.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.SkyRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->IASetVertexBuffers(0, Owner.SkyGeometry.VertexBufferCount, Owner.SkyGeometry.VertexBufferViews.data());
        LocalCommandList->IASetIndexBuffer(&Owner.SkyGeometry.IndexBufferView);
        const D3D12_CPU_DESCRIPTOR_HANDLE& LocalDepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(1, &Owner.LightingRTVHandle, FALSE, &LocalDepthHandle);

        Owner.UpdateSkyConstants(*Data.Camera);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.SkyConstantBuffer->GetGPUVirtualAddress());
        LocalCommandList->DrawIndexedInstanced(Owner.SkyGeometry.IndexCount, 1, 0, 0, 0);
    });
}


void FDeferredRenderer::AddSsrBuildIndirectArgsPass(FRenderGraph& Graph, uint32_t FrameIndex, bool bHwMiss)
{
    struct FSsrBuildIndirectArgsPassData
    {
        bool bEnabled = false;
        bool bHwMiss = false;
    };

    const wchar_t* PassLabel = bHwMiss ? L"SSR Build IndirectArgs HW Miss" : L"SSR Build IndirectArgs Primary";
    const char* PassName = bHwMiss ? "SSR Build IndirectArgs HW Miss" : "SSR Build IndirectArgs Primary";
    Graph.AddPass<FSsrBuildIndirectArgsPassData>(PassName, [this, FrameIndex, bHwMiss, &Graph](FSsrBuildIndirectArgsPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = SsrBuildIndirectArgsPipeline && SsrBuildIndirectArgsRootSignature;
        Data.bHwMiss = bHwMiss;
        if (!Data.bEnabled)
        {
            return;
        }

        const bool bValidFrame = bHwMiss
            ? (FrameIndex < SsrRayCounterHwMissBuffers.size() && FrameIndex < SsrIndirectArgsHwMissBuffers.size())
            : (FrameIndex < SsrRayCounterPrimaryBuffers.size() && FrameIndex < SsrIndirectArgsPrimaryBuffers.size());
        if (!bValidFrame)
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle CounterHandle = Graph.ImportBuffer(
            bHwMiss ? "SSR_RayCounterHwMiss" : "SSR_RayCounterPrimary",
            bHwMiss ? SsrRayCounterHwMissBuffers[FrameIndex].Get() : SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            bHwMiss ? &SsrRayCounterHwMissStates[FrameIndex] : &SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            bHwMiss ? "SSR_IndirectArgsHwMiss" : "SSR_IndirectArgsPrimary",
            bHwMiss ? SsrIndirectArgsHwMissBuffers[FrameIndex].Get() : SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(),
            bHwMiss ? &SsrIndirectArgsHwMissStates[FrameIndex] : &SsrIndirectArgsPrimaryStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(CounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(IndirectHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, PassLabel](const FSsrBuildIndirectArgsPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent BuildEvent(LocalCommandList, PassLabel);

        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        if (Data.bHwMiss)
        {
            if (FrameIndex >= SsrRayCounterHwMissBuffers.size() || FrameIndex >= SsrIndirectArgsHwMissBuffers.size())
            {
                return;
            }
        }
        else
        {
            if (FrameIndex >= SsrRayCounterPrimaryBuffers.size() || FrameIndex >= SsrIndirectArgsPrimaryBuffers.size())
            {
                return;
            }
        }

        ID3D12Resource* RayCounterBuffer = Data.bHwMiss ? SsrRayCounterHwMissBuffers[FrameIndex].Get() : SsrRayCounterPrimaryBuffers[FrameIndex].Get();
        ID3D12Resource* IndirectArgsBuffer = Data.bHwMiss ? SsrIndirectArgsHwMissBuffers[FrameIndex].Get() : SsrIndirectArgsPrimaryBuffers[FrameIndex].Get();
        const uint32_t RayCounterSrvIndex = Data.bHwMiss ? SsrRayCounterHwMissSrvBindlessIndices[FrameIndex] : SsrRayCounterPrimarySrvBindlessIndices[FrameIndex];
        const uint32_t IndirectArgsUavIndex = Data.bHwMiss ? SsrIndirectArgsHwMissUavBindlessIndices[FrameIndex] : SsrIndirectArgsPrimaryUavBindlessIndices[FrameIndex];

        if (!RayCounterBuffer || !IndirectArgsBuffer || RayCounterSrvIndex == UINT32_MAX || IndirectArgsUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->SetPipelineState(SsrBuildIndirectArgsPipeline.Get());
        LocalCommandList->SetComputeRootSignature(SsrBuildIndirectArgsRootSignature.Get());

        constexpr uint32_t ThreadGroupSizeX = 64u;
        const uint32_t Constants[] = { ThreadGroupSizeX, SsrMaxRayCount };
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

        const uint32_t BindlessIndices[] = { RayCounterSrvIndex, IndirectArgsUavIndex };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

        LocalCommandList->Dispatch(1, 1, 1);
    });
}

void FDeferredRenderer::AddSsrSwTracePass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle HZBHandle, FRGResourceHandle SsrHandle)
{
    struct FSsrSwTracePassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        bool bUseHzb = false;
        uint32_t HistoryIndex = 0;
        uint32_t PipelineIndex = 0;
        FRGResourceHandle SceneColorHandle{};
        FRGResourceHandle SsrHandle{};
        FRGResourceHandle HzbHandle{};
    };

    Graph.AddPass<FSsrSwTracePassData>("SSR SW Trace", [this, FrameIndex, FrameState, TaaHandles, LinearDepthHandle, HZBHandle, SsrHandle, &Graph](FSsrSwTracePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseHzb = bSsrHzbEnabled && bHZBReady && HZBSrvBindlessIndex != UINT32_MAX;
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = (Data.bUseHzb ? 2u : 0u) + (bSsrRefineEnabled ? 1u : 0u) + (bSsrSwEnabled ? 0u : 4u);
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && SsrSwTraceRootSignature;

        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= SsrRayCounterPrimaryBuffers.size() || FrameIndex >= SsrRayListPrimaryBuffers.size()
            || FrameIndex >= SsrRayCounterHwMissBuffers.size() || FrameIndex >= SsrRayListHwMissBuffers.size()
            || FrameIndex >= SsrIndirectArgsPrimaryBuffers.size())
        {
            return;
        }

        if (Data.bUseHistory)
        {
            Data.SceneColorHandle = TaaHandles[Data.HistoryIndex];
            Builder.ReadTexture(Data.SceneColorHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Data.bUseHzb)
        {
            Data.HzbHandle = HZBHandle;
            Builder.ReadTexture(Data.HzbHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Data.SsrHandle = SsrHandle;
        Builder.WriteTexture(Data.SsrHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListPrimary",
            SsrRayListPrimaryBuffers[FrameIndex].Get(),
            &SsrRayListPrimaryStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle HwMissCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle HwMissListHandle = Graph.ImportBuffer(
            "SSR_RayListHwMiss",
            SsrRayListHwMissBuffers[FrameIndex].Get(),
            &SsrRayListHwMissStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            "SSR_IndirectArgsPrimary",
            SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(),
            &SsrIndirectArgsPrimaryStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(HwMissCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }, [this, &Graph](const FSsrSwTracePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (GBufferBindlessIndices[2] == UINT32_MAX || LinearDepthBindlessIndex == UINT32_MAX || SsrUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        if (FrameIndex >= SsrRayCounterPrimaryBuffers.size() || FrameIndex >= SsrRayListPrimaryBuffers.size()
            || FrameIndex >= SsrRayCounterHwMissBuffers.size() || FrameIndex >= SsrRayListHwMissBuffers.size()
            || FrameIndex >= SsrIndirectArgsPrimaryBuffers.size())
        {
            return;
        }

        const uint32_t RayCounterPrimarySrvIndex = SsrRayCounterPrimarySrvBindlessIndices[FrameIndex];
        const uint32_t RayListPrimarySrvIndex = SsrRayListPrimarySrvBindlessIndices[FrameIndex];
        const uint32_t RayCounterHwMissUavIndex = SsrRayCounterHwMissUavBindlessIndices[FrameIndex];
        const uint32_t RayListHwMissUavIndex = SsrRayListHwMissUavBindlessIndices[FrameIndex];
        if (RayCounterPrimarySrvIndex == UINT32_MAX || RayListPrimarySrvIndex == UINT32_MAX
            || RayCounterHwMissUavIndex == UINT32_MAX || RayListHwMissUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12Resource* IndirectArgsBuffer = SsrIndirectArgsPrimaryBuffers[FrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        if (!SsrDispatchCommandSignature)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER UavBarrier = {};
        UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        UavBarrier.UAV.pResource = IndirectArgsBuffer;
        Cmd.GetCommandList()->ResourceBarrier(1, &UavBarrier);

        ID3D12Resource* SsrOutput = Graph.GetTextureResource(Data.SsrHandle);
        if (!SsrOutput)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrSwTraceEvent(LocalCommandList, L"SSR SW Trace");
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const D3D12_GPU_DESCRIPTOR_HANDLE OutputGpuHandle = GetBindlessGpuHandle(SsrUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE OutputCpuHandle = GetBindlessCpuClearHandle(SsrUavBindlessIndex);
        const float ClearValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearUnorderedAccessViewFloat(OutputGpuHandle, OutputCpuHandle, SsrOutput, ClearValues, 0, nullptr);

        if (!EnsureSsrSwTracePipelineOrFail(Data.PipelineIndex, "SSR SW Trace"))
        {
            return;
        }

        LocalCommandList->SetPipelineState(SsrSwTracePipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetComputeRootSignature(SsrSwTraceRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(0, GetSceneConstantBufferAddress());

        struct FSsrSwTraceConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            uint32_t MaxSteps = 0;
            float MaxDistance = 0.0f;
            float Thickness = 0.0f;
            float Stride = 0.0f;
            float RoughnessCutoff = 0.0f;
            float Intensity = 0.0f;
            uint32_t HZBWidth = 0;
            uint32_t HZBHeight = 0;
            uint32_t HZBMipCount = 0;
            uint32_t HZBAvailable = 0;
            uint32_t MaxRayCount = 0;
        };

        const FSsrSwTraceConstants Constants =
        {
            static_cast<uint32_t>(Viewport.Width),
            static_cast<uint32_t>(Viewport.Height),
            SsrMaxSteps,
            SsrMaxDistance,
            SsrThickness,
            SsrStride,
            SsrRoughnessCutoff,
            SsrIntensity,
            HZBWidth,
            HZBHeight,
            HZBMipCount,
            Data.bUseHzb ? 1u : 0u,
            SsrMaxRayCount
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, sizeof(FSsrSwTraceConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t SceneColorIndex = Data.bUseHistory && Data.HistoryIndex < TaaSrvBindlessIndices.size()
            ? TaaSrvBindlessIndices[Data.HistoryIndex]
            : GBufferBindlessIndices[2];
        if (SceneColorIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t HzbIndex = Data.bUseHzb && HZBSrvBindlessIndex != UINT32_MAX ? HZBSrvBindlessIndex : LinearDepthBindlessIndex;
        const uint32_t BindlessIndices[] =
        {
            LinearDepthBindlessIndex,
            SceneColorIndex,
            RayCounterPrimarySrvIndex,
            RayListPrimarySrvIndex,
            RayCounterHwMissUavIndex,
            RayListHwMissUavIndex,
            SsrUavBindlessIndex,
            HzbIndex,
            Device->GetPointClampSamplerIndex(),
            Device->GetLinearClampSamplerIndex()
        };
        LocalCommandList->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        LocalCommandList->ExecuteIndirect(SsrDispatchCommandSignature.Get(), 1, IndirectArgsBuffer, 0, nullptr, 0);
    });
}

void FDeferredRenderer::AddSsrHwTracePass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const FCamera& Camera, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle SsrHandle)
{
    struct FSsrHwTracePassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        uint32_t HistoryIndex = 0;
        FRGResourceHandle SceneColorHandle{};
        FRGResourceHandle SsrHandle{};
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSsrHwTracePassData>("SSR HW Trace", [this, FrameIndex, FrameState, &Camera, TaaHandles, SsrHandle, &Graph](FSsrHwTracePassData& Data, FRGPassBuilder& Builder)
    {
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bEnabled = bSsrHwEnabled && bRayTracingPipelineReady && RayQueryRootSignature && RayQuerySsrHwPipeline;
        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= SsrRayCounterHwMissBuffers.size() || FrameIndex >= SsrRayListHwMissBuffers.size()
            || FrameIndex >= SsrIndirectArgsHwMissBuffers.size())
        {
            return;
        }

        Data.SsrHandle = SsrHandle;
        Data.Camera = &Camera;
        if (Data.bUseHistory)
        {
            Data.SceneColorHandle = TaaHandles[Data.HistoryIndex];
            Builder.ReadTexture(Data.SceneColorHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteTexture(Data.SsrHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListHwMiss",
            SsrRayListHwMissBuffers[FrameIndex].Get(),
            &SsrRayListHwMissStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            "SSR_IndirectArgsHwMiss",
            SsrIndirectArgsHwMissBuffers[FrameIndex].Get(),
            &SsrIndirectArgsHwMissStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }, [this, &Graph](const FSsrHwTracePassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FScopedPixEvent SsrHWTraceEvent(CommandList4, L"SSR HW Trace");

        if (Data.Camera == nullptr)
        {
            return;
        }

        if (GBufferBindlessIndices[2] == UINT32_MAX || SsrUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
        if (FrameIndex >= TlasResultBuffers.size() || !TlasResultBuffers[FrameIndex])
        {
            return;
        }

        const uint32_t RayCounterHwMissIndex = SsrRayCounterHwMissSrvBindlessIndices[FrameIndex];
        const uint32_t RayListHwMissIndex = SsrRayListHwMissSrvBindlessIndices[FrameIndex];
        if (RayCounterHwMissIndex == UINT32_MAX || RayListHwMissIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12Resource* IndirectArgsBuffer = SsrIndirectArgsHwMissBuffers[FrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        if (!SsrDispatchCommandSignature)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER UavBarrier = {};
        UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        UavBarrier.UAV.pResource = IndirectArgsBuffer;
        CommandList4->ResourceBarrier(1, &UavBarrier);

        ID3D12Resource* SsrOutput = Graph.GetTextureResource(Data.SsrHandle);
        if (!SsrOutput)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t SceneColorIndex = Data.bUseHistory && Data.HistoryIndex < TaaSrvBindlessIndices.size()
            ? TaaSrvBindlessIndices[Data.HistoryIndex]
            : GBufferBindlessIndices[2];
        if (SceneColorIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t OutputWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t OutputHeight = static_cast<uint32_t>(Viewport.Height);
        if (OutputWidth == 0 || OutputHeight == 0 || SsrMaxRayCount == 0)
        {
            return;
        }

        CommandList4->SetPipelineState(RayQuerySsrHwPipeline.Get());
        CommandList4->SetComputeRootSignature(RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        UpdateSceneConstants(*Data.Camera, SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (FrameIndex >= PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = PathTracingInstanceDataBindlessIndices[FrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        std::array<uint32_t, 13> BindlessIndices =
        {
            RayListHwMissIndex,
            RayCounterHwMissIndex,
            SsrUavBindlessIndex,
            SceneColorIndex,
            PathTracingInstanceDataBindlessIndex,
            EnvironmentCubeBindlessIndex,
            Device->GetLinearClampSamplerIndex(),
            SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            0u,
            0u,
            0u
        };
        static_assert(sizeof(float) == sizeof(uint32_t), "Float size mismatch.");
        std::memcpy(&BindlessIndices[10], &SsrIntensity, sizeof(float));
        std::memcpy(&BindlessIndices[11], &SsrRoughnessCutoff, sizeof(float));
        CommandList4->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);

        CommandList4->ExecuteIndirect(SsrDispatchCommandSignature.Get(), 1, IndirectArgsBuffer, 0, nullptr, 0);
    });
}

void FDeferredRenderer::AddSsrResolvePass(FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle SsrInputHandle, FRGResourceHandle SsrResolveHandle)
{
    struct FSsrResolvePassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrResolvePassData>("SSR Resolve", [this, GBufferHandles, LinearDepthHandle, SsrInputHandle, SsrResolveHandle](FSsrResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = SsrResolvePipeline && SsrResolveRootSignature;
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(SsrInputHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SsrResolveHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this](const FSsrResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (SsrBindlessIndex == UINT32_MAX || SsrResolveUavBindlessIndex == UINT32_MAX || LinearDepthBindlessIndex == UINT32_MAX
            || GBufferBindlessIndices[0] == UINT32_MAX || GBufferBindlessIndices[1] == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrResolveEvent(LocalCommandList, L"SSR Resolve");
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetPipelineState(SsrResolvePipeline.Get());
        LocalCommandList->SetComputeRootSignature(SsrResolveRootSignature.Get());

        struct FSsrResolveConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            float DepthWeight = 1.0f;
            float NormalWeight = 1.0f;
            float RoughnessWeight = 1.0f;
        };

        const FSsrResolveConstants Constants =
        {
            static_cast<uint32_t>(Viewport.Width),
            static_cast<uint32_t>(Viewport.Height),
            1.0f,
            1.0f,
            0.5f
        };
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FSsrResolveConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t BindlessIndices[] =
        {
            SsrBindlessIndex,
            SsrResolveUavBindlessIndex,
            GBufferBindlessIndices[0],
            GBufferBindlessIndices[1],
            LinearDepthBindlessIndex,
            Device->GetPointClampSamplerIndex()
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

        const uint32_t DispatchX = (Constants.OutputWidth + 7u) / 8u;
        const uint32_t DispatchY = (Constants.OutputHeight + 7u) / 8u;
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FDeferredRenderer::AddSsrPass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle HZBHandle, FRGResourceHandle SsrHandle)
{
    struct FSsrPassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        uint32_t HistoryIndex = 0;
        bool bUseHzb = false;
        uint32_t PipelineIndex = 0;
    };

    Graph.AddPass<FSsrPassData>("SSR", [this, FrameIndex, GBufferHandles, LinearDepthHandle, TaaHandles, HZBHandle, SsrHandle, FrameState, &Graph](FSsrPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseHzb = bSsrHzbEnabled && bHZBReady && HZBSrvBindlessIndex != UINT32_MAX;
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = (Data.bUseHzb ? 2u : 0u) + (bSsrRefineEnabled ? 1u : 0u) + (bSsrSwEnabled ? 0u : 4u);
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && SsrRootSignature;

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (Data.bUseHzb)
        {
            Builder.ReadTexture(HZBHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (Data.bUseHistory)
        {
            Builder.ReadTexture(TaaHandles[Data.HistoryIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteTexture(SsrHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);

        if (FrameIndex >= SsrRayCounterBuffers.size() || FrameIndex >= SsrRayListBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounter",
            SsrRayCounterBuffers[FrameIndex].Get(),
            &SsrRayCounterStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayList",
            SsrRayListBuffers[FrameIndex].Get(),
            &SsrRayListStates[FrameIndex],
            RayListDesc);

        Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this](const FSsrPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrEvent(LocalCommandList, L"SSR");

        if (!Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(SsrRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(SsrRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        if (GBufferBindlessIndices[0] == UINT32_MAX || GBufferBindlessIndices[1] == UINT32_MAX || LinearDepthBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t HistoryIndex = Data.bUseHistory && Data.HistoryIndex < TaaSrvBindlessIndices.size()
            ? TaaSrvBindlessIndices[Data.HistoryIndex]
            : GBufferBindlessIndices[2];
        if (HistoryIndex == UINT32_MAX)
        {
            return;
        }

        if (!EnsureSsrGraphicsPipelineOrFail(Data.PipelineIndex, "SSR"))
        {
            return;
        }

        LocalCommandList->SetPipelineState(SsrPipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(SsrRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, GetSceneConstantBufferAddress());

        struct FSsrConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            uint32_t MaxSteps = 0;
            float Thickness = 0.0f;
            float MaxDistance = 0.0f;
            float Stride = 0.0f;
            float RoughnessCutoff = 0.0f;
            float Intensity = 0.0f;
            uint32_t MaxRayCount = 0;
            uint32_t UseHistory = 0;
            uint32_t HZBWidth = 0;
            uint32_t HZBHeight = 0;
            uint32_t HZBMipCount = 0;
            uint32_t HZBAvailable = 0;
            uint32_t HwEnabled = 0;
        };

        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        uint32_t RayCounterUavIndex = UINT32_MAX;
        uint32_t RayListUavIndex = UINT32_MAX;
        ID3D12Resource* RayCounterBuffer = nullptr;
        if (FrameIndex < SsrRayCounterBuffers.size() && FrameIndex < SsrRayListBuffers.size())
        {
            RayCounterBuffer = SsrRayCounterBuffers[FrameIndex].Get();
            RayCounterUavIndex = SsrRayCounterUavBindlessIndices[FrameIndex];
            RayListUavIndex = SsrRayListUavBindlessIndices[FrameIndex];
        }

        if (RayCounterBuffer && RayCounterUavIndex != UINT32_MAX && RayListUavIndex != UINT32_MAX)
        {
            const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = GetBindlessGpuHandle(RayCounterUavIndex);
            const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = GetBindlessCpuClearHandle(RayCounterUavIndex);
            const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
            LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, RayCounterBuffer, ClearValues, 0, nullptr);
        }

        const FSsrConstants SsrConstants =
        {
            static_cast<uint32_t>(Viewport.Width),
            static_cast<uint32_t>(Viewport.Height),
            SsrMaxSteps,
            SsrThickness,
            SsrMaxDistance,
            SsrStride,
            SsrRoughnessCutoff,
            SsrIntensity,
            (bSsrHwEnabled && RayCounterUavIndex != UINT32_MAX && RayListUavIndex != UINT32_MAX) ? SsrMaxRayCount : 0u,
            Data.bUseHistory ? 1u : 0u,
            HZBWidth,
            HZBHeight,
            HZBMipCount,
            Data.bUseHzb ? 1u : 0u,
            bSsrHwEnabled ? 1u : 0u
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, sizeof(FSsrConstants) / sizeof(uint32_t), &SsrConstants, 0);

        const uint32_t HzbIndex = (HZBSrvBindlessIndex != UINT32_MAX) ? HZBSrvBindlessIndex : LinearDepthBindlessIndex;
        const uint32_t SsrBindlessIndices[] =
        {
            GBufferBindlessIndices[0],
            GBufferBindlessIndices[1],
            LinearDepthBindlessIndex,
            HistoryIndex,
            HzbIndex,
            Device->GetPointClampSamplerIndex(),
            Device->GetLinearClampSamplerIndex(),
            RayCounterUavIndex,
            RayListUavIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(2, _countof(SsrBindlessIndices), SsrBindlessIndices, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredRenderer::AddSsrFallbackPass(FRenderGraph& Graph, uint32_t FrameIndex, const FDeferredFrameState& FrameState, const FCamera& Camera, const std::vector<FRGResourceHandle>& TaaHandles, FRGResourceHandle SsrFallbackHandle)
{
    struct FSsrFallbackPassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        bool bDoRayTracing = false;
        uint32_t HistoryIndex = 0;
        FRGResourceHandle SceneColorHandle{};
        FRGResourceHandle FallbackHandle{};
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSsrFallbackPassData>("SSR Fallback", [this, FrameIndex, FrameState, &Camera, TaaHandles, SsrFallbackHandle, &Graph](FSsrFallbackPassData& Data, FRGPassBuilder& Builder)
    {
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bEnabled = static_cast<bool>(SsrFallbackHandle);
        Data.bDoRayTracing = bSsrHwEnabled && Data.bUseHistory;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.FallbackHandle = SsrFallbackHandle;
        Data.Camera = &Camera;
        if (Data.bDoRayTracing)
        {
            Data.SceneColorHandle = TaaHandles[Data.HistoryIndex];
            Builder.ReadTexture(Data.SceneColorHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteTexture(Data.FallbackHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (!Data.bDoRayTracing)
        {
            return;
        }

        if (FrameIndex >= SsrRayListBuffers.size() || FrameIndex >= SsrRayCounterBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayList",
            SsrRayListBuffers[FrameIndex].Get(),
            &SsrRayListStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounter",
            SsrRayCounterBuffers[FrameIndex].Get(),
            &SsrRayCounterStates[FrameIndex],
            CounterDesc);

        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }, [this, &Graph](const FSsrFallbackPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12Resource* FallbackTexture = Graph.GetTextureResource(Data.FallbackHandle);
        if (!FallbackTexture)
        {
            return;
        }

        const uint32_t FallbackUavIndex = SsrFallbackUavBindlessIndex;
        if (FallbackUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FScopedPixEvent SsrFallbackEvent(CommandList4, L"SSR Fallback");

        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
        const D3D12_GPU_DESCRIPTOR_HANDLE UavGpuHandle = GetBindlessGpuHandle(FallbackUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE UavCpuHandle = GetBindlessCpuClearHandle(FallbackUavIndex);
        const float ClearValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CommandList4->ClearUnorderedAccessViewFloat(UavGpuHandle, UavCpuHandle, FallbackTexture, ClearValues, 0, nullptr);

        if (!Data.bDoRayTracing || !bRayTracingPipelineReady || !RayQuerySsrFallbackPipeline || !RayQueryRootSignature)
        {
            return;
        }

        if (SceneModels.empty() || Data.Camera == nullptr)
        {
            return;
        }

        ID3D12Resource* SceneColor = Graph.GetTextureResource(Data.SceneColorHandle);
        if (!SceneColor)
        {
            return;
        }

        const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
        if (FrameIndex >= TlasResultBuffers.size() || !TlasResultBuffers[FrameIndex])
        {
            return;
        }

        if (FrameIndex >= SsrRayListBuffers.size() || FrameIndex >= SsrRayCounterBuffers.size())
        {
            return;
        }

        const uint32_t RayListIndex = SsrRayListSrvBindlessIndices[FrameIndex];
        const uint32_t RayCounterIndex = SsrRayCounterSrvBindlessIndices[FrameIndex];
        if (RayListIndex == UINT32_MAX || RayCounterIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t SceneColorIndex = Data.HistoryIndex < TaaSrvBindlessIndices.size()
            ? TaaSrvBindlessIndices[Data.HistoryIndex]
            : UINT32_MAX;
        if (SceneColorIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t OutputWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t OutputHeight = static_cast<uint32_t>(Viewport.Height);
        if (OutputWidth == 0 || OutputHeight == 0 || SsrMaxRayCount == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 64;
        const uint32_t DispatchCount = (SsrMaxRayCount + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        CommandList4->SetPipelineState(RayQuerySsrFallbackPipeline.Get());
        CommandList4->SetComputeRootSignature(RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        UpdateSceneConstants(*Data.Camera, SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (FrameIndex >= PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = PathTracingInstanceDataBindlessIndices[FrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        std::array<uint32_t, 13> BindlessIndices =
        {
            RayListIndex,
            RayCounterIndex,
            FallbackUavIndex,
            SceneColorIndex,
            PathTracingInstanceDataBindlessIndex,
            EnvironmentCubeBindlessIndex,
            Device->GetLinearClampSamplerIndex(),
            SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            0u,
            0u,
            0u
        };
        static_assert(sizeof(float) == sizeof(uint32_t), "Float size mismatch.");
        std::memcpy(&BindlessIndices[10], &SsrIntensity, sizeof(float));
        std::memcpy(&BindlessIndices[11], &SsrRoughnessCutoff, sizeof(float));
        CommandList4->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);
        CommandList4->Dispatch(DispatchCount, 1, 1);
    });
}

void FDeferredRenderer::AddSsrDenoisePass(FRenderGraph& Graph, FRGResourceHandle SsrHandle, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle SsrDenoiseHandle)
{
    struct FSsrDenoisePassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrDenoisePassData>("SSR Denoise", [this, SsrHandle, GBufferHandles, LinearDepthHandle, SsrDenoiseHandle](FSsrDenoisePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && bSsrDenoiseEnabled && SsrDenoiseRootSignature && SsrDenoisePipeline;

        Builder.ReadTexture(SsrHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SsrDenoiseHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [this](const FSsrDenoisePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrDenoiseEvent(LocalCommandList, L"SSR Denoise");

        if (!Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(SsrDenoiseRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(SsrDenoiseRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        const uint32_t SsrInputIndex = (SsrMode == ESSRMode::CS) ? SsrResolveBindlessIndex : SsrBindlessIndex;
        if (SsrInputIndex == UINT32_MAX || GBufferBindlessIndices[0] == UINT32_MAX || LinearDepthBindlessIndex == UINT32_MAX)
        {
            return;
        }

        LocalCommandList->SetPipelineState(SsrDenoisePipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(SsrDenoiseRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        struct FSsrDenoiseConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            float DepthSigma = 0.0f;
            float NormalPower = 0.0f;
        };

        const FSsrDenoiseConstants Constants =
        {
            static_cast<uint32_t>(Viewport.Width),
            static_cast<uint32_t>(Viewport.Height),
            0.5f,
            32.0f
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(FSsrDenoiseConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t DenoiseBindlessIndices[] =
        {
            SsrInputIndex,
            GBufferBindlessIndices[0],
            LinearDepthBindlessIndex,
            Device->GetPointClampSamplerIndex(),
            Device->GetLinearClampSamplerIndex()
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(DenoiseBindlessIndices), DenoiseBindlessIndices, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredRenderer::AddLightingPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle GtaoHandle, FRGResourceHandle RestirGIHandle, FRGResourceHandle SsrHandle, FRGResourceHandle SsrFallbackHandle, FRGResourceHandle ShadowHandle, FRGResourceHandle LightingHandle)
{
    struct FLightingPassData
    {
        bool bUseShadows = false;
    };

    Graph.AddPass<FLightingPassData>("Lighting", [&](FLightingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseShadows = FrameState.bRenderShadows;

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[3], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GtaoHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SsrHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SsrFallbackHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (Data.bUseShadows)
        {
            Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [this](const FLightingPassData&, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent LightingEvent(LocalCommandList, L"Lighting");

        if (!Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t DepthIndex = GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthIndex];
        const uint32_t BaseSsrIndex = (SsrMode == ESSRMode::CS) ? SsrResolveBindlessIndex : SsrBindlessIndex;
        const uint32_t SsrLightingBindlessIndex = bSsrDenoiseEnabled ? SsrDenoiseBindlessIndex : BaseSsrIndex;
        const uint32_t SsrFallbackIndex = SsrFallbackBindlessIndex;
        if (DepthBindlessIndex == UINT32_MAX || GtaoBindlessIndex == UINT32_MAX || RestirGIBindlessIndex == UINT32_MAX || SsrLightingBindlessIndex == UINT32_MAX || SsrFallbackIndex == UINT32_MAX || ShadowMapBindlessIndex == UINT32_MAX
            || EnvironmentCubeBindlessIndex == UINT32_MAX || BrdfLutBindlessIndex == UINT32_MAX
            || GBufferBindlessIndices[0] == UINT32_MAX || GBufferBindlessIndices[1] == UINT32_MAX || GBufferBindlessIndices[2] == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(LightingRTVHandle, nullptr);

        const bool bUseShadowMask = bShadowsEnabled && bRayTracedShadowsEnabled && bRayTracingPipelineReady && ShadowMaskBindlessIndex != UINT32_MAX;
        const uint32_t PipelineIndex = (bUseShadowMask ? 1u : 0u) | (bEnablePbrResearch ? 2u : 0u);
        LocalCommandList->SetPipelineState(LightingPipelines[PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(LightingRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, GetSceneConstantBufferAddress());
        const uint32_t ResolvedShadowMaskIndex = bUseShadowMask ? ShadowMaskBindlessIndex : ShadowMapBindlessIndex;
        const uint32_t LightingBindlessIndices[] =
        {
            GBufferBindlessIndices[0],
            GBufferBindlessIndices[1],
            GBufferBindlessIndices[2],
            GBufferBindlessIndices[3],
            ShadowMapBindlessIndex,
            ResolvedShadowMaskIndex,
            EnvironmentCubeBindlessIndex,
            BrdfLutBindlessIndex,
            DepthBindlessIndex,
            GtaoBindlessIndex,
            RestirGIBindlessIndex,
            SsrLightingBindlessIndex,
            SsrFallbackIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(LightingBindlessIndices), LightingBindlessIndices, 0);

        struct FRestirGIConstants
        {
            float Intensity = 0.0f;
            uint32_t Enabled = 0;
            uint32_t SamplesPerPixel = 0;
            uint32_t ShowOnly = 0;
            uint32_t Padding = 0;
        };

        const float EffectiveRestirGIIntensity = (std::max)(0.0f, RestirGIIntensity);
        const uint32_t EffectiveRestirGISamples = std::clamp(RestirGISamplesPerPixel, 1u, 32u);

        const FRestirGIConstants RestirGIConstants =
        {
            1.0f,
            bRestirGIEnabled ? 1u : 0u,
            EffectiveRestirGISamples,
            bRestirGIShowOnly ? 1u : 0u,
            0u
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &RestirGIConstants, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}


