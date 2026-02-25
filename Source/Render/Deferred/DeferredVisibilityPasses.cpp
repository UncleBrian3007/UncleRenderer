#include "DeferredVisibilityPasses.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"

bool FDeferredVisibilityPasses::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device) const
{
    return Owner.CreateGpuDrivenResources(Device);
}

void FDeferredVisibilityPasses::AddVisibilityListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex, uint32_t VisibilityFrameIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FVisibilityListPassData
    {
        bool bEnabled = false;
        uint32_t VisibilityIndex = UINT32_MAX;
        uint32_t VisibilityFrameIndex = UINT32_MAX;
        uint32_t FrameIndex = 0;
    };

    Context.Graph.AddPass<FVisibilityListPassData>("Build Prev Visibility Lists", [&Owner, &Context, VisibilityIndex, VisibilityFrameIndex](FVisibilityListPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bUseHzbTwoPass && Owner.BuildVisibilityListsPipeline && Owner.ClearVisibilityCountsPipeline
            && Owner.VisibilityListRootSignature && Owner.Device && Owner.Device->GetBindlessDescriptorHeap();
        Data.VisibilityIndex = VisibilityIndex;
        Data.VisibilityFrameIndex = VisibilityFrameIndex;
        Data.FrameIndex = Context.FrameIndex;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [&Owner](const FVisibilityListPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (Data.VisibilityIndex == UINT32_MAX || Data.FrameIndex >= Owner.PrevVisibleListUavBindlessIndices.size())
        {
            return;
        }

        if (Owner.PrevVisibleListUavBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.PrevInvisibleListUavBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.PrevVisibleCountUavBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.PrevInvisibleCountUavBindlessIndices[Data.FrameIndex] == UINT32_MAX)
        {
            return;
        }

        Owner.DispatchBuildVisibilityLists(
            Cmd,
            Data.VisibilityIndex,
            Owner.PrevVisibleListUavBindlessIndices[Data.FrameIndex],
            Owner.PrevInvisibleListUavBindlessIndices[Data.FrameIndex],
            Owner.PrevVisibleCountUavBindlessIndices[Data.FrameIndex],
            Owner.PrevInvisibleCountUavBindlessIndices[Data.FrameIndex],
            Data.VisibilityFrameIndex,
            Data.FrameIndex);
    });
}

