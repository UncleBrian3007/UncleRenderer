#pragma once
#include "DX12Commons.h"
#include "DX12CommandQueue.h"
#include <memory>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef WITH_BINDLESS_DESCRIPTOR_STATS
#define WITH_BINDLESS_DESCRIPTOR_STATS 0
#endif

class FDX12Device
{
public:
    struct FBindlessDescriptorStats
    {
#if WITH_BINDLESS_DESCRIPTOR_STATS
        uint32_t DescriptorCount = 0u;
        uint32_t NextIndex = 0u;
        uint32_t FreeTransientCount = 0u;
        uint32_t RetiredTransientCount = 0u;
        uint32_t ReclaimableTransientCount = 0u;
        uint32_t MinFreeTransientThisFrame = 0u;
        uint32_t PeakTransientLiveThisFrame = 0u;
        uint32_t LiveTransientDescriptorCount = 0u;
        uint64_t PermanentAllocationCount = 0u;
        uint64_t TransientHeapAllocationCount = 0u;
        uint64_t TransientHeapAllocsThisFrame = 0u;
        uint64_t TransientReuseCount = 0u;
        uint64_t TransientRetireCount = 0u;
        uint64_t TransientReclaimCount = 0u;
        uint64_t CompletedFenceValue = 0u;
        uint64_t LastSignaledFenceValue = 0u;
        uint64_t OldestRetiredFenceValue = 0u;
        uint64_t NewestRetiredFenceValue = 0u;
        std::vector<std::string> LiveTransientOwnerSamples;
#endif
    };

    FDX12Device();
    ~FDX12Device();

    bool Initialize();
    static void ReportLiveObjects();

    D3D_SHADER_MODEL    GetShaderModel() const { return ShaderModel; }

    ID3D12Device*        GetDevice() const { return Device.Get(); }
    FDX12CommandQueue*   GetGraphicsQueue() { return GraphicsQueue.get(); }
    ID3D12DescriptorHeap* GetBindlessDescriptorHeap() const { return BindlessDescriptorHeap.Get(); }
    ID3D12DescriptorHeap* GetBindlessCpuDescriptorHeap() const { return BindlessCpuDescriptorHeap.Get(); }
    uint32_t             GetBindlessDescriptorCount() const { return BindlessDescriptorCount; }
    uint32_t             GetBindlessDescriptorStride() const { return BindlessDescriptorStride; }
    uint32_t             GetRtvDescriptorStride() const { return RtvDescriptorStride; }
    FBindlessDescriptorStats GetBindlessDescriptorStats() const;
    uint32_t             CreateBindlessSrv(ID3D12Resource* Resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc);
    uint32_t             CreateBindlessUav(ID3D12Resource* Resource, ID3D12Resource* Counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc);
    uint32_t             AllocateTransientBindlessDescriptorIndex();
    void                 RetireTransientBindlessDescriptorIndex(uint32_t Index, uint64_t FenceValue);
    void                 PumpTransientBindlessDescriptorReclaim();
    void                 ResetBindlessDescriptorFrameStats();
    void                 SetLiveTransientBindlessOwnerTrackingEnabled(bool bEnabled);
    bool                 IsLiveTransientBindlessOwnerTrackingEnabled() const
    {
#if WITH_BINDLESS_DESCRIPTOR_STATS
        return bTrackLiveTransientBindlessOwners;
#else
        return false;
#endif
    }
    void                 TrackTransientBindlessDescriptorOwner(uint32_t Index, const std::string& OwnerLabel);
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
    bool                 SupportsAtomicInt64OnTypedResource() const { return bSupportsAtomicInt64OnTypedResource; }

    bool                 CreateRayTracingDevice(class FRayTracingDevice& OutDevice) const;

private:
    bool CreateFactory();
    bool PickAdapter();
    bool CreateDevice();
    bool QueryRayTracingSupport();
    bool QueryAtomicInt64Support();
    bool CreateBindlessDescriptorHeap();
    bool CreateSamplerDescriptorHeap();
    bool CreateCommandQueues();
    bool CheckTearingSupport();
    bool DetermineShaderModel();
    uint32_t AllocateBindlessDescriptorIndex();
    void ReclaimTransientBindlessDescriptorIndicesLocked(uint64_t CompletedFenceValue);
#if WITH_BINDLESS_DESCRIPTOR_STATS
    void UpdateTransientBindlessFrameStatsLocked();
    void MaybeLogBindlessDescriptorPressure(const char* Reason, uint32_t UsedCount);
    void LogBindlessDescriptorStats(const char* Reason) const;
#endif

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
    uint32_t RtvDescriptorStride = 0;
    mutable std::mutex TransientBindlessDescriptorMutex;
    std::vector<uint32_t> FreeTransientBindlessDescriptorIndices;
    std::deque<FRetiredBindlessDescriptor> RetiredTransientBindlessDescriptorIndices;
#if WITH_BINDLESS_DESCRIPTOR_STATS
    std::unordered_map<uint32_t, std::string> LiveTransientBindlessDescriptorOwners;
    std::atomic<uint64_t> PermanentBindlessDescriptorAllocationCount{ 0 };
    std::atomic<uint64_t> TransientBindlessDescriptorHeapAllocationCount{ 0 };
    std::atomic<uint64_t> TransientBindlessDescriptorReuseCount{ 0 };
    std::atomic<uint64_t> TransientBindlessDescriptorRetireCount{ 0 };
    std::atomic<uint64_t> TransientBindlessDescriptorReclaimCount{ 0 };
    std::atomic<uint32_t> BindlessPressureLogLevel{ 0 };
    uint32_t MinFreeTransientThisFrame = 0u;
    uint32_t PeakTransientLiveThisFrame = 0u;
    uint64_t TransientHeapAllocsThisFrame = 0u;
    bool bTrackLiveTransientBindlessOwners = false;
#endif

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
    bool bSupportsAtomicInt64OnTypedResource = false;
    D3D_SHADER_MODEL ShaderModel = D3D_SHADER_MODEL_6_6;
};
