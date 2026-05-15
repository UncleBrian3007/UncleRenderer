#include "ClusterDagStreamingManager.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/Logger.h"
#include "../../Core/RendererConfig.h"
#include "../../RHI/DX12CommandContext.h"
#include "../../RHI/DX12Device.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <d3dx12.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t GClusterDagRootPageIndex = 0u;
    constexpr uint32_t GClusterDagPageResidentFlag = 1u;

    uint32_t ClampConfigUint(uint32_t Value, uint32_t MinValue, uint32_t MaxValue)
    {
        return (std::min)((std::max)(Value, MinValue), MaxValue);
    }
}

void FClusterDagStreamingManager::ApplyConfig(const FRendererConfig& Config)
{
    const bool bPreviousEnabled = bEnabled;
    bEnabled = Config.bEnableClusterDAGStreaming;
    StreamingPoolMB = ClampConfigUint(Config.ClusterDAGStreamingPoolMB, 1u, 4096u);
    RequestBufferCapacity = ClampConfigUint(Config.ClusterDAGStreamingRequestBufferCapacity, 64u, 1048576u);
    MaxPendingPages = ClampConfigUint(Config.ClusterDAGStreamingMaxPendingPages, 1u, 65536u);
    MaxPageInstallsPerFrame = ClampConfigUint(Config.ClusterDAGStreamingMaxPageInstallsPerFrame, 1u, 1024u);
    if (bPreviousEnabled != bEnabled && !PageTableEntries.empty())
    {
        ClearPendingPages();
        InitializePageTable();
    }
}

void FClusterDagStreamingManager::Reset()
{
    if (PageTableUpload.Resource && PageTableUpload.MappedData)
    {
        PageTableUpload.Resource->Unmap(0, nullptr);
    }
    FeedbackBuffers.clear();
    FeedbackClearUpload = {};
    PageTableBuffer = {};
    PageTableUpload = {};
    PageDataBuffer = {};
    PageTableEntries.clear();
    ClearPendingPages();
    bResourcesReady = false;
    bPageTableUploadDirty = false;
    NextRequestSerial = 0;
    PageCount = 0;
    ResidentPageCount = 0;
    LastFrameRequestCount = 0;
    LastFrameInstallCount = 0;
    DroppedRequestCount = 0;
    ReplacedRequestCount = 0;
}

