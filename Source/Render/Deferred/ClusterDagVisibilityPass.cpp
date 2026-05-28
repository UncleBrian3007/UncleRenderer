#include "ClusterDagVisibilityPass.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "ClusterDagRuntime.h"
#include "DeferredPassContext.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12CommandContext.h"
#include "../../RHI/DX12Device.h"

#include <vector>
#include <d3dx12.h>

constexpr DXGI_FORMAT GClusterDagVisibility64Format = DXGI_FORMAT_R32G32_UINT;

constexpr uint32_t kVisibilityPassConstantsDwordCount = 6;
constexpr uint32_t kSoftwareRasterArgsBindlessDwordCount = 2;
constexpr uint32_t kSoftwareRasterBindlessDwordCount = 15;
constexpr uint32_t kDepthExportBindlessDwordCount = 1;
constexpr uint32_t kVisibilityResolveBindlessDwordCount = 5;

uint32_t GetVisibilityPipelineIndex(bool bAlphaMask, bool bDoubleSided)
{
    return (bAlphaMask ? 2u : 0u) | (bDoubleSided ? 1u : 0u);
}

CD3DX12_STATIC_SAMPLER_DESC BuildClusterDagMaterialSampler(D3D12_SHADER_VISIBILITY ShaderVisibility)
{
    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
        0,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f,
        4,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f,
        D3D12_FLOAT32_MAX,
        ShaderVisibility);
    return SamplerDesc;
}

bool FClusterDagVisibilityPass::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    this->Owner = &Owner;
    this->Device = Device;
    bPipelinesReady = false;
    if (!Device->SupportsAtomicInt64OnTypedResource())
    {
        LogWarning("ClusterDag visibility Vis64 path disabled: AtomicInt64OnTypedResource is not supported.");
        return false;
    }

    bPipelinesReady = CreateVisibilityRootSignature(Device)
        && CreateVisibilityPipeline(Device)
        && CreateSoftwareRasterPipeline(Device)
        && CreateDepthExportRootSignature(Device)
        && CreateDepthExportPipeline(Device)
        && CreateResolveRootSignature(Device)
        && CreateResolvePipeline(Device)
        && CreateCommandSignature(Device);
    return bPipelinesReady;
}

bool FClusterDagVisibilityPass::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    this->Owner = &Owner;
    this->Device = Device;
    bResourcesReady = false;
    return CreateVisibilityResources(Device, Width, Height);
}

void FClusterDagVisibilityPass::ImportPersistentResources(FDeferredPassContext& Context)
{
    if (!IsReady())
    {
        return;
    }

    Context.Resources.ClusterDagVisibility.Visibility64Handle = ImportBindlessTexture(
        Context.Graph,
        "ClusterDagVisibility64",
        VisibilityTexture64);
}

void FClusterDagVisibilityPass::AddPasses(FDeferredPassContext& Context) const
{
    if (!IsReady())
    {
        return;
    }

    AddVisibilityPass(Context);
    AddSoftwareRasterPass(Context);
    AddDepthExportPass(Context);
    AddResolvePass(Context);
}

