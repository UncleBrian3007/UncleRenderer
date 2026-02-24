#include "DX12CommandContext.h"
#include "DX12Device.h"
#include "DX12CommandQueue.h"
#include "../Core/Logger.h"
#include <cwchar>
#include <cassert>
#include <algorithm>


D3D12_BARRIER_SYNC DX12MapStateToBarrierSync(D3D12_RESOURCE_STATES State)
    {
        if (State == D3D12_RESOURCE_STATE_COMMON)
        {
            return D3D12_BARRIER_SYNC_ALL;
        }

        D3D12_BARRIER_SYNC Sync = D3D12_BARRIER_SYNC_NONE;
        if (State & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) Sync |= D3D12_BARRIER_SYNC_VERTEX_SHADING;
        if (State & D3D12_RESOURCE_STATE_INDEX_BUFFER) Sync |= D3D12_BARRIER_SYNC_INDEX_INPUT;
        if (State & D3D12_RESOURCE_STATE_RENDER_TARGET) Sync |= D3D12_BARRIER_SYNC_RENDER_TARGET;
        if (State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) Sync |= D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        if (State & D3D12_RESOURCE_STATE_DEPTH_WRITE) Sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
        if (State & D3D12_RESOURCE_STATE_DEPTH_READ) Sync |= D3D12_BARRIER_SYNC_DEPTH_STENCIL;
        if (State & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) Sync |= D3D12_BARRIER_SYNC_NON_PIXEL_SHADING;
        if (State & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) Sync |= D3D12_BARRIER_SYNC_PIXEL_SHADING;
        if (State & D3D12_RESOURCE_STATE_COPY_DEST) Sync |= D3D12_BARRIER_SYNC_COPY;
        if (State & D3D12_RESOURCE_STATE_COPY_SOURCE) Sync |= D3D12_BARRIER_SYNC_COPY;
        if (State & D3D12_RESOURCE_STATE_RESOLVE_DEST) Sync |= D3D12_BARRIER_SYNC_RESOLVE;
        if (State & D3D12_RESOURCE_STATE_RESOLVE_SOURCE) Sync |= D3D12_BARRIER_SYNC_RESOLVE;
        if (State & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) Sync |= D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
        if (State & D3D12_RESOURCE_STATE_PRESENT) Sync |= D3D12_BARRIER_SYNC_ALL;

        return (Sync == D3D12_BARRIER_SYNC_NONE) ? D3D12_BARRIER_SYNC_ALL : Sync;
    }

D3D12_BARRIER_ACCESS DX12MapStateToBarrierAccess(D3D12_RESOURCE_STATES State)
    {
        if (State == D3D12_RESOURCE_STATE_COMMON)
        {
            return D3D12_BARRIER_ACCESS_COMMON;
        }

        D3D12_BARRIER_ACCESS Access = D3D12_BARRIER_ACCESS_COMMON;
        if (State & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) Access |= D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
        if (State & D3D12_RESOURCE_STATE_INDEX_BUFFER) Access |= D3D12_BARRIER_ACCESS_INDEX_BUFFER;
        if (State & D3D12_RESOURCE_STATE_RENDER_TARGET) Access |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
        if (State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) Access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        if (State & D3D12_RESOURCE_STATE_DEPTH_WRITE) Access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        if (State & D3D12_RESOURCE_STATE_DEPTH_READ) Access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
        if (State & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) Access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        if (State & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) Access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        if (State & D3D12_RESOURCE_STATE_COPY_DEST) Access |= D3D12_BARRIER_ACCESS_COPY_DEST;
        if (State & D3D12_RESOURCE_STATE_COPY_SOURCE) Access |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
        if (State & D3D12_RESOURCE_STATE_RESOLVE_DEST) Access |= D3D12_BARRIER_ACCESS_RESOLVE_DEST;
        if (State & D3D12_RESOURCE_STATE_RESOLVE_SOURCE) Access |= D3D12_BARRIER_ACCESS_RESOLVE_SOURCE;
        if (State & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) Access |= D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
        if (State & D3D12_RESOURCE_STATE_PRESENT) Access |= D3D12_BARRIER_ACCESS_COMMON;

        return Access;
    }

