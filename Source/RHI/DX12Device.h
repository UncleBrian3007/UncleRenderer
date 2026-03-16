#pragma once
#include "DX12Commons.h"
#include "DX12CommandQueue.h"
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

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
    ID3D12DescriptorHeap* GetBindlessCpuDescriptorHeap() const { return BindlessCpuDescriptorHeap.Get(); }
    uint32_t             GetBindlessDescriptorCount() const { return BindlessDescriptorCount; }
    uint32_t             CreateBindlessSrv(ID3D12Resource* Resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc);
    uint32_t             CreateBindlessUav(ID3D12Resource* Resource, ID3D12Resource* Counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc);
    uint32_t             AllocateTransientBindlessDescriptorIndex();
    void                 RetireTransientBindlessDescriptorIndex(uint32_t Index, uint64_t FenceValue);
    void                 WriteBindlessSrv(uint32_t Index, ID3D12Resource* Resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const;
    void                 WriteBindlessUav(uint32_t Index, ID3D12Resource* Resource, ID3D12Resource* Counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc) const;
    ID3D12DescriptorHeap* GetSamplerDescriptorHeap() const { return SamplerDescriptorHeap.Get(); }
    uint32_t             GetLinearClampSamplerIndex() const { return LinearClampSamplerIndex; }
    uint32_t             GetLinearWrapSamplerIndex() const { return LinearWrapSamplerIndex; }
    uint32_t             GetAnisotropicClampSamplerIndex() const { return AnisotropicClampSamplerIndex; }
    uint32_t             GetAnisotropicWrapSamplerIndex() const { return AnisotropicWrapSamplerIndex; }
    uint32_t             GetPointClampSamplerIndex() const { return PointClampSamplerIndex; }

    IDXGIFactory6*       GetFactory() const { return Factory.Get(); }
    IDXGIAdapter4*       GetAdapter() const { return Adapter.Get(); }
    bool                 IsTearingSupported() const { return bAllowTearing; }
    bool                 QueryLocalVideoMemory(DXGI_QUERY_VIDEO_MEMORY_INFO& OutInfo) const;
    bool                 IsShaderModelForIndirectDrawSupported() const { return bIndirectDrawSupported; }
    bool                 IsRayTracingSupported() const { return bSupportsRayTracing; }
    bool                 SupportsEnhancedBarriers() const { return bSupportsEnhancedBarriers && !bForceLegacyBarriers; }
    bool                 IsEnhancedBarrierFeatureSupported() const { return bSupportsEnhancedBarriers; }
    void                 SetForceLegacyBarriers(bool bEnable) { bForceLegacyBarriers = bEnable; }
    bool                 IsForceLegacyBarriersEnabled() const { return bForceLegacyBarriers; }
    bool                 CreateRayTracingDevice(class FRayTracingDevice& OutDevice) const;

private:
    bool CreateFactory();
    bool PickAdapter();
    bool CreateDevice();
    bool QueryRayTracingSupport();
    bool QueryEnhancedBarrierSupport();
    bool CreateBindlessDescriptorHeap();
    bool CreateSamplerDescriptorHeap();
    bool CreateCommandQueues();
    bool CheckTearingSupport();
    bool DetermineShaderModel();
    uint32_t AllocateBindlessDescriptorIndex();
    void ReclaimTransientBindlessDescriptorIndicesLocked(uint64_t CompletedFenceValue);

    struct FRetiredBindlessDescriptor
    {
        uint32_t Index = UINT32_MAX;
        uint64_t FenceValue = 0;
    };

private:
    ComPtr<IDXGIFactory6> Factory;
    ComPtr<IDXGIAdapter4> Adapter;
    ComPtr<ID3D12Device>  Device;
    ComPtr<ID3D12DescriptorHeap> BindlessDescriptorHeap;
    ComPtr<ID3D12DescriptorHeap> BindlessCpuDescriptorHeap;
    std::atomic<uint32_t> BindlessDescriptorNextIndex{ 0 };
    uint32_t BindlessDescriptorCount = 0;
    uint32_t BindlessDescriptorStride = 0;
    mutable std::mutex TransientBindlessDescriptorMutex;
    std::vector<uint32_t> FreeTransientBindlessDescriptorIndices;
    std::deque<FRetiredBindlessDescriptor> RetiredTransientBindlessDescriptorIndices;
    ComPtr<ID3D12DescriptorHeap> SamplerDescriptorHeap;
    uint32_t LinearClampSamplerIndex = 0;
    uint32_t LinearWrapSamplerIndex = 0;
    uint32_t AnisotropicClampSamplerIndex = 0;
    uint32_t AnisotropicWrapSamplerIndex = 0;
    uint32_t PointClampSamplerIndex = 0;

    std::unique_ptr<FDX12CommandQueue> GraphicsQueue;

    bool bAllowTearing = false;
    bool bIndirectDrawSupported = true;
    bool bSupportsRayTracing = false;
    bool bSupportsEnhancedBarriers = false;
    bool bForceLegacyBarriers = false;
    D3D_SHADER_MODEL ShaderModel = D3D_SHADER_MODEL_6_6;
};
