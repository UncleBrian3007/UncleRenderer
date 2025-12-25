#include "DX12CommandContext.h"
#include "DX12Device.h"
#include "DX12CommandQueue.h"
#include "../Core/Logger.h"

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
}

void FDX12CommandContext::TransitionResource(ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
{
    if (Before == After)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    Barrier.Transition.pResource = Resource;
    Barrier.Transition.StateBefore = Before;
    Barrier.Transition.StateAfter = After;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    CommandList->ResourceBarrier(1, &Barrier);
}

void FDX12CommandContext::TransitionResources(const std::vector<D3D12_RESOURCE_BARRIER>& Barriers)
{
    if (Barriers.empty())
    {
        return;
    }

    CommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
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
