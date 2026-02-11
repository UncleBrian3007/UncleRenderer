#include "DX12CommandQueue.h"
#include "DX12DeviceRemoved.h"

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

    const wchar_t* QueueName = L"GraphicsQueue";
    switch (Type)
    {
    case EDX12QueueType::Direct:
        Desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        QueueName = L"MainQueue";
        break;
    case EDX12QueueType::Compute:
        Desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        QueueName = L"ComputeQueue";
        break;
    case EDX12QueueType::Copy:
        Desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        QueueName = L"CopyQueue";
        break;
    }

    OwnerDevice = InDevice;

    HR_CHECK_DX(OwnerDevice.Get(), InDevice->CreateCommandQueue(&Desc, IID_PPV_ARGS(D3DCommandQueue.GetAddressOf())), L"ID3D12Device::CreateCommandQueue");
    HR_CHECK_DX(OwnerDevice.Get(), InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(Fence.GetAddressOf())), L"ID3D12Device::CreateFence");
    if (D3DCommandQueue)
    {
        D3DCommandQueue->SetName(QueueName);
    }

    CurrentFenceValue = 1;
    FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    return FenceEvent != nullptr;
}

void FDX12CommandQueue::ExecuteCommandLists(uint32 NumCommandLists, ID3D12CommandList* const* CommandLists)
{
    D3DCommandQueue->ExecuteCommandLists(NumCommandLists, CommandLists);
}

uint64 FDX12CommandQueue::Signal()
{
    const uint64 FenceValueToSignal = CurrentFenceValue;
    HR_CHECK_DX(OwnerDevice.Get(), D3DCommandQueue->Signal(Fence.Get(), FenceValueToSignal), L"ID3D12CommandQueue::Signal");
    CurrentFenceValue++;
    return FenceValueToSignal;
}

void FDX12CommandQueue::Wait(uint64 FenceValue)
{
    constexpr DWORD FenceWaitTimeoutMs = 100;

    while (Fence->GetCompletedValue() < FenceValue)
    {
        if (GDeviceRemoved.load())
        {
            return;
        }

        const HRESULT Hr = Fence->SetEventOnCompletion(FenceValue, FenceEvent);
        if (FAILED(Hr))
        {
            ReportDxFailure(OwnerDevice.Get(), Hr, L"ID3D12Fence::SetEventOnCompletion");
            return;
        }

        const DWORD WaitResult = WaitForSingleObject(FenceEvent, FenceWaitTimeoutMs);
        if (GDeviceRemoved.load())
        {
            return;
        }

        if (WaitResult == WAIT_OBJECT_0)
        {
            continue;
        }

        if (WaitResult == WAIT_TIMEOUT)
        {
            continue;
        }

        if (WaitResult == WAIT_FAILED)
        {
            const DWORD ErrorCode = GetLastError();
            ReportDxFailure(OwnerDevice.Get(), HRESULT_FROM_WIN32(ErrorCode), L"WaitForSingleObject(FenceEvent) WAIT_FAILED");
            return;
        }

        ReportDxFailure(OwnerDevice.Get(), E_FAIL, L"WaitForSingleObject(FenceEvent) unexpected result");
        return;
    }
}

void FDX12CommandQueue::Flush()
{
    if (GDeviceRemoved.load())
    {
        return;
    }

    uint64 FenceValueToWait = CurrentFenceValue;
    HR_CHECK_DX(OwnerDevice.Get(), D3DCommandQueue->Signal(Fence.Get(), FenceValueToWait), L"ID3D12CommandQueue::Signal(Flush)");
    CurrentFenceValue++;

    Wait(FenceValueToWait);
}
