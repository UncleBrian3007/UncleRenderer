#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <deque>
#include <wrl.h>
#include <chrono>
#include <unordered_map>
#include "../RHI/DX12Commons.h"
#include "../Core/GpuDebugMarkers.h"
#include "../RHI/DX12CommandContext.h"

class FDX12Device;

struct FRGTextureDesc
{
    uint32 Width = 0;
    uint32 Height = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    uint16 MipLevels = 1;
    uint16 DepthOrArraySize = 1;
    D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
};

inline FRGTextureDesc BuildTextureDescFromResource(ID3D12Resource* Resource)
{
    FRGTextureDesc Desc = {};
    if (Resource == nullptr)
    {
        return Desc;
    }

    const D3D12_RESOURCE_DESC ResourceDesc = Resource->GetDesc();
    Desc.Width = static_cast<uint32_t>(ResourceDesc.Width);
    Desc.Height = ResourceDesc.Height;
    Desc.MipLevels = static_cast<uint16_t>(ResourceDesc.MipLevels);
    Desc.Format = ResourceDesc.Format;
    return Desc;
}

struct FRGBufferDesc
{
    uint64 Size = 0;
    D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
    DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN;
    uint32 NumElements = 0;
    uint32 StructureByteStride = 0;
    D3D12_BUFFER_SRV_FLAGS SrvFlags = D3D12_BUFFER_SRV_FLAG_NONE;
    D3D12_BUFFER_UAV_FLAGS UavFlags = D3D12_BUFFER_UAV_FLAG_NONE;
};

struct FRGTextureHandle
{
    uint32 Id = UINT32_MAX;
    explicit operator bool() const { return Id != UINT32_MAX; }
};

using FRGResourceHandle = FRGTextureHandle;

struct FRGBufferHandle
{
    uint32 Id = UINT32_MAX;
    explicit operator bool() const { return Id != UINT32_MAX; }
};

enum class ERGResourceAccess
{
    Read,
    Write,
};

class FRGPassBuilder;

class FRenderGraph
{
public:
    FRenderGraph();

    struct FGpuPassTimingStats
    {
        std::string Name;
        double AvgMs = 0.0;
        double MinMs = 0.0;
        double MaxMs = 0.0;
        uint32 SampleCount = 0;
    };

    void SetDevice(FDX12Device* InDevice) { Device = InDevice; }

    friend class FRGPassBuilder;

    FRGTextureHandle ImportTexture(
        const std::string& Name,
        ID3D12Resource* Resource,
        D3D12_RESOURCE_STATES* StatePtr,
        const FRGTextureDesc& Desc,
        uint32 ImportedSrvBindlessIndex = UINT32_MAX,
        uint32 ImportedUavBindlessIndex = UINT32_MAX);
    ID3D12Resource* GetTextureResource(const FRGTextureHandle& Handle) const;
    uint32 GetTextureSrvBindlessIndex(const FRGTextureHandle& Handle);
    uint32 GetTextureUavBindlessIndex(const FRGTextureHandle& Handle);
    uint32 GetTextureMipSrvBindlessIndex(const FRGTextureHandle& Handle, uint32 MipIndex);
    uint32 GetTextureMipUavBindlessIndex(const FRGTextureHandle& Handle, uint32 MipIndex);
    FRGBufferHandle ImportBuffer(
        const std::string& Name,
        ID3D12Resource* Resource,
        D3D12_RESOURCE_STATES* StatePtr,
        const FRGBufferDesc& Desc,
        uint32 ImportedSrvBindlessIndex = UINT32_MAX,
        uint32 ImportedUavBindlessIndex = UINT32_MAX);
    ID3D12Resource* GetBufferResource(const FRGBufferHandle& Handle) const;
    uint32 GetBufferSrvBindlessIndex(const FRGBufferHandle& Handle);
    uint32 GetBufferUavBindlessIndex(const FRGBufferHandle& Handle);

    template <typename PassData, typename SetupFunc, typename ExecuteFunc>
    void AddPass(const std::string& Name, SetupFunc&& Setup, ExecuteFunc&& Execute)
    {
        PassEntry Entry;
        Entry.Name = Name;
        Entry.PixEventName.assign(Name.begin(), Name.end());
        Entry.DataStorage.resize(sizeof(PassData));
        new (Entry.DataStorage.data()) PassData();

        PassData& Data = *reinterpret_cast<PassData*>(Entry.DataStorage.data());
        FRGPassBuilder Builder(*this, Entry);
        Setup(Data, Builder);

        Entry.ExecuteFunc = [Execute = std::forward<ExecuteFunc>(Execute)](const std::vector<uint8_t>& Storage, FDX12CommandContext& Cmd)
        {
            const PassData& PassStorage = *reinterpret_cast<const PassData*>(Storage.data());
            Execute(PassStorage, Cmd);
        };

        Passes.push_back(std::move(Entry));
    }

