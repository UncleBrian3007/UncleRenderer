#pragma once

#include "DX12Commons.h"

class FDX12Device;
class FDX12SwapChain;
class FDX12CommandQueue;

class FDX12CommandContext
{
public:
    FDX12CommandContext();
    ~FDX12CommandContext();

    bool Initialize(FDX12Device* InDevice, FDX12CommandQueue* InQueue);

    void BeginFrame();
    void TransitionResource(ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After);
    void SetRenderTarget(const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const D3D12_CPU_DESCRIPTOR_HANDLE* DsvHandle = nullptr);
    void ClearRenderTarget(const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FLOAT Color[4]);
    void ClearDepth(const D3D12_CPU_DESCRIPTOR_HANDLE& DsvHandle, float Depth = 1.0f, uint8 Stencil = 0);

    void CloseAndExecute();

    ID3D12GraphicsCommandList* GetCommandList() const { return CommandList.Get(); }

private:
    FDX12Device*             Device;
    FDX12CommandQueue*       Queue;
    ComPtr<ID3D12CommandAllocator> CommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> CommandList;
};

