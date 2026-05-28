#include "ClusterDagStreamingManager.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/Logger.h"
#include "../../Core/RendererConfig.h"
#include "../../Core/StringUtils.h"
#include "../../RHI/DX12CommandContext.h"
#include "../../RHI/DX12Device.h"
#include "../../Scene/ClusterDAG.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <d3dx12.h>

#include "../../../Shaders/ClusterDag/ClusterDagShared.h"

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t GClusterDagRootPageIndex = 0u;
    constexpr uint32_t GClusterDagPageResidentFlag = 1u;

    struct FClusterDagGpuPagePayloadHeader
    {
        uint32_t Magic = kClusterDagGpuPagePayloadMagic;
        uint32_t Version = kClusterDagGpuPagePayloadVersion;
        uint32_t PageIndex = 0xffffffffu;
        uint32_t GlobalGroupIndex = 0xffffffffu;
        uint32_t GroupByteOffset = 0u;
        uint32_t ChildRefByteOffset = 0u;
        uint32_t ChildRefCount = 0u;
        uint32_t ClusterRecordByteOffset = 0u;
        uint32_t ClusterRecordCount = 0u;
        uint32_t DrawDataRecordByteOffset = 0u;
        uint32_t DrawDataRecordCount = 0u;
        uint32_t PackedIndexByteOffset = 0u;
        uint32_t PackedIndexCount = 0u;
        uint32_t PackedPositionByteOffset = 0u;
        uint32_t PackedPositionCount = 0u;
        uint32_t PackedNormalByteOffset = 0u;
        uint32_t PackedNormalCount = 0u;
        uint32_t PackedUvByteOffset = 0u;
        uint32_t PackedUvCount = 0u;
        uint32_t PackedTangentByteOffset = 0u;
        uint32_t PackedTangentCount = 0u;
        uint32_t PackedColorByteOffset = 0u;
        uint32_t PackedColorCount = 0u;
        uint32_t Reserved0 = 0u;
    };
    static_assert(sizeof(FClusterDagGpuPagePayloadHeader) == 96);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, Magic) == kClusterDagGpuPageHeaderMagicOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, Version) == kClusterDagGpuPageHeaderVersionOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PageIndex) == kClusterDagGpuPageHeaderPageIndexOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, GlobalGroupIndex) == kClusterDagGpuPageHeaderGlobalGroupIndexOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, GroupByteOffset) == kClusterDagGpuPageHeaderGroupByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, ChildRefByteOffset) == kClusterDagGpuPageHeaderChildRefByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, ChildRefCount) == kClusterDagGpuPageHeaderChildRefCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, ClusterRecordByteOffset) == kClusterDagGpuPageHeaderClusterRecordByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, ClusterRecordCount) == kClusterDagGpuPageHeaderClusterRecordCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, DrawDataRecordByteOffset) == kClusterDagGpuPageHeaderDrawDataRecordByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, DrawDataRecordCount) == kClusterDagGpuPageHeaderDrawDataRecordCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedIndexByteOffset) == kClusterDagGpuPageHeaderPackedIndexByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedIndexCount) == kClusterDagGpuPageHeaderPackedIndexCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedPositionByteOffset) == kClusterDagGpuPageHeaderPackedPositionByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedPositionCount) == kClusterDagGpuPageHeaderPackedPositionCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedNormalByteOffset) == kClusterDagGpuPageHeaderPackedNormalByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedNormalCount) == kClusterDagGpuPageHeaderPackedNormalCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedUvByteOffset) == kClusterDagGpuPageHeaderPackedUvByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedUvCount) == kClusterDagGpuPageHeaderPackedUvCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedTangentByteOffset) == kClusterDagGpuPageHeaderPackedTangentByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedTangentCount) == kClusterDagGpuPageHeaderPackedTangentCountOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedColorByteOffset) == kClusterDagGpuPageHeaderPackedColorByteOffsetOffset);
    static_assert(offsetof(FClusterDagGpuPagePayloadHeader, PackedColorCount) == kClusterDagGpuPageHeaderPackedColorCountOffset);

    struct FClusterDagGpuPageGroupData
    {
        float Bounds[4] = {};
        float LodBounds[4] = {};
        float ParentLODError = 0.0f;
        uint32_t ChildRefStart = 0u;
        uint32_t ChildRefCount = 0u;
        uint32_t Flags = 0u;
        uint32_t MipLevel = 0u;
    };
    static_assert(sizeof(FClusterDagGpuPageGroupData) == 52);
    static_assert(offsetof(FClusterDagGpuPageGroupData, Bounds) == kClusterDagGpuPageGroupBoundsOffset);
    static_assert(offsetof(FClusterDagGpuPageGroupData, LodBounds) == kClusterDagGpuPageGroupLodBoundsOffset);
    static_assert(offsetof(FClusterDagGpuPageGroupData, ParentLODError) == kClusterDagGpuPageGroupParentLODErrorOffset);
    static_assert(offsetof(FClusterDagGpuPageGroupData, ChildRefStart) == kClusterDagGpuPageGroupChildRefStartOffset);
    static_assert(offsetof(FClusterDagGpuPageGroupData, ChildRefCount) == kClusterDagGpuPageGroupChildRefCountOffset);
    static_assert(offsetof(FClusterDagGpuPageGroupData, Flags) == kClusterDagGpuPageGroupFlagsOffset);
    static_assert(offsetof(FClusterDagGpuPageGroupData, MipLevel) == kClusterDagGpuPageGroupMipLevelOffset);

    struct FClusterDagGpuPageClusterRecord
    {
        uint32_t GlobalClusterIndex = GClusterDAGInvalidIndex;
        uint32_t Reserved0 = 0u;
        uint32_t Reserved1 = 0u;
        uint32_t Reserved2 = 0u;
        float Bounds[4] = {};
        float LodBounds[4] = {};
        float LODError = 0.0f;
        float MaxEdgeLength = 0.0f;
        uint32_t GroupIndex = GClusterDAGInvalidIndex;
        uint32_t GeneratingGroupIndex = GClusterDAGInvalidIndex;
        uint32_t DrawDataStart = 0u;
        uint32_t DrawDataCount = 0u;
        uint32_t TriangleCount = 0u;
        uint32_t MipLevel = 0u;
    };
    static_assert(sizeof(FClusterDagGpuPageClusterRecord) == kClusterDagGpuPageClusterRecordStride);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, GlobalClusterIndex) == kClusterDagGpuPageClusterRecordGlobalClusterIndexOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, Bounds) == kClusterDagGpuPageClusterRecordBoundsOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, LodBounds) == kClusterDagGpuPageClusterRecordLodBoundsOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, LODError) == kClusterDagGpuPageClusterRecordLODErrorOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, MaxEdgeLength) == kClusterDagGpuPageClusterRecordMaxEdgeLengthOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, GroupIndex) == kClusterDagGpuPageClusterRecordGroupIndexOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, GeneratingGroupIndex) == kClusterDagGpuPageClusterRecordGeneratingGroupIndexOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, DrawDataStart) == kClusterDagGpuPageClusterRecordDrawDataStartOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, DrawDataCount) == kClusterDagGpuPageClusterRecordDrawDataCountOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, TriangleCount) == kClusterDagGpuPageClusterRecordTriangleCountOffset);
    static_assert(offsetof(FClusterDagGpuPageClusterRecord, MipLevel) == kClusterDagGpuPageClusterRecordMipLevelOffset);

    struct FClusterDagGpuPageDrawDataRecord
    {
        uint32_t GlobalDrawDataIndex = GClusterDAGInvalidIndex;
        uint32_t StartIndex = 0u;
        uint32_t IndexCount = 0u;
        uint32_t RangeIndex = 0u;
        uint32_t RangeCommandStart = 0u;
        uint32_t RangeCommandCount = 0u;
        uint32_t DrawSectionIndex = 0u;
        uint32_t Reserved1 = 0u;
    };
    static_assert(sizeof(FClusterDagGpuPageDrawDataRecord) == kClusterDagGpuPageDrawDataRecordStride);
    static_assert(offsetof(FClusterDagGpuPageDrawDataRecord, GlobalDrawDataIndex) == kClusterDagGpuPageDrawDataRecordGlobalDrawDataIndexOffset);
    static_assert(offsetof(FClusterDagGpuPageDrawDataRecord, StartIndex) == kClusterDagGpuPageDrawDataRecordStartIndexOffset);
    static_assert(offsetof(FClusterDagGpuPageDrawDataRecord, IndexCount) == kClusterDagGpuPageDrawDataRecordIndexCountOffset);
    static_assert(offsetof(FClusterDagGpuPageDrawDataRecord, RangeIndex) == kClusterDagGpuPageDrawDataRecordRangeIndexOffset);
    static_assert(offsetof(FClusterDagGpuPageDrawDataRecord, RangeCommandStart) == kClusterDagGpuPageDrawDataRecordRangeCommandStartOffset);
    static_assert(offsetof(FClusterDagGpuPageDrawDataRecord, RangeCommandCount) == kClusterDagGpuPageDrawDataRecordRangeCommandCountOffset);
    static_assert(offsetof(FClusterDagGpuPageDrawDataRecord, DrawSectionIndex) == kClusterDagGpuPageDrawDataRecordDrawSectionIndexOffset);

    uint32_t ClampConfigUint(uint32_t Value, uint32_t MinValue, uint32_t MaxValue)
    {
        return (std::min)((std::max)(Value, MinValue), MaxValue);
    }

    uint32_t AlignUpUint32(uint32_t Value, uint32_t Alignment)
    {
        return (Value + Alignment - 1u) & ~(Alignment - 1u);
    }

}

