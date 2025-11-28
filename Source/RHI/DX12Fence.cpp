#include "DX12Fence.h"

bool FDX12Fence::Initialize(ID3D12Device* Device, uint64_t InitialValue)
{
    if (Device == nullptr)
    {
        return false;
    }

    Value = InitialValue;
    HR_CHECK(Device->CreateFence(Value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
    return true;
}

void FDX12Fence::Signal(ID3D12CommandQueue* Queue)
{
    if (Queue && Fence)
    {
        ++Value;
        Queue->Signal(Fence.Get(), Value);
    }
}

void FDX12Fence::WaitOnCPU(HANDLE EventHandle)
{
    if (!Fence)
    {
        return;
    }

    if (Fence->GetCompletedValue() < Value)
    {
        Fence->SetEventOnCompletion(Value, EventHandle);
        WaitForSingleObject(EventHandle, INFINITE);
    }
}