void FDeferredVisibilityPasses::AddGpuCullingPass(
    FDeferredPassContext& Context,
    FRenderer::ECullingMode Mode,
    uint32_t VisibilityInputIndex,
    uint32_t VisibilityInputFrameIndex,
    uint32_t CullingListIndex,
    uint32_t CullingListCountIndex,
    const char* PassName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FGpuCullingPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        FRenderer::ECullingMode Mode = FRenderer::ECullingMode::All;
        uint32_t VisibilityInputIndex = UINT32_MAX;
        uint32_t VisibilityInputFrameIndex = UINT32_MAX;
        uint32_t CullingListIndex = UINT32_MAX;
        uint32_t CullingListCountIndex = UINT32_MAX;
    };

    Context.Graph.AddPass<FGpuCullingPassData>(PassName, [&Owner, &Context, Mode, VisibilityInputIndex, VisibilityInputFrameIndex, CullingListIndex, CullingListCountIndex](FGpuCullingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.bEnableIndirectDraw && Owner.CullingPipeline && Owner.CullingRootSignature && Owner.GetIndirectCommandBuffer()
            && Owner.ModelBoundsBuffer && Owner.MeshletConeAxisBuffer && Owner.MeshletConeApexBuffer && Owner.Device && Owner.Device->GetBindlessDescriptorHeap();
        Data.Camera = &Context.Camera;
        Data.Mode = Mode;
        Data.VisibilityInputIndex = VisibilityInputIndex;
        Data.VisibilityInputFrameIndex = VisibilityInputFrameIndex;
        Data.CullingListIndex = CullingListIndex;
        Data.CullingListCountIndex = CullingListCountIndex;
        if (Data.bEnabled)
        {
            if (Context.FrameState.bUseHZBOcclusion)
            {
                Builder.ReadTexture(Context.Resources.HZBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            Builder.KeepAlive();
        }
    }, [&Owner, PassName](const FGpuCullingPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        Owner.DispatchGpuCulling(
            Cmd,
            *Data.Camera,
            PassName,
            Data.Mode,
            Data.VisibilityInputIndex,
            Data.VisibilityInputFrameIndex,
            Data.CullingListIndex,
            Data.CullingListCountIndex,
            Data.Mode == FRenderer::ECullingMode::LateAfterEarly);
    });
}

void FDeferredVisibilityPasses::AddEarlyRejectListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FEarlyRejectPassData
    {
        bool bEnabled = false;
        uint32_t VisibilityIndex = UINT32_MAX;
        uint32_t FrameIndex = 0;
    };

    Context.Graph.AddPass<FEarlyRejectPassData>("Build Early Reject List", [&Owner, &Context, VisibilityIndex](FEarlyRejectPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bUseHzbTwoPass && Owner.BuildEarlyRejectListPipeline && Owner.ClearVisibilityCountsPipeline
            && Owner.VisibilityListRootSignature && Owner.Device && Owner.Device->GetBindlessDescriptorHeap();
        Data.VisibilityIndex = VisibilityIndex;
        Data.FrameIndex = Context.FrameIndex;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [&Owner](const FEarlyRejectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (Data.VisibilityIndex == UINT32_MAX || Data.FrameIndex >= Owner.EarlyRejectListUavBindlessIndices.size())
        {
            return;
        }

        if (Owner.EarlyRejectListUavBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.EarlyRejectCountUavBindlessIndices[Data.FrameIndex] == UINT32_MAX)
        {
            return;
        }

        Owner.DispatchBuildEarlyRejectList(
            Cmd,
            Data.VisibilityIndex,
            Owner.EarlyRejectListUavBindlessIndices[Data.FrameIndex],
            Owner.EarlyRejectCountUavBindlessIndices[Data.FrameIndex],
            Data.FrameIndex);
    });
}

void FDeferredVisibilityPasses::AddLateListMergePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FMergeListPassData
    {
        bool bEnabled = false;
        uint32_t FrameIndex = 0;
    };

    Context.Graph.AddPass<FMergeListPassData>("Merge Late Visibility Lists", [&Owner, &Context](FMergeListPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bUseHzbTwoPass && Owner.MergeVisibilityListsPipeline && Owner.VisibilityListRootSignature
            && Owner.Device && Owner.Device->GetBindlessDescriptorHeap();
        Data.FrameIndex = Context.FrameIndex;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [&Owner](const FMergeListPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (Data.FrameIndex >= Owner.LateListUavBindlessIndices.size() || Data.FrameIndex >= Owner.LateListFlagUavBindlessIndices.size())
        {
            return;
        }

        if (Owner.PrevInvisibleListSrvBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.EarlyRejectListSrvBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.PrevInvisibleCountSrvBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.EarlyRejectCountSrvBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.LateListUavBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.LateListCountUavBindlessIndices[Data.FrameIndex] == UINT32_MAX
            || Owner.LateListFlagUavBindlessIndices[Data.FrameIndex] == UINT32_MAX)
        {
            return;
        }

        Owner.DispatchMergeVisibilityLists(
            Cmd,
            Owner.PrevInvisibleListSrvBindlessIndices[Data.FrameIndex],
            Owner.EarlyRejectListSrvBindlessIndices[Data.FrameIndex],
            Owner.PrevInvisibleCountSrvBindlessIndices[Data.FrameIndex],
            Owner.EarlyRejectCountSrvBindlessIndices[Data.FrameIndex],
            Owner.LateListUavBindlessIndices[Data.FrameIndex],
            Owner.LateListCountUavBindlessIndices[Data.FrameIndex],
            Owner.LateListFlagUavBindlessIndices[Data.FrameIndex],
            Data.FrameIndex);
    });
}

void FDeferredVisibilityPasses::AddHZBPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle HZBHandle = Context.Resources.HZBHandle;

    struct FHZBPassData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipCount = 0;
        uint32_t SourceWidth = 0;
        uint32_t SourceHeight = 0;
        uint32_t DepthBindlessIndex = UINT32_MAX;
        std::vector<uint32_t> HZBSrvMips;
        std::vector<uint32_t> HZBUavs;
        uint32_t HZBNullUav = UINT32_MAX;
    };

    if (!Owner.bHZBEnabled || !FrameState.bBuildHZB)
    {
        return;
    }

    Graph.AddPass<FHZBPassData>("Build HZB", [&Owner, DepthHandle, HZBHandle](FHZBPassData& Data, FRGPassBuilder& Builder)
    {
        Data.Width = Owner.HZBWidth;
        Data.Height = Owner.HZBHeight;
        Data.MipCount = Owner.HZBMipCount;
        ID3D12Resource* DepthBuffer = Owner.GetDepthBuffer();
        const D3D12_RESOURCE_DESC DepthDesc = DepthBuffer ? DepthBuffer->GetDesc() : D3D12_RESOURCE_DESC{};
        Data.SourceWidth = static_cast<uint32>(DepthDesc.Width);
        Data.SourceHeight = DepthDesc.Height;
        const uint32_t DepthIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
        Data.DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[DepthIndex];
        Data.HZBSrvMips = Owner.HZBSrvMipBindlessIndices;
        Data.HZBUavs = Owner.HZBUavBindlessIndices;
        Data.HZBNullUav = Owner.HZBNullUavBindlessIndex;

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(HZBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FHZBPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Owner.HZBRootSignature || Data.MipCount == 0 || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent HZBEvent(LocalCommandList, L"BuildHZB");

        if (Data.DepthBindlessIndex == UINT32_MAX || Data.HZBNullUav == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(Owner.HZBRootSignature.Get());

        struct FHZBConstants
        {
            uint32_t SourceWidth;
            uint32_t SourceHeight;
            uint32_t DestWidth;
            uint32_t DestHeight;
            uint32_t DestWidth1;
            uint32_t DestHeight1;
            uint32_t DestWidth2;
            uint32_t DestHeight2;
            uint32_t DestWidth3;
            uint32_t DestHeight3;
            uint32_t SourceMip;
            uint32_t SourceIsDepth;
        };

        uint32_t CurrentWidth = Data.Width;
        uint32_t CurrentHeight = Data.Height;
        std::vector<D3D12_RESOURCE_STATES> MipStates(Data.MipCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        uint32_t MipIndex = 0;
        while (MipIndex < Data.MipCount)
        {
            const uint32_t RemainingMips = Data.MipCount - MipIndex;
            const uint32_t MipsThisDispatch = (std::min)(4u, RemainingMips);
            const bool bHasSecondMip = MipsThisDispatch > 1;
            const bool bHasThirdMip = MipsThisDispatch > 2;
            const bool bHasFourthMip = MipsThisDispatch > 3;

            const uint32_t SourceWidth = (MipIndex == 0) ? Data.SourceWidth : (std::max)(1u, CurrentWidth);
            const uint32_t SourceHeight = (MipIndex == 0) ? Data.SourceHeight : (std::max)(1u, CurrentHeight);

            const uint32_t DestWidth = (MipIndex == 0) ? CurrentWidth : (std::max)(1u, CurrentWidth / 2);
            const uint32_t DestHeight = (MipIndex == 0) ? CurrentHeight : (std::max)(1u, CurrentHeight / 2);
            const uint32_t DestWidth1 = bHasSecondMip ? (std::max)(1u, DestWidth / 2) : 0u;
            const uint32_t DestHeight1 = bHasSecondMip ? (std::max)(1u, DestHeight / 2) : 0u;
            const uint32_t DestWidth2 = bHasThirdMip ? (std::max)(1u, DestWidth1 / 2) : 0u;
            const uint32_t DestHeight2 = bHasThirdMip ? (std::max)(1u, DestHeight1 / 2) : 0u;
            const uint32_t DestWidth3 = bHasFourthMip ? (std::max)(1u, DestWidth2 / 2) : 0u;
            const uint32_t DestHeight3 = bHasFourthMip ? (std::max)(1u, DestHeight2 / 2) : 0u;

            FHZBConstants Constants = {};
            Constants.SourceWidth = SourceWidth;
            Constants.SourceHeight = SourceHeight;
            Constants.DestWidth = DestWidth;
            Constants.DestHeight = DestHeight;
            Constants.DestWidth1 = DestWidth1;
            Constants.DestHeight1 = DestHeight1;
            Constants.DestWidth2 = DestWidth2;
            Constants.DestHeight2 = DestHeight2;
            Constants.DestWidth3 = DestWidth3;
            Constants.DestHeight3 = DestHeight3;
            Constants.SourceMip = (MipIndex > 0) ? (MipIndex - 1) : 0u;
            Constants.SourceIsDepth = (MipIndex == 0) ? 1u : 0u;

            uint32_t SourceBindlessIndex = Data.DepthBindlessIndex;
            if (MipIndex > 0)
            {
                const uint32_t SourceMipIndex = MipIndex - 1;
                SourceBindlessIndex = Owner.HZBSrvBindlessIndex;

                if (SourceMipIndex < MipStates.size() && MipStates[SourceMipIndex] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                {
                    D3D12_RESOURCE_BARRIER ToSrvBarrier = {};
                    ToSrvBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    ToSrvBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    ToSrvBarrier.Transition.pResource = Owner.HierarchicalZBuffer.Get();
                    ToSrvBarrier.Transition.StateBefore = MipStates[SourceMipIndex];
                    ToSrvBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    ToSrvBarrier.Transition.Subresource = D3D12CalcSubresource(SourceMipIndex, 0, 0, Data.MipCount, 1);
                    if (Owner.bLogResourceBarriers)
                    {
                        LogInfo("HZB Barrier: Mip " + std::to_string(SourceMipIndex) + " "
                            + RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateBefore) + " -> "
                            + RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateAfter));
                    }
                    LocalCommandList->ResourceBarrier(1, &ToSrvBarrier);
                    MipStates[SourceMipIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
            }
            const uint32_t DestIndex0 = (MipIndex < Data.HZBUavs.size()) ? Data.HZBUavs[MipIndex] : UINT32_MAX;
            const uint32_t DestIndex1 = (bHasSecondMip && (MipIndex + 1) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 1]
                : Data.HZBNullUav;
            const uint32_t DestIndex2 = (bHasThirdMip && (MipIndex + 2) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 2]
                : Data.HZBNullUav;
            const uint32_t DestIndex3 = (bHasFourthMip && (MipIndex + 3) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 3]
                : Data.HZBNullUav;

            if (SourceBindlessIndex == UINT32_MAX || DestIndex0 == UINT32_MAX || DestIndex1 == UINT32_MAX
                || DestIndex2 == UINT32_MAX || DestIndex3 == UINT32_MAX)
            {
                break;
            }

            ID3D12PipelineState* SelectedPipeline = Owner.HZBPipelines[MipsThisDispatch - 1].Get();
            if (!SelectedPipeline)
            {
                break;
            }

            LocalCommandList->SetPipelineState(SelectedPipeline);
            LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
            const uint32_t HZBBindlessIndices[] = { Data.DepthBindlessIndex, SourceBindlessIndex, DestIndex0, DestIndex1, DestIndex2, DestIndex3 };
            LocalCommandList->SetComputeRoot32BitConstants(1, _countof(HZBBindlessIndices), HZBBindlessIndices, 0);

            const uint32_t GroupX = (Constants.DestWidth + 7) / 8;
            const uint32_t GroupY = (Constants.DestHeight + 7) / 8;
            LocalCommandList->Dispatch(GroupX, GroupY, 1);

            if (bHasFourthMip)
            {
                CurrentWidth = DestWidth3;
                CurrentHeight = DestHeight3;
            }
            else if (bHasThirdMip)
            {
                CurrentWidth = DestWidth2;
                CurrentHeight = DestHeight2;
            }
            else if (bHasSecondMip)
            {
                CurrentWidth = DestWidth1;
                CurrentHeight = DestHeight1;
            }
            else
            {
                CurrentWidth = DestWidth;
                CurrentHeight = DestHeight;
            }

            std::vector<D3D12_RESOURCE_BARRIER> Barriers;
            Barriers.reserve(MipsThisDispatch + 1);

            D3D12_RESOURCE_BARRIER UavBarrier = {};
            UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UavBarrier.UAV.pResource = Owner.HierarchicalZBuffer.Get();
            if (Owner.bLogResourceBarriers)
            {
                LogInfo("HZB Barrier: UAV sync");
            }
            Barriers.push_back(UavBarrier);

            for (uint32_t LocalMip = 0; LocalMip < MipsThisDispatch; ++LocalMip)
            {
                const uint32_t TargetMip = MipIndex + LocalMip;
                if (TargetMip >= Data.MipCount)
                {
                    break;
                }

                D3D12_RESOURCE_BARRIER Barrier = {};
                Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                Barrier.Transition.pResource = Owner.HierarchicalZBuffer.Get();
                Barrier.Transition.StateBefore = MipStates[TargetMip];
                Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                Barrier.Transition.Subresource = D3D12CalcSubresource(TargetMip, 0, 0, Data.MipCount, 1);
                if (Owner.bLogResourceBarriers)
                {
                    LogInfo("HZB Barrier: Mip " + std::to_string(TargetMip) + " "
                        + RendererUtils::ResourceStateToString(Barrier.Transition.StateBefore) + " -> "
                        + RendererUtils::ResourceStateToString(Barrier.Transition.StateAfter));
                }
                Barriers.push_back(Barrier);
                MipStates[TargetMip] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }

            if (!Barriers.empty())
            {
                LocalCommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
            }

            MipIndex += MipsThisDispatch;
        }

        Owner.HZBState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Owner.bHZBReady = true;
    });
}
