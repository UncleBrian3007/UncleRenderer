#include "RenderGraph.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include "RendererUtils.h"
#include <d3dx12.h>
#include <algorithm>
#include <sstream>
#include "../Core/Logger.h"
#include <filesystem>

std::vector<FRenderGraph::FPooledTexture> FRenderGraph::TexturePool;
std::unordered_map<uint32, FRenderGraph::FGpuTimingData> FRenderGraph::PendingGpuTimings;
std::unordered_map<uint32, FRenderGraph::FGpuTimingResources> FRenderGraph::GpuTimingResources;
std::unordered_map<std::string, std::deque<FRenderGraph::FGpuTimingSample>> FRenderGraph::GpuTimingSamples;
std::vector<FRenderGraph::FGpuPassTimingStats> FRenderGraph::CachedGpuTimingStats;
double FRenderGraph::GpuTimingWindowSeconds = 1.0;
uint32 FRenderGraph::GpuTimingDisplayCount = 5;

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

void FRGPassBuilder::KeepAlive()
{
    if (Entry)
    {
        Entry->bForceExecute = true;
    }
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
    if (!Device)
    {
        LogError("RenderGraph Execute called without a valid device");
        return;
    }

    ProcessPendingGpuTimings(CmdContext, CmdContext.GetCurrentFrameIndex());

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

        if (!bTouchesRequiredResource && !Entry.bForceExecute)
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

    for (int32_t PassIndex = 0; PassIndex < static_cast<int32_t>(Passes.size()); ++PassIndex)
    {
        PassEntry& Entry = Passes[PassIndex];
        Entry.bCulled = !PassRequired[PassIndex];

        if (Entry.bCulled)
        {
            continue;
        }

        if (QueryHeap && QueryReadback)
        {
            CmdContext.GetCommandList()->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex++);
            GpuTimedPassNames.push_back(Entry.Name);
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

        CmdContext.TransitionResources(PendingBarriers);

        std::chrono::high_resolution_clock::time_point PassBegin, PassEnd;
        if (bEnableDebugRecording)
        {
            PassBegin = std::chrono::high_resolution_clock::now();
        }

        if (Entry.ExecuteFunc)
        {
            Entry.ExecuteFunc(Entry.DataStorage, CmdContext);
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
        Pending.bPending = true;
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

        const auto Cutoff = Now - std::chrono::duration<double>(WindowSeconds);
        while (!Samples.empty() && Samples.front().Timestamp < Cutoff)
        {
            Samples.pop_front();
        }
    }

    Timing.ReadbackBuffer->Unmap(0, nullptr);

    UpdateCachedGpuTimingStats(Now);

    PendingGpuTimings.erase(It);
}
