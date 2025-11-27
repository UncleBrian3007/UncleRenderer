#include "DX12CommandQueue.h"

FDX12CommandQueue::FDX12CommandQueue()
    : FenceEvent(nullptr)
    , CurrentFenceValue(0)
{
}

FDX12CommandQueue::~FDX12CommandQueue()
{
    if (FenceEvent)
    {
        CloseHandle(FenceEvent);
        FenceEvent = nullptr;
    }
}

bool FDX12CommandQueue::Initialize(ID3D12Device* InDevice, EDX12QueueType Type)
{
    D3D12_COMMAND_QUEUE_DESC Desc = {};
    Desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    switch (Type)
    {
    case EDX12QueueType::Direct:  Desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;  break;
    case EDX12QueueType::Compute: Desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE; break;
    case EDX12QueueType::Copy:    Desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;    break;
    }

    HR_CHECK(InDevice->CreateCommandQueue(&Desc, IID_PPV_ARGS(D3DCommandQueue.GetAddressOf())));
    HR_CHECK(InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(Fence.GetAddressOf())));

    CurrentFenceValue = 1;
    FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return FenceEvent != nullptr;
}

uint64 FDX12CommandQueue::Signal()
{
    const uint64 FenceValueToSignal = CurrentFenceValue;
    HR_CHECK(D3DCommandQueue->Signal(Fence.Get(), FenceValueToSignal));
    CurrentFenceValue++;
    return FenceValueToSignal;
}

void FDX12CommandQueue::Wait(uint64 FenceValue)
{
    if (Fence->GetCompletedValue() < FenceValue)
    {
        HR_CHECK(Fence->SetEventOnCompletion(FenceValue, FenceEvent));
        WaitForSingleObject(FenceEvent, INFINITE);
    }
}

void FDX12CommandQueue::Flush()
{
    Wait(Signal());
}