bool FClusterDagStreamingManager::BuildGpuPagePayload(
    uint32_t PageIndex,
    const std::vector<uint8_t>& DiskPayload,
    std::vector<uint8_t>& OutPayload) const
{
    OutPayload.clear();
    if (PageIndex >= PageSources.size())
    {
        OutPayload = DiskPayload;
        return !OutPayload.empty();
    }

    const FClusterDagStreamingPageSource& Source = PageSources[PageIndex];
    if (!Source.bValid
        || Source.GlobalGroupIndex == 0xffffffffu
        || Source.SceneGroupChildRefCount == 0u
        || Source.SceneGroupChildRefs.empty())
    {
        OutPayload = DiskPayload;
        return !OutPayload.empty();
    }

    FClusterDagGpuPageGroupData GroupData;
    std::memcpy(GroupData.Bounds, Source.SceneGroupBounds, sizeof(GroupData.Bounds));
    std::memcpy(GroupData.LodBounds, Source.SceneGroupLodBounds, sizeof(GroupData.LodBounds));
    GroupData.ParentLODError = Source.SceneGroupParentLODError;
    GroupData.ChildRefStart = 0u;
    GroupData.ChildRefCount = static_cast<uint32_t>(Source.SceneGroupChildRefs.size());
    GroupData.Flags = Source.SceneGroupFlags;
    GroupData.MipLevel = Source.SceneGroupMipLevel;

    std::vector<FClusterDagGpuPageClusterRecord> ClusterRecords;
    ClusterRecords.reserve(Source.ScenePageClusters.size());
    for (const FClusterDagStreamingPageSource::FSceneClusterRecord& SourceCluster : Source.ScenePageClusters)
    {
        FClusterDagGpuPageClusterRecord ClusterRecord;
        ClusterRecord.GlobalClusterIndex = SourceCluster.GlobalClusterIndex;
        std::memcpy(ClusterRecord.Bounds, SourceCluster.Bounds, sizeof(ClusterRecord.Bounds));
        std::memcpy(ClusterRecord.LodBounds, SourceCluster.LodBounds, sizeof(ClusterRecord.LodBounds));
        ClusterRecord.LODError = SourceCluster.LODError;
        ClusterRecord.MaxEdgeLength = SourceCluster.MaxEdgeLength;
        ClusterRecord.GroupIndex = SourceCluster.GroupIndex;
        ClusterRecord.GeneratingGroupIndex = SourceCluster.GeneratingGroupIndex;
        ClusterRecord.DrawDataStart = SourceCluster.DrawDataStart;
        ClusterRecord.DrawDataCount = SourceCluster.DrawDataCount;
        ClusterRecord.TriangleCount = SourceCluster.TriangleCount;
        ClusterRecord.MipLevel = SourceCluster.MipLevel;
        ClusterRecords.push_back(ClusterRecord);
    }

    std::vector<FClusterDagGpuPageDrawDataRecord> DrawDataRecords;
    DrawDataRecords.reserve(Source.ScenePageDrawDatas.size());
    for (const FClusterDagStreamingPageSource::FSceneDrawDataRecord& SourceDrawData : Source.ScenePageDrawDatas)
    {
        FClusterDagGpuPageDrawDataRecord DrawDataRecord;
        DrawDataRecord.GlobalDrawDataIndex = SourceDrawData.GlobalDrawDataIndex;
        DrawDataRecord.StartIndex = SourceDrawData.StartIndex;
        DrawDataRecord.IndexCount = SourceDrawData.IndexCount;
        DrawDataRecord.RangeIndex = SourceDrawData.RangeIndex;
        DrawDataRecord.RangeCommandStart = SourceDrawData.RangeCommandStart;
        DrawDataRecord.RangeCommandCount = SourceDrawData.RangeCommandCount;
        DrawDataRecord.DrawSectionIndex = SourceDrawData.DrawSectionIndex;
        DrawDataRecords.push_back(DrawDataRecord);
    }

    FClusterDagGpuPagePayloadHeader Header;
    Header.PageIndex = PageIndex;
    Header.GlobalGroupIndex = Source.GlobalGroupIndex;
    Header.GroupByteOffset = sizeof(FClusterDagGpuPagePayloadHeader);
    Header.ChildRefByteOffset = AlignUpUint32(Header.GroupByteOffset + sizeof(FClusterDagGpuPageGroupData), 16u);
    Header.ChildRefCount = static_cast<uint32_t>(Source.SceneGroupChildRefs.size());
    Header.ClusterRecordByteOffset = AlignUpUint32(
        Header.ChildRefByteOffset + Header.ChildRefCount * static_cast<uint32_t>(sizeof(FRuntimeClusterChildRef)),
        16u);
    Header.ClusterRecordCount = static_cast<uint32_t>(ClusterRecords.size());
    Header.DrawDataRecordByteOffset = AlignUpUint32(
        Header.ClusterRecordByteOffset + Header.ClusterRecordCount * static_cast<uint32_t>(sizeof(FClusterDagGpuPageClusterRecord)),
        16u);
    Header.DrawDataRecordCount = static_cast<uint32_t>(DrawDataRecords.size());
    Header.PackedIndexByteOffset = AlignUpUint32(
        Header.DrawDataRecordByteOffset + Header.DrawDataRecordCount * static_cast<uint32_t>(sizeof(FClusterDagGpuPageDrawDataRecord)),
        16u);
    Header.PackedIndexCount = static_cast<uint32_t>(Source.ScenePagePackedIndices.size());
    Header.PackedPositionByteOffset = AlignUpUint32(
        Header.PackedIndexByteOffset + Header.PackedIndexCount * static_cast<uint32_t>(sizeof(uint32_t)),
        16u);
    Header.PackedPositionCount = static_cast<uint32_t>(Source.ScenePagePackedPositions.size());
    Header.PackedNormalByteOffset = AlignUpUint32(
        Header.PackedPositionByteOffset + Header.PackedPositionCount * static_cast<uint32_t>(sizeof(FClusterDagPackedPosition)),
        16u);
    Header.PackedNormalCount = static_cast<uint32_t>(Source.ScenePagePackedNormals.size());
    Header.PackedUvByteOffset = AlignUpUint32(
        Header.PackedNormalByteOffset + Header.PackedNormalCount * static_cast<uint32_t>(sizeof(uint32_t)),
        16u);
    Header.PackedUvCount = static_cast<uint32_t>(Source.ScenePagePackedUVs.size());
    Header.PackedTangentByteOffset = AlignUpUint32(
        Header.PackedUvByteOffset + Header.PackedUvCount * static_cast<uint32_t>(sizeof(uint32_t)),
        16u);
    Header.PackedTangentCount = static_cast<uint32_t>(Source.ScenePagePackedTangents.size());
    Header.PackedColorByteOffset = AlignUpUint32(
        Header.PackedTangentByteOffset + Header.PackedTangentCount * static_cast<uint32_t>(sizeof(uint32_t)),
        16u);
    Header.PackedColorCount = static_cast<uint32_t>(Source.ScenePagePackedColors.size());

    const uint64_t PayloadBytes =
        static_cast<uint64_t>(Header.PackedColorByteOffset)
        + static_cast<uint64_t>(Header.PackedColorCount) * sizeof(uint32_t);
    if (PayloadBytes == 0u || PayloadBytes > PageSlotBytes)
    {
        return false;
    }

    OutPayload.resize(static_cast<size_t>(PayloadBytes));
    std::memcpy(OutPayload.data(), &Header, sizeof(Header));
    std::memcpy(OutPayload.data() + Header.GroupByteOffset, &GroupData, sizeof(GroupData));
    std::memcpy(
        OutPayload.data() + Header.ChildRefByteOffset,
        Source.SceneGroupChildRefs.data(),
        static_cast<size_t>(Header.ChildRefCount) * sizeof(FRuntimeClusterChildRef));
    if (!ClusterRecords.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.ClusterRecordByteOffset,
            ClusterRecords.data(),
            ClusterRecords.size() * sizeof(FClusterDagGpuPageClusterRecord));
    }
    if (!DrawDataRecords.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.DrawDataRecordByteOffset,
            DrawDataRecords.data(),
            DrawDataRecords.size() * sizeof(FClusterDagGpuPageDrawDataRecord));
    }
    if (!Source.ScenePagePackedIndices.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.PackedIndexByteOffset,
            Source.ScenePagePackedIndices.data(),
            Source.ScenePagePackedIndices.size() * sizeof(uint32_t));
    }
    if (!Source.ScenePagePackedPositions.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.PackedPositionByteOffset,
            Source.ScenePagePackedPositions.data(),
            Source.ScenePagePackedPositions.size() * sizeof(FClusterDagPackedPosition));
    }
    if (!Source.ScenePagePackedNormals.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.PackedNormalByteOffset,
            Source.ScenePagePackedNormals.data(),
            Source.ScenePagePackedNormals.size() * sizeof(uint32_t));
    }
    if (!Source.ScenePagePackedUVs.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.PackedUvByteOffset,
            Source.ScenePagePackedUVs.data(),
            Source.ScenePagePackedUVs.size() * sizeof(uint32_t));
    }
    if (!Source.ScenePagePackedTangents.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.PackedTangentByteOffset,
            Source.ScenePagePackedTangents.data(),
            Source.ScenePagePackedTangents.size() * sizeof(uint32_t));
    }
    if (!Source.ScenePagePackedColors.empty())
    {
        std::memcpy(
            OutPayload.data() + Header.PackedColorByteOffset,
            Source.ScenePagePackedColors.data(),
            Source.ScenePagePackedColors.size() * sizeof(uint32_t));
    }
    return true;
}