    void Execute(FDX12CommandContext& CmdContext);

    void SetDebugRecording(bool bEnable) { bEnableDebugRecording = bEnable; }
    void SetGraphDumpEnabled(bool bEnable) { bEnableGraphDump = bEnable; }
    void SetResourceLifetimeLogging(bool bEnable) { bEnableResourceLifetimeLog = bEnable; }
    void SetBarrierLoggingEnabled(bool bEnable) { bEnableBarrierLogs = bEnable; }
    void SetGpuTimingEnabled(bool bEnable) { bEnableGpuTiming = bEnable; }

    static void SetGpuTimingWindowSeconds(double Seconds);
    static double GetGpuTimingWindowSeconds();
    static void SetGpuTimingDisplayCount(uint32 Count);
    static uint32 GetGpuTimingDisplayCount();
    static const std::vector<FGpuPassTimingStats>& GetGpuTimingStats();
    static void AddExternalGpuTimingSample(const std::string& Name, double Milliseconds);

private:
    template <typename HandleType>
    struct TRGUsage
    {
        HandleType Handle;
        D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_COMMON;
        ERGResourceAccess Access = ERGResourceAccess::Read;
    };

    using FRGTextureUsage = TRGUsage<FRGTextureHandle>;
    using FRGBufferUsage = TRGUsage<FRGBufferHandle>;

    struct FRGTextureResource
    {
        std::string Name;
        FRGTextureDesc Desc;
        D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* Resource = nullptr;
        D3D12_RESOURCE_STATES* ExternalState = nullptr;
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
        uint32 ReferenceCount = 0;
        uint32 InitialReferenceCount = 0;
        int32 PoolIndex = -1;
        uint32 DefaultSrvBindlessIndex = UINT32_MAX;
        uint32 DefaultUavBindlessIndex = UINT32_MAX;
        ID3D12Resource* DefaultSrvViewResource = nullptr;
        ID3D12Resource* DefaultUavViewResource = nullptr;
        std::vector<uint32> MipSrvBindlessIndices;
        std::vector<uint32> MipUavBindlessIndices;
        std::vector<ID3D12Resource*> MipSrvViewResources;
        std::vector<ID3D12Resource*> MipUavViewResources;
        bool bExternal = false;
    };

    struct FRGBufferResource
    {
        std::string Name;
        FRGBufferDesc Desc;
        ID3D12Resource* Resource = nullptr;
        D3D12_RESOURCE_STATES* ExternalState = nullptr;
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
        uint32 ReferenceCount = 0;
        uint32 InitialReferenceCount = 0;
        int32 PoolIndex = -1;
        uint32 DefaultSrvBindlessIndex = UINT32_MAX;
        uint32 DefaultUavBindlessIndex = UINT32_MAX;
        ID3D12Resource* DefaultSrvViewResource = nullptr;
        ID3D12Resource* DefaultUavViewResource = nullptr;
        bool bExternal = false;
    };

    struct PassEntry
    {
        std::string Name;
        std::wstring PixEventName;
        std::vector<uint8_t> DataStorage;
        std::function<void(const std::vector<uint8_t>&, FDX12CommandContext&)> ExecuteFunc;
        std::vector<FRGTextureUsage> TextureUsages;
        std::vector<FRGBufferUsage> BufferUsages;
        std::vector<FRGTextureHandle> TextureUavBarriers;
        std::vector<FRGBufferHandle> BufferUavBarriers;
        bool bCulled = false;
        std::string PixGroupName;
        bool bForceExecute = false;
        double ElapsedMs = 0.0;
        double GpuElapsedMs = 0.0;
    };

    FRGTextureHandle RegisterTexture(const std::string& Name, const FRGTextureDesc& Desc);
    FRGBufferHandle RegisterBuffer(const std::string& Name, const FRGBufferDesc& Desc);
    void RegisterUsage(PassEntry& Entry, const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access);
    void RegisterUsage(PassEntry& Entry, const FRGBufferHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access);
    void RegisterUavBarrier(PassEntry& Entry, const FRGTextureHandle& Handle);
    void RegisterUavBarrier(PassEntry& Entry, const FRGBufferHandle& Handle);

