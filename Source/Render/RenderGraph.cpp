#include "RenderGraph.h"
#include "GpuResource.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include "RendererUtils.h"
#include <d3dx12.h>
#include <algorithm>
#include <sstream>
#include "../Core/Logger.h"
#include "../Core/GpuDebugMarkers.h"
#include <filesystem>

std::vector<FRenderGraph::FPooledTexture> FRenderGraph::TexturePool;
std::vector<FRenderGraph::FPooledBuffer> FRenderGraph::BufferPool;
std::unordered_map<uint32, FRenderGraph::FGpuTimingData> FRenderGraph::PendingGpuTimings;
std::unordered_map<uint32, FRenderGraph::FGpuTimingResources> FRenderGraph::GpuTimingResources;
std::unordered_map<std::string, std::deque<FRenderGraph::FGpuTimingSample>> FRenderGraph::GpuTimingSamples;
std::vector<FRenderGraph::FGpuPassTimingStats> FRenderGraph::CachedGpuTimingStats;
double FRenderGraph::GpuTimingWindowSeconds = 1.0;
uint32 FRenderGraph::GpuTimingDisplayCount = 3;

FRenderGraph::FRenderGraph()
{
}

void FRenderGraph::SetGpuTimingWindowSeconds(double Seconds)
{
    GpuTimingWindowSeconds = (std::max)(0.1, Seconds);
}

double FRenderGraph::GetGpuTimingWindowSeconds()
{
    return GpuTimingWindowSeconds;
}

void FRenderGraph::SetGpuTimingDisplayCount(uint32 Count)
{
    GpuTimingDisplayCount = (std::max)(1u, Count);
}

uint32 FRenderGraph::GetGpuTimingDisplayCount()
{
    return GpuTimingDisplayCount;
}

const std::vector<FRenderGraph::FGpuPassTimingStats>& FRenderGraph::GetGpuTimingStats()
{
    return CachedGpuTimingStats;
}

void FRenderGraph::AddExternalGpuTimingSample(const std::string& Name, double Milliseconds)
{
    const auto Now = std::chrono::steady_clock::now();
    std::deque<FGpuTimingSample>& Samples = GpuTimingSamples[Name];
    Samples.push_back({ Now, Milliseconds });
    UpdateCachedGpuTimingStats(Now);
}

void FRenderGraph::UpdateCachedGpuTimingStats(const std::chrono::steady_clock::time_point& Now)
{
    const double WindowSeconds = (std::max)(0.1, GpuTimingWindowSeconds);
    const auto Cutoff = Now - std::chrono::duration<double>(WindowSeconds);

    CachedGpuTimingStats.clear();
    CachedGpuTimingStats.reserve(GpuTimingSamples.size());

    for (auto MapIt = GpuTimingSamples.begin(); MapIt != GpuTimingSamples.end();)
    {
        std::deque<FGpuTimingSample>& Samples = MapIt->second;
        while (!Samples.empty() && Samples.front().Timestamp < Cutoff)
        {
            Samples.pop_front();
        }

        if (Samples.empty())
        {
            MapIt = GpuTimingSamples.erase(MapIt);
            continue;
        }

        double Sum = 0.0;
        double MinValue = Samples.front().Milliseconds;
        double MaxValue = Samples.front().Milliseconds;
        for (const FGpuTimingSample& Sample : Samples)
        {
            Sum += Sample.Milliseconds;
            MinValue = (std::min)(MinValue, Sample.Milliseconds);
            MaxValue = (std::max)(MaxValue, Sample.Milliseconds);
        }

        FGpuPassTimingStats Stats;
        Stats.Name = MapIt->first;
        Stats.SampleCount = static_cast<uint32>(Samples.size());
        Stats.AvgMs = Sum / static_cast<double>(Samples.size());
        Stats.MinMs = MinValue;
        Stats.MaxMs = MaxValue;
        CachedGpuTimingStats.push_back(std::move(Stats));

        ++MapIt;
    }

    std::sort(CachedGpuTimingStats.begin(), CachedGpuTimingStats.end(),
        [](const FGpuPassTimingStats& A, const FGpuPassTimingStats& B)
        {
            return A.AvgMs > B.AvgMs;
        });
}

FRGPassBuilder::FRGPassBuilder(FRenderGraph& InGraph, FRenderGraph::PassEntry& InEntry)
    : Graph(&InGraph)
    , Entry(&InEntry)
{
}

FRGTextureHandle FRGPassBuilder::CreateTexture(const std::string& Name, const FRGTextureDesc& Desc)
{
    return Graph->RegisterTexture(Name, Desc);
}

FRGBufferHandle FRGPassBuilder::CreateBuffer(const std::string& Name, const FRGBufferDesc& Desc)
{
    return Graph->RegisterBuffer(Name, Desc);
}

FRGTextureHandle FRGPassBuilder::ReadTexture(const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState)
{
    Graph->RegisterUsage(*Entry, Handle, RequiredState, ERGResourceAccess::Read);
    return Handle;
}

FRGTextureHandle FRGPassBuilder::WriteTexture(const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState)
{
    Graph->RegisterUsage(*Entry, Handle, RequiredState, ERGResourceAccess::Write);
    return Handle;
}

FRGBufferHandle FRGPassBuilder::ReadBuffer(const FRGBufferHandle& Handle, D3D12_RESOURCE_STATES RequiredState)
{
    Graph->RegisterUsage(*Entry, Handle, RequiredState, ERGResourceAccess::Read);
    return Handle;
}

FRGBufferHandle FRGPassBuilder::WriteBuffer(const FRGBufferHandle& Handle, D3D12_RESOURCE_STATES RequiredState)
{
    Graph->RegisterUsage(*Entry, Handle, RequiredState, ERGResourceAccess::Write);
    return Handle;
}

void FRGPassBuilder::KeepAlive()
{
    if (Entry)
    {
        Entry->bForceExecute = true;
    }
}

void FRGPassBuilder::SetPixGroup(const char* GroupName)
{
    if (!Entry)
    {
        return;
    }

    Entry->PixGroupName = GroupName ? GroupName : "";
}

FRGTextureHandle FRGPassBuilder::UavBarrier(const FRGTextureHandle& Handle)
{
    Graph->RegisterUavBarrier(*Entry, Handle);
    return Handle;
}

FRGBufferHandle FRGPassBuilder::UavBarrier(const FRGBufferHandle& Handle)
{
    Graph->RegisterUavBarrier(*Entry, Handle);
    return Handle;
}

FRGTextureHandle FRenderGraph::ImportTexture(
    const std::string& Name,
    ID3D12Resource* Resource,
    D3D12_RESOURCE_STATES* StatePtr,
    const FRGTextureDesc& Desc,
    uint32 ImportedSrvBindlessIndex,
    uint32 ImportedUavBindlessIndex)
{
    FRGTextureHandle Handle = RegisterTexture(Name, Desc);
    FRGTextureResource& ResourceEntry = Textures[Handle.Id];
    ResourceEntry.Resource = Resource;
    ResourceEntry.ExternalState = StatePtr;
    ResourceEntry.bExternal = true;
    ResourceEntry.DefaultSrvBindlessIndex = ImportedSrvBindlessIndex;
    ResourceEntry.DefaultUavBindlessIndex = ImportedUavBindlessIndex;
    ResourceEntry.DefaultSrvViewResource = IsValidBindlessIndex(ImportedSrvBindlessIndex) ? Resource : nullptr;
    ResourceEntry.DefaultUavViewResource = IsValidBindlessIndex(ImportedUavBindlessIndex) ? Resource : nullptr;
    if (StatePtr)
    {
        ResourceEntry.CurrentState = *StatePtr;
    }

    return Handle;
}

