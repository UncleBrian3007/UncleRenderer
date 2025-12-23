#pragma once
#include "DX12Commons.h"

enum class EDX12QueueType : uint8
{
    Direct,
    Compute,
    Copy
};

class FDX12CommandQueue
{
public:
    FDX12CommandQueue();
    ~FDX12CommandQueue();

    bool Initialize(ID3D12Device* InDevice, EDX12QueueType Type);

    ID3D12CommandQueue* GetD3DQueue() const { return D3DCommandQueue.Get(); }
    uint64 GetCompletedFenceValue() const { return Fence ? Fence->GetCompletedValue() : 0; }
    uint64 GetLastSignaledFenceValue() const { return CurrentFenceValue > 0 ? CurrentFenceValue - 1 : 0; }

    void ExecuteCommandLists(uint32 NumCommandLists, ID3D12CommandList* const* CommandLists);
    uint64 Signal();
    void Wait(uint64 FenceValue);
    void Flush();

private:
    ComPtr<ID3D12CommandQueue> D3DCommandQueue;
    ComPtr<ID3D12Fence>        Fence;
    HANDLE                      FenceEvent;
    uint64                      CurrentFenceValue;
};