D3D12_BARRIER_LAYOUT DX12MapStateToTextureLayout(D3D12_RESOURCE_STATES State)
    {
        if (State & D3D12_RESOURCE_STATE_RENDER_TARGET) return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        if (State & D3D12_RESOURCE_STATE_DEPTH_WRITE) return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        if (State & D3D12_RESOURCE_STATE_DEPTH_READ) return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
        if (State & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
        if (State & D3D12_RESOURCE_STATE_COPY_DEST) return D3D12_BARRIER_LAYOUT_COPY_DEST;
        if (State & D3D12_RESOURCE_STATE_COPY_SOURCE) return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        if (State & D3D12_RESOURCE_STATE_RESOLVE_DEST) return D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
        if (State & D3D12_RESOURCE_STATE_RESOLVE_SOURCE) return D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;
        if (State & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
        {
            return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        }
        if (State & D3D12_RESOURCE_STATE_PRESENT) return D3D12_BARRIER_LAYOUT_PRESENT;
        return D3D12_BARRIER_LAYOUT_COMMON;
    }

D3D12_BARRIER_SUBRESOURCE_RANGE DX12MakeTextureBarrierRange(ID3D12Resource* Resource, UINT Subresource)
    {
        D3D12_BARRIER_SUBRESOURCE_RANGE Range = {};
        if (!Resource)
        {
            return Range;
        }

        const D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
        const UINT MipLevels = (std::max)(1u, static_cast<UINT>(Desc.MipLevels));
        const UINT ArraySize = (Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            ? 1u
            : (std::max)(1u, static_cast<UINT>(Desc.DepthOrArraySize));

        if (Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
        {
            Range.IndexOrFirstMipLevel = 0;
            Range.NumMipLevels = MipLevels;
            Range.FirstArraySlice = 0;
            Range.NumArraySlices = ArraySize;
            Range.FirstPlane = 0;
            Range.NumPlanes = 1;
            return Range;
        }

        UINT MipSlice = 0;
        UINT ArraySlice = 0;
        UINT PlaneSlice = 0;
        D3D12DecomposeSubresource(Subresource, MipLevels, ArraySize, MipSlice, ArraySlice, PlaneSlice);

        Range.IndexOrFirstMipLevel = MipSlice;
        Range.NumMipLevels = 1;
        Range.FirstArraySlice = ArraySlice;
        Range.NumArraySlices = 1;
        Range.FirstPlane = PlaneSlice;
        Range.NumPlanes = 1;
        return Range;
    }

FDX12CommandContext::FDX12CommandContext()
    : Device(nullptr)
    , Queue(nullptr)
    , FrameCount(0)
    , CurrentAllocatorIndex(0)
{
}

FDX12CommandContext::~FDX12CommandContext()
{
}

bool FDX12CommandContext::Initialize(FDX12Device* InDevice, FDX12CommandQueue* InQueue, uint32 InFrameCount)
{
    Device = InDevice;
    Queue = InQueue;
    FrameCount = InFrameCount;

    if (FrameCount == 0)
    {
        LogError("Frame count must be greater than zero");
        return false;
    }

    LogInfo("Command context initialization started");

    CommandAllocators.resize(FrameCount);
    FrameFenceValues.assign(FrameCount, 0);

    for (uint32 Index = 0; Index < FrameCount; ++Index)
    {
        HR_CHECK(Device->GetDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(CommandAllocators[Index].GetAddressOf())));
    }

    HR_CHECK(Device->GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        CommandAllocators[0].Get(),
        nullptr,
        IID_PPV_ARGS(CommandList.GetAddressOf())));

    CommandList->SetName(L"FrameCommandList_Init");
    HR_CHECK(CommandList->Close());

    LogInfo("Command context initialization complete");
    return true;
}

void FDX12CommandContext::BeginFrame(uint32 FrameIndex)
{
    if (FrameCount == 0)
    {
        LogError("BeginFrame called before initialization");
        return;
    }

    CurrentAllocatorIndex = FrameIndex % FrameCount;

    const uint64 FenceValue = FrameFenceValues[CurrentAllocatorIndex];
    if (FenceValue > 0 && Queue->GetCompletedFenceValue() < FenceValue)
    {
        Queue->Wait(FenceValue);
    }

    HR_CHECK(CommandAllocators[CurrentAllocatorIndex]->Reset());
    HR_CHECK(CommandList->Reset(CommandAllocators[CurrentAllocatorIndex].Get(), nullptr));

    wchar_t CommandListName[64] = {};
    swprintf_s(CommandListName, L"Frame_%u_Main_CL", CurrentAllocatorIndex);
    CommandList->SetName(CommandListName);

    CommandList4.Reset();
    CommandList7.Reset();
}

void FDX12CommandContext::TransitionResource(ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
{
    TransitionResourceEx(Resource, Before, After, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

void FDX12CommandContext::TransitionResourceEx(ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After, UINT Subresource)
{
    if (!Resource || Before == After)
    {
        return;
    }

    const bool bUseEnhanced = Device && Device->SupportsEnhancedBarriers();
    ID3D12GraphicsCommandList7* CommandListV7 = bUseEnhanced ? GetCommandList7() : nullptr;

#if defined(_DEBUG)
    if (bUseEnhanced && !CommandListV7)
    {
        assert(false && "Enhanced barriers supported but ID3D12GraphicsCommandList7 query failed.");
    }
#endif

    if (CommandListV7)
    {
        const D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
        if (Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            D3D12_BUFFER_BARRIER BufferBarrier = {};
            BufferBarrier.SyncBefore = DX12MapStateToBarrierSync(Before);
            BufferBarrier.SyncAfter = DX12MapStateToBarrierSync(After);
            BufferBarrier.AccessBefore = DX12MapStateToBarrierAccess(Before);
            BufferBarrier.AccessAfter = DX12MapStateToBarrierAccess(After);
            BufferBarrier.pResource = Resource;
            BufferBarrier.Offset = 0;
            BufferBarrier.Size = UINT64_MAX;

            D3D12_BARRIER_GROUP Group = {};
            Group.Type = D3D12_BARRIER_TYPE_BUFFER;
            Group.NumBarriers = 1;
            Group.pBufferBarriers = &BufferBarrier;
            CommandListV7->Barrier(1, &Group);
            return;
        }

        D3D12_TEXTURE_BARRIER TextureBarrier = {};
        TextureBarrier.SyncBefore = DX12MapStateToBarrierSync(Before);
        TextureBarrier.SyncAfter = DX12MapStateToBarrierSync(After);
        TextureBarrier.AccessBefore = DX12MapStateToBarrierAccess(Before);
        TextureBarrier.AccessAfter = DX12MapStateToBarrierAccess(After);
        TextureBarrier.LayoutBefore = DX12MapStateToTextureLayout(Before);
        TextureBarrier.LayoutAfter = DX12MapStateToTextureLayout(After);
        TextureBarrier.pResource = Resource;
        TextureBarrier.Subresources = DX12MakeTextureBarrierRange(Resource, Subresource);
        TextureBarrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

        D3D12_BARRIER_GROUP Group = {};
        Group.Type = D3D12_BARRIER_TYPE_TEXTURE;
        Group.NumBarriers = 1;
        Group.pTextureBarriers = &TextureBarrier;
        CommandListV7->Barrier(1, &Group);
        return;
    }

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    Barrier.Transition.pResource = Resource;
    Barrier.Transition.StateBefore = Before;
    Barrier.Transition.StateAfter = After;
    Barrier.Transition.Subresource = Subresource;

    CommandList->ResourceBarrier(1, &Barrier);
}

void FDX12CommandContext::UavBarrierEx(ID3D12Resource* Resource)
{
    if (!Resource)
    {
        return;
    }

    const bool bUseEnhanced = Device && Device->SupportsEnhancedBarriers();
    ID3D12GraphicsCommandList7* CommandListV7 = bUseEnhanced ? GetCommandList7() : nullptr;

    if (CommandListV7)
    {
        const D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
        if (Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            D3D12_BUFFER_BARRIER BufferBarrier = {};
            BufferBarrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
            BufferBarrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
            BufferBarrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            BufferBarrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            BufferBarrier.pResource = Resource;
            BufferBarrier.Offset = 0;
            BufferBarrier.Size = UINT64_MAX;

            D3D12_BARRIER_GROUP Group = {};
            Group.Type = D3D12_BARRIER_TYPE_BUFFER;
            Group.NumBarriers = 1;
            Group.pBufferBarriers = &BufferBarrier;
            CommandListV7->Barrier(1, &Group);
            return;
        }

        D3D12_TEXTURE_BARRIER TextureBarrier = {};
        TextureBarrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
        TextureBarrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        TextureBarrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        TextureBarrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        TextureBarrier.LayoutBefore = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
        TextureBarrier.LayoutAfter = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
        TextureBarrier.pResource = Resource;
        TextureBarrier.Subresources = DX12MakeTextureBarrierRange(Resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        TextureBarrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

        D3D12_BARRIER_GROUP Group = {};
        Group.Type = D3D12_BARRIER_TYPE_TEXTURE;
        Group.NumBarriers = 1;
        Group.pTextureBarriers = &TextureBarrier;
        CommandListV7->Barrier(1, &Group);
        return;
    }

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    Barrier.UAV.pResource = Resource;
    CommandList->ResourceBarrier(1, &Barrier);
}

void FDX12CommandContext::TransitionResources(const std::vector<D3D12_RESOURCE_BARRIER>& Barriers)
{
    if (Barriers.empty())
    {
        return;
    }

    if (!Device || !Device->SupportsEnhancedBarriers())
    {
        CommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
        return;
    }

    for (const D3D12_RESOURCE_BARRIER& Barrier : Barriers)
    {
        if (Barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
        {
            TransitionResourceEx(
                Barrier.Transition.pResource,
                Barrier.Transition.StateBefore,
                Barrier.Transition.StateAfter,
                Barrier.Transition.Subresource);
        }
        else if (Barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
        {
            UavBarrierEx(Barrier.UAV.pResource);
        }
    }
}

void FDX12CommandContext::SetRenderTarget(const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const D3D12_CPU_DESCRIPTOR_HANDLE* DsvHandle)
{
    CommandList->OMSetRenderTargets(1, &RtvHandle, FALSE, DsvHandle);
}

void FDX12CommandContext::ClearRenderTarget(const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FLOAT Color[4])
{
    CommandList->ClearRenderTargetView(RtvHandle, Color, 0, nullptr);
}

void FDX12CommandContext::ClearDepth(const D3D12_CPU_DESCRIPTOR_HANDLE& DsvHandle, float Depth, uint8 Stencil)
{
    CommandList->ClearDepthStencilView(DsvHandle, D3D12_CLEAR_FLAG_DEPTH, Depth, Stencil, 0, nullptr);
}

void FDX12CommandContext::CloseAndExecute()
{
    HR_CHECK(CommandList->Close());

    ID3D12CommandList* Lists[] = { CommandList.Get() };
    Queue->ExecuteCommandLists(1, Lists);
}

void FDX12CommandContext::SetFrameFenceValue(uint32 FrameIndex, uint64 FenceValue)
{
    if (FrameIndex < FrameFenceValues.size())
    {
        FrameFenceValues[FrameIndex] = FenceValue;
    }
}

uint64 FDX12CommandContext::GetFrameFenceValue(uint32 FrameIndex) const
{
    if (FrameIndex < FrameFenceValues.size())
    {
        return FrameFenceValues[FrameIndex];
    }

    return 0;
}

ID3D12GraphicsCommandList4* FDX12CommandContext::GetCommandList4()
{
    if (!CommandList4 && CommandList)
    {
        if (FAILED(CommandList->QueryInterface(IID_PPV_ARGS(CommandList4.ReleaseAndGetAddressOf()))))
        {
            return nullptr;
        }
    }

    return CommandList4.Get();
}

ID3D12GraphicsCommandList7* FDX12CommandContext::GetCommandList7()
{
    if (!CommandList7 && CommandList)
    {
        if (FAILED(CommandList->QueryInterface(IID_PPV_ARGS(CommandList7.ReleaseAndGetAddressOf()))))
        {
            return nullptr;
        }
    }

    return CommandList7.Get();
}
