#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>
#include "../RHI/DX12Commons.h"

class FDX12CommandContext;

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

class FRGPassBuilder
{
public:
    FRGPassBuilder() = default;

    FRGResourceHandle CreateTexture(const std::string& Name, const FRGTextureDesc& Desc);
    FRGResourceHandle ReadTexture(const FRGResourceHandle& Handle);
    FRGResourceHandle WriteTexture(const FRGResourceHandle& Handle);
};

class FRenderGraph
{
public:
    FRenderGraph();

    template <typename PassData, typename SetupFunc, typename ExecuteFunc>
    void AddPass(const std::string& Name, SetupFunc&& Setup, ExecuteFunc&& Execute)
    {
        PassEntry Entry;
        Entry.Name = Name;
        Entry.DataStorage.resize(sizeof(PassData));
        new (Entry.DataStorage.data()) PassData();

        PassData& Data = *reinterpret_cast<PassData*>(Entry.DataStorage.data());
        FRGPassBuilder Builder;
        Setup(Data, Builder);

        Entry.ExecuteFunc = [Execute = std::forward<ExecuteFunc>(Execute)](const std::vector<uint8_t>& Storage, FDX12CommandContext& Cmd)
        {
            const PassData& PassStorage = *reinterpret_cast<const PassData*>(Storage.data());
            Execute(PassStorage, Cmd);
        };

        Passes.push_back(std::move(Entry));
    }

    void Execute(FDX12CommandContext& CmdContext);

private:
    struct PassEntry
    {
        std::string Name;
        std::vector<uint8_t> DataStorage;
        std::function<void(const std::vector<uint8_t>&, FDX12CommandContext&)> ExecuteFunc;
    };

    std::vector<PassEntry> Passes;
};

