#pragma once
#include "DX12Commons.h"
#include "DX12CommandQueue.h"
#include <memory>
#include <atomic>

class FDX12Device
{
public:
    FDX12Device();
    ~FDX12Device();

    bool Initialize();

    D3D_SHADER_MODEL    GetShaderModel() const { return ShaderModel; }

    ID3D12Device*        GetDevice() const { return Device.Get(); }
    FDX12CommandQueue*   GetGraphicsQueue() { return GraphicsQueue.get(); }
    ID3D12DescriptorHeap* GetBindlessDescriptorHeap() const { return BindlessDescriptorHeap.Get(); }
    uint32_t             GetBindlessDescriptorCount() const { return BindlessDescriptorCount; }
    uint32_t             CreateBindlessSrv(ID3D12Resource* Resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc);
    uint32_t             CreateBindlessUav(ID3D12Resource* Resource, ID3D12Resource* Counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc);

    IDXGIFactory6*       GetFactory() const { return Factory.Get(); }
    IDXGIAdapter4*       GetAdapter() const { return Adapter.Get(); }
    bool                 IsTearingSupported() const { return bAllowTearing; }
    bool                 QueryLocalVideoMemory(DXGI_QUERY_VIDEO_MEMORY_INFO& OutInfo) const;
    bool                 IsShaderModelForIndirectDrawSupported() const { return bIndirectDrawSupported; }
    bool                 IsRayTracingSupported() const { return bSupportsRayTracing; }
    bool                 CreateRayTracingDevice(class FRayTracingDevice& OutDevice) const;

private:
    bool CreateFactory();
    bool PickAdapter();
    bool CreateDevice();
    bool QueryRayTracingSupport();
    bool CreateBindlessDescriptorHeap();
    bool CreateCommandQueues();
    bool CheckTearingSupport();
    bool DetermineShaderModel();
    uint32_t AllocateBindlessDescriptorIndex();

private:
    ComPtr<IDXGIFactory6> Factory;
    ComPtr<IDXGIAdapter4> Adapter;
    ComPtr<ID3D12Device>  Device;
    ComPtr<ID3D12DescriptorHeap> BindlessDescriptorHeap;
    std::atomic<uint32_t> BindlessDescriptorNextIndex{ 0 };
    uint32_t BindlessDescriptorCount = 0;
    uint32_t BindlessDescriptorStride = 0;

    std::unique_ptr<FDX12CommandQueue> GraphicsQueue;

    bool bAllowTearing = false;
    bool bIndirectDrawSupported = true;
    bool bSupportsRayTracing = false;
    D3D_SHADER_MODEL ShaderModel = D3D_SHADER_MODEL_6_6;
};
