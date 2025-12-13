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

    D3D_SHADER_MODEL    GetShaderModel() const { return ShaderModel; }

    ID3D12Device*        GetDevice() const { return Device.Get(); }
    FDX12CommandQueue*   GetGraphicsQueue() { return GraphicsQueue.get(); }

    IDXGIFactory6*       GetFactory() const { return Factory.Get(); }
    IDXGIAdapter4*       GetAdapter() const { return Adapter.Get(); }
    bool                 IsTearingSupported() const { return bAllowTearing; }
    bool                 QueryLocalVideoMemory(DXGI_QUERY_VIDEO_MEMORY_INFO& OutInfo) const;

private:
    bool LoadAgilitySDK();
    bool CreateFactory();
    bool PickAdapter();
    bool CreateDevice();
    bool CreateCommandQueues();
    bool CheckTearingSupport();
    bool DetermineShaderModel();

private:
    ComPtr<IDXGIFactory6> Factory;
    ComPtr<IDXGIAdapter4> Adapter;
    ComPtr<ID3D12Device>  Device;

    std::unique_ptr<FDX12CommandQueue> GraphicsQueue;

    bool bAllowTearing = false;
    D3D_SHADER_MODEL ShaderModel = D3D_SHADER_MODEL_6_0;
};