FRGBufferHandle FRenderGraph::ImportBuffer(
    const std::string& Name,
    ID3D12Resource* Resource,
    D3D12_RESOURCE_STATES* StatePtr,
    const FRGBufferDesc& Desc,
    uint32 ImportedSrvBindlessIndex,
    uint32 ImportedUavBindlessIndex)
{
    FRGBufferHandle Handle = RegisterBuffer(Name, Desc);
    FRGBufferResource& ResourceEntry = Buffers[Handle.Id];
    ResourceEntry.Resource = Resource;
    ResourceEntry.ExternalState = StatePtr;
    ResourceEntry.bExternal = true;
    ResourceEntry.DefaultSrvBindlessIndex = ImportedSrvBindlessIndex;
    ResourceEntry.DefaultUavBindlessIndex = ImportedUavBindlessIndex;
    ResourceEntry.DefaultSrvViewResource = IsValidBindlessIndex(ImportedSrvBindlessIndex) ? Resource : nullptr;
    ResourceEntry.DefaultUavViewResource = IsValidBindlessIndex(ImportedUavBindlessIndex) ? Resource : nullptr;
    if (StatePtr)
    {
        ResourceEntry.CurrentState = *StatePtr;
    }

    return Handle;
}

ID3D12Resource* FRenderGraph::GetBufferResource(const FRGBufferHandle& Handle) const
{
    if (!Handle || Handle.Id >= Buffers.size())
    {
        return nullptr;
    }

    const FRGBufferResource& Resource = Buffers[Handle.Id];
    return Resource.Resource;
}

FRGTextureHandle FRenderGraph::RegisterTexture(const std::string& Name, const FRGTextureDesc& Desc)
{
    FRGTextureHandle Handle = { static_cast<uint32>(Textures.size()) };
    FRGTextureResource Resource;
    Resource.Name = Name;
    Resource.Desc = Desc;
    Textures.push_back(Resource);
    return Handle;
}

FRGBufferHandle FRenderGraph::RegisterBuffer(const std::string& Name, const FRGBufferDesc& Desc)
{
    FRGBufferHandle Handle = { static_cast<uint32>(Buffers.size()) };
    FRGBufferResource Resource;
    Resource.Name = Name;
    Resource.Desc = Desc;
    Buffers.push_back(Resource);
    return Handle;
}

ID3D12Resource* FRenderGraph::GetTextureResource(const FRGTextureHandle& Handle) const
{
    if (!Handle || Handle.Id >= Textures.size())
    {
        return nullptr;
    }

    const FRGTextureResource& Resource = Textures[Handle.Id];
    return Resource.Resource;
}

uint32 FRenderGraph::GetTextureSrvBindlessIndex(const FRGTextureHandle& Handle)
{
    FRGTextureResource* Resource = ResolveTexture(Handle);
    return Resource ? GetTextureViewBindlessIndex(*Resource, false) : UINT32_MAX;
}

uint32 FRenderGraph::GetTextureUavBindlessIndex(const FRGTextureHandle& Handle)
{
    FRGTextureResource* Resource = ResolveTexture(Handle);
    return Resource ? GetTextureViewBindlessIndex(*Resource, true) : UINT32_MAX;
}

uint32 FRenderGraph::GetTextureMipSrvBindlessIndex(const FRGTextureHandle& Handle, uint32 MipIndex)
{
    FRGTextureResource* Resource = ResolveTexture(Handle);
    return Resource ? GetTextureMipViewBindlessIndex(*Resource, false, MipIndex) : UINT32_MAX;
}

uint32 FRenderGraph::GetTextureMipUavBindlessIndex(const FRGTextureHandle& Handle, uint32 MipIndex)
{
    FRGTextureResource* Resource = ResolveTexture(Handle);
    return Resource ? GetTextureMipViewBindlessIndex(*Resource, true, MipIndex) : UINT32_MAX;
}

uint32 FRenderGraph::GetBufferUavBindlessIndex(const FRGBufferHandle& Handle)
{
    FRGBufferResource* Resource = ResolveBuffer(Handle);
    return Resource ? GetBufferUavBindlessIndex(*Resource) : UINT32_MAX;
}

uint32 FRenderGraph::GetBufferSrvBindlessIndex(const FRGBufferHandle& Handle)
{
    FRGBufferResource* Resource = ResolveBuffer(Handle);
    return Resource ? GetBufferSrvBindlessIndex(*Resource) : UINT32_MAX;
}

void FRenderGraph::RegisterUsage(PassEntry& Entry, const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access)
{
    if (!Handle)
    {
        return;
    }

    AccumulateTextureFlags(Handle, RequiredState, Access);
    Entry.TextureUsages.push_back({ Handle, RequiredState, Access });
}

void FRenderGraph::RegisterUsage(PassEntry& Entry, const FRGBufferHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access)
{
    if (!Handle)
    {
        return;
    }

    Entry.BufferUsages.push_back({ Handle, RequiredState, Access });
}

void FRenderGraph::RegisterUavBarrier(PassEntry& Entry, const FRGTextureHandle& Handle)
{
    if (!Handle)
    {
        return;
    }

    Entry.TextureUavBarriers.push_back(Handle);
}

void FRenderGraph::RegisterUavBarrier(PassEntry& Entry, const FRGBufferHandle& Handle)
{
    if (!Handle)
    {
        return;
    }

    Entry.BufferUavBarriers.push_back(Handle);
}