bool FClusterDagStreamingManager::InitializeResources(FDeferredRenderer& InOwner, FDX12Device* InDevice, uint32_t InPageCount, uint32_t FramesInFlight)
{
    Reset();
    Owner = &InOwner;
    Device = InDevice;

    if (FramesInFlight == 0u)
    {
        return false;
    }

    PageCount = (std::max)(InPageCount, 1u);
    PageTableEntries.resize(PageCount);
    InitializePageTable();

    const uint64_t RequestBufferSize = static_cast<uint64_t>(RequestBufferCapacity + 1u) * sizeof(FClusterDagStreamingRequest);
    const FClusterDagStreamingRequest ZeroFeedbackHeader = {};
    CreateUploadBuffer(
        Device,
        L"ClusterDagStreamingFeedbackClearUpload",
        sizeof(FClusterDagStreamingRequest),
        FeedbackClearUpload,
        &ZeroFeedbackHeader);

    FeedbackBuffers.resize(FramesInFlight);
    for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
    {
        CreateBindlessBuffer(
            Device,
            L"ClusterDagStreamingFeedback_Frame" + std::to_wstring(FrameIndex),
            CreateRWStructuredBufferDesc<FClusterDagStreamingRequest>(RequestBufferCapacity + 1u),
            D3D12_RESOURCE_STATE_COMMON,
            FeedbackBuffers[FrameIndex].Gpu,
            true,
            true);

        if (!CreateReadbackBuffer(Device, L"ClusterDagStreamingFeedbackReadback_Frame" + std::to_wstring(FrameIndex), RequestBufferSize, FeedbackBuffers[FrameIndex].Readback))
        {
            Reset();
            return false;
        }
    }

    CreateBindlessBuffer(
        Device,
        L"ClusterDagPageTable",
        CreateRWStructuredBufferDesc<FClusterDagPageTableEntry>(PageTableEntries.size()),
        D3D12_RESOURCE_STATE_COMMON,
        PageTableBuffer,
        true,
        true);

    if (!CreateMappedUploadBuffer(
        Device,
        L"ClusterDagPageTableUpload",
        static_cast<uint64_t>(PageTableEntries.size()) * sizeof(FClusterDagPageTableEntry),
        PageTableUpload))
    {
        Reset();
        return false;
    }
    UpdateMappedPageTable();

    const uint64_t PageDataSize = (std::max)(static_cast<uint64_t>(StreamingPoolMB) * 1024ull * 1024ull, 4ull);
    CreateBindlessBuffer(
        Device,
        L"ClusterDagStreamingPageData",
        CreateRawBufferDesc(PageDataSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_COMMON,
        PageDataBuffer,
        true,
        true);

    bResourcesReady = PageTableBuffer.IsFullyBound()
        && PageTableUpload.IsValid()
        && PageDataBuffer.IsFullyBound()
        && FeedbackClearUpload.IsValid()
        && std::all_of(FeedbackBuffers.begin(), FeedbackBuffers.end(), [](const FFeedbackBuffer& Buffer)
        {
            return Buffer.IsValid();
        });

    if (!bResourcesReady)
    {
        LogWarning("ClusterDag streaming resources are incomplete; streaming will stay disabled.");
    }

    return bResourcesReady;
}


void FClusterDagStreamingManager::InitializePageTable()
{
    ResidentPageCount = 0;
    for (uint32_t PageIndex = 0; PageIndex < PageCount; ++PageIndex)
    {
        FClusterDagPageTableEntry& Entry = PageTableEntries[PageIndex];
        Entry.PhysicalPageIndex = bEnabled && PageIndex != GClusterDagRootPageIndex ? 0xffffffffu : PageIndex;
        Entry.Flags = bEnabled && PageIndex != GClusterDagRootPageIndex ? 0u : GClusterDagPageResidentFlag;
        Entry.LastUsedFrame = 0u;
        Entry.Reserved = 0u;
        if ((Entry.Flags & GClusterDagPageResidentFlag) != 0u)
        {
            ResidentPageCount++;
        }
    }
    bPageTableUploadDirty = true;
}

void FClusterDagStreamingManager::UpdateMappedPageTable()
{
    if (!PageTableUpload.MappedData || PageTableEntries.empty())
    {
        return;
    }

    std::memcpy(
        PageTableUpload.MappedData,
        PageTableEntries.data(),
        PageTableEntries.size() * sizeof(FClusterDagPageTableEntry));
}

uint32_t FClusterDagStreamingManager::GetFeedbackUavBindlessIndex(uint32_t FrameIndex) const
{
    if (FrameIndex >= FeedbackBuffers.size())
    {
        return UINT32_MAX;
    }

    return FeedbackBuffers[FrameIndex].Gpu.UavBindlessIndex;
}

void FClusterDagStreamingManager::AddBeginFramePass(FDeferredPassContext& Context)
{
    if (!bResourcesReady)
    {
        return;
    }

    InstallPendingPages(static_cast<uint32_t>(Context.Owner.GetFrameNumber()));
    if (bPageTableUploadDirty)
    {
        UpdateMappedPageTable();
    }

    struct FPassData
    {
        bool bEnabled = false;
        bool bUploadPageTable = false;
        FRGBufferHandle FeedbackHandle{};
        FRGBufferHandle PageTableHandle{};
        ID3D12Resource* FeedbackBuffer = nullptr;
        ID3D12Resource* FeedbackClearUpload = nullptr;
        ID3D12Resource* PageTableBuffer = nullptr;
        ID3D12Resource* PageTableUpload = nullptr;
        uint64_t PageTableSize = 0;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagStreamingBeginFrame", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        const uint32_t FrameIndex = Context.FrameIndex;
        Data.bEnabled = bResourcesReady && FrameIndex < FeedbackBuffers.size();
        if (!Data.bEnabled)
        {
            return;
        }

        Data.bUploadPageTable = bPageTableUploadDirty;
        Data.FeedbackHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagStreamingFeedback", FeedbackBuffers[FrameIndex].Gpu);
        Data.FeedbackBuffer = FeedbackBuffers[FrameIndex].Gpu.Get();
        Data.FeedbackClearUpload = FeedbackClearUpload.Get();
        Builder.WriteBuffer(Data.FeedbackHandle, D3D12_RESOURCE_STATE_COPY_DEST);

        if (Data.bUploadPageTable)
        {
            Data.PageTableHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagStreamingPageTableUpload", PageTableBuffer);
            Data.PageTableBuffer = PageTableBuffer.Get();
            Data.PageTableUpload = PageTableUpload.Get();
            Data.PageTableSize = PageTableUpload.Size;
            Builder.WriteBuffer(Data.PageTableHandle, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        Builder.KeepAlive();
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        CommandList->CopyBufferRegion(
            Data.FeedbackBuffer,
            0,
            Data.FeedbackClearUpload,
            0,
            sizeof(FClusterDagStreamingRequest));

        if (Data.bUploadPageTable && Data.PageTableBuffer && Data.PageTableUpload && Data.PageTableSize > 0u)
        {
            CommandList->CopyBufferRegion(Data.PageTableBuffer, 0, Data.PageTableUpload, 0, Data.PageTableSize);
            bPageTableUploadDirty = false;
        }
    });
}

void FClusterDagStreamingManager::AddFeedbackReadbackPass(FDeferredPassContext& Context)
{
    if (!bResourcesReady || !bEnabled)
    {
        return;
    }

    struct FPassData
    {
        bool bEnabled = false;
        FRGBufferHandle FeedbackHandle{};
        ID3D12Resource* FeedbackBuffer = nullptr;
        ID3D12Resource* ReadbackBuffer = nullptr;
        uint64_t CopySize = 0;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagStreamingFeedbackReadback", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        const uint32_t FrameIndex = Context.FrameIndex;
        Data.bEnabled = FrameIndex < FeedbackBuffers.size() && FeedbackBuffers[FrameIndex].IsValid();
        if (!Data.bEnabled)
        {
            return;
        }

        Data.FeedbackHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagStreamingFeedbackReadbackSrc", FeedbackBuffers[FrameIndex].Gpu);
        Data.FeedbackBuffer = FeedbackBuffers[FrameIndex].Gpu.Get();
        Data.ReadbackBuffer = FeedbackBuffers[FrameIndex].Readback.Get();
        Data.CopySize = FeedbackBuffers[FrameIndex].Gpu.Desc.Size;
        Builder.ReadBuffer(Data.FeedbackHandle, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Builder.KeepAlive();
    }, [](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        Cmd.GetCommandList()->CopyBufferRegion(Data.ReadbackBuffer, 0, Data.FeedbackBuffer, 0, Data.CopySize);
    });
}

void FClusterDagStreamingManager::OnFrameFenceSignaled(uint32_t FrameIndex, uint64_t FenceValue)
{
    (void)FenceValue;
    if (!bResourcesReady || !bEnabled)
    {
        return;
    }

    ConsumeFeedback(FrameIndex);
}

void FClusterDagStreamingManager::ConsumeFeedback(uint32_t FrameIndex)
{
    if (FrameIndex >= FeedbackBuffers.size() || !FeedbackBuffers[FrameIndex].Readback.IsValid())
    {
        return;
    }

    const uint64_t ReadbackSize = static_cast<uint64_t>(RequestBufferCapacity + 1u) * sizeof(FClusterDagStreamingRequest);
    FClusterDagStreamingRequest* Requests = nullptr;
    D3D12_RANGE ReadRange{ 0, ReadbackSize };
    if (FAILED(FeedbackBuffers[FrameIndex].Readback->Map(0, &ReadRange, reinterpret_cast<void**>(&Requests))) || !Requests)
    {
        return;
    }

    const uint32_t RequestCount = (std::min)(Requests[0].StreamingResourceId, RequestBufferCapacity);
    LastFrameRequestCount = RequestCount;
    const uint32_t FrameNumber = Owner != nullptr ? static_cast<uint32_t>(Owner->GetFrameNumber()) : 0u;
    for (uint32_t RequestIndex = 0; RequestIndex < RequestCount; ++RequestIndex)
    {
        const FClusterDagStreamingRequest& Request = Requests[RequestIndex + 1u];
        if (Request.StreamingResourceId != StreamingResourceId || Request.PageIndex >= PageCount)
        {
            continue;
        }
        QueueRequestedPage(Request.PageIndex, Request.Priority, FrameNumber);
    }

    D3D12_RANGE WriteRange{ 0, 0 };
    FeedbackBuffers[FrameIndex].Readback->Unmap(0, &WriteRange);
}

void FClusterDagStreamingManager::QueueRequestedPage(uint32_t PageIndex, uint32_t Priority, uint32_t FrameNumber)
{
    Priority = (std::max)(Priority, 1u);
    if (PageIndex >= PageTableEntries.size()
        || (PageTableEntries[PageIndex].Flags & GClusterDagPageResidentFlag) != 0u)
    {
        return;
    }

    const auto ExistingIt = PendingPageIndices.find(PageIndex);
    if (ExistingIt != PendingPageIndices.end())
    {
        FPendingPage& PendingPage = PendingPages[ExistingIt->second];
        PendingPage.Priority = (std::max)(PendingPage.Priority, Priority);
        PendingPage.LastRequestFrame = FrameNumber;
        return;
    }

    const FPendingPage NewPendingPage =
    {
        PageIndex,
        Priority,
        NextRequestSerial++,
        FrameNumber
    };

    if (PendingPages.size() >= MaxPendingPages)
    {
        uint32_t LowestPriorityIndex = 0u;
        for (uint32_t Index = 1u; Index < PendingPages.size(); ++Index)
        {
            const FPendingPage& Candidate = PendingPages[Index];
            const FPendingPage& Lowest = PendingPages[LowestPriorityIndex];
            if (Candidate.Priority < Lowest.Priority
                || (Candidate.Priority == Lowest.Priority && Candidate.FirstRequestSerial > Lowest.FirstRequestSerial))
            {
                LowestPriorityIndex = Index;
            }
        }

        if (Priority <= PendingPages[LowestPriorityIndex].Priority)
        {
            DroppedRequestCount++;
            return;
        }

        PendingPageIndices.erase(PendingPages[LowestPriorityIndex].PageIndex);
        PendingPages[LowestPriorityIndex] = NewPendingPage;
        PendingPageIndices[PageIndex] = LowestPriorityIndex;
        ReplacedRequestCount++;
        return;
    }

    PendingPageIndices[PageIndex] = static_cast<uint32_t>(PendingPages.size());
    PendingPages.push_back(NewPendingPage);
}

void FClusterDagStreamingManager::InstallPendingPages(uint32_t FrameNumber)
{
    LastFrameInstallCount = 0;
    if (PendingPages.empty())
    {
        return;
    }

    std::sort(PendingPages.begin(), PendingPages.end(), [](const FPendingPage& A, const FPendingPage& B)
    {
        if (A.Priority != B.Priority)
        {
            return A.Priority > B.Priority;
        }
        return A.FirstRequestSerial < B.FirstRequestSerial;
    });

    const uint32_t InstallCount = (std::min)(MaxPageInstallsPerFrame, static_cast<uint32_t>(PendingPages.size()));
    for (uint32_t Index = 0u; Index < InstallCount; ++Index)
    {
        MarkPageResident(PendingPages[Index].PageIndex, FrameNumber);
        LastFrameInstallCount++;
    }

    PendingPages.erase(PendingPages.begin(), PendingPages.begin() + InstallCount);
    RebuildPendingPageIndices();
}

void FClusterDagStreamingManager::ClearPendingPages()
{
    PendingPages.clear();
    PendingPageIndices.clear();
}

void FClusterDagStreamingManager::RebuildPendingPageIndices()
{
    PendingPageIndices.clear();
    PendingPageIndices.reserve(PendingPages.size());
    for (uint32_t Index = 0u; Index < PendingPages.size(); ++Index)
    {
        PendingPageIndices[PendingPages[Index].PageIndex] = Index;
    }
}

void FClusterDagStreamingManager::MarkPageResident(uint32_t PageIndex, uint32_t FrameNumber)
{
    if (PageIndex >= PageTableEntries.size())
    {
        return;
    }

    FClusterDagPageTableEntry& Entry = PageTableEntries[PageIndex];
    const bool bWasResident = (Entry.Flags & GClusterDagPageResidentFlag) != 0u;
    Entry.PhysicalPageIndex = PageIndex;
    Entry.Flags |= GClusterDagPageResidentFlag;
    Entry.LastUsedFrame = FrameNumber;
    if (!bWasResident)
    {
        ResidentPageCount++;
        bPageTableUploadDirty = true;
    }
}