FClusterDagStreamingManager::FPageReadResult FClusterDagStreamingManager::ReadPagePayload(
    const FPageReadSource& Source,
    uint32_t PageIndex)
{
    FPageReadResult Result;
    Result.PageIndex = PageIndex;
    if (!Source.bValid)
    {
        return Result;
    }

    Result.Payload.resize(Source.PayloadBytes);
    if (Source.PayloadBytes == 0u)
    {
        Result.bSucceeded = true;
        return Result;
    }

    std::ifstream Stream(std::filesystem::path(Source.CacheFilePath), std::ios::binary);
    if (!Stream.is_open())
    {
        Result.Payload.clear();
        return Result;
    }

    Stream.seekg(static_cast<std::streamoff>(Source.FileOffset), std::ios::beg);
    if (!Stream.good())
    {
        Result.Payload.clear();
        return Result;
    }

    Stream.read(reinterpret_cast<char*>(Result.Payload.data()), static_cast<std::streamsize>(Result.Payload.size()));
    Result.bSucceeded = Stream.good();
    if (!Result.bSucceeded)
    {
        Result.Payload.clear();
    }
    return Result;
}

void FClusterDagStreamingManager::ApplyConfig(const FRendererConfig& Config)
{
    const bool bPreviousEnabled = bEnabled;
    bEnabled = Config.bEnableClusterDAGStreaming;
    StreamingPoolMB = ClampConfigUint(Config.ClusterDAGStreamingPoolMB, 1u, 4096u);
    RequestBufferCapacity = ClampConfigUint(Config.ClusterDAGStreamingRequestBufferCapacity, 64u, 1048576u);
    MaxPendingPages = ClampConfigUint(Config.ClusterDAGStreamingMaxPendingPages, 1u, 65536u);
    MaxPageInstallsPerFrame = ClampConfigUint(Config.ClusterDAGStreamingMaxPageInstallsPerFrame, 1u, 1024u);
    const uint32_t PreviousPageSlotBytes = PageSlotBytes;
    PageSlotBytes = GClusterDAGVmeshStreamingPageSlotBytes;
    MaxIoInFlight = ClampConfigUint(Config.ClusterDAGStreamingMaxIoInFlight, 1u, 1024u);
    MaxPageUploadBytesPerFrame = ClampConfigUint(Config.ClusterDAGStreamingMaxPageUploadBytesPerFrame, 4096u, 1024u * 1024u * 1024u);
    if ((bPreviousEnabled != bEnabled || PreviousPageSlotBytes != PageSlotBytes) && !PageTableEntries.empty())
    {
        ClearPendingPages();
        GpuUploadingPages.clear();
        const uint64_t PoolBytes = PageDataBuffer.IsValid()
            ? PageDataBuffer.Desc.Size
            : static_cast<uint64_t>(StreamingPoolMB) * 1024ull * 1024ull;
        InitializePageSlotPool(PoolBytes);
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
    PageSlotPool = {};
    PageTableEntries.clear();
    PageReadSources.clear();
    PageSources.clear();
    GpuUploadingPages.clear();
    ClearPendingPages();
    bResourcesReady = false;
    bPageTableUploadDirty = false;
    NextRequestSerial = 0;
    PageCount = 0;
    ResidentPageCount = 0;
    LastFrameRequestCount = 0;
    LastFrameInstallCount = 0;
    LastFrameIoIssueCount = 0;
    LastFrameIoCompleteCount = 0;
    LastFrameIoFailCount = 0;
    LastFrameUploadIssueCount = 0;
    LastFrameUploadCompleteCount = 0;
    LastFrameUploadBytes = 0;
    DroppedRequestCount = 0;
    ReplacedRequestCount = 0;
    SlotPoolFullDropCount = 0;
}

bool FClusterDagStreamingManager::InitializeResources(
    FDeferredRenderer& InOwner,
    FDX12Device* InDevice,
    uint32_t InPageCount,
    uint32_t FramesInFlight,
    const std::vector<FClusterDagStreamingPageSource>& PageSources)
{
    const bool bConfiguredEnabled = bEnabled;
    Reset();
    bEnabled = bConfiguredEnabled;
    Owner = &InOwner;
    Device = InDevice;

    if (FramesInFlight == 0u)
    {
        return false;
    }

    PageCount = (std::max)(InPageCount, 1u);
    PageTableEntries.resize(PageCount);
    InitializePageReadSources(PageSources);
    const uint64_t PageDataSize = (std::max)(static_cast<uint64_t>(StreamingPoolMB) * 1024ull * 1024ull, static_cast<uint64_t>(PageSlotBytes));
    InitializePageSlotPool(PageDataSize);
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


void FClusterDagStreamingManager::InitializePageReadSources(const std::vector<FClusterDagStreamingPageSource>& Sources)
{
    PageReadSources.assign(PageCount, {});
    PageSources.assign(PageCount, {});
    if (Sources.empty())
    {
        return;
    }

    const FClusterDAGBuildParams Params{};
    std::unordered_map<std::wstring, std::vector<FClusterDAGStreamingPageDesc>> DirectoryCache;
    for (const FClusterDagStreamingPageSource& Source : Sources)
    {
        if (!Source.bValid
            || Source.PageIndex == GClusterDagRootPageIndex
            || Source.PageIndex >= PageReadSources.size()
            || Source.CacheFilePath.empty()
            || Source.SourceFilePath.empty())
        {
            continue;
        }
        PageSources[Source.PageIndex] = Source;

        auto DirectoryIt = DirectoryCache.find(Source.CacheFilePath);
        if (DirectoryIt == DirectoryCache.end())
        {
            std::vector<FClusterDAGStreamingPageDesc> Directory;
            if (!LoadClusterDAGStreamingPageDirectory(Source.CacheFilePath, Source.SourceFilePath, Params, Directory))
            {
                LogWarning("ClusterDag streaming page directory load failed: " + StringUtils::WideToUtf8(Source.CacheFilePath));
                DirectoryIt = DirectoryCache.emplace(Source.CacheFilePath, std::vector<FClusterDAGStreamingPageDesc>{}).first;
            }
            else
            {
                DirectoryIt = DirectoryCache.emplace(Source.CacheFilePath, std::move(Directory)).first;
            }
        }

        for (const FClusterDAGStreamingPageDesc& Desc : DirectoryIt->second)
        {
            const bool bStreamingPayload = (Desc.Flags & GClusterDAGVmeshPageFlagStreamingPayload) != 0u;
            const bool bRestorePayload = (Desc.Flags & GClusterDAGVmeshPageFlagRestorePayload) != 0u;
            if (bStreamingPayload
                && !bRestorePayload
                && Desc.MeshIndex == Source.MeshIndex
                && Desc.LocalPageIndex == Source.LocalPageIndex)
            {
                FPageReadSource& ReadSource = PageReadSources[Source.PageIndex];
                ReadSource.bValid = true;
                ReadSource.FileOffset = Desc.FileOffset;
                ReadSource.PayloadBytes = Desc.PayloadBytes;
                ReadSource.CacheFilePath = Source.CacheFilePath;
                break;
            }
        }
    }
}

void FClusterDagStreamingManager::InitializePageSlotPool(uint64_t PoolBytes)
{
    PageSlotPool = {};
    PageSlotPool.SlotBytes = PageSlotBytes;
    PageSlotPool.SlotCount = static_cast<uint32_t>(PoolBytes / (std::max)(static_cast<uint64_t>(PageSlotBytes), 1ull));
    PageSlotPool.FreeSlots.reserve(PageSlotPool.SlotCount);
    for (uint32_t SlotIndex = PageSlotPool.SlotCount; SlotIndex > 0u; --SlotIndex)
    {
        PageSlotPool.FreeSlots.push_back(SlotIndex - 1u);
    }

    if (bEnabled && PageSlotPool.SlotCount > 0u)
    {
        uint32_t RootSlotIndex = 0u;
        AllocatePhysicalPageSlot(GClusterDagRootPageIndex, RootSlotIndex);
    }
}

bool FClusterDagStreamingManager::AllocatePhysicalPageSlot(uint32_t PageIndex, uint32_t& OutSlotIndex)
{
    const auto ExistingIt = PageSlotPool.LogicalPageToSlot.find(PageIndex);
    if (ExistingIt != PageSlotPool.LogicalPageToSlot.end())
    {
        OutSlotIndex = ExistingIt->second;
        return true;
    }

    if (PageSlotPool.FreeSlots.empty())
    {
        SlotPoolFullDropCount++;
        return false;
    }

    OutSlotIndex = PageSlotPool.FreeSlots.back();
    PageSlotPool.FreeSlots.pop_back();
    PageSlotPool.LogicalPageToSlot[PageIndex] = OutSlotIndex;
    return true;
}

void FClusterDagStreamingManager::InitializePageTable()
{
    ResidentPageCount = 0;
    for (uint32_t PageIndex = 0; PageIndex < PageCount; ++PageIndex)
    {
        FClusterDagPageTableEntry& Entry = PageTableEntries[PageIndex];
        if (bEnabled && PageIndex != GClusterDagRootPageIndex)
        {
            Entry.PhysicalPageIndex = 0xffffffffu;
            Entry.Flags = 0u;
        }
        else
        {
            uint32_t PhysicalSlotIndex = PageIndex;
            if (bEnabled)
            {
                AllocatePhysicalPageSlot(PageIndex, PhysicalSlotIndex);
            }
            Entry.PhysicalPageIndex = PhysicalSlotIndex;
            Entry.Flags = GClusterDagPageResidentFlag;
        }
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

    UpdateStreamingWork(Context.FrameIndex, static_cast<uint32_t>(Context.Owner.GetFrameNumber()));
    if (bPageTableUploadDirty)
    {
        UpdateMappedPageTable();
    }

    struct FPassData
    {
        bool bEnabled = false;
        bool bUploadPageTable = false;
        uint32_t FrameIndex = 0xffffffffu;
        FRGBufferHandle FeedbackHandle{};
        FRGBufferHandle PageTableHandle{};
        FRGBufferHandle PageDataHandle{};
        ID3D12Resource* FeedbackBuffer = nullptr;
        ID3D12Resource* FeedbackClearUpload = nullptr;
        ID3D12Resource* PageTableBuffer = nullptr;
        ID3D12Resource* PageTableUpload = nullptr;
        uint64_t PageTableSize = 0;
        struct FPageCopy
        {
            uint32_t PageIndex = 0xffffffffu;
            ID3D12Resource* UploadBuffer = nullptr;
            uint64_t DestinationOffset = 0;
            uint64_t PayloadBytes = 0;
        };
        ID3D12Resource* PageDataBuffer = nullptr;
        std::vector<FPageCopy> PageCopies;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagStreamingBeginFrame", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        const uint32_t FrameIndex = Context.FrameIndex;
        Data.bEnabled = bResourcesReady && FrameIndex < FeedbackBuffers.size();
        if (!Data.bEnabled)
        {
            return;
        }

        Data.FrameIndex = FrameIndex;
        Data.bUploadPageTable = bPageTableUploadDirty;
        Data.FeedbackHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagStreamingFeedback", FeedbackBuffers[FrameIndex].Gpu);
        Data.FeedbackBuffer = FeedbackBuffers[FrameIndex].Gpu.Get();
        Data.FeedbackClearUpload = FeedbackClearUpload.Get();
        Builder.WriteBuffer(Data.FeedbackHandle, D3D12_RESOURCE_STATE_COPY_DEST);

        for (const FGpuUploadingPage& UploadingPage : GpuUploadingPages)
        {
            if (UploadingPage.SubmissionFrameIndex != FrameIndex || UploadingPage.FenceValue != 0u)
            {
                continue;
            }

            FPassData::FPageCopy Copy;
            Copy.PageIndex = UploadingPage.PageIndex;
            Copy.UploadBuffer = UploadingPage.UploadBuffer.Get();
            Copy.DestinationOffset = static_cast<uint64_t>(UploadingPage.PhysicalSlotIndex) * PageSlotPool.SlotBytes;
            Copy.PayloadBytes = UploadingPage.PayloadBytes;
            if (Copy.UploadBuffer && Copy.PayloadBytes > 0u)
            {
                Data.PageCopies.push_back(Copy);
            }
        }

        if (!Data.PageCopies.empty())
        {
            Data.PageDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagStreamingPageDataUpload", PageDataBuffer);
            Data.PageDataBuffer = PageDataBuffer.Get();
            Builder.WriteBuffer(Data.PageDataHandle, D3D12_RESOURCE_STATE_COPY_DEST);
        }

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

        if (Data.PageDataBuffer)
        {
            for (const FPassData::FPageCopy& Copy : Data.PageCopies)
            {
                CommandList->CopyBufferRegion(
                    Data.PageDataBuffer,
                    Copy.DestinationOffset,
                    Copy.UploadBuffer,
                    0,
                    Copy.PayloadBytes);
                MarkUploadCopySubmitted(Copy.PageIndex, Data.FrameIndex);
            }
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
    if (!bResourcesReady || !bEnabled)
    {
        return;
    }

    AssignSubmittedUploadFence(FrameIndex, FenceValue);
    const uint64_t CompletedFenceValue = Device && Device->GetGraphicsQueue()
        ? Device->GetGraphicsQueue()->GetCompletedFenceValue()
        : 0u;
    RetireCompletedUploads(CompletedFenceValue, false);
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

    FPendingPage NewPendingPage;
    NewPendingPage.PageIndex = PageIndex;
    NewPendingPage.Priority = Priority;
    NewPendingPage.FirstRequestSerial = NextRequestSerial++;
    NewPendingPage.LastRequestFrame = FrameNumber;

    if (PendingPages.size() >= MaxPendingPages)
    {
        uint32_t LowestPriorityIndex = 0xffffffffu;
        for (uint32_t Index = 0u; Index < PendingPages.size(); ++Index)
        {
            const FPendingPage& Candidate = PendingPages[Index];
            if (Candidate.State != EPendingPageState::Pending)
            {
                continue;
            }

            if (LowestPriorityIndex == 0xffffffffu)
            {
                LowestPriorityIndex = Index;
                continue;
            }

            const FPendingPage& Lowest = PendingPages[LowestPriorityIndex];
            if (Candidate.Priority < Lowest.Priority
                || (Candidate.Priority == Lowest.Priority && Candidate.FirstRequestSerial > Lowest.FirstRequestSerial))
            {
                LowestPriorityIndex = Index;
            }
        }

        if (LowestPriorityIndex == 0xffffffffu || Priority <= PendingPages[LowestPriorityIndex].Priority)
        {
            DroppedRequestCount++;
            return;
        }

        PendingPageIndices.erase(PendingPages[LowestPriorityIndex].PageIndex);
        PendingPages[LowestPriorityIndex] = std::move(NewPendingPage);
        PendingPageIndices[PageIndex] = LowestPriorityIndex;
        ReplacedRequestCount++;
        return;
    }

    PendingPageIndices[PageIndex] = static_cast<uint32_t>(PendingPages.size());
    PendingPages.push_back(std::move(NewPendingPage));
}

void FClusterDagStreamingManager::UpdateStreamingWork(uint32_t FrameIndex, uint32_t FrameNumber)
{
    LastFrameInstallCount = 0;
    LastFrameIoIssueCount = 0;
    LastFrameIoCompleteCount = 0;
    LastFrameIoFailCount = 0;
    LastFrameUploadIssueCount = 0;
    LastFrameUploadCompleteCount = 0;
    LastFrameUploadBytes = 0;

    const uint64_t CompletedFenceValue = Device && Device->GetGraphicsQueue()
        ? Device->GetGraphicsQueue()->GetCompletedFenceValue()
        : 0u;
    RetireCompletedUploads(CompletedFenceValue, true);
    PollPageReads();
    IssuePageReads();
    IssuePageUploads(FrameIndex, FrameNumber);

    PendingPages.erase(
        std::remove_if(PendingPages.begin(), PendingPages.end(), [](const FPendingPage& Page)
        {
            return Page.State == EPendingPageState::Failed;
        }),
        PendingPages.end());
    RebuildPendingPageIndices();
}

void FClusterDagStreamingManager::IssuePageReads()
{
    uint32_t IoInFlight = CountIoInFlightPages();
    const uint32_t PagesAwaitingSlots = CountPagesAwaitingSlots();
    uint32_t ReadSlotBudget = PageSlotPool.FreeSlots.size() > PagesAwaitingSlots
        ? static_cast<uint32_t>(PageSlotPool.FreeSlots.size() - PagesAwaitingSlots)
        : 0u;
    if (ReadSlotBudget == 0u)
    {
        return;
    }

    while (IoInFlight < MaxIoInFlight)
    {
        if (ReadSlotBudget == 0u)
        {
            break;
        }

        uint32_t BestIndex = 0xffffffffu;
        for (uint32_t Index = 0u; Index < PendingPages.size(); ++Index)
        {
            const FPendingPage& Candidate = PendingPages[Index];
            if (Candidate.State != EPendingPageState::Pending)
            {
                continue;
            }

            if (BestIndex == 0xffffffffu
                || Candidate.Priority > PendingPages[BestIndex].Priority
                || (Candidate.Priority == PendingPages[BestIndex].Priority
                    && Candidate.FirstRequestSerial < PendingPages[BestIndex].FirstRequestSerial))
            {
                BestIndex = Index;
            }
        }

        if (BestIndex == 0xffffffffu)
        {
            break;
        }

        FPendingPage& PendingPage = PendingPages[BestIndex];
        if (!HasValidPageReadSource(PendingPage.PageIndex))
        {
            PendingPage.State = EPendingPageState::Failed;
            LastFrameIoFailCount++;
            continue;
        }

        const FPageReadSource Source = PageReadSources[PendingPage.PageIndex];
        const uint32_t PageIndex = PendingPage.PageIndex;
        try
        {
            PendingPage.ReadFuture = std::async(std::launch::async, [Source, PageIndex]()
            {
                return ReadPagePayload(Source, PageIndex);
            });
            PendingPage.State = EPendingPageState::IoInFlight;
            LastFrameIoIssueCount++;
            IoInFlight++;
            ReadSlotBudget--;
        }
        catch (...)
        {
            PendingPage.State = EPendingPageState::Failed;
            LastFrameIoFailCount++;
        }
    }
}

void FClusterDagStreamingManager::PollPageReads()
{
    for (FPendingPage& PendingPage : PendingPages)
    {
        if (PendingPage.State != EPendingPageState::IoInFlight || !PendingPage.ReadFuture.valid())
        {
            continue;
        }

        if (PendingPage.ReadFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            continue;
        }

        FPageReadResult Result = PendingPage.ReadFuture.get();
        if (Result.bSucceeded && Result.PageIndex == PendingPage.PageIndex)
        {
            PendingPage.Payload = std::move(Result.Payload);
            PendingPage.State = EPendingPageState::IoComplete;
            LastFrameIoCompleteCount++;
        }
        else
        {
            PendingPage.State = EPendingPageState::Failed;
            LastFrameIoFailCount++;
        }
    }
}

void FClusterDagStreamingManager::IssuePageUploads(uint32_t FrameIndex, uint32_t FrameNumber)
{
    uint32_t UploadCount = 0u;
    uint32_t UploadBytes = 0u;
    while (UploadCount < MaxPageInstallsPerFrame && UploadBytes < MaxPageUploadBytesPerFrame)
    {
        uint32_t BestIndex = 0xffffffffu;
        for (uint32_t Index = 0u; Index < PendingPages.size(); ++Index)
        {
            const FPendingPage& Candidate = PendingPages[Index];
            if (Candidate.State != EPendingPageState::IoComplete)
            {
                continue;
            }

            if (BestIndex == 0xffffffffu
                || Candidate.Priority > PendingPages[BestIndex].Priority
                || (Candidate.Priority == PendingPages[BestIndex].Priority
                    && Candidate.FirstRequestSerial < PendingPages[BestIndex].FirstRequestSerial))
            {
                BestIndex = Index;
            }
        }

        if (BestIndex == 0xffffffffu)
        {
            break;
        }

        FPendingPage& PendingPage = PendingPages[BestIndex];
        std::vector<uint8_t> UploadPayload;
        if (!BuildGpuPagePayload(PendingPage.PageIndex, PendingPage.Payload, UploadPayload))
        {
            PendingPage.State = EPendingPageState::Failed;
            LastFrameIoFailCount++;
            continue;
        }

        const uint32_t PayloadBytes = static_cast<uint32_t>(UploadPayload.size());
        if (PayloadBytes == 0u || PayloadBytes > PageSlotBytes)
        {
            PendingPage.State = EPendingPageState::Failed;
            LastFrameIoFailCount++;
            continue;
        }

        if (UploadCount > 0u && UploadBytes + PayloadBytes > MaxPageUploadBytesPerFrame)
        {
            break;
        }

        uint32_t PhysicalSlotIndex = 0xffffffffu;
        if (!AllocatePhysicalPageSlot(PendingPage.PageIndex, PhysicalSlotIndex))
        {
            PendingPage.State = EPendingPageState::Failed;
            break;
        }

        FGpuUploadingPage UploadingPage;
        UploadingPage.PageIndex = PendingPage.PageIndex;
        UploadingPage.PhysicalSlotIndex = PhysicalSlotIndex;
        UploadingPage.PayloadBytes = PayloadBytes;
        UploadingPage.SubmissionFrameIndex = FrameIndex;
        UploadingPage.RequestFrameNumber = FrameNumber;
        CreateUploadBuffer(
            Device,
            L"ClusterDagStreamingPageUpload_" + std::to_wstring(PendingPage.PageIndex),
            PayloadBytes,
            UploadingPage.UploadBuffer,
            UploadPayload.data());

        if (!UploadingPage.UploadBuffer.IsValid())
        {
            PendingPage.State = EPendingPageState::Failed;
            LastFrameIoFailCount++;
            continue;
        }

        PendingPage.State = EPendingPageState::GpuUploading;
        GpuUploadingPages.push_back(std::move(UploadingPage));
        PendingPages.erase(PendingPages.begin() + BestIndex);
        UploadBytes += PayloadBytes;
        UploadCount++;
        LastFrameUploadIssueCount++;
        LastFrameUploadBytes += PayloadBytes;
    }
}

void FClusterDagStreamingManager::MarkUploadCopySubmitted(uint32_t PageIndex, uint32_t FrameIndex)
{
    for (FGpuUploadingPage& UploadingPage : GpuUploadingPages)
    {
        if (UploadingPage.PageIndex == PageIndex
            && UploadingPage.SubmissionFrameIndex == FrameIndex
            && UploadingPage.FenceValue == 0u)
        {
            UploadingPage.bCopySubmitted = true;
            return;
        }
    }
}

void FClusterDagStreamingManager::AssignSubmittedUploadFence(uint32_t FrameIndex, uint64_t FenceValue)
{
    for (FGpuUploadingPage& UploadingPage : GpuUploadingPages)
    {
        if (UploadingPage.SubmissionFrameIndex == FrameIndex
            && UploadingPage.FenceValue == 0u
            && UploadingPage.bCopySubmitted)
        {
            UploadingPage.FenceValue = FenceValue;
        }
    }
}

void FClusterDagStreamingManager::RetireCompletedUploads(uint64_t CompletedFenceValue, bool bUpdateFrameStats)
{
    if (CompletedFenceValue == 0u || GpuUploadingPages.empty())
    {
        return;
    }

    GpuUploadingPages.erase(
        std::remove_if(GpuUploadingPages.begin(), GpuUploadingPages.end(), [&](const FGpuUploadingPage& UploadingPage)
        {
            if (UploadingPage.FenceValue == 0u || UploadingPage.FenceValue > CompletedFenceValue)
            {
                return false;
            }

            MarkPageResident(UploadingPage.PageIndex, UploadingPage.PhysicalSlotIndex, UploadingPage.RequestFrameNumber);
            if (bUpdateFrameStats)
            {
                LastFrameInstallCount++;
                LastFrameUploadCompleteCount++;
            }
            return true;
        }),
        GpuUploadingPages.end());
}

bool FClusterDagStreamingManager::HasValidPageReadSource(uint32_t PageIndex) const
{
    return PageIndex < PageReadSources.size()
        && PageReadSources[PageIndex].bValid
        && PageReadSources[PageIndex].PayloadBytes <= PageSlotBytes;
}

uint32_t FClusterDagStreamingManager::CountIoInFlightPages() const
{
    uint32_t Count = 0u;
    for (const FPendingPage& PendingPage : PendingPages)
    {
        if (PendingPage.State == EPendingPageState::IoInFlight)
        {
            Count++;
        }
    }
    return Count;
}

uint32_t FClusterDagStreamingManager::CountPagesAwaitingSlots() const
{
    uint32_t Count = 0u;
    for (const FPendingPage& PendingPage : PendingPages)
    {
        if (PendingPage.State == EPendingPageState::IoInFlight
            || PendingPage.State == EPendingPageState::IoComplete)
        {
            Count++;
        }
    }
    return Count;
}

uint32_t FClusterDagStreamingManager::GetIoInFlightCount() const
{
    return CountIoInFlightPages();
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

void FClusterDagStreamingManager::MarkPageResident(uint32_t PageIndex, uint32_t PhysicalSlotIndex, uint32_t FrameNumber)
{
    if (PageIndex >= PageTableEntries.size())
    {
        return;
    }

    FClusterDagPageTableEntry& Entry = PageTableEntries[PageIndex];
    const bool bWasResident = (Entry.Flags & GClusterDagPageResidentFlag) != 0u;
    Entry.PhysicalPageIndex = PhysicalSlotIndex;
    Entry.Flags |= GClusterDagPageResidentFlag;
    Entry.LastUsedFrame = FrameNumber;
    if (!bWasResident)
    {
        ResidentPageCount++;
        bPageTableUploadDirty = true;
    }
}
