#pragma once
#include "DX12Commons.h"
#include "DX12CommandQueue.h"
#include <memory>

class FDX12Device
{
public:
    FDX12Device();
    ~FDX12Device();

    bool Initialize();

    ID3D12Device*        GetDevice() const { return Device.Get(); }
    FDX12CommandQueue*   GetGraphicsQueue() { return GraphicsQueue.get(); }

    IDXGIFactory6*       GetFactory() const { return Factory.Get(); }
    IDXGIAdapter4*       GetAdapter() const { return Adapter.Get(); }
    bool                 IsTearingSupported() const { return bAllowTearing; }

private:
    bool CreateFactory();
    bool PickAdapter();
    bool CreateDevice();
    bool CreateCommandQueues();
    bool CheckTearingSupport();

private:
    ComPtr<IDXGIFactory6> Factory;
    ComPtr<IDXGIAdapter4> Adapter;
    ComPtr<ID3D12Device>  Device;

    std::unique_ptr<FDX12CommandQueue> GraphicsQueue;

    bool bAllowTearing = false;
};
