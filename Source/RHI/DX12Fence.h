#pragma once

#include "DX12Commons.h"

class FDX12Fence
{
public:
    FDX12Fence() = default;

    bool Initialize(ID3D12Device* Device, uint64_t InitialValue = 0);
    void Signal(ID3D12CommandQueue* Queue);
    void WaitOnCPU(HANDLE EventHandle);

    uint64_t GetValue() const { return Value; }

private:
    ComPtr<ID3D12Fence> Fence;
    uint64_t Value = 0;
};
