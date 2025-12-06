#include "DX12CommandContext.h"
#include "DX12Device.h"
#include "DX12CommandQueue.h"
#include "../Core/Logger.h"

FDX12CommandContext::FDX12CommandContext()
    : Device(nullptr)
    , Queue(nullptr)
{
}

FDX12CommandContext::~FDX12CommandContext()
{
}

bool FDX12CommandContext::Initialize(FDX12Device* InDevice, FDX12CommandQueue* InQueue)
{
    Device = InDevice;
    Queue = InQueue;

    LogInfo("Command context initialization started");

    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(CommandAllocator.GetAddressOf())));

    HR_CHECK(Device->GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        CommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(CommandList.GetAddressOf())));

    HR_CHECK(CommandList->Close());

    LogInfo("Command context initialization complete");
    return true;
}

void FDX12CommandContext::BeginFrame()
{
    HR_CHECK(CommandAllocator->Reset());
    HR_CHECK(CommandList->Reset(CommandAllocator.Get(), nullptr));
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
    Queue->GetD3DQueue()->ExecuteCommandLists(1, Lists);
}

