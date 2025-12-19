#include "RenderGraph.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include <d3dx12.h>
#include <algorithm>
#include <sstream>
#include "../Core/Logger.h"

FRenderGraph::FRenderGraph()
{
}

std::vector<FRenderGraph::FPooledTexture> FRenderGraph::TexturePool;

FRGPassBuilder::FRGPassBuilder(FRenderGraph& InGraph, FRenderGraph::PassEntry& InEntry)
    : Graph(&InGraph)
    , Entry(&InEntry)
{
}

FRGResourceHandle FRGPassBuilder::CreateTexture(const std::string& Name, const FRGTextureDesc& Desc)
{
    return Graph->RegisterTexture(Name, Desc);
}

FRGResourceHandle FRGPassBuilder::ReadTexture(const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState)
{
    Graph->RegisterUsage(*Entry, Handle, RequiredState, ERGResourceAccess::Read);
    return Handle;
}

FRGResourceHandle FRGPassBuilder::WriteTexture(const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState)
{
    Graph->RegisterUsage(*Entry, Handle, RequiredState, ERGResourceAccess::Write);
    return Handle;
}

FRGResourceHandle FRenderGraph::ImportTexture(
    const std::string& Name,
    ID3D12Resource* Resource,
    D3D12_RESOURCE_STATES* StatePtr,
    const FRGTextureDesc& Desc)
{
    FRGResourceHandle Handle = RegisterTexture(Name, Desc);
    FRGTextureResource& ResourceEntry = Textures[Handle.Id];
    ResourceEntry.Resource = Resource;
    ResourceEntry.ExternalState = StatePtr;
    ResourceEntry.bExternal = true;
    if (StatePtr)
    {
        ResourceEntry.CurrentState = *StatePtr;
    }

    return Handle;
}

FRGResourceHandle FRenderGraph::RegisterTexture(const std::string& Name, const FRGTextureDesc& Desc)
{
    FRGResourceHandle Handle = { static_cast<uint32>(Textures.size()) };
    FRGTextureResource Resource = {};
    Resource.Name = Name;
    Resource.Desc = Desc;
    Textures.push_back(Resource);
    return Handle;
}

void FRenderGraph::RegisterUsage(PassEntry& Entry, const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access)
{
    if (!Handle)
    {
        return;
    }

    AccumulateResourceFlags(Handle, RequiredState, Access);
    Entry.ResourceUsages.push_back({ Handle, RequiredState, Access });
}