void FRenderGraph::AccumulateTextureFlags(const FRGTextureHandle& Handle, D3D12_RESOURCE_STATES RequiredState, ERGResourceAccess Access)
{
    FRGTextureResource* Resource = ResolveTexture(Handle);
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

FRenderGraph::FRGTextureResource* FRenderGraph::ResolveTexture(const FRGTextureHandle& Handle)
{
    if (!Handle || Handle.Id >= Textures.size())
    {
        return nullptr;
    }

    return &Textures[Handle.Id];
}

FRenderGraph::FRGBufferResource* FRenderGraph::ResolveBuffer(const FRGBufferHandle& Handle)
{
    if (!Handle || Handle.Id >= Buffers.size())
    {
        return nullptr;
    }

    return &Buffers[Handle.Id];
}

std::string FRenderGraph::BuildTextureViewOwnerLabel(const FRGTextureResource& Texture, bool bUav, int32 MipIndex) const
{
    std::ostringstream Stream;
    Stream << "Pass=" << (CurrentExecutingPassName.empty() ? "<NoPass>" : CurrentExecutingPassName)
        << " Resource=" << (Texture.Name.empty() ? "<UnnamedTexture>" : Texture.Name)
        << " View=";

    if (MipIndex >= 0)
    {
        Stream << (bUav ? "MipUAV" : "MipSRV") << "[" << MipIndex << "]";
    }
    else
    {
        Stream << (bUav ? "UAV" : "SRV");
    }

    return Stream.str();
}

std::string FRenderGraph::BuildBufferUavOwnerLabel(const FRGBufferResource& Buffer) const
{
    std::ostringstream Stream;
    Stream << "Pass=" << (CurrentExecutingPassName.empty() ? "<NoPass>" : CurrentExecutingPassName)
        << " Resource=" << (Buffer.Name.empty() ? "<UnnamedBuffer>" : Buffer.Name)
        << " View=UAV";
    return Stream.str();
}

std::string FRenderGraph::BuildBufferSrvOwnerLabel(const FRGBufferResource& Buffer) const
{
    std::ostringstream Stream;
    Stream << "Pass=" << (CurrentExecutingPassName.empty() ? "<NoPass>" : CurrentExecutingPassName)
        << " Resource=" << (Buffer.Name.empty() ? "<UnnamedBuffer>" : Buffer.Name)
        << " View=SRV";
    return Stream.str();
}

void FRenderGraph::Execute(FDX12CommandContext& CmdContext)
{
    CurrentFrameIndex = CmdContext.GetCurrentFrameIndex();
    const FDX12CommandQueue* Queue = Device->GetGraphicsQueue();
    CurrentFrameFenceValue = Queue ? (Queue->GetLastSignaledFenceValue() + 1u) : 0;

    ProcessPendingGpuTimings(CmdContext, CmdContext.GetCurrentFrameIndex());

    const size_t ResourceCount = Textures.size();
    const size_t BufferCount = Buffers.size();

    std::vector<bool> ResourceUsed(ResourceCount, false);
    std::vector<bool> ResourceRead(ResourceCount, false);
    std::vector<bool> BufferUsed(BufferCount, false);
    std::vector<bool> BufferRead(BufferCount, false);
    std::vector<bool> TexturePendingUavWrite(ResourceCount, false);
    std::vector<bool> BufferPendingUavWrite(BufferCount, false);
    std::vector<std::vector<uint32_t>> PassTextureReferences(Passes.size());
    std::vector<std::vector<uint32_t>> PassBufferReferences(Passes.size());

    for (int32_t PassIndex = 0; PassIndex < static_cast<int32_t>(Passes.size()); ++PassIndex)
    {
        const PassEntry& Entry = Passes[PassIndex];
        std::vector<uint32_t>& UniqueTextureReferences = PassTextureReferences[PassIndex];
        std::vector<uint32_t>& UniqueBufferReferences = PassBufferReferences[PassIndex];
        for (const FRGTextureUsage& Usage : Entry.TextureUsages)
        {
            if (!Usage.Handle || Usage.Handle.Id >= Textures.size())
            {
                continue;
            }

            ResourceUsed[Usage.Handle.Id] = true;
            if (Usage.Access == ERGResourceAccess::Read)
            {
                ResourceRead[Usage.Handle.Id] = true;
            }

            FRGTextureResource& Resource = Textures[Usage.Handle.Id];
            if (!Resource.bExternal &&
                std::find(UniqueTextureReferences.begin(), UniqueTextureReferences.end(), Usage.Handle.Id) == UniqueTextureReferences.end())
            {
                UniqueTextureReferences.push_back(Usage.Handle.Id);
            }
        }

        for (const FRGBufferUsage& Usage : Entry.BufferUsages)
        {
            if (Usage.Handle.Id >= Buffers.size())
            {
                continue;
            }

            BufferUsed[Usage.Handle.Id] = true;
            if (Usage.Access == ERGResourceAccess::Read)
            {
                BufferRead[Usage.Handle.Id] = true;
            }

            FRGBufferResource& Resource = Buffers[Usage.Handle.Id];
            if (!Resource.bExternal &&
                std::find(UniqueBufferReferences.begin(), UniqueBufferReferences.end(), Usage.Handle.Id) == UniqueBufferReferences.end())
            {
                UniqueBufferReferences.push_back(Usage.Handle.Id);
            }
        }
    }

    std::vector<bool> ResourceRequired(ResourceCount, false);
    for (uint32_t Index = 0; Index < ResourceCount; ++Index)
    {
        if (ResourceRead[Index])
        {
            ResourceRequired[Index] = true;
        }
        else if (Textures[Index].ExternalState && ResourceUsed[Index])
        {
            ResourceRequired[Index] = true;
        }
    }
    std::vector<bool> BufferRequired(BufferCount, false);
    for (uint32_t Index = 0; Index < BufferCount; ++Index)
    {
        if (BufferRead[Index])
        {
            BufferRequired[Index] = true;
        }
        else if (Buffers[Index].ExternalState && BufferUsed[Index])
        {
            BufferRequired[Index] = true;
        }
    }

    std::vector<bool> PassRequired(Passes.size(), false);
    for (int32_t PassIndex = static_cast<int32_t>(Passes.size()) - 1; PassIndex >= 0; --PassIndex)
    {
        const PassEntry& Entry = Passes[PassIndex];
        bool bTouchesRequiredResource = false;

        for (const FRGTextureUsage& Usage : Entry.TextureUsages)
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
            for (const FRGBufferUsage& Usage : Entry.BufferUsages)
            {
                if (Usage.Handle.Id >= Buffers.size())
                {
                    continue;
                }

                if (BufferRequired[Usage.Handle.Id])
                {
                    bTouchesRequiredResource = true;
                    break;
                }
            }
        }

        if (!bTouchesRequiredResource && !Entry.bForceExecute)
        {
            continue;
        }

        PassRequired[PassIndex] = true;

        for (const FRGTextureUsage& Usage : Entry.TextureUsages)
        {
            if (!Usage.Handle || Usage.Handle.Id >= Textures.size())
            {
                continue;
            }

            ResourceRequired[Usage.Handle.Id] = true;
        }

        for (const FRGBufferUsage& Usage : Entry.BufferUsages)
        {
            if (Usage.Handle.Id >= Buffers.size())
            {
                continue;
            }

            BufferRequired[Usage.Handle.Id] = true;
        }
    }

    for (FRGTextureResource& Resource : Textures)
    {
        Resource.ReferenceCount = 0u;
        Resource.InitialReferenceCount = 0u;
    }
    for (FRGBufferResource& Resource : Buffers)
    {
        Resource.ReferenceCount = 0u;
        Resource.InitialReferenceCount = 0u;
    }

    for (size_t PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
    {
        for (uint32_t TextureIndex : PassTextureReferences[PassIndex])
        {
            if (TextureIndex < Textures.size())
            {
                Textures[TextureIndex].ReferenceCount++;
            }
        }

        for (uint32_t BufferIndex : PassBufferReferences[PassIndex])
        {
            if (BufferIndex < Buffers.size())
            {
                Buffers[BufferIndex].ReferenceCount++;
            }
        }
    }

    for (size_t PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
    {
        if (PassRequired[PassIndex])
        {
            continue;
        }

        for (uint32_t TextureIndex : PassTextureReferences[PassIndex])
        {
            if (TextureIndex < Textures.size() && Textures[TextureIndex].ReferenceCount > 0u)
            {
                Textures[TextureIndex].ReferenceCount--;
            }
        }

        for (uint32_t BufferIndex : PassBufferReferences[PassIndex])
        {
            if (BufferIndex < Buffers.size() && Buffers[BufferIndex].ReferenceCount > 0u)
            {
                Buffers[BufferIndex].ReferenceCount--;
            }
        }
    }

    for (FRGTextureResource& Resource : Textures)
    {
        Resource.InitialReferenceCount = Resource.ReferenceCount;
    }
    for (FRGBufferResource& Resource : Buffers)
    {
        Resource.InitialReferenceCount = Resource.ReferenceCount;
    }

    if (bEnableGraphDump)
    {
        DumpDebugInfo(PassRequired, ResourceRequired, BufferRequired);
    }

    uint32 ActivePassCount = 0;
    for (int32_t PassIndex = 0; PassIndex < static_cast<int32_t>(Passes.size()); ++PassIndex)
    {
        if (PassRequired[PassIndex])
        {
            ActivePassCount++;
        }
    }

    const bool bDoGpuTiming = bEnableGpuTiming && ActivePassCount > 0;
    std::vector<std::string> GpuTimedPassNames;
    std::vector<std::string> GpuTimedPixGroupNames;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> QueryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> QueryReadback;
    uint64 TimestampFrequency = 0;
    uint32 QueryIndex = 0;

    if (bDoGpuTiming)
    {
        const uint32 FrameIndex = CmdContext.GetCurrentFrameIndex();
        ID3D12CommandQueue* Queue = CmdContext.GetQueue() ? CmdContext.GetQueue()->GetD3DQueue() : nullptr;
        ID3D12Device* D3DDevice = Device->GetDevice();

        if (Queue && D3DDevice)
        {
            Queue->GetTimestampFrequency(&TimestampFrequency);

            FGpuTimingResources& Resources = GpuTimingResources[FrameIndex];
            const uint32 NeededQueryCount = ActivePassCount * 2;

            if (!Resources.QueryHeap || !Resources.ReadbackBuffer || Resources.QueryCapacity < NeededQueryCount)
            {
                D3D12_QUERY_HEAP_DESC HeapDesc = {};
                HeapDesc.Count = NeededQueryCount;
                HeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                HeapDesc.NodeMask = 0;

                if (SUCCEEDED(D3DDevice->CreateQueryHeap(&HeapDesc, IID_PPV_ARGS(Resources.QueryHeap.ReleaseAndGetAddressOf()))))
                {
                    const UINT64 ReadbackSize = static_cast<UINT64>(HeapDesc.Count) * sizeof(uint64);

                    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_READBACK);
                    CD3DX12_RESOURCE_DESC BufferDesc = CD3DX12_RESOURCE_DESC::Buffer(ReadbackSize);

                    if (FAILED(D3DDevice->CreateCommittedResource(
                        &HeapProps,
                        D3D12_HEAP_FLAG_NONE,
                        &BufferDesc,
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr,
                        IID_PPV_ARGS(Resources.ReadbackBuffer.ReleaseAndGetAddressOf()))))
                    {
                        Resources.QueryHeap.Reset();
                        Resources.ReadbackBuffer.Reset();
                        Resources.QueryCapacity = 0;
                    }
                    else
                    {
                        Resources.QueryCapacity = HeapDesc.Count;
                    }
                }
                else
                {
                    Resources.QueryHeap.Reset();
                    Resources.ReadbackBuffer.Reset();
                    Resources.QueryCapacity = 0;
                }
            }

            QueryHeap = Resources.QueryHeap;
            QueryReadback = Resources.ReadbackBuffer;
        }

        if (!QueryHeap || !QueryReadback || TimestampFrequency == 0)
        {
            LogWarning("GPU timing disabled for this frame due to initialization failure");
        }
    }

    std::string ActivePixGroup;
    bool bPixGroupOpen = false;

    for (int32_t PassIndex = 0; PassIndex < static_cast<int32_t>(Passes.size()); ++PassIndex)
    {
        PassEntry& Entry = Passes[PassIndex];
        Entry.bCulled = !PassRequired[PassIndex];

        if (Entry.bCulled)
        {
            continue;
        }

        if (GPixEventsEnabled)
        {
            if (bPixGroupOpen && Entry.PixGroupName != ActivePixGroup)
            {
                PIXEndEvent(CmdContext.GetCommandList());
                bPixGroupOpen = false;
                ActivePixGroup.clear();
            }

            if (!Entry.PixGroupName.empty() && (!bPixGroupOpen || Entry.PixGroupName != ActivePixGroup))
            {
                std::wstring GroupNameWide(Entry.PixGroupName.begin(), Entry.PixGroupName.end());
                PIXBeginEvent(CmdContext.GetCommandList(), PIX_COLOR_DEFAULT, GroupNameWide.c_str());
                ActivePixGroup = Entry.PixGroupName;
                bPixGroupOpen = true;
            }
        }

        if (QueryHeap && QueryReadback)
        {
            CmdContext.GetCommandList()->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex++);
            GpuTimedPassNames.push_back(Entry.Name);
            GpuTimedPixGroupNames.push_back(Entry.PixGroupName);
        }

        std::chrono::high_resolution_clock::time_point PassBegin, PassEnd;
        if (bEnableDebugRecording)
        {
            PassBegin = std::chrono::high_resolution_clock::now();
        }

        CurrentExecutingPassName = Entry.Name;
        {
            FScopedPixEvent PassEvent(
                CmdContext.GetCommandList(),
                Entry.PixEventName.empty() ? L"" : Entry.PixEventName.c_str(),
                !Entry.PixEventName.empty());

            std::vector<D3D12_RESOURCE_BARRIER> PendingBarriers;
            PendingBarriers.reserve(Entry.TextureUsages.size() + Entry.BufferUsages.size() + Entry.TextureUavBarriers.size() + Entry.BufferUavBarriers.size());
            std::vector<bool> TextureUavBarrierQueued(ResourceCount, false);
            std::vector<bool> BufferUavBarrierQueued(BufferCount, false);

            const auto QueueTextureUavBarrier = [&](uint32 TextureIndex)
            {
                if (TextureIndex >= Textures.size() || TextureUavBarrierQueued[TextureIndex])
                {
                    return;
                }

                FRGTextureResource& Resource = Textures[TextureIndex];
                if (!Resource.Resource)
                {
                    return;
                }

                PendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(Resource.Resource));
                TextureUavBarrierQueued[TextureIndex] = true;
                TexturePendingUavWrite[TextureIndex] = false;

                if (bEnableBarrierLogs)
                {
                    std::ostringstream Stream;
                    Stream << "[RG] Pass '" << Entry.Name << "' inserting UAV barrier for '"
                        << (Resource.Name.empty() ? "<Unnamed>" : Resource.Name) << "'";
                    LogInfo(Stream.str());
                }
            };

            const auto QueueBufferUavBarrier = [&](uint32 BufferIndex)
            {
                if (BufferIndex >= Buffers.size() || BufferUavBarrierQueued[BufferIndex])
                {
                    return;
                }

                FRGBufferResource& Resource = Buffers[BufferIndex];
                if (!Resource.Resource)
                {
                    return;
                }

                PendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(Resource.Resource));
                BufferUavBarrierQueued[BufferIndex] = true;
                BufferPendingUavWrite[BufferIndex] = false;

                if (bEnableBarrierLogs)
                {
                    std::ostringstream Stream;
                    Stream << "[RG] Pass '" << Entry.Name << "' inserting UAV barrier for buffer '"
                        << (Resource.Name.empty() ? "<Unnamed>" : Resource.Name) << "'";
                    LogInfo(Stream.str());
                }
            };

            for (const FRGTextureUsage& Usage : Entry.TextureUsages)
            {
                FRGTextureResource* Resource = ResolveTexture(Usage.Handle);
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

                if ((Usage.RequiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0 && TexturePendingUavWrite[Usage.Handle.Id])
                {
                    QueueTextureUavBarrier(Usage.Handle.Id);
                }

                D3D12_RESOURCE_STATES& StateRef = Resource->ExternalState ? *Resource->ExternalState : Resource->CurrentState;
                if (StateRef != Usage.RequiredState)
                {
                    PendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource->Resource, StateRef, Usage.RequiredState));

                    if (bEnableBarrierLogs)
                    {
                        std::ostringstream Stream;
                        Stream << "[RG] Pass '" << Entry.Name << "' transitioning '"
                            << (Resource->Name.empty() ? "<Unnamed>" : Resource->Name) << "': "
                            << RendererUtils::ResourceStateToString(StateRef) << " -> "
                            << RendererUtils::ResourceStateToString(Usage.RequiredState);
                        LogInfo(Stream.str());
                    }

                    StateRef = Usage.RequiredState;
                    Resource->CurrentState = Usage.RequiredState;
                }
            }

            for (const FRGBufferUsage& Usage : Entry.BufferUsages)
            {
                if (Usage.Handle.Id >= Buffers.size())
                {
                    continue;
                }

                FRGBufferResource& Resource = Buffers[Usage.Handle.Id];
                if (!Resource.Resource && !Resource.bExternal)
                {
                    AcquireTransientBuffer(Resource, Usage.RequiredState);
                }

                if (!Resource.Resource)
                {
                    continue;
                }

                if ((Usage.RequiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0 && BufferPendingUavWrite[Usage.Handle.Id])
                {
                    QueueBufferUavBarrier(Usage.Handle.Id);
                }

                D3D12_RESOURCE_STATES& StateRef = Resource.ExternalState ? *Resource.ExternalState : Resource.CurrentState;
                if (StateRef != Usage.RequiredState)
                {
                    PendingBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource.Resource, StateRef, Usage.RequiredState));

                    if (bEnableBarrierLogs)
                    {
                        std::ostringstream Stream;
                        Stream << "[RG] Pass '" << Entry.Name << "' transitioning buffer '"
                            << (Resource.Name.empty() ? "<Unnamed>" : Resource.Name) << "': "
                            << RendererUtils::ResourceStateToString(StateRef) << " -> "
                            << RendererUtils::ResourceStateToString(Usage.RequiredState);
                        LogInfo(Stream.str());
                    }

                    StateRef = Usage.RequiredState;
                    Resource.CurrentState = Usage.RequiredState;
                }
            }

            for (const FRGTextureHandle& Handle : Entry.TextureUavBarriers)
            {
                if (Handle && Handle.Id < Textures.size())
                {
                    QueueTextureUavBarrier(Handle.Id);
                }
            }

            for (const FRGBufferHandle& Handle : Entry.BufferUavBarriers)
            {
                if (Handle.Id < Buffers.size())
                {
                    QueueBufferUavBarrier(Handle.Id);
                }
            }

            CmdContext.TransitionResources(PendingBarriers);

            if (Entry.ExecuteFunc)
            {
                Entry.ExecuteFunc(Entry.DataStorage, CmdContext);
            }
        }
        CurrentExecutingPassName.clear();

        for (const FRGTextureUsage& Usage : Entry.TextureUsages)
        {
            if (!Usage.Handle || Usage.Handle.Id >= Textures.size())
            {
                continue;
            }

            if (Usage.Access == ERGResourceAccess::Write && (Usage.RequiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0)
            {
                TexturePendingUavWrite[Usage.Handle.Id] = true;
            }
            else if ((Usage.RequiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0)
            {
                TexturePendingUavWrite[Usage.Handle.Id] = false;
            }
        }

        for (const FRGBufferUsage& Usage : Entry.BufferUsages)
        {
            if (Usage.Handle.Id >= Buffers.size())
            {
                continue;
            }

            if (Usage.Access == ERGResourceAccess::Write && (Usage.RequiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0)
            {
                BufferPendingUavWrite[Usage.Handle.Id] = true;
            }
            else if ((Usage.RequiredState & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == 0)
            {
                BufferPendingUavWrite[Usage.Handle.Id] = false;
            }
        }

        if (bEnableDebugRecording)
        {
            PassEnd = std::chrono::high_resolution_clock::now();
            const std::chrono::duration<double, std::milli> Elapsed = PassEnd - PassBegin;
            Entry.ElapsedMs = Elapsed.count();
        }

        if (QueryHeap && QueryReadback)
        {
            CmdContext.GetCommandList()->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex++);
        }

        for (uint32_t TextureIndex : PassTextureReferences[PassIndex])
        {
            if (TextureIndex >= Textures.size())
            {
                continue;
            }

            FRGTextureResource& Resource = Textures[TextureIndex];
            if (Resource.ReferenceCount == 0u)
            {
                continue;
            }

            Resource.ReferenceCount--;
            if (Resource.ReferenceCount == 0u)
            {
                ReleaseTransientTexture(Resource);
            }
        }

        for (uint32_t BufferIndex : PassBufferReferences[PassIndex])
        {
            if (BufferIndex >= Buffers.size())
            {
                continue;
            }

            FRGBufferResource& Resource = Buffers[BufferIndex];
            if (Resource.ReferenceCount == 0u)
            {
                continue;
            }

            Resource.ReferenceCount--;
            if (Resource.ReferenceCount == 0u)
            {
                ReleaseTransientBuffer(Resource);
            }
        }
    }

    if (bPixGroupOpen && GPixEventsEnabled)
    {
        PIXEndEvent(CmdContext.GetCommandList());
    }

    if (QueryHeap && QueryReadback && QueryIndex > 0)
    {
        CmdContext.GetCommandList()->ResolveQueryData(
            QueryHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            0,
            QueryIndex,
            QueryReadback.Get(),
            0);

        FGpuTimingData& Pending = PendingGpuTimings[CmdContext.GetCurrentFrameIndex()];
        Pending.ReadbackBuffer = QueryReadback;
        Pending.QueryCount = QueryIndex;
        Pending.Frequency = TimestampFrequency;
        Pending.PassNames = std::move(GpuTimedPassNames);
        Pending.PixGroupNames = std::move(GpuTimedPixGroupNames);
        Pending.bPending = true;
    }

    if (bEnableDebugRecording)
    {
        LogTimingSummary();
    }
}

bool FRenderGraph::AcquireTransientTexture(FRGTextureResource& Texture, D3D12_RESOURCE_STATES InitialState)
{
    const FDX12CommandQueue* Queue = Device->GetGraphicsQueue();
    const uint64 CompletedFenceValue = Queue ? Queue->GetCompletedFenceValue() : 0;

    const auto Matches = [&](const FPooledTexture& Candidate)
    {
        const bool bFenceComplete = Candidate.LastFenceValue == 0 || CompletedFenceValue >= Candidate.LastFenceValue;
        return !Candidate.bInUse &&
            bFenceComplete &&
            Candidate.Desc.Width == Texture.Desc.Width &&
            Candidate.Desc.Height == Texture.Desc.Height &&
            Candidate.Desc.Format == Texture.Desc.Format &&
            Candidate.Desc.MipLevels == Texture.Desc.MipLevels &&
            Candidate.Flags == Texture.Flags;
    };

    auto Found = std::find_if(TexturePool.begin(), TexturePool.end(), Matches);
    if (Found != TexturePool.end())
    {
        Found->bInUse = true;
        Found->FirstUseFrame = CurrentFrameIndex;
        Texture.Resource = Found->Resource.Get();
        Texture.CurrentState = Found->CurrentState;
        Texture.PoolIndex = static_cast<int32>(Found - TexturePool.begin());
        return true;
    }

    const D3D12_RESOURCE_DESC ResourceDesc = CreateTextureResourceDesc(Texture.Desc, Texture.Flags);

    D3D12_CLEAR_VALUE ClearValue = {};
    D3D12_CLEAR_VALUE* ClearPtr = nullptr;
    if (Texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    {
        ClearValue.Format = Texture.Desc.Format;
        ClearPtr = &ClearValue;
    }
    else if (Texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    {
        ClearValue.Format = Texture.Desc.Format;
        ClearValue.DepthStencil = { 1.0f, 0 };
        ClearPtr = &ClearValue;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> NewResource;
    const D3D12_HEAP_PROPERTIES HeapProps = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
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

    FPooledTexture Pooled;
    Pooled.Desc = Texture.Desc;
    Pooled.Flags = Texture.Flags;
    Pooled.Resource = NewResource;
    Pooled.CurrentState = InitialState;
    Pooled.bInUse = true;
    Pooled.FirstUseFrame = CurrentFrameIndex;

    TexturePool.push_back(Pooled);
    Texture.Resource = NewResource.Get();
    Texture.CurrentState = InitialState;
    Texture.PoolIndex = static_cast<int32>(TexturePool.size() - 1);

    return true;
}

bool FRenderGraph::AcquireTransientBuffer(FRGBufferResource& Buffer, D3D12_RESOURCE_STATES InitialState)
{
    const FDX12CommandQueue* Queue = Device->GetGraphicsQueue();
    const uint64 CompletedFenceValue = Queue ? Queue->GetCompletedFenceValue() : 0;

    const auto Matches = [&](const FPooledBuffer& Candidate)
    {
        const bool bFenceComplete = Candidate.LastFenceValue == 0 || CompletedFenceValue >= Candidate.LastFenceValue;
        return !Candidate.bInUse &&
            bFenceComplete &&
            Candidate.Desc.Size == Buffer.Desc.Size &&
            Candidate.Desc.Flags == Buffer.Desc.Flags &&
            Candidate.Desc.ViewFormat == Buffer.Desc.ViewFormat &&
            Candidate.Desc.NumElements == Buffer.Desc.NumElements &&
            Candidate.Desc.StructureByteStride == Buffer.Desc.StructureByteStride &&
            Candidate.Desc.SrvFlags == Buffer.Desc.SrvFlags &&
            Candidate.Desc.UavFlags == Buffer.Desc.UavFlags;
    };

    auto Found = std::find_if(BufferPool.begin(), BufferPool.end(), Matches);
    if (Found != BufferPool.end())
    {
        Found->bInUse = true;
        Found->FirstUseFrame = CurrentFrameIndex;
        Buffer.Resource = Found->Resource.Get();
        Buffer.CurrentState = Found->CurrentState;
        Buffer.PoolIndex = static_cast<int32>(Found - BufferPool.begin());
        return true;
    }

    const D3D12_RESOURCE_DESC ResourceDesc = CreateBufferResourceDesc(Buffer.Desc.Size, Buffer.Desc.Flags);

    Microsoft::WRL::ComPtr<ID3D12Resource> NewResource;
    const D3D12_HEAP_PROPERTIES HeapProps = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const HRESULT Hr = Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ResourceDesc,
        InitialState,
        nullptr,
        IID_PPV_ARGS(&NewResource));

    if (FAILED(Hr))
    {
        return false;
    }

    if (!Buffer.Name.empty())
    {
        std::wstring WName(Buffer.Name.begin(), Buffer.Name.end());
        NewResource->SetName(WName.c_str());
    }

    FPooledBuffer Pooled;
    Pooled.Desc = Buffer.Desc;
    Pooled.Resource = NewResource;
    Pooled.CurrentState = InitialState;
    Pooled.bInUse = true;
    Pooled.FirstUseFrame = CurrentFrameIndex;

    BufferPool.push_back(Pooled);
    Buffer.Resource = NewResource.Get();
    Buffer.CurrentState = InitialState;
    Buffer.PoolIndex = static_cast<int32>(BufferPool.size() - 1);
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
    Pooled.LastUseFrame = CurrentFrameIndex;
    Pooled.LastFenceValue = CurrentFrameFenceValue;
    if (Device)
    {
        Device->RetireTransientBindlessDescriptorIndex(Texture.DefaultSrvBindlessIndex, CurrentFrameFenceValue);
        Device->RetireTransientBindlessDescriptorIndex(Texture.DefaultUavBindlessIndex, CurrentFrameFenceValue);
        for (uint32 Index : Texture.MipSrvBindlessIndices)
        {
            Device->RetireTransientBindlessDescriptorIndex(Index, CurrentFrameFenceValue);
        }
        for (uint32 Index : Texture.MipUavBindlessIndices)
        {
            Device->RetireTransientBindlessDescriptorIndex(Index, CurrentFrameFenceValue);
        }
    }
    Texture.DefaultSrvBindlessIndex = UINT32_MAX;
    Texture.DefaultUavBindlessIndex = UINT32_MAX;
    Texture.DefaultSrvViewResource = nullptr;
    Texture.DefaultUavViewResource = nullptr;
    Texture.MipSrvBindlessIndices.clear();
    Texture.MipUavBindlessIndices.clear();
    Texture.MipSrvViewResources.clear();
    Texture.MipUavViewResources.clear();
    Texture.Resource = nullptr;
    Texture.PoolIndex = -1;
}

void FRenderGraph::ReleaseTransientBuffer(FRGBufferResource& Buffer)
{
    if (Buffer.PoolIndex < 0 || Buffer.PoolIndex >= static_cast<int32>(BufferPool.size()))
    {
        return;
    }

    FPooledBuffer& Pooled = BufferPool[Buffer.PoolIndex];
    Pooled.CurrentState = Buffer.CurrentState;
    Pooled.bInUse = false;
    Pooled.LastUseFrame = CurrentFrameIndex;
    Pooled.LastFenceValue = CurrentFrameFenceValue;
    if (Device)
    {
        Device->RetireTransientBindlessDescriptorIndex(Buffer.DefaultSrvBindlessIndex, CurrentFrameFenceValue);
        Device->RetireTransientBindlessDescriptorIndex(Buffer.DefaultUavBindlessIndex, CurrentFrameFenceValue);
    }
    Buffer.DefaultSrvBindlessIndex = UINT32_MAX;
    Buffer.DefaultUavBindlessIndex = UINT32_MAX;
    Buffer.DefaultSrvViewResource = nullptr;
    Buffer.DefaultUavViewResource = nullptr;
    Buffer.Resource = nullptr;
    Buffer.PoolIndex = -1;
}

uint32 FRenderGraph::GetTextureViewBindlessIndex(FRGTextureResource& Texture, bool bUav)
{
    uint32& BindlessIndex = bUav ? Texture.DefaultUavBindlessIndex : Texture.DefaultSrvBindlessIndex;
    ID3D12Resource*& ViewResource = bUav ? Texture.DefaultUavViewResource : Texture.DefaultSrvViewResource;

    if (Texture.bExternal)
    {
        if (IsValidBindlessIndex(BindlessIndex))
        {
            return BindlessIndex;
        }

        std::ostringstream Stream;
        Stream << "RenderGraph bindless view requested for imported texture '"
            << (Texture.Name.empty() ? "<Unnamed>" : Texture.Name)
            << "'";
        LogWarning(Stream.str());
        return UINT32_MAX;
    }

    if (!Texture.Resource)
    {
        return UINT32_MAX;
    }

    if (bUav && (Texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        return UINT32_MAX;
    }

    if (BindlessIndex == UINT32_MAX)
    {
        BindlessIndex = Device->AllocateTransientBindlessDescriptorIndex();
        if (BindlessIndex == UINT32_MAX)
        {
            return UINT32_MAX;
        }

        Device->TrackTransientBindlessDescriptorOwner(BindlessIndex, BuildTextureViewOwnerLabel(Texture, bUav, -1));
    }

    if (ViewResource != Texture.Resource)
    {
        if (bUav)
        {
            Device->WriteBindlessUav(BindlessIndex, Texture.Resource, nullptr,
                CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(Texture.Resource->GetDesc().Format));
        }
        else
        {
            Device->WriteBindlessSrv(BindlessIndex, Texture.Resource,
                CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(Texture.Resource->GetDesc().Format, Texture.Desc.MipLevels));
        }

        ViewResource = Texture.Resource;
    }

    return BindlessIndex;
}

uint32 FRenderGraph::GetTextureMipViewBindlessIndex(FRGTextureResource& Texture, bool bUav, uint32 MipIndex)
{
    if (!Device || Texture.bExternal || !Texture.Resource)
    {
        return UINT32_MAX;
    }

    if (MipIndex >= Texture.Desc.MipLevels)
    {
        return UINT32_MAX;
    }

    if (bUav && (Texture.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        return UINT32_MAX;
    }

    std::vector<uint32>& BindlessIndices = bUav ? Texture.MipUavBindlessIndices : Texture.MipSrvBindlessIndices;
    std::vector<ID3D12Resource*>& ViewResources = bUav ? Texture.MipUavViewResources : Texture.MipSrvViewResources;
    if (BindlessIndices.size() < Texture.Desc.MipLevels)
    {
        BindlessIndices.resize(Texture.Desc.MipLevels, UINT32_MAX);
        ViewResources.resize(Texture.Desc.MipLevels, nullptr);
    }

    uint32& BindlessIndex = BindlessIndices[MipIndex];
    ID3D12Resource*& ViewResource = ViewResources[MipIndex];
    if (BindlessIndex == UINT32_MAX)
    {
        BindlessIndex = Device->AllocateTransientBindlessDescriptorIndex();
        if (BindlessIndex == UINT32_MAX)
        {
            return UINT32_MAX;
        }

        Device->TrackTransientBindlessDescriptorOwner(BindlessIndex, BuildTextureViewOwnerLabel(Texture, bUav, static_cast<int32>(MipIndex)));
    }

    if (ViewResource != Texture.Resource)
    {
        if (bUav)
        {
            Device->WriteBindlessUav(BindlessIndex, Texture.Resource, nullptr,
                CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(Texture.Resource->GetDesc().Format, MipIndex));
        }
        else
        {
            Device->WriteBindlessSrv(BindlessIndex, Texture.Resource,
                CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(Texture.Resource->GetDesc().Format, 1, MipIndex));
        }

        ViewResource = Texture.Resource;
    }

    return BindlessIndex;
}

uint32 FRenderGraph::GetBufferUavBindlessIndex(FRGBufferResource& Buffer)
{
    if (Buffer.bExternal)
    {
        if (IsValidBindlessIndex(Buffer.DefaultUavBindlessIndex))
        {
            return Buffer.DefaultUavBindlessIndex;
        }

        std::ostringstream Stream;
        Stream << "RenderGraph bindless UAV requested for imported buffer '"
            << (Buffer.Name.empty() ? "<Unnamed>" : Buffer.Name)
            << "'";
        LogWarning(Stream.str());
        return UINT32_MAX;
    }

    if (!Buffer.Resource || (Buffer.Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        return UINT32_MAX;
    }

    if (Buffer.Desc.NumElements == 0)
    {
        return UINT32_MAX;
    }

    if (Buffer.DefaultUavBindlessIndex == UINT32_MAX)
    {
        Buffer.DefaultUavBindlessIndex = Device->AllocateTransientBindlessDescriptorIndex();
        if (Buffer.DefaultUavBindlessIndex == UINT32_MAX)
        {
            return UINT32_MAX;
        }

        Device->TrackTransientBindlessDescriptorOwner(Buffer.DefaultUavBindlessIndex, BuildBufferUavOwnerLabel(Buffer));
    }

    if (Buffer.DefaultUavViewResource != Buffer.Resource)
    {
        Device->WriteBindlessUav(Buffer.DefaultUavBindlessIndex, Buffer.Resource, nullptr, CreateBufferUavDesc(Buffer.Desc));
        Buffer.DefaultUavViewResource = Buffer.Resource;
    }

    return Buffer.DefaultUavBindlessIndex;
}

uint32 FRenderGraph::GetBufferSrvBindlessIndex(FRGBufferResource& Buffer)
{
    if (Buffer.bExternal)
    {
        if (IsValidBindlessIndex(Buffer.DefaultSrvBindlessIndex))
        {
            return Buffer.DefaultSrvBindlessIndex;
        }

        std::ostringstream Stream;
        Stream << "RenderGraph bindless SRV requested for imported buffer '"
            << (Buffer.Name.empty() ? "<Unnamed>" : Buffer.Name)
            << "'";
        LogWarning(Stream.str());
        return UINT32_MAX;
    }

    if (!Buffer.Resource || Buffer.Desc.NumElements == 0)
    {
        return UINT32_MAX;
    }

    if (Buffer.DefaultSrvBindlessIndex == UINT32_MAX)
    {
        Buffer.DefaultSrvBindlessIndex = Device->AllocateTransientBindlessDescriptorIndex();
        if (Buffer.DefaultSrvBindlessIndex == UINT32_MAX)
        {
            return UINT32_MAX;
        }

        Device->TrackTransientBindlessDescriptorOwner(Buffer.DefaultSrvBindlessIndex, BuildBufferSrvOwnerLabel(Buffer));
    }

    if (Buffer.DefaultSrvViewResource != Buffer.Resource)
    {
        Device->WriteBindlessSrv(Buffer.DefaultSrvBindlessIndex, Buffer.Resource, CreateBufferSrvDesc(Buffer.Desc));
        Buffer.DefaultSrvViewResource = Buffer.Resource;
    }

    return Buffer.DefaultSrvBindlessIndex;
}

void FRenderGraph::DumpDebugInfo(const std::vector<bool>& PassRequired, const std::vector<bool>& ResourceRequired, const std::vector<bool>& BufferRequired)
{
    LogInfo("RenderGraph Debug Dump Begin");

    if (bEnableResourceLifetimeLog)
    {
        std::vector<int32_t> TextureFirstUse(Textures.size(), -1);
        std::vector<int32_t> TextureLastUse(Textures.size(), -1);
        std::vector<int32_t> BufferFirstUse(Buffers.size(), -1);
        std::vector<int32_t> BufferLastUse(Buffers.size(), -1);

        for (int32_t PassIndex = 0; PassIndex < static_cast<int32_t>(Passes.size()); ++PassIndex)
        {
            if (!PassRequired[PassIndex])
            {
                continue;
            }

            const PassEntry& Entry = Passes[PassIndex];
            for (const FRGTextureUsage& Usage : Entry.TextureUsages)
            {
                if (!Usage.Handle || Usage.Handle.Id >= Textures.size())
                {
                    continue;
                }

                if (TextureFirstUse[Usage.Handle.Id] == -1)
                {
                    TextureFirstUse[Usage.Handle.Id] = PassIndex;
                }
                TextureLastUse[Usage.Handle.Id] = PassIndex;
            }

            for (const FRGBufferUsage& Usage : Entry.BufferUsages)
            {
                if (Usage.Handle.Id >= Buffers.size())
                {
                    continue;
                }

                if (BufferFirstUse[Usage.Handle.Id] == -1)
                {
                    BufferFirstUse[Usage.Handle.Id] = PassIndex;
                }
                BufferLastUse[Usage.Handle.Id] = PassIndex;
            }
        }

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
                << " (FirstUse: " << TextureFirstUse[Index]
                << ", LastUse: " << TextureLastUse[Index]
                << ", RefCount: " << Resource.InitialReferenceCount
                << ", External: " << (Resource.bExternal ? "Yes" : "No")
                << ")";
            LogInfo(Stream.str());
        }

        LogInfo("Buffers:");
        for (size_t Index = 0; Index < Buffers.size(); ++Index)
        {
            if (!BufferRequired[Index])
            {
                continue;
            }

            const FRGBufferResource& Resource = Buffers[Index];

            std::ostringstream Stream;
            Stream << " - " << Resource.Name
                << " (FirstUse: " << BufferFirstUse[Index]
                << ", LastUse: " << BufferLastUse[Index]
                << ", RefCount: " << Resource.InitialReferenceCount
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

        for (const FRGTextureUsage& Usage : Entry.TextureUsages)
        {
            const FRGTextureResource* Resource = ResolveTexture(Usage.Handle);
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

void FRenderGraph::ProcessPendingGpuTimings(const FDX12CommandContext& CmdContext, uint32 FrameIndex)
{
    auto It = PendingGpuTimings.find(FrameIndex);
    if (It == PendingGpuTimings.end())
    {
        return;
    }

	if (!bEnableGpuTiming)
	{
		PendingGpuTimings.erase(It);
		return;
	}

    const FDX12CommandQueue* Queue = Device ? Device->GetGraphicsQueue() : nullptr;
    const uint64 FenceValue = CmdContext.GetFrameFenceValue(FrameIndex);
    if (!Queue || FenceValue == 0 || Queue->GetCompletedFenceValue() < FenceValue)
    {
        return;
    }

	FGpuTimingData& Timing = It->second;
    if (!Timing.bPending || !Timing.ReadbackBuffer || Timing.QueryCount == 0 || Timing.Frequency == 0)
    {
        PendingGpuTimings.erase(It);
        return;
    }

    const UINT64 ReadbackSize = static_cast<UINT64>(Timing.QueryCount) * sizeof(uint64);
    uint64* TimestampData = nullptr;
    D3D12_RANGE ReadRange{ 0, ReadbackSize };

    HRESULT MapResult = Timing.ReadbackBuffer->Map(0, &ReadRange, reinterpret_cast<void**>(&TimestampData));
    if (FAILED(MapResult) || !TimestampData)
    {
        PendingGpuTimings.erase(It);
        return;
    }

    const size_t PassCount = Timing.PassNames.size();
    const auto Now = std::chrono::steady_clock::now();
    const double WindowSeconds = (std::max)(0.1, GpuTimingWindowSeconds);
    const auto Cutoff = Now - std::chrono::duration<double>(WindowSeconds);
    struct FGroupTimingAccumulator
    {
        double Milliseconds = 0.0;
        uint32 PassCount = 0;
    };
    std::unordered_map<std::string, FGroupTimingAccumulator> GroupTimings;

    for (size_t Index = 0; Index < PassCount; ++Index)
    {
        const size_t StartIdx = Index * 2;
        const size_t EndIdx = StartIdx + 1;

        if (EndIdx >= Timing.QueryCount)
        {
            continue;
        }

        const uint64 StartTimestamp = TimestampData[StartIdx];
        const uint64 EndTimestamp = TimestampData[EndIdx];
        const double Delta = static_cast<double>(EndTimestamp - StartTimestamp) / static_cast<double>(Timing.Frequency);
        const double Milliseconds = Delta * 1000.0;

        const std::string& PassName = Timing.PassNames[Index];
        std::deque<FGpuTimingSample>& Samples = GpuTimingSamples[PassName];
        Samples.push_back({ Now, Milliseconds });

        while (!Samples.empty() && Samples.front().Timestamp < Cutoff)
        {
            Samples.pop_front();
        }

        if (Index < Timing.PixGroupNames.size() && !Timing.PixGroupNames[Index].empty())
        {
            FGroupTimingAccumulator& GroupTiming = GroupTimings[Timing.PixGroupNames[Index]];
            GroupTiming.Milliseconds += Milliseconds;
            GroupTiming.PassCount++;
        }
    }

    for (const auto& GroupTimingIt : GroupTimings)
    {
        const FGroupTimingAccumulator& GroupTiming = GroupTimingIt.second;
        if (GroupTiming.PassCount <= 1u)
        {
            continue;
        }

        std::deque<FGpuTimingSample>& Samples = GpuTimingSamples[GroupTimingIt.first];
        Samples.push_back({ Now, GroupTiming.Milliseconds });
        while (!Samples.empty() && Samples.front().Timestamp < Cutoff)
        {
            Samples.pop_front();
        }
    }

    Timing.ReadbackBuffer->Unmap(0, nullptr);

    UpdateCachedGpuTimingStats(Now);

    PendingGpuTimings.erase(It);
}
