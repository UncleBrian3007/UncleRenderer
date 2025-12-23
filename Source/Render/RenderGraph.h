#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include <wrl.h>
#include <chrono>
#include <unordered_map>
#include "../RHI/DX12Commons.h"

class FDX12CommandContext;
class FDX12Device;

struct FRGTextureDesc
{
    uint32 Width = 0;
    uint32 Height = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
};

struct FRGResourceHandle
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

    void SetDevice(FDX12Device* InDevice) { Device = InDevice; }

    friend class FRGPassBuilder;

    FRGResourceHandle ImportTexture(
        const std::string& Name,
        ID3D12Resource* Resource,
        D3D12_RESOURCE_STATES* StatePtr,
        const FRGTextureDesc& Desc);

    template <typename PassData, typename SetupFunc, typename ExecuteFunc>
    void AddPass(const std::string& Name, SetupFunc&& Setup, ExecuteFunc&& Execute)
    {
        PassEntry Entry;
        Entry.Name = Name;
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

private:
    struct FRGResourceUsage
    {
        FRGResourceHandle Handle;
        D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_COMMON;
        ERGResourceAccess Access = ERGResourceAccess::Read;
    };

    struct FRGTextureResource
    {
        std::string Name;
        FRGTextureDesc Desc;
        D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* Resource = nullptr;
        D3D12_RESOURCE_STATES* ExternalState = nullptr;
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
        int32 FirstUsePass = -1;
        int32 LastUsePass = -1;
        int32 PoolIndex = -1;
        bool bExternal = false;
    };

    struct PassEntry
    {
        std::string Name;
        std::vector<uint8_t> DataStorage;
        std::function<void(const std::vector<uint8_t>&, FDX12CommandContext&)> ExecuteFunc;
        std::vector<FRGResourceUsage> ResourceUsages;
        bool bCulled = false;
        double ElapsedMs = 0.0;
        double GpuElapsedMs = 0.0;
    };

    FRGResourceHandle RegisterTexture(const std::string& Name, const FRGTextureDesc& Desc);
    void RegisterUsage(PassEntry& Entry, const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access);

    void AccumulateResourceFlags(const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access);
    bool AcquireTransientTexture(FRGTextureResource& Texture, D3D12_RESOURCE_STATES InitialState);
    void ReleaseTransientTexture(FRGTextureResource& Texture);
    void DumpDebugInfo(const std::vector<bool>& PassRequired, const std::vector<bool>& ResourceRequired);
    void LogTimingSummary();

    FRGTextureResource* ResolveResource(const FRGResourceHandle& Handle);

    FDX12Device* Device = nullptr;

    std::vector<FRGTextureResource> Textures;
    std::vector<PassEntry> Passes;

    struct FPooledTexture
    {
        FRGTextureDesc Desc;
        D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
        bool bInUse = false;
    };

    static std::vector<FPooledTexture> TexturePool;

    struct FGpuTimingData
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> ReadbackBuffer;
        uint32 QueryCount = 0;
        uint64 Frequency = 0;
        std::vector<std::string> PassNames;
        bool bPending = false;
    };

    static std::unordered_map<uint32, FGpuTimingData> PendingGpuTimings;

    void ProcessPendingGpuTimings(uint32 FrameIndex);

    bool bEnableDebugRecording = false;
    bool bEnableGraphDump = false;
    bool bEnableResourceLifetimeLog = false;
    bool bEnableBarrierLogs = false;
    bool bEnableGpuTiming = false;
};

class FRGPassBuilder
{
public:
    FRGPassBuilder(FRenderGraph& InGraph, FRenderGraph::PassEntry& InEntry);

    FRGResourceHandle CreateTexture(const std::string& Name, const FRGTextureDesc& Desc);
    FRGResourceHandle ReadTexture(const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    FRGResourceHandle WriteTexture(const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState = D3D12_RESOURCE_STATE_RENDER_TARGET);

private:
    FRenderGraph* Graph = nullptr;
    FRenderGraph::PassEntry* Entry = nullptr;
};