void FClusterDagVisibilityPass::AddVisibilityPass(FDeferredPassContext& Context) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle Visibility64Handle{};
        FRGBufferHandle DagIndirectHandle{};
        FRGBufferHandle DrawDataVisibleEntryHandle{};
        FRGBufferHandle VisibleEntryHandle{};
        FRGBufferHandle PageDataHandle{};
        ID3D12Resource* DagIndirectBuffer = nullptr;
        ID3D12Resource* DagRunCountBuffer = nullptr;
        const std::vector<FRenderer::FIndirectDrawRange>* DagRanges = nullptr;
        const FCamera* Camera = nullptr;
        uint32_t Visibility64UavIndex = UINT32_MAX;
        uint32_t DrawDataVisibleEntrySrvIndex = UINT32_MAX;
        uint32_t VisibleEntrySrvIndex = UINT32_MAX;
        uint32_t PageDataSrvIndex = UINT32_MAX;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagVisibility", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        FClusterDagRuntime* Runtime = Owner->ClusterDagRuntime.get();
        Data.DagIndirectBuffer = Runtime->IndirectCommandBuffers[Context.FrameIndex].Get();
        Data.DagRunCountBuffer = Runtime->RunCountBuffers[Context.FrameIndex].Get();
        Data.DagRanges = &Runtime->GetIndirectDrawRanges();
        Data.bEnabled =
            Context.Resources.ClusterDagVisibility.Visibility64Handle
            && Data.DagIndirectBuffer != nullptr
            && Data.DagRunCountBuffer != nullptr
            && !Data.DagRanges->empty();
        Data.Visibility64Handle = Context.Resources.ClusterDagVisibility.Visibility64Handle;
        Data.Camera = &Context.Camera;
        Data.Visibility64UavIndex = VisibilityTexture64.UavBindlessIndex;
        Data.DrawDataVisibleEntrySrvIndex = Runtime->DrawDataVisibleEntryIndexBuffers[Context.FrameIndex].SrvBindlessIndex;
        Data.VisibleEntrySrvIndex = Runtime->VisibleEntryBuffers[Context.FrameIndex].SrvBindlessIndex;
        if (FClusterDagStreamingManager* StreamingManager = Owner->GetClusterDagStreamingManager(); StreamingManager && StreamingManager->IsEnabled())
        {
            Data.PageDataSrvIndex = StreamingManager->GetPageDataSrvBindlessIndex();
        }

        if (Data.bEnabled)
        {
            Data.DagIndirectHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagIndirectCommandsVisibility", Runtime->IndirectCommandBuffers[Context.FrameIndex]);
            Data.DrawDataVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagDrawDataVisibleEntryVisibility", Runtime->DrawDataVisibleEntryIndexBuffers[Context.FrameIndex]);
            Data.VisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagVisibleEntriesVisibility", Runtime->VisibleEntryBuffers[Context.FrameIndex]);
            Builder.WriteTexture(Data.Visibility64Handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Builder.ReadBuffer(Data.DagIndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            Builder.ReadBuffer(Data.DrawDataVisibleEntryHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Builder.ReadBuffer(Data.VisibleEntryHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (Data.PageDataSrvIndex != UINT32_MAX)
            {
                FClusterDagStreamingManager* StreamingManager = Owner->GetClusterDagStreamingManager();
                Data.PageDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagStreamingPageDataVisibility", StreamingManager->GetPageDataBuffer());
                Builder.ReadBuffer(Data.PageDataHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner->Device->GetBindlessDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        CommandList->ClearUnorderedAccessViewUint(
            Owner->GetBindlessGpuHandle(Data.Visibility64UavIndex),
            Owner->GetBindlessCpuClearHandle(Data.Visibility64UavIndex),
            VisibilityTexture64.Get(),
            ClearValues,
            0,
            nullptr);
        const D3D12_RESOURCE_BARRIER ClearBarrier = CD3DX12_RESOURCE_BARRIER::UAV(VisibilityTexture64.Get());
        CommandList->ResourceBarrier(1, &ClearBarrier);
        CommandList->SetGraphicsRootSignature(VisibilityRootSignature.Get());
        CommandList->RSSetViewports(1, &Owner->Viewport);
        CommandList->RSSetScissorRects(1, &Owner->ScissorRect);
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE DepthHandle = Owner->GetDSVHandle();
        CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthHandle);

        Owner->EnsureClusterDagSceneConstantsPrepared(*Data.Camera);

        const uint32_t VisibilityExtraConstants[] =
        {
            Data.Visibility64UavIndex,
            Data.DrawDataVisibleEntrySrvIndex,
            Data.VisibleEntrySrvIndex,
            Data.PageDataSrvIndex
        };
        CommandList->SetGraphicsRoot32BitConstants(1, _countof(VisibilityExtraConstants), VisibilityExtraConstants, 2);

        for (size_t RangeIndex = 0; RangeIndex < Data.DagRanges->size(); ++RangeIndex)
        {
            const FRenderer::FIndirectDrawRange& Range = (*Data.DagRanges)[RangeIndex];
            const bool bDoubleSided = (Range.PipelineKey & RendererUtils::GPipelineKeyDoubleSidedMask) != 0;
            const bool bAlphaMask = (Range.PipelineKey & RendererUtils::GPipelineKeyAlphaMaskMask) != 0;
            ID3D12PipelineState* Pipeline = VisibilityPipelines[GetVisibilityPipelineIndex(bAlphaMask, bDoubleSided)].Get();
            CommandList->SetPipelineState(Pipeline);
            const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
            const uint64_t CountOffset = RangeIndex * sizeof(uint32_t);
            if (AreSectionPixEventsEnabled())
            {
                const wchar_t* Label = Range.Name.empty() ? L"ClusterDagVisibilityRange" : Range.Name.c_str();
                FScopedPixEvent RangeEvent(CommandList, Label);
                CommandList->ExecuteIndirect(CommandSignature.Get(), Range.Count, Data.DagIndirectBuffer, Offset, Data.DagRunCountBuffer, CountOffset);
            }
            else
            {
                CommandList->ExecuteIndirect(CommandSignature.Get(), Range.Count, Data.DagIndirectBuffer, Offset, Data.DagRunCountBuffer, CountOffset);
            }
        }
    });
}

void FClusterDagVisibilityPass::AddSoftwareRasterPass(FDeferredPassContext& Context) const
{
    struct FPreparePassData
    {
        bool bEnabled = false;
        FRGBufferHandle VisibleEntryCounterHandle{};
        FRGBufferHandle DispatchArgsHandle{};
        uint32_t BindlessIndices[kSoftwareRasterArgsBindlessDwordCount] = {};
    };

    Context.Graph.AddPass<FPreparePassData>("ClusterDagPrepareSoftwareRasterArgs", [this, &Context](FPreparePassData& Data, FRGPassBuilder& Builder)
    {
        FClusterDagRuntime* Runtime = Owner->ClusterDagRuntime.get();
        const uint32_t FrameIndex = Context.FrameIndex;
        Data.bEnabled = static_cast<bool>(Context.Resources.ClusterDagVisibility.Visibility64Handle);
        if (!Data.bEnabled)
        {
            return;
        }

        Data.VisibleEntryCounterHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagPrepareSWVisibleEntryCounters", Runtime->VisibleEntryCounterBuffers[FrameIndex]);
        Data.DispatchArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagPrepareSWDispatchArgs", Runtime->SwRasterDispatchArgsBuffers[FrameIndex]);
        Data.BindlessIndices[0] = Runtime->VisibleEntryCounterBuffers[FrameIndex].SrvBindlessIndex;
        Data.BindlessIndices[1] = Runtime->SwRasterDispatchArgsBuffers[FrameIndex].UavBindlessIndex;

        Builder.ReadBuffer(Data.VisibleEntryCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.DispatchArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.DispatchArgsHandle);
    }, [this](const FPreparePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner->Device->GetBindlessDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(SoftwareRasterRootSignature.Get());
        CommandList->SetPipelineState(PrepareSoftwareRasterArgsPipeline.Get());
        CommandList->SetComputeRoot32BitConstants(0, kSoftwareRasterArgsBindlessDwordCount, Data.BindlessIndices, 0);
        CommandList->Dispatch(1, 1, 1);
    });

    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle Visibility64Handle{};
        FRGResourceHandle DepthHandle{};
        FRGBufferHandle VisibleEntryHandle{};
        FRGBufferHandle SwVisibleEntryHandle{};
        FRGBufferHandle VisibleEntryCounterHandle{};
        FRGBufferHandle ClusterDataHandle{};
        FRGBufferHandle DrawDataHandle{};
        FRGBufferHandle SceneDataHandle{};
        FRGBufferHandle PageDataHandle{};
        FRGBufferHandle DispatchArgsHandle{};
        FRGResourceHandle HzbHandle{};
        ID3D12Resource* DispatchArgsBuffer = nullptr;
        bool bUseHzbReject = false;
        uint32_t BindlessIndices[kSoftwareRasterBindlessDwordCount] = {};
    };

    Context.Graph.AddPass<FPassData>("ClusterDagSoftwareRaster", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        FClusterDagRuntime* Runtime = Owner->ClusterDagRuntime.get();
        const uint32_t FrameIndex = Context.FrameIndex;
        const uint32_t DepthIndex = Owner->GetCurrentDepthSrvBindlessIndex();
        Data.bEnabled = Context.Resources.ClusterDagVisibility.Visibility64Handle
            && IsValidBindlessIndex(DepthIndex);
        if (!Data.bEnabled)
        {
            return;
        }

        Data.bUseHzbReject = bSoftwareRasterHzbRejectEnabled
            && Context.FrameState.bUseHZBOcclusion
            && Context.Resources.Hzb.HzbHandle
            && IsValidBindlessIndex(Owner->Hzb->GetSrvBindlessIndex())
            && Owner->Hzb->GetWidth() > 0u
            && Owner->Hzb->GetHeight() > 0u
            && Owner->Hzb->GetMipCount() > 0u;
        Data.Visibility64Handle = Context.Resources.ClusterDagVisibility.Visibility64Handle;
        Data.DepthHandle = Context.Resources.DepthHandle;
        Data.VisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWVisibleEntries", Runtime->VisibleEntryBuffers[FrameIndex]);
        Data.SwVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWVisibleEntryIndices", Runtime->SwVisibleEntryIndexBuffers[FrameIndex]);
        Data.VisibleEntryCounterHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWVisibleEntryCounters", Runtime->VisibleEntryCounterBuffers[FrameIndex]);
        Data.ClusterDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWClusterData", Runtime->ClusterBuffer);
        Data.DrawDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWDrawData", Runtime->DrawDataBuffer);
        Data.SceneDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWSceneData", Owner->ClusterDagSceneConstantBuffers[FrameIndex]);
        Data.DispatchArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWDispatchArgs", Runtime->SwRasterDispatchArgsBuffers[FrameIndex]);
        Data.DispatchArgsBuffer = Runtime->SwRasterDispatchArgsBuffers[FrameIndex].Get();
        Data.BindlessIndices[0] = VisibilityTexture64.UavBindlessIndex;
        Data.BindlessIndices[1] = Runtime->VisibleEntryBuffers[FrameIndex].SrvBindlessIndex;
        Data.BindlessIndices[2] = Runtime->SwVisibleEntryIndexBuffers[FrameIndex].SrvBindlessIndex;
        Data.BindlessIndices[3] = Runtime->VisibleEntryCounterBuffers[FrameIndex].SrvBindlessIndex;
        Data.BindlessIndices[4] = Runtime->ClusterBuffer.SrvBindlessIndex;
        Data.BindlessIndices[5] = Runtime->DrawDataBuffer.SrvBindlessIndex;
        Data.BindlessIndices[6] = Owner->ClusterDagSceneConstantBuffers[FrameIndex].SrvBindlessIndex;
        Data.BindlessIndices[7] = DepthIndex;
        Data.BindlessIndices[8] = static_cast<uint32_t>(Owner->Viewport.Width);
        Data.BindlessIndices[9] = static_cast<uint32_t>(Owner->Viewport.Height);
        Data.BindlessIndices[10] = Data.bUseHzbReject ? Owner->Hzb->GetSrvBindlessIndex() : UINT32_MAX;
        Data.BindlessIndices[11] = Data.bUseHzbReject ? Owner->Hzb->GetWidth() : 0u;
        Data.BindlessIndices[12] = Data.bUseHzbReject ? Owner->Hzb->GetHeight() : 0u;
        Data.BindlessIndices[13] = Data.bUseHzbReject ? Owner->Hzb->GetMipCount() : 0u;
        Data.BindlessIndices[14] = UINT32_MAX;
        if (FClusterDagStreamingManager* StreamingManager = Owner->GetClusterDagStreamingManager(); StreamingManager && StreamingManager->IsEnabled())
        {
            Data.BindlessIndices[14] = StreamingManager->GetPageDataSrvBindlessIndex();
            Data.PageDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSWPageData", StreamingManager->GetPageDataBuffer());
            Builder.ReadBuffer(Data.PageDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        Builder.WriteTexture(Data.Visibility64Handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadTexture(Data.DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Data.bUseHzbReject)
        {
            Data.HzbHandle = Context.Resources.Hzb.HzbHandle;
            Builder.ReadTexture(Data.HzbHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.ReadBuffer(Data.VisibleEntryHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.SwVisibleEntryHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.VisibleEntryCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ClusterDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.DrawDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.SceneDataHandle, D3D12_RESOURCE_STATE_GENERIC_READ);
        Builder.ReadBuffer(Data.DispatchArgsHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.UavBarrier(Data.Visibility64Handle);
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner->Device->GetBindlessDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(SoftwareRasterRootSignature.Get());
        CommandList->SetPipelineState(SoftwareRasterPipelines[Data.bUseHzbReject ? 1u : 0u].Get());
        CommandList->SetComputeRoot32BitConstants(0, kSoftwareRasterBindlessDwordCount, Data.BindlessIndices, 0);
        CommandList->ExecuteIndirect(Owner->ClusterDagRuntime->DispatchCommandSignature.Get(), 1, Data.DispatchArgsBuffer, 0u, nullptr, 0u);
    });
}

void FClusterDagVisibilityPass::AddDepthExportPass(FDeferredPassContext& Context) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle Visibility64Handle{};
        uint32_t Visibility64SrvIndex = UINT32_MAX;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagDepthExport", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = static_cast<bool>(Context.Resources.ClusterDagVisibility.Visibility64Handle);
        if (!Data.bEnabled)
        {
            return;
        }

        Data.Visibility64Handle = Context.Resources.ClusterDagVisibility.Visibility64Handle;
        Data.Visibility64SrvIndex = VisibilityTexture64.SrvBindlessIndex;
        Builder.ReadTexture(Data.Visibility64Handle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner->Device->GetBindlessDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetGraphicsRootSignature(DepthExportRootSignature.Get());
        CommandList->SetPipelineState(DepthExportPipeline.Get());
        CommandList->RSSetViewports(1, &Owner->Viewport);
        CommandList->RSSetScissorRects(1, &Owner->ScissorRect);
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->SetGraphicsRoot32BitConstants(0, 1, &Data.Visibility64SrvIndex, 0);
        const D3D12_CPU_DESCRIPTOR_HANDLE DepthHandle = Owner->GetDSVHandle();
        CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthHandle);
        CommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FClusterDagVisibilityPass::AddResolvePass(FDeferredPassContext& Context) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle Visibility64Handle{};
        FRGBufferHandle VisibleEntryHandle{};
        FRGBufferHandle DrawDataHandle{};
        FRGBufferHandle SceneDataHandle{};
        FRGBufferHandle PageDataHandle{};
        uint32_t Visibility64SrvIndex = UINT32_MAX;
        uint32_t VisibleEntrySrvIndex = UINT32_MAX;
        uint32_t DrawDataSrvIndex = UINT32_MAX;
        uint32_t SceneDataSrvIndex = UINT32_MAX;
        uint32_t PageDataSrvIndex = UINT32_MAX;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagResolve", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        FClusterDagRuntime* Runtime = Owner->ClusterDagRuntime.get();
        Data.bEnabled = static_cast<bool>(Context.Resources.ClusterDagVisibility.Visibility64Handle);
        if (!Data.bEnabled)
        {
            return;
        }

        Data.Visibility64Handle = Context.Resources.ClusterDagVisibility.Visibility64Handle;
        Data.VisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagResolveVisibleEntries", Runtime->VisibleEntryBuffers[Context.FrameIndex]);
        Data.DrawDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagResolveDrawData", Runtime->DrawDataBuffer);
        Data.SceneDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSceneData", Owner->ClusterDagSceneConstantBuffers[Context.FrameIndex]);
        Data.Visibility64SrvIndex = VisibilityTexture64.SrvBindlessIndex;
        Data.VisibleEntrySrvIndex = Runtime->VisibleEntryBuffers[Context.FrameIndex].SrvBindlessIndex;
        Data.DrawDataSrvIndex = Runtime->DrawDataBuffer.SrvBindlessIndex;
        Data.SceneDataSrvIndex = Owner->ClusterDagSceneConstantBuffers[Context.FrameIndex].SrvBindlessIndex;
        if (FClusterDagStreamingManager* StreamingManager = Owner->GetClusterDagStreamingManager(); StreamingManager && StreamingManager->IsEnabled())
        {
            Data.PageDataSrvIndex = StreamingManager->GetPageDataSrvBindlessIndex();
            Data.PageDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagResolvePageData", StreamingManager->GetPageDataBuffer());
            Builder.ReadBuffer(Data.PageDataHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        Builder.ReadTexture(Data.Visibility64Handle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.VisibleEntryHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.DrawDataHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.SceneDataHandle, D3D12_RESOURCE_STATE_GENERIC_READ);
        for (FRGResourceHandle GBufferHandle : Context.Resources.GBufferHandles)
        {
            Builder.WriteTexture(GBufferHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        Builder.WriteTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner->Device->GetBindlessDescriptorHeap(), Owner->Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetGraphicsRootSignature(ResolveRootSignature.Get());
        CommandList->SetPipelineState(ResolvePipeline.Get());
        CommandList->RSSetViewports(1, &Owner->Viewport);
        CommandList->RSSetScissorRects(1, &Owner->ScissorRect);
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const uint32_t ResolveIndices[kVisibilityResolveBindlessDwordCount] =
        {
            Data.Visibility64SrvIndex,
            Data.VisibleEntrySrvIndex,
            Data.DrawDataSrvIndex,
            Data.SceneDataSrvIndex,
            Data.PageDataSrvIndex
        };
        CommandList->SetGraphicsRoot32BitConstants(0, _countof(ResolveIndices), ResolveIndices, 0);

        D3D12_CPU_DESCRIPTOR_HANDLE ResolveRtvs[5] =
        {
            Owner->GBufferRTVHandles[0],
            Owner->GBufferRTVHandles[1],
            Owner->GBufferRTVHandles[2],
            Owner->GBufferRTVHandles[3],
            Owner->LightingRTVHandle
        };
        CommandList->OMSetRenderTargets(_countof(ResolveRtvs), ResolveRtvs, FALSE, nullptr);
        CommandList->DrawInstanced(3, 1, 0, 0);
    });
}

bool FClusterDagVisibilityPass::IsReady() const
{
    return bPipelinesReady
        && bResourcesReady
        && Owner != nullptr
        && Device != nullptr
        && Device->SupportsAtomicInt64OnTypedResource()
        && Owner->ClusterDagRuntime != nullptr
        && Owner->ClusterDagRuntime->HasResources();
}

bool FClusterDagVisibilityPass::CreateVisibilityRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kVisibilityPassConstantsDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_ALL);
    const CD3DX12_STATIC_SAMPLER_DESC MaterialSampler = BuildClusterDagMaterialSampler(D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams),
        RootParams,
        1,
        &MaterialSampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(VisibilityRootSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateVisibilityPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = VisibilityRootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PsoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;

    for (uint32_t AlphaMaskVariant = 0; AlphaMaskVariant < 2; ++AlphaMaskVariant)
    {
        const bool bAlphaMask = AlphaMaskVariant != 0u;
        const std::vector<std::wstring> Defines =
        {
            bAlphaMask
                ? L"CLUSTER_DAG_VISIBILITY_ALPHA_MASK=1"
                : L"CLUSTER_DAG_VISIBILITY_ALPHA_MASK=0"
        };

        std::vector<uint8_t> VsByteCode;
        std::vector<uint8_t> PsByteCode;
        if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ClusterDagVisibility.hlsl", VsByteCode, Defines))
        {
            return false;
        }
        if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/ClusterDagVisibility.hlsl", PsByteCode, Defines))
        {
            return false;
        }

        PsoDesc.VS = { VsByteCode.data(), VsByteCode.size() };
        PsoDesc.PS = { PsByteCode.data(), PsByteCode.size() };

        for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
        {
            const bool bDoubleSided = DoubleSidedVariant != 0u;
            PsoDesc.RasterizerState.CullMode = bDoubleSided ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
            HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(
                &PsoDesc,
                IID_PPV_ARGS(VisibilityPipelines[GetVisibilityPipelineIndex(bAlphaMask, bDoubleSided)].ReleaseAndGetAddressOf())));
        }
    }

    return true;
}

bool FClusterDagVisibilityPass::CreateSoftwareRasterPipeline(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(kSoftwareRasterBindlessDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    const CD3DX12_STATIC_SAMPLER_DESC MaterialSampler = BuildClusterDagMaterialSampler(D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams),
        RootParams,
        1,
        &MaterialSampler,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SoftwareRasterRootSignature.ReleaseAndGetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> PrepareArgsByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/PrepareClusterDagSwRasterArgs.hlsl", PrepareArgsByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SoftwareRasterRootSignature.Get();
    PsoDesc.CS = { PrepareArgsByteCode.data(), PrepareArgsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(PrepareSoftwareRasterArgsPipeline.ReleaseAndGetAddressOf())));

    for (uint32_t HzbRejectVariant = 0; HzbRejectVariant < static_cast<uint32_t>(SoftwareRasterPipelines.size()); ++HzbRejectVariant)
    {
        const bool bUseHzbReject = HzbRejectVariant != 0u;
        const std::vector<std::wstring> Defines =
        {
            bUseHzbReject
                ? L"CLUSTER_DAG_SW_RASTER_HZB_REJECT=1"
                : L"CLUSTER_DAG_SW_RASTER_HZB_REJECT=0"
        };

        std::vector<uint8_t> CsByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/RasterizeClusterSW.hlsl", CsByteCode, Defines))
        {
            return false;
        }

        PsoDesc.CS = { CsByteCode.data(), CsByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SoftwareRasterPipelines[HzbRejectVariant].ReleaseAndGetAddressOf())));
    }
    return true;
}

bool FClusterDagVisibilityPass::CreateDepthExportRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(kDepthExportBindlessDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams),
        RootParams,
        0,
        nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(DepthExportRootSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateDepthExportPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VsByteCode;
    std::vector<uint8_t> PsByteCode;
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ClusterDagDepthExport.hlsl", VsByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/ClusterDagDepthExport.hlsl", PsByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = DepthExportRootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.VS = { VsByteCode.data(), VsByteCode.size() };
    PsoDesc.PS = { PsByteCode.data(), PsByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;
    PsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthExportPipeline.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateResolveRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(kVisibilityResolveBindlessDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
        0,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f,
        4,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f,
        D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams),
        RootParams,
        1,
        &SamplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
            | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(ResolveRootSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateResolvePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VsByteCode;
    std::vector<uint8_t> PsByteCode;
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ClusterDagResolve.hlsl", VsByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/ClusterDagResolve.hlsl", PsByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = ResolveRootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.VS = { VsByteCode.data(), VsByteCode.size() };
    PsoDesc.PS = { PsByteCode.data(), PsByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;
    PsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    for (UINT TargetIndex = 0; TargetIndex < 5; ++TargetIndex)
    {
        PsoDesc.BlendState.RenderTarget[TargetIndex].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    PsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = kDeferredGBufferCount + 1u;
    for (uint32_t i = 0; i < kDeferredGBufferCount; ++i)
    {
        PsoDesc.RTVFormats[i] = FDeferredRenderer::GBufferFormats[i];
    }
    PsoDesc.RTVFormats[kDeferredGBufferCount] = FDeferredRenderer::LightingBufferFormat;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(ResolvePipeline.ReleaseAndGetAddressOf())));
    return true;
}


bool FClusterDagVisibilityPass::CreateCommandSignature(FDX12Device* Device)
{
    D3D12_INDIRECT_ARGUMENT_DESC IndirectArgs[3] = {};
    IndirectArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    IndirectArgs[0].ConstantBufferView.RootParameterIndex = 0;
    IndirectArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    IndirectArgs[1].Constant.RootParameterIndex = 1;
    IndirectArgs[1].Constant.Num32BitValuesToSet = 2;
    IndirectArgs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.pArgumentDescs = IndirectArgs;
    CommandDesc.NumArgumentDescs = _countof(IndirectArgs);
    CommandDesc.ByteStride = sizeof(FIndirectDrawCommand);
    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, VisibilityRootSignature.Get(), IID_PPV_ARGS(CommandSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateVisibilityResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    bResourcesReady = false;

    const FRGTextureDesc Desc =
    {
        Width,
        Height,
        GClusterDagVisibility64Format
    };

    CreateBindlessTexture(
        Device,
        L"ClusterDagVisibility64",
        Desc,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        VisibilityTexture64,
        true,
        true,
        nullptr);

    bResourcesReady = VisibilityTexture64
        && IsValidBindlessIndex(VisibilityTexture64.SrvBindlessIndex)
        && IsValidBindlessIndex(VisibilityTexture64.UavBindlessIndex);
    if (!bResourcesReady)
    {
        LogWarning("ClusterDag visibility resources are incomplete; visibility pass will remain disabled.");
    }

    return bResourcesReady;
}
