#pragma once

#include "DX12Commons.h"

class FDX12Device;

class FDX12SwapChain
{
public:
    FDX12SwapChain();
    ~FDX12SwapChain();

    bool Initialize(FDX12Device* InDevice, HWND WindowHandle, uint32 Width, uint32 Height, uint32 BufferCount = 2);
    bool Resize(FDX12Device* InDevice, uint32 Width, uint32 Height);

    IDXGISwapChain4* GetSwapChain() const { return SwapChain.Get(); }
    uint32 GetCurrentBackBufferIndex() const { return SwapChain ? SwapChain->GetCurrentBackBufferIndex() : 0; }
    uint32 GetBackBufferCount() const { return BufferCount; }
    ID3D12Resource* GetBackBuffer(uint32 Index) const { return BackBuffers[Index].Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTV(uint32 Index) const { return RTVHandles[Index]; }
    DXGI_FORMAT GetFormat() const { return BackBufferFormat; }
    D3D12_RESOURCE_STATES GetBackBufferState(uint32 Index) const { return BackBufferStates[Index]; }
    void SetBackBufferState(uint32 Index, D3D12_RESOURCE_STATES State) { BackBufferStates[Index] = State; }

private:
    bool CreateSwapChain(FDX12Device* InDevice, HWND WindowHandle, uint32 Width, uint32 Height);
    bool CreateRTVs(FDX12Device* InDevice);
    void ReleaseBuffers();

private:
    ComPtr<IDXGISwapChain4> SwapChain;
    std::vector<ComPtr<ID3D12Resource>> BackBuffers;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> RTVHandles;
    std::vector<D3D12_RESOURCE_STATES> BackBufferStates;

    ComPtr<ID3D12DescriptorHeap> RTVHeap;
    UINT RTVDescriptorSize;

    DXGI_FORMAT BackBufferFormat;
    uint32 BufferCount;
};