void FRenderGraph::AccumulateResourceFlags(const FRGResourceHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access)
{
    FRGTextureResource* Resource = ResolveResource(Handle);
    if (!Resource || Resource->bExternal)
    {
        return;
    }

    if (Access == ERGResourceAccess::Write)
    {
        if (RequiredState & D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            Resource->Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }

        if (RequiredState & D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            Resource->Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }

        if (RequiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            Resource->Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
    }
}

FRenderGraph::FRGTextureResource* FRenderGraph::ResolveResource(const FRGResourceHandle& Handle)
{
    if (!Handle || Handle.Id >= Textures.size())
    {
        return nullptr;
    }

    return &Textures[Handle.Id];
}

void FRenderGraph::Execute(FDX12CommandContext& CmdContext)
{
    const size_t ResourceCount = Textures.size();

    std::vector<int32_t> FirstUse(ResourceCount, -1);
    std::vector<int32_t> LastUse(ResourceCount, -1);
    std::vector<bool> ResourceRead(ResourceCount, false);

    for (int32_t PassIndex = 0; PassIndex < static_cast<int32_t>(Passes.size()); ++PassIndex)
    {
        const PassEntry& Entry = Passes[PassIndex];
        for (const FRGResourceUsage& Usage : Entry.ResourceUsages)
        {
            if (!Usage.Handle || Usage.Handle.Id >= Textures.size())
            {
                continue;
            }

            if (FirstUse[Usage.Handle.Id] == -1)
            {
                FirstUse[Usage.Handle.Id] = PassIndex;
            }
            LastUse[Usage.Handle.Id] = PassIndex;
            if (Usage.Access == ERGResourceAccess::Read)
            {
                ResourceRead[Usage.Handle.Id] = true;
            }
        }
    }

    for (uint32_t Index = 0; Index < ResourceCount; ++Index)
    {
        Textures[Index].FirstUsePass = FirstUse[Index];
        Textures[Index].LastUsePass = LastUse[Index];
    }

    std::vector<bool> ResourceRequired(ResourceCount, false);
    for (uint32_t Index = 0; Index < ResourceCount; ++Index)
    {
        if (ResourceRead[Index])
        {
            ResourceRequired[Index] = true;
        }
        else if (Textures[Index].ExternalState && FirstUse[Index] != -1)
        {
            ResourceRequired[Index] = true;
        }
    }

    std::vector<bool> PassRequired(Passes.size(), false);
    for (int32_t PassIndex = static_cast<int32_t>(Passes.size()) - 1; PassIndex >= 0; --PassIndex)
    {
        const PassEntry& Entry = Passes[PassIndex];
        bool bTouchesRequiredResource = false;

        for (const FRGResourceUsage& Usage : Entry.ResourceUsages)
        {
            if (!Usage.Handle || Usage.Handle.Id >= Textures.size())
            {
                continue;
            }

            if (ResourceRequired[Usage.Handle.Id])
            {
                bTouchesRequiredResource = true;
                break;
            }
        }

        if (!bTouchesRequiredResource)
        {
            continue;
        }

        PassRequired[PassIndex] = true;

        for (const FRGResourceUsage& Usage : Entry.ResourceUsages)
        {
            if (!Usage.Handle || Usage.Handle.Id >= Textures.size())
            {
                continue;
            }

            ResourceRequired[Usage.Handle.Id] = true;
        }
    }

    if (bEnableGraphDump)
    {
        DumpDebugInfo(PassRequired, ResourceRequired);
    }

    for (int32_t PassIndex = 0; PassIndex < static_cast<int32_t>(Passes.size()); ++PassIndex)
    {
        PassEntry& Entry = Passes[PassIndex];
        Entry.bCulled = !PassRequired[PassIndex];

        if (Entry.bCulled)
        {
            continue;
        }

        std::vector<D3D12_RESOURCE_BARRIER> PendingBarriers;
        PendingBarriers.reserve(Entry.ResourceUsages.size());

        for (const FRGResourceUsage& Usage : Entry.ResourceUsages)
        {
            FRGTextureResource* Resource = ResolveResource(Usage.Handle);
            if (!Resource)
            {
                continue;
            }

            if (!Resource->Resource && !Resource->bExternal)
            {
                AcquireTransientTexture(*Resource, Usage.RequiredState);
            }

            if (!Resource->Resource)
            {
                continue;
            }

            D3D12_RESOURCE_STATES& StateRef = Resource->ExternalState ? *Resource->ExternalState : Resource->CurrentState;
            if (StateRef != Usage.RequiredState)
            {
                D3D12_RESOURCE_BARRIER Barrier = {};
                Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                Barrier.Transition.pResource = Resource->Resource;
                Barrier.Transition.StateBefore = StateRef;
                Barrier.Transition.StateAfter = Usage.RequiredState;
                Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                PendingBarriers.push_back(Barrier);

                StateRef = Usage.RequiredState;
                Resource->CurrentState = Usage.RequiredState;
            }
        }

        CmdContext.TransitionResources(PendingBarriers);

        auto PassBegin = std::chrono::high_resolution_clock::now();
        if (Entry.ExecuteFunc)
        {
            Entry.ExecuteFunc(Entry.DataStorage, CmdContext);
        }
        auto PassEnd = std::chrono::high_resolution_clock::now();

        if (bEnableDebugRecording)
        {
            const std::chrono::duration<double, std::milli> Elapsed = PassEnd - PassBegin;
            Entry.ElapsedMs = Elapsed.count();
        }

        for (const FRGResourceUsage& Usage : Entry.ResourceUsages)
        {
            FRGTextureResource* Resource = ResolveResource(Usage.Handle);
            if (!Resource || Resource->bExternal)
            {
                continue;
            }

            if (Resource->LastUsePass == PassIndex)
            {
                ReleaseTransientTexture(*Resource);
            }
        }
    }

    if (bEnableDebugRecording)
    {
        LogTimingSummary();
    }
}

bool FRenderGraph::AcquireTransientTexture(FRGTextureResource& Texture, D3D12_RESOURCE_STATES InitialState)
{
    if (!Device)
    {
        return false;
    }

    const auto Matches = [&](const FPooledTexture& Candidate)
    {
        return !Candidate.bInUse &&
            Candidate.Desc.Width == Texture.Desc.Width &&
            Candidate.Desc.Height == Texture.Desc.Height &&
            Candidate.Desc.Format == Texture.Desc.Format &&
            Candidate.Flags == Texture.Flags;
    };

    auto Found = std::find_if(TexturePool.begin(), TexturePool.end(), Matches);
    if (Found != TexturePool.end())
    {
        Found->bInUse = true;
        Texture.Resource = Found->Resource.Get();
        Texture.CurrentState = Found->CurrentState;
        Texture.PoolIndex = static_cast<int32>(Found - TexturePool.begin());
        return true;
    }

    D3D12_RESOURCE_DESC ResourceDesc = {};
    ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ResourceDesc.Alignment = 0;
    ResourceDesc.Width = Texture.Desc.Width;
    ResourceDesc.Height = Texture.Desc.Height;
    ResourceDesc.DepthOrArraySize = 1;
    ResourceDesc.MipLevels = 1;
    ResourceDesc.Format = Texture.Desc.Format;
    ResourceDesc.SampleDesc.Count = 1;
    ResourceDesc.SampleDesc.Quality = 0;
    ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ResourceDesc.Flags = Texture.Flags;

    D3D12_CLEAR_VALUE ClearValue = {};
    D3D12_CLEAR_VALUE* ClearPtr = nullptr;
    if (Texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    {
        ClearValue.Format = Texture.Desc.Format;
        ClearValue.Color[0] = 0.0f;
        ClearValue.Color[1] = 0.0f;
        ClearValue.Color[2] = 0.0f;
        ClearValue.Color[3] = 0.0f;
        ClearPtr = &ClearValue;
    }
    else if (Texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    {
        ClearValue.Format = Texture.Desc.Format;
        ClearValue.DepthStencil.Depth = 1.0f;
        ClearValue.DepthStencil.Stencil = 0;
        ClearPtr = &ClearValue;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> NewResource;
    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ResourceDesc,
        InitialState,
        ClearPtr,
        IID_PPV_ARGS(&NewResource));

    if (FAILED(hr))
    {
        return false;
    }

    if (!Texture.Name.empty())
    {
        std::wstring WName(Texture.Name.begin(), Texture.Name.end());
        NewResource->SetName(WName.c_str());
    }

    FPooledTexture Pooled = {};
    Pooled.Desc = Texture.Desc;
    Pooled.Flags = Texture.Flags;
    Pooled.Resource = NewResource;
    Pooled.CurrentState = InitialState;
    Pooled.bInUse = true;

    TexturePool.push_back(Pooled);
    Texture.Resource = NewResource.Get();
    Texture.CurrentState = InitialState;
    Texture.PoolIndex = static_cast<int32>(TexturePool.size() - 1);

    return true;
}

void FRenderGraph::ReleaseTransientTexture(FRGTextureResource& Texture)
{
    if (Texture.PoolIndex < 0 || Texture.PoolIndex >= static_cast<int32>(TexturePool.size()))
    {
        return;
    }

    FPooledTexture& Pooled = TexturePool[Texture.PoolIndex];
    Pooled.CurrentState = Texture.CurrentState;
    Pooled.bInUse = false;
    Texture.Resource = nullptr;
    Texture.PoolIndex = -1;
}

void FRenderGraph::DumpDebugInfo(const std::vector<bool>& PassRequired, const std::vector<bool>& ResourceRequired)
{
    LogInfo("RenderGraph Debug Dump Begin");

    if (bEnableResourceLifetimeLog)
    {
        LogInfo("Resources:");
        for (size_t Index = 0; Index < Textures.size(); ++Index)
        {
            if (!ResourceRequired[Index])
            {
                continue;
            }

            const FRGTextureResource& Resource = Textures[Index];

            std::ostringstream Stream;
            Stream << " - " << Resource.Name
                << " (FirstUse: " << Resource.FirstUsePass
                << ", LastUse: " << Resource.LastUsePass
                << ", External: " << (Resource.bExternal ? "Yes" : "No")
                << ")";
            LogInfo(Stream.str());
        }
    }

    LogInfo("Passes:");
    for (size_t PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
    {
        const PassEntry& Entry = Passes[PassIndex];
        std::ostringstream Stream;
        Stream << " - [" << PassIndex << "] " << Entry.Name
            << (PassRequired[PassIndex] ? "" : " (Culled)");
        LogInfo(Stream.str());

        for (const FRGResourceUsage& Usage : Entry.ResourceUsages)
        {
            const FRGTextureResource* Resource = ResolveResource(Usage.Handle);
            if (!Resource)
            {
                continue;
            }

            std::ostringstream UsageStream;
            UsageStream << "    * " << Resource->Name
                << " Access: " << (Usage.Access == ERGResourceAccess::Read ? "Read" : "Write")
                << " State: 0x" << std::hex << static_cast<uint32_t>(Usage.RequiredState);
            LogInfo(UsageStream.str());
        }
    }

    LogInfo("RenderGraph Debug Dump End");
}

void FRenderGraph::LogTimingSummary()
{
    LogInfo("RenderGraph Timing (ms):");
    for (size_t PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
    {
        const PassEntry& Entry = Passes[PassIndex];
        if (Entry.bCulled)
        {
            continue;
        }

        std::ostringstream Stream;
        Stream << " - [" << PassIndex << "] " << Entry.Name << ": " << Entry.ElapsedMs;
        LogInfo(Stream.str());
    }
}

