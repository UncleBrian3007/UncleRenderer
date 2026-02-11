#include "DX12Fence.h"
#include "DX12DeviceRemoved.h"

bool FDX12Fence::Initialize(ID3D12Device* Device, uint64_t InitialValue)
{
    if (Device == nullptr)
    {
        return false;
    }

    Value = InitialValue;
    HR_CHECK_DX(Device, Device->CreateFence(Value, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)), L"ID3D12Device::CreateFence(FDX12Fence)");
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

    constexpr DWORD FenceWaitTimeoutMs = 100;

    while (Fence->GetCompletedValue() < Value)
    {
        if (GDeviceRemoved.load())
        {
            return;
        }

        const HRESULT Hr = Fence->SetEventOnCompletion(Value, EventHandle);
        if (FAILED(Hr))
        {
            ReportDxFailure(nullptr, Hr, L"ID3D12Fence::SetEventOnCompletion(FDX12Fence)");
            return;
        }

        const DWORD WaitResult = WaitForSingleObject(EventHandle, FenceWaitTimeoutMs);
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
            ReportDxFailure(nullptr, HRESULT_FROM_WIN32(ErrorCode), L"WaitForSingleObject(FDX12Fence) WAIT_FAILED");
            return;
        }

        ReportDxFailure(nullptr, E_FAIL, L"WaitForSingleObject(FDX12Fence) unexpected result");
        return;
    }
}
