#include "DeferredVisibilityPasses.h"

#include "../DeferredRenderer.h"
#include "../../RHI/DX12Device.h"

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
    Context.Owner.AddHZBPass(Context.Graph, Context.FrameState, Context.Resources.DepthHandle, Context.Resources.HZBHandle);
}