    void AccumulateTextureFlags(const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access);
    bool AcquireTransientTexture(FRGTextureResource& Texture, D3D12_RESOURCE_STATES InitialState);
    bool AcquireTransientBuffer(FRGBufferResource& Buffer, D3D12_RESOURCE_STATES InitialState);
    void ReleaseTransientTexture(FRGTextureResource& Texture);
    void ReleaseTransientBuffer(FRGBufferResource& Buffer);
    std::string BuildTextureViewOwnerLabel(const FRGTextureResource& Texture, bool bUav, int32 MipIndex) const;
    std::string BuildBufferSrvOwnerLabel(const FRGBufferResource& Buffer) const;
    std::string BuildBufferUavOwnerLabel(const FRGBufferResource& Buffer) const;
    uint32 GetTextureViewBindlessIndex(FRGTextureResource& Texture, bool bUav);
    uint32 GetTextureMipViewBindlessIndex(FRGTextureResource& Texture, bool bUav, uint32 MipIndex);
    uint32 GetBufferSrvBindlessIndex(FRGBufferResource& Buffer);
    uint32 GetBufferUavBindlessIndex(FRGBufferResource& Buffer);
    void DumpDebugInfo(const std::vector<bool>& PassRequired, const std::vector<bool>& ResourceRequired, const std::vector<bool>& BufferRequired);
    void LogTimingSummary();

    FRGTextureResource* ResolveTexture(const FRGTextureHandle& Handle);
    FRGBufferResource* ResolveBuffer(const FRGBufferHandle& Handle);

    FDX12Device* Device = nullptr;

    std::vector<FRGTextureResource> Textures;
    std::vector<FRGBufferResource> Buffers;
    std::vector<PassEntry> Passes;

    struct FPooledTexture
    {
        FRGTextureDesc Desc;
        D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
        bool bInUse = false;
        uint32 FirstUseFrame = UINT32_MAX;
        uint32 LastUseFrame = UINT32_MAX;
        uint64 LastFenceValue = 0;
    };

    static std::vector<FPooledTexture> TexturePool;

    struct FPooledBuffer
    {
        FRGBufferDesc Desc;
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
        bool bInUse = false;
        uint32 FirstUseFrame = UINT32_MAX;
        uint32 LastUseFrame = UINT32_MAX;
        uint64 LastFenceValue = 0;
    };

    static std::vector<FPooledBuffer> BufferPool;

    struct FGpuTimingData
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> ReadbackBuffer;
        uint32 QueryCount = 0;
        uint64 Frequency = 0;
        std::vector<std::string> PassNames;
        std::vector<std::string> PixGroupNames;
        bool bPending = false;
    };

    static std::unordered_map<uint32, FGpuTimingData> PendingGpuTimings;

    void ProcessPendingGpuTimings(const FDX12CommandContext& CmdContext, uint32 FrameIndex);

    struct FGpuTimingSample
    {
        std::chrono::steady_clock::time_point Timestamp;
        double Milliseconds = 0.0;
    };

    static std::unordered_map<std::string, std::deque<FGpuTimingSample>> GpuTimingSamples;
    static std::vector<FGpuPassTimingStats> CachedGpuTimingStats;
    static double GpuTimingWindowSeconds;
    static uint32 GpuTimingDisplayCount;

    static void UpdateCachedGpuTimingStats(const std::chrono::steady_clock::time_point& Now);

    struct FGpuTimingResources
    {
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> QueryHeap;
        Microsoft::WRL::ComPtr<ID3D12Resource> ReadbackBuffer;
        uint32 QueryCapacity = 0;
    };
    static std::unordered_map<uint32, FGpuTimingResources> GpuTimingResources;

    bool bEnableDebugRecording = false;
    bool bEnableGraphDump = false;
    bool bEnableResourceLifetimeLog = false;
    bool bEnableBarrierLogs = false;
    bool bEnableGpuTiming = false;
    uint32 CurrentFrameIndex = 0;
    uint64 CurrentFrameFenceValue = 0;
    std::string CurrentExecutingPassName;
};

class FRGPassBuilder
{
public:
    FRGPassBuilder(FRenderGraph& InGraph, FRenderGraph::PassEntry& InEntry);

    FRGTextureHandle CreateTexture(const std::string& Name, const FRGTextureDesc& Desc);
    FRGBufferHandle CreateBuffer(const std::string& Name, const FRGBufferDesc& Desc);
    FRGTextureHandle ReadTexture(const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    FRGTextureHandle WriteTexture(const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_RENDER_TARGET);
    FRGBufferHandle ReadBuffer(const FRGBufferHandle& Handle, D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    FRGBufferHandle WriteBuffer(const FRGBufferHandle& Handle, D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    FRGTextureHandle UavBarrier(const FRGTextureHandle& Handle);
    FRGBufferHandle UavBarrier(const FRGBufferHandle& Handle);
    void KeepAlive();
    void SetPixGroup(const char* GroupName);

private:
    FRenderGraph* Graph = nullptr;
    FRenderGraph::PassEntry* Entry = nullptr;
};
