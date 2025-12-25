#pragma once

#include "DX12Commons.h"
#include <vector>

class FDX12Device;
class FDX12SwapChain;
class FDX12CommandQueue;

class FDX12CommandContext
{
public:
    FDX12CommandContext();
    ~FDX12CommandContext();

    bool Initialize(FDX12Device* InDevice, FDX12CommandQueue* InQueue, uint32 InFrameCount);

    void BeginFrame(uint32 FrameIndex);
    void TransitionResource(ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After);
    void TransitionResources(const std::vector<D3D12_RESOURCE_BARRIER>& Barriers);
    void SetRenderTarget(const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const D3D12_CPU_DESCRIPTOR_HANDLE* DsvHandle = nullptr);
    void ClearRenderTarget(const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FLOAT Color[4]);
    void ClearDepth(const D3D12_CPU_DESCRIPTOR_HANDLE& DsvHandle, float Depth = 0.0f, uint8 Stencil = 0);

    void CloseAndExecute();

    void SetFrameFenceValue(uint32 FrameIndex, uint64 FenceValue);
    uint64 GetFrameFenceValue(uint32 FrameIndex) const;

    FDX12CommandQueue* GetQueue() const { return Queue; }
    uint32 GetCurrentFrameIndex() const { return CurrentAllocatorIndex; }

    ID3D12GraphicsCommandList* GetCommandList() const { return CommandList.Get(); }

private:
    FDX12Device*             Device;
    FDX12CommandQueue*       Queue;
    std::vector<ComPtr<ID3D12CommandAllocator>> CommandAllocators;
    std::vector<uint64>               FrameFenceValues;
    uint32                            FrameCount;
    uint32                            CurrentAllocatorIndex;
    ComPtr<ID3D12GraphicsCommandList> CommandList;
};
