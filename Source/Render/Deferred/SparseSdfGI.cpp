#include "SparseSdfGI.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../../World/MeshSection.h"
#include "../ShaderCompiler.h"
#include "DeferredPassContext.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12CommandContext.h"
#include "../../RHI/DX12Device.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <d3dx12.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t kSparseSdfGIBrickGridResolution = 64u;
    constexpr uint32_t kSparseSdfGIBrickVoxelResolution = 8u;
    constexpr uint32_t kSparseSdfGIExactBrickIntervalResolution = kSparseSdfGIBrickVoxelResolution - 1u;
    constexpr uint32_t kSparseSdfGIAtlasResolution = kSparseSdfGIBrickGridResolution * kSparseSdfGIBrickVoxelResolution;
    constexpr uint32_t kSparseSdfGIConstantsDwordCount = 51u;
    constexpr uint32_t kSparseSdfGIMaxBindlessDwordCount = 13u;
    constexpr uint32_t kSparseSdfGIConstantBufferSlotsPerFrame = 8192u;
    constexpr uint32_t kSparseSdfGIConstantBufferStride = 256u;
    constexpr uint32_t kSparseSdfGIHierarchyBottomResolution = 16u;
    constexpr uint32_t kSparseSdfGIHierarchyTopResolution = 4u;
    constexpr uint32_t kSparseSdfGIHierarchyBottomCellSize = 4u;
    constexpr uint32_t kSparseSdfGIHierarchyTopCellSize = 16u;
    constexpr uint32_t kSparseSdfGIGroupSize2D = 8u;
    constexpr uint32_t kSparseSdfGIGroupSize3D = 8u;
    constexpr uint32_t kSparseSdfGIReferenceEmitGroupSize = 64u;
    constexpr uint32_t kSparseSdfGISolveDispatchChunkGroups = 1024u;
    constexpr uint32_t kSparseSdfGIScatterScanGroupSize = 256u;
    constexpr uint32_t kSparseSdfGIScatterSampleGroupSize = 128u;
    constexpr uint32_t kSparseSdfGIScatterBrickGroupSize = 512u;
    constexpr uint32_t kSparseSdfGIDefaultMaxScatterBricks = 64u * 1024u;
    constexpr uint32_t kSparseSdfGIMinScatterBricks = 4u * 1024u;
    constexpr uint32_t kSparseSdfGIMaxScatterBricksLimit = kSparseSdfGIBrickGridResolution * kSparseSdfGIBrickGridResolution * kSparseSdfGIBrickGridResolution;
    constexpr float kSparseSdfGICascadeSceneRadiusMargin = 1.10f;
    constexpr float kSparseSdfGIMinVoxelSize = 0.001f;
    constexpr float kSparseSdfGISurfaceThicknessVoxels = 1.5f;
    constexpr float kSparseSdfGISurfaceHitThresholdVoxels = 0.75f;
    constexpr uint32_t kSparseSdfGIDefaultMaxBrickTriangleReferences = 8u * 1024u * 1024u;
    constexpr uint32_t kSparseSdfGIMinBrickTriangleReferences = 1u * 1024u * 1024u;
    constexpr uint32_t kSparseSdfGIMaxBrickTriangleReferencesLimit = 32u * 1024u * 1024u;
    constexpr uint64_t kSparseSdfGIHashOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kSparseSdfGIHashPrime = 1099511628211ull;

    uint32_t GetBrickMapElementCount()
    {
        return kSparseSdfGIBrickGridResolution * kSparseSdfGIBrickGridResolution * kSparseSdfGIBrickGridResolution;
    }

    DXGI_FORMAT SelectSparseSdfGIAtlasFormat(FDX12Device* Device)
    {
        constexpr D3D12_FORMAT_SUPPORT1 RequiredFlags = static_cast<D3D12_FORMAT_SUPPORT1>(
            D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
        if (CheckFormatSupport(Device, DXGI_FORMAT_R16_UNORM, RequiredFlags))
        {
            LogInfo("SparseSdfGI atlas format selected: DXGI_FORMAT_R16_UNORM (typed UAV supported)");
            return DXGI_FORMAT_R16_UNORM;
        }

        if (CheckFormatSupport(Device, DXGI_FORMAT_R32_FLOAT, RequiredFlags))
        {
            LogWarning("SparseSdfGI atlas format fallback: DXGI_FORMAT_R16_UNORM typed UAV unsupported; using DXGI_FORMAT_R32_FLOAT");
            return DXGI_FORMAT_R32_FLOAT;
        }

        LogError("SparseSdfGI requires a Texture3D typed UAV format for the SDF atlas.");
        return DXGI_FORMAT_UNKNOWN;
    }

    void HashBytes(uint64_t& Hash, const void* Data, size_t Size)
    {
        const uint8_t* Bytes = static_cast<const uint8_t*>(Data);
        for (size_t Index = 0; Index < Size; ++Index)
        {
            Hash ^= static_cast<uint64_t>(Bytes[Index]);
            Hash *= kSparseSdfGIHashPrime;
        }
    }

    template <typename T>
    void HashValue(uint64_t& Hash, const T& Value)
    {
        HashBytes(Hash, &Value, sizeof(T));
    }

    void HashFloat3(uint64_t& Hash, const DirectX::XMFLOAT3& Value)
    {
        HashValue(Hash, Value.x);
        HashValue(Hash, Value.y);
        HashValue(Hash, Value.z);
    }

    void HashFloat4x4(uint64_t& Hash, const DirectX::XMFLOAT4X4& Value)
    {
        HashBytes(Hash, &Value, sizeof(Value));
    }

    struct FScatterJobGpu
    {
        DirectX::XMFLOAT4 P0{};
        DirectX::XMFLOAT4 P1{};
        DirectX::XMFLOAT4 P2{};
        DirectX::XMUINT4 PairMinSampleCount{};
        DirectX::XMUINT4 PairCount{};
    };

    uint32_t GetScatterJobGroupCount(uint32_t MaxJobCount)
    {
        return AlignDispatch((std::max)(MaxJobCount, 1u), kSparseSdfGIScatterScanGroupSize);
    }

    uint32_t GetScatterJobGroup2Count(uint32_t MaxJobCount)
    {
        return AlignDispatch(GetScatterJobGroupCount(MaxJobCount), kSparseSdfGIScatterScanGroupSize);
    }

    FRGBufferDesc CreateScatterJobsDesc(uint32_t MaxJobCount)
    {
        return CreateRWStructuredBufferDesc<FScatterJobGpu>((std::max)(MaxJobCount, 1u));
    }

    FRGBufferDesc CreateScatterJobOffsetsDesc(uint32_t MaxJobCount)
    {
        return CreateRWStructuredBufferDesc<uint32_t>((std::max)(MaxJobCount, 1u));
    }

    FRGBufferDesc CreateScatterGroupDesc(uint32_t Count)
    {
        return CreateRWStructuredBufferDesc<uint32_t>((std::max)(Count, 1u));
    }

    FRGBufferDesc CreateScatterTouchedBricksDesc()
    {
        return CreateRWStructuredBufferDesc<uint32_t>(GetBrickMapElementCount());
    }

    FRGBufferDesc CreateScatterBrickListDesc(uint32_t MaxScatterBricks)
    {
        return CreateRWStructuredBufferDesc<uint32_t>((std::max)(MaxScatterBricks, 1u));
    }

    FRGBufferDesc CreateScatterBrickSdfDesc(uint32_t MaxScatterBricks)
    {
        return CreateRWStructuredBufferDesc<uint32_t>(static_cast<size_t>((std::max)(MaxScatterBricks, 1u)) * kSparseSdfGIScatterBrickGroupSize);
    }

    FRGBufferDesc CreateScatterCountersDesc()
    {
        return CreateRWStructuredBufferDesc<uint32_t>(8u);
    }

    FRGBufferDesc CreateScatterDispatchArgsDesc()
    {
        return CreateRawBufferDesc(sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    }

    uint32_t GetTraceHierarchyBottomNodeCount()
    {
        return kSparseSdfGIHierarchyBottomResolution * kSparseSdfGIHierarchyBottomResolution * kSparseSdfGIHierarchyBottomResolution;
    }

    uint32_t GetTraceHierarchyTopNodeCount()
    {
        return kSparseSdfGIHierarchyTopResolution * kSparseSdfGIHierarchyTopResolution * kSparseSdfGIHierarchyTopResolution;
    }

    struct FTraceHierarchyNodeGpu
    {
        uint32_t MinPacked = 0;
        uint32_t MaxPacked = 0;
        uint32_t Flags = 0;
        uint32_t Reserved = 0;
    };

    FRGBufferDesc CreateTraceHierarchyBottomDesc()
    {
        return CreateRWStructuredBufferDesc<FTraceHierarchyNodeGpu>(GetTraceHierarchyBottomNodeCount());
    }

    FRGBufferDesc CreateTraceHierarchyTopDesc()
    {
        return CreateRWStructuredBufferDesc<FTraceHierarchyNodeGpu>(GetTraceHierarchyTopNodeCount());
    }

    struct FBrickRadianceGpu
    {
        DirectX::XMFLOAT4 Radiance{ 0.0f, 0.0f, 0.0f, 0.0f };
    };

    struct FBrickRadianceAccumGpu
    {
        uint32_t X = 0;
        uint32_t Y = 0;
        uint32_t Z = 0;
        uint32_t W = 0;
    };

    FRGBufferDesc CreateBrickRadianceDesc()
    {
        return CreateRWStructuredBufferDesc<FBrickRadianceGpu>(GetBrickMapElementCount());
    }

    FRGBufferDesc CreateBrickRadianceAccumDesc()
    {
        return CreateRWStructuredBufferDesc<FBrickRadianceAccumGpu>(GetBrickMapElementCount());
    }

    struct FScreenProbeHeaderGpu
    {
        DirectX::XMFLOAT4 WorldPositionDepth{};
        DirectX::XMFLOAT4 NormalValid{};
        uint32_t PixelX = 0;
        uint32_t PixelY = 0;
        uint32_t PrevProbeIndex = 0;
        uint32_t PrevProbeValid = 0;
    };

    FRGBufferDesc CreateScreenProbeHeaderDesc(uint32_t ProbeCount)
    {
        return CreateRWStructuredBufferDesc<FScreenProbeHeaderGpu>((std::max)(ProbeCount, 1u));
    }

    FRGBufferDesc CreateScreenProbeSHDesc(uint32_t ProbeCount)
    {
        return CreateRWStructuredBufferDesc<DirectX::XMUINT4>((std::max)(ProbeCount, 1u));
    }

    FRGBufferDesc CreateScreenProbeVarianceDesc(uint32_t ProbeCount)
    {
        return CreateRWStructuredBufferDesc<DirectX::XMFLOAT4>((std::max)(ProbeCount, 1u));
    }

    struct FScreenProbeHistoryGpu
    {
        DirectX::XMFLOAT4 WorldPositionCount{};
        DirectX::XMFLOAT4 NormalDepth{};
        DirectX::XMUINT4 PackedSH{};
    };

    FRGBufferDesc CreateScreenProbeHistoryDesc(uint32_t ProbeCount)
    {
        return CreateRWStructuredBufferDesc<FScreenProbeHistoryGpu>((std::max)(ProbeCount, 1u));
    }

    struct FSparseSdfGIConstants
    {
        uint32_t OutputWidth = 0;
        uint32_t OutputHeight = 0;
        uint32_t AtlasResolution = kSparseSdfGIAtlasResolution;
        uint32_t BrickGridResolution = kSparseSdfGIBrickGridResolution;
        uint32_t BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        uint32_t Reserved0 = 0;
        uint32_t FrameIndex = 0;
        uint32_t DebugMode = 0;
        uint32_t Enabled = 0;
        uint32_t TraceHalfResolution = 0;
        uint32_t ModelTriangleCount = 0;
        uint32_t ModelDrawIndexStart = 0;
        uint32_t ModelDrawIndexCount = 0;
        uint32_t MaxBrickTriangleReferences = kSparseSdfGIDefaultMaxBrickTriangleReferences;
        float Intensity = 1.0f;
        float MaxTraceDistance = 64.0f;
        DirectX::XMFLOAT3 CascadeMin{ 0.0f, 0.0f, 0.0f };
        float VoxelSize = 0.25f;
        DirectX::XMFLOAT3 CascadeExtent{ 1.0f, 1.0f, 1.0f };
        float SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        DirectX::XMFLOAT4X4 World{};
        float BounceStrength = 1.0f;
        uint32_t ProbeTileSize = 8u;
        uint32_t ProbeCountX = 1u;
        uint32_t ProbeCountY = 1u;
        uint32_t ProbeRaysPerProbe = 16u;
        uint32_t ProbeDebugMode = 0u;
        uint32_t ProbeHistoryValid = 0u;
        float SurfaceHitThresholdVoxels = kSparseSdfGISurfaceHitThresholdVoxels;
        uint32_t TrianglePoolCapacity = 0;
        uint32_t BuildWorkOffset = 0u;
        uint32_t UseHierarchicalTrace = 1u;
    };
    static_assert(offsetof(FSparseSdfGIConstants, CascadeMin) == 16u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, CascadeExtent) == 20u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, World) == 24u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, BounceStrength) == 40u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, ProbeRaysPerProbe) == 44u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, TrianglePoolCapacity) == 48u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, UseHierarchicalTrace) == 50u * sizeof(uint32_t));
    static_assert(sizeof(FSparseSdfGIConstants) / sizeof(uint32_t) == kSparseSdfGIConstantsDwordCount);

    struct FSparseSdfGIReferenceInitBindlessConstants
    {
        uint32_t SdfAtlasUavIndex = UINT32_MAX;
        uint32_t CascadeBrickMapUavIndex = UINT32_MAX;
        uint32_t BrickMetadataUavIndex = UINT32_MAX;
        uint32_t ReferenceHeadsUavIndex = UINT32_MAX;
        uint32_t ReferenceCountersUavIndex = UINT32_MAX;
        uint32_t ReferenceStatsUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIReferenceEmitBindlessConstants
    {
        uint32_t PositionBufferIndex = UINT32_MAX;
        uint32_t IndexBufferIndex = UINT32_MAX;
        uint32_t TrianglePoolUavIndex = UINT32_MAX;
        uint32_t ReferenceHeadsUavIndex = UINT32_MAX;
        uint32_t ReferenceNodesUavIndex = UINT32_MAX;
        uint32_t ReferenceCountersUavIndex = UINT32_MAX;
        uint32_t OccupiedBrickListUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIReferenceSolveBindlessConstants
    {
        uint32_t SdfAtlasUavIndex = UINT32_MAX;
        uint32_t BrickMetadataUavIndex = UINT32_MAX;
        uint32_t TrianglePoolSrvIndex = UINT32_MAX;
        uint32_t ReferenceHeadsSrvIndex = UINT32_MAX;
        uint32_t ReferenceNodesSrvIndex = UINT32_MAX;
        uint32_t ReferenceCountersSrvIndex = UINT32_MAX;
        uint32_t OccupiedBrickListSrvIndex = UINT32_MAX;
        uint32_t ReferenceStatsUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterInitBindlessConstants
    {
        uint32_t SdfAtlasUavIndex = UINT32_MAX;
        uint32_t CascadeBrickMapUavIndex = UINT32_MAX;
        uint32_t BrickMetadataUavIndex = UINT32_MAX;
        uint32_t ScatterTouchedBricksUavIndex = UINT32_MAX;
        uint32_t ScatterCountersUavIndex = UINT32_MAX;
        uint32_t ReferenceStatsUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterPrepareBindlessConstants
    {
        uint32_t PositionBufferIndex = UINT32_MAX;
        uint32_t IndexBufferIndex = UINT32_MAX;
        uint32_t ScatterJobsUavIndex = UINT32_MAX;
        uint32_t ScatterCountersUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterScanJobsBindlessConstants
    {
        uint32_t ScatterJobsSrvIndex = UINT32_MAX;
        uint32_t ScatterJobOffsetsUavIndex = UINT32_MAX;
        uint32_t ScatterGroupSumsUavIndex = UINT32_MAX;
        uint32_t ScatterCountersSrvIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterScanGroupsBindlessConstants
    {
        uint32_t ScatterInputGroupSumsSrvIndex = UINT32_MAX;
        uint32_t ScatterGroupOffsetsUavIndex = UINT32_MAX;
        uint32_t ScatterOutputGroupSumsUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterScanGroup2BindlessConstants
    {
        uint32_t ScatterGroup2SumsSrvIndex = UINT32_MAX;
        uint32_t ScatterGroup2OffsetsUavIndex = UINT32_MAX;
        uint32_t ScatterCountersUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterAddOffsetsBindlessConstants
    {
        uint32_t ScatterJobOffsetsUavIndex = UINT32_MAX;
        uint32_t ScatterGroupOffsetsSrvIndex = UINT32_MAX;
        uint32_t ScatterGroup2OffsetsSrvIndex = UINT32_MAX;
        uint32_t ScatterCountersSrvIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterBuildArgsBindlessConstants
    {
        uint32_t ScatterCountersSrvIndex = UINT32_MAX;
        uint32_t ScatterDispatchArgsUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterMarkTouchedBindlessConstants
    {
        uint32_t ScatterJobsSrvIndex = UINT32_MAX;
        uint32_t ScatterJobOffsetsSrvIndex = UINT32_MAX;
        uint32_t ScatterTouchedBricksUavIndex = UINT32_MAX;
        uint32_t ScatterCountersSrvIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterAllocateBindlessConstants
    {
        uint32_t ScatterTouchedBricksSrvIndex = UINT32_MAX;
        uint32_t CascadeBrickMapUavIndex = UINT32_MAX;
        uint32_t ScatterBrickListUavIndex = UINT32_MAX;
        uint32_t ScatterCountersUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterClearBrickBindlessConstants
    {
        uint32_t ScatterBrickSdfUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterSamplesBindlessConstants
    {
        uint32_t ScatterJobsSrvIndex = UINT32_MAX;
        uint32_t ScatterJobOffsetsSrvIndex = UINT32_MAX;
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
        uint32_t ScatterBrickSdfUavIndex = UINT32_MAX;
        uint32_t ScatterCountersSrvIndex = UINT32_MAX;
    };

    struct FSparseSdfGIScatterFinalizeBindlessConstants
    {
        uint32_t SdfAtlasUavIndex = UINT32_MAX;
        uint32_t BrickMetadataUavIndex = UINT32_MAX;
        uint32_t ScatterBrickListSrvIndex = UINT32_MAX;
        uint32_t ScatterBrickSdfSrvIndex = UINT32_MAX;
        uint32_t ScatterCountersSrvIndex = UINT32_MAX;
        uint32_t ReferenceStatsUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIBuildTraceHierarchyBottomBindlessConstants
    {
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
        uint32_t BrickMetadataSrvIndex = UINT32_MAX;
        uint32_t TraceHierarchyBottomUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIBuildTraceHierarchyTopBindlessConstants
    {
        uint32_t TraceHierarchyBottomSrvIndex = UINT32_MAX;
        uint32_t TraceHierarchyTopUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIReferenceStatsPresentBindlessConstants
    {
        uint32_t ReferenceStatsSrvIndex = UINT32_MAX;
        uint32_t DebugPrintStatsUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGITraceBindlessConstants
    {
        uint32_t SdfAtlasSrvIndex = UINT32_MAX;
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
        uint32_t BrickMetadataSrvIndex = UINT32_MAX;
        uint32_t DiffuseGIUavIndex = UINT32_MAX;
        uint32_t DepthIndex = UINT32_MAX;
        uint32_t GBufferAIndex = UINT32_MAX;
        uint32_t EnvironmentCubeIndex = UINT32_MAX;
        uint32_t LinearClampSamplerIndex = UINT32_MAX;
        uint32_t BrickRadianceSrvIndex = UINT32_MAX;
        uint32_t InputSHUavIndex = UINT32_MAX;
        uint32_t VarianceUavIndex = UINT32_MAX;
        uint32_t TraceHierarchyBottomSrvIndex = UINT32_MAX;
        uint32_t TraceHierarchyTopSrvIndex = UINT32_MAX;
    };

    struct FSparseSdfGIProbeSpawnBindlessConstants
    {
        uint32_t DepthIndex = UINT32_MAX;
        uint32_t GBufferAIndex = UINT32_MAX;
        uint32_t ProbeHeaderUavIndex = UINT32_MAX;
        uint32_t JitterEnabled = 0u;
        uint32_t BlueNoiseSobolTextureIndex = UINT32_MAX;
        uint32_t BlueNoiseScramblingRankingTextureIndex = UINT32_MAX;
        uint32_t VelocityIndex = UINT32_MAX;
    };

    struct FSparseSdfGIProbeTraceBindlessConstants
    {
        uint32_t SdfAtlasSrvIndex = UINT32_MAX;
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
        uint32_t BrickMetadataSrvIndex = UINT32_MAX;
        uint32_t BrickRadianceSrvIndex = UINT32_MAX;
        uint32_t EnvironmentCubeIndex = UINT32_MAX;
        uint32_t LinearClampSamplerIndex = UINT32_MAX;
        uint32_t ProbeHeaderSrvIndex = UINT32_MAX;
        uint32_t ProbeSHUavIndex = UINT32_MAX;
        uint32_t ProbeVarianceUavIndex = UINT32_MAX;
        uint32_t ProbeHistoryReadSrvIndex = UINT32_MAX;
        uint32_t ProbeHistoryWriteUavIndex = UINT32_MAX;
        uint32_t TraceHierarchyBottomSrvIndex = UINT32_MAX;
        uint32_t TraceHierarchyTopSrvIndex = UINT32_MAX;
    };

    struct FSparseSdfGIProbeInterpolateBindlessConstants
    {
        uint32_t DepthIndex = UINT32_MAX;
        uint32_t GBufferAIndex = UINT32_MAX;
        uint32_t DiffuseGIUavIndex = UINT32_MAX;
        uint32_t ProbeHeaderSrvIndex = UINT32_MAX;
        uint32_t ProbeSHSrvIndex = UINT32_MAX;
        uint32_t ProbeVarianceSrvIndex = UINT32_MAX;
        uint32_t InputSHUavIndex = UINT32_MAX;
        uint32_t VarianceUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIRadianceClearBindlessConstants
    {
        uint32_t BrickRadianceAccumUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIRadianceInjectBindlessConstants
    {
        uint32_t DepthIndex = UINT32_MAX;
        uint32_t GBufferAIndex = UINT32_MAX;
        uint32_t GBufferBIndex = UINT32_MAX;
        uint32_t GBufferCIndex = UINT32_MAX;
        uint32_t BrickRadianceAccumUavIndex = UINT32_MAX;
        uint32_t ShadowMaskIndex = UINT32_MAX;
        uint32_t ShadowMaskEnabled = 0u;
        uint32_t BrickIrradianceReadIndex = UINT32_MAX;
    };

    struct FSparseSdfGIIrradianceAccumulateBindlessConstants
    {
        uint32_t DepthIndex = UINT32_MAX;
        uint32_t DiffuseGIIndex = UINT32_MAX;
        uint32_t BrickIrradianceAccumUavIndex = UINT32_MAX;
    };

    struct FSparseSdfGIRadianceResolveBindlessConstants
    {
        uint32_t BrickRadianceAccumSrvIndex = UINT32_MAX;
        uint32_t BrickRadianceHistorySrvIndex = UINT32_MAX;
        uint32_t BrickRadianceUavIndex = UINT32_MAX;
        uint32_t RadianceHistoryValid = 0u;
    };

    struct FBrickMetadataGpu
    {
        uint32_t X = 0;
        uint32_t Y = 0;
        uint32_t Z = 0;
        uint32_t W = 0;
    };
}

bool FSparseSdfGI::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    bPersistentInputsValid = false;
    if (!CreateRootSignature(Device) || !CreateDispatchCommandSignature(Device) || !CreatePipelines(Device))
    {
        LogWarning("Deferred renderer: SparseSdfGI pipeline creation failed; feature disabled.");
        bEnabled = false;
        RootSignature.Reset();
        DispatchCommandSignature.Reset();
        ScatterInitPipeline.Reset();
        ScatterPreparePipeline.Reset();
        ScatterScanJobsPipeline.Reset();
        ScatterScanGroupsPipeline.Reset();
        ScatterScanGroup2Pipeline.Reset();
        ScatterAddOffsetsPipeline.Reset();
        ScatterBuildSampleArgsPipeline.Reset();
        ScatterMarkTouchedPipeline.Reset();
        ScatterAllocateBricksPipeline.Reset();
        ScatterBuildBrickArgsPipeline.Reset();
        ScatterClearBrickStoragePipeline.Reset();
        ScatterSdfSamplesPipeline.Reset();
        ScatterFinalizeBricksPipeline.Reset();
        BuildTraceHierarchyBottomPipeline.Reset();
        BuildTraceHierarchyTopPipeline.Reset();
        ReferenceStatsPresentPipeline.Reset();
        RadianceClearPipeline.Reset();
        RadianceInjectPipeline.Reset();
        IrradianceAccumulatePipeline.Reset();
        RadianceResolvePipeline.Reset();
        ProbeSpawnPipeline.Reset();
        ProbeTracePipeline.Reset();
        ProbeTraceDirectionalPipeline.Reset();
        ProbeInterpolatePipeline.Reset();
        DebugTracePipeline.Reset();
        DiffuseTracePipeline.Reset();
        return true;
    }

    return true;
}

bool FSparseSdfGI::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    (void)Owner;
    bPersistentInputsValid = false;
    InvalidateCache();
    return CreateResources(Device, Width, Height, Owner.GetFramesInFlight());
}

bool FSparseSdfGI::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    (void)Device;
    return RefreshPersistentInputValidation();
}

void FSparseSdfGI::ApplyConfig(const FRendererConfig& Config)
{
    const bool bPreviousEnabled = bEnabled;
    const bool bPreviousRadianceTemporalReuse = bEnableRadianceTemporalReuse;
    const bool bPreviousProbeTemporalReuse = bProbeTemporalReuse;
    const bool bPreviousUseScreenProbes = bUseScreenProbes;
    const bool bPreviousProbeDirectionalSH = bProbeDirectionalSH;
    const bool bPreviousMultiBounce = bMultiBounce;
    const uint32_t PreviousProbeTileSize = ProbeTileSize;
    const uint32_t NewCascadeCount = std::clamp(Config.SparseSdfGICascadeCount, 1u, 1u);
    const float NewBaseVoxelSize = Config.SparseSdfGIBaseVoxelSize;
    const float NewCascadeScale = (std::max)(Config.SparseSdfGICascadeScale, 1.01f);
    const uint32_t NewMaxBrickTriangleReferences = std::clamp(Config.SparseSdfGIMaxBrickTriangleReferences, kSparseSdfGIMinBrickTriangleReferences, kSparseSdfGIMaxBrickTriangleReferencesLimit);
    const uint32_t NewMaxScatterBricks = std::clamp(Config.SparseSdfGIMaxScatterBricks, kSparseSdfGIMinScatterBricks, kSparseSdfGIMaxScatterBricksLimit);
    const bool bBuildSettingsChanged =
        CascadeCount != NewCascadeCount ||
        BaseVoxelSize != NewBaseVoxelSize ||
        CascadeScale != NewCascadeScale ||
        MaxBrickTriangleReferences != NewMaxBrickTriangleReferences ||
        MaxScatterBricks != NewMaxScatterBricks;

    bEnabled = Config.DiffuseGISource == EDiffuseGISource::SparseSdfGI;
    DebugMode = static_cast<ESparseSdfGIDebugMode>(std::clamp(Config.SparseSdfGIDebugMode, 0u, 9u));
    CascadeCount = NewCascadeCount;
    BaseVoxelSize = NewBaseVoxelSize;
    CascadeScale = NewCascadeScale;
    bTraceHalfResolution = Config.bSparseSdfGITraceHalfResolution;
    bUseHierarchicalTrace = Config.bSparseSdfGIUseHierarchicalTrace;
    Intensity = (std::max)(0.0f, Config.SparseSdfGIIntensity);
    BounceStrength = (std::max)(0.0f, Config.SparseSdfGIBounceStrength);
    bEnableRadianceTemporalReuse = Config.bSparseSdfGIEnableRadianceTemporalReuse;
    bUseScreenProbes = Config.bSparseSdfGIUseScreenProbes;
    ProbeTileSize = std::clamp(Config.SparseSdfGIProbeTileSize, 4u, 16u);
    ProbeRaysPerProbe = std::clamp(Config.SparseSdfGIProbeRaysPerProbe, 4u, 64u);
    ProbeDebugMode = std::clamp(Config.SparseSdfGIProbeDebugMode, 0u, 6u);
    bProbeTemporalReuse = Config.bSparseSdfGIProbeTemporalReuse;
    bProbeDirectionalSH = Config.bSparseSdfGIProbeDirectionalSH;
    bProbeSpawnJitter = Config.bSparseSdfGIProbeSpawnJitter;
    bProbeMotionReproject = Config.bSparseSdfGIProbeMotionReproject;
    bMultiBounce = Config.bSparseSdfGIMultiBounce;
    MultiBounceStrength = (std::max)(0.0f, Config.SparseSdfGIMultiBounceStrength);
    SurfaceHitThresholdVoxels = std::clamp(Config.SparseSdfGISurfaceHitThresholdVoxels, 0.05f, kSparseSdfGISurfaceHitThresholdVoxels);
    MaxBrickTriangleReferences = NewMaxBrickTriangleReferences;
    MaxScatterBricks = NewMaxScatterBricks;

    if ((!bPreviousEnabled && bEnabled) || bBuildSettingsChanged)
    {
        InvalidateCache();
    }
    else if (bPreviousRadianceTemporalReuse != bEnableRadianceTemporalReuse)
    {
        std::fill(BrickRadianceHistoryValid.begin(), BrickRadianceHistoryValid.end(), false);
        std::fill(PendingBrickRadianceWrite.begin(), PendingBrickRadianceWrite.end(), false);
    }

    if (bPreviousProbeTemporalReuse != bProbeTemporalReuse ||
        bPreviousUseScreenProbes != bUseScreenProbes ||
        bPreviousProbeDirectionalSH != bProbeDirectionalSH ||
        PreviousProbeTileSize != ProbeTileSize)
    {
        std::fill(ProbeHistoryValid.begin(), ProbeHistoryValid.end(), false);
        std::fill(PendingProbeHistoryWrite.begin(), PendingProbeHistoryWrite.end(), false);
    }

    if (bPreviousMultiBounce != bMultiBounce)
    {
        std::fill(BrickIrradianceHistoryValid.begin(), BrickIrradianceHistoryValid.end(), false);
        std::fill(PendingBrickIrradianceWrite.begin(), PendingBrickIrradianceWrite.end(), false);
    }
}

void FSparseSdfGI::ForceInvalidateCache() const
{
    InvalidateCache();
}

void FSparseSdfGI::OnFrameFenceSignaled(uint32_t FrameIndex)
{
    if (!bPersistentInputsValid)
    {
        return;
    }

    if (!BrickRadiance.empty())
    {
        const uint32_t Slot = FrameIndex % static_cast<uint32_t>(BrickRadiance.size());
        if (Slot < PendingBrickRadianceWrite.size() && Slot < BrickRadianceHistoryValid.size() && PendingBrickRadianceWrite[Slot])
        {
            BrickRadianceHistoryValid[Slot] = true;
            PendingBrickRadianceWrite[Slot] = false;
        }
    }

    if (!BrickIrradiance.empty())
    {
        const uint32_t Slot = FrameIndex % static_cast<uint32_t>(BrickIrradiance.size());
        if (Slot < PendingBrickIrradianceWrite.size() && Slot < BrickIrradianceHistoryValid.size() && PendingBrickIrradianceWrite[Slot])
        {
            BrickIrradianceHistoryValid[Slot] = true;
            PendingBrickIrradianceWrite[Slot] = false;
        }
    }

    if (!ProbeHistory.empty())
    {
        const uint32_t ProbeSlot = FrameIndex % static_cast<uint32_t>(ProbeHistory.size());
        if (ProbeSlot < PendingProbeHistoryWrite.size() && ProbeSlot < ProbeHistoryValid.size() && PendingProbeHistoryWrite[ProbeSlot])
        {
            ProbeHistoryValid[ProbeSlot] = true;
            PendingProbeHistoryWrite[ProbeSlot] = false;
        }
    }
}

void FSparseSdfGI::ResetSparseConstantCursor(uint32_t FrameIndex) const
{
    if (SparseConstantCursors.empty())
    {
        return;
    }

    SparseConstantCursors[FrameIndex % static_cast<uint32_t>(SparseConstantCursors.size())] = 0u;
}

bool FSparseSdfGI::BindSparseConstants(FDeferredRenderer& Owner, ID3D12GraphicsCommandList* CommandList, const void* Constants, size_t ConstantsSize) const
{
    if (!CommandList || !Constants || ConstantsSize == 0u || ConstantsSize > kSparseSdfGIConstantBufferStride || SparseConstantBuffers.empty() || SparseConstantCursors.empty())
    {
        return false;
    }

    const uint32_t FrameSlot = Owner.GetFrameIndex() % static_cast<uint32_t>(SparseConstantBuffers.size());
    if (FrameSlot >= SparseConstantCursors.size())
    {
        return false;
    }

    uint32_t& Cursor = SparseConstantCursors[FrameSlot];
    if (Cursor >= kSparseSdfGIConstantBufferSlotsPerFrame)
    {
        LogError("SparseSdfGI constants skipped: per-frame constant buffer slot budget exceeded.");
        return false;
    }

    FMappedUploadBuffer& Buffer = SparseConstantBuffers[FrameSlot];
    if (!Buffer.IsValid())
    {
        return false;
    }

    const uint64_t Offset = static_cast<uint64_t>(Cursor) * kSparseSdfGIConstantBufferStride;
    if (Offset + ConstantsSize > Buffer.Size)
    {
        LogError("SparseSdfGI constants skipped: constant buffer offset exceeded allocation.");
        return false;
    }

    std::memset(Buffer.MappedData + Offset, 0, kSparseSdfGIConstantBufferStride);
    std::memcpy(Buffer.MappedData + Offset, Constants, ConstantsSize);
    CommandList->SetComputeRootConstantBufferView(1, Buffer.GetGPUVirtualAddress() + Offset);
    ++Cursor;
    return true;
}

void FSparseSdfGI::ImportPersistentResources(FDeferredPassContext& Context)
{
    ResetSparseConstantCursor(Context.FrameIndex);

    FRenderGraph& Graph = Context.Graph;
    FSparseSdfGIFrameResources& Resources = Context.Resources.SparseSdfGI;

    Resources.SdfAtlasHandle = ImportBindlessTexture(Graph, "SparseSdfGI SDF Atlas", SdfAtlas);
    Resources.ScatterJobsHandle = {};
    Resources.ScatterJobOffsetsHandle = {};
    Resources.ScatterJobGroupSumsHandle = {};
    Resources.ScatterJobGroupOffsetsHandle = {};
    Resources.ScatterJobGroup2SumsHandle = {};
    Resources.ScatterJobGroup2OffsetsHandle = {};
    Resources.ScatterTouchedBricksHandle = {};
    Resources.ScatterBrickListHandle = {};
    Resources.ScatterBrickSdfHandle = {};
    Resources.ScatterCountersHandle = {};
    Resources.ScatterSampleDispatchArgsHandle = {};
    Resources.ScatterBrickDispatchArgsHandle = {};
    Resources.ReferenceCounterStatsHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Reference Counter Stats", ReferenceCounterStats);
    Resources.TraceHierarchyBottomHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Trace Hierarchy Bottom", TraceHierarchyBottom);
    Resources.TraceHierarchyTopHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Trace Hierarchy Top", TraceHierarchyTop);
    Resources.BrickRadianceAccumHandle = {};
    Resources.BrickIrradianceAccumHandle = {};
    Resources.DiffuseGIInputSHHandle = {};
    Resources.DiffuseGIVarianceHandle = {};
    Resources.CascadeBrickMapHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Cascade Brick Map", CascadeBrickMap);
    Resources.BrickMetadataHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Metadata", BrickMetadata);
    Resources.BrickRadianceReadHandle = {};
    Resources.BrickRadianceWriteHandle = {};
    const uint32_t FrameCount = static_cast<uint32_t>(BrickRadiance.size());
    if (FrameCount > 0u)
    {
        CurrentBrickRadianceWriteSlot = Context.FrameIndex % FrameCount;
        CurrentBrickRadianceReadSlot = (Context.FrameIndex + FrameCount - 1u) % FrameCount;
        Resources.BrickRadianceReadHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Radiance Read", BrickRadiance[CurrentBrickRadianceReadSlot]);
        Resources.BrickRadianceWriteHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Radiance Write", BrickRadiance[CurrentBrickRadianceWriteSlot]);
    }
    Resources.BrickIrradianceReadHandle = {};
    Resources.BrickIrradianceWriteHandle = {};
    const uint32_t IrradianceFrameCount = static_cast<uint32_t>(BrickIrradiance.size());
    if (IrradianceFrameCount > 0u)
    {
        CurrentBrickIrradianceWriteSlot = Context.FrameIndex % IrradianceFrameCount;
        CurrentBrickIrradianceReadSlot = (Context.FrameIndex + IrradianceFrameCount - 1u) % IrradianceFrameCount;
        Resources.BrickIrradianceReadHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Irradiance Read", BrickIrradiance[CurrentBrickIrradianceReadSlot]);
        Resources.BrickIrradianceWriteHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Irradiance Write", BrickIrradiance[CurrentBrickIrradianceWriteSlot]);
    }
    Resources.ProbeHistoryReadHandle = {};
    Resources.ProbeHistoryWriteHandle = {};
    const uint32_t ProbeFrameCount = static_cast<uint32_t>(ProbeHistory.size());
    if (ProbeFrameCount > 0u)
    {
        CurrentProbeHistoryWriteSlot = Context.FrameIndex % ProbeFrameCount;
        CurrentProbeHistoryReadSlot = (Context.FrameIndex + ProbeFrameCount - 1u) % ProbeFrameCount;
        Resources.ProbeHistoryReadHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Probe History Read", ProbeHistory[CurrentProbeHistoryReadSlot]);
        Resources.ProbeHistoryWriteHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Probe History Write", ProbeHistory[CurrentProbeHistoryWriteSlot]);
    }
    Resources.DiffuseGIHandle = ImportBindlessTexture(Graph, "SparseSdfGI Diffuse", DiffuseGI);
}

void FSparseSdfGI::AddSdfUpdatePasses(FDeferredPassContext& Context) const
{
    if (!bEnabled || !bPersistentInputsValid)
    {
        return;
    }

    const FCascadeBounds Bounds = ComputeCascadeBounds(Context.Owner);
    uint32_t StaticCandidateCount = 0;
    const uint64_t SceneSignature = ComputeStaticSceneSignature(Context.Owner, StaticCandidateCount);
    const uint64_t BuildSettingsSignature = ComputeBuildSettingsSignature(Bounds);
    if (bSdfCacheValid &&
        CachedSceneSignature == SceneSignature &&
        CachedBuildSettingsSignature == BuildSettingsSignature &&
        CachedStaticCandidateCount == StaticCandidateCount)
    {
        AddReferenceStatsPresentPass(Context);
        return;
    }

    std::fill(BrickRadianceHistoryValid.begin(), BrickRadianceHistoryValid.end(), false);
    std::fill(PendingBrickRadianceWrite.begin(), PendingBrickRadianceWrite.end(), false);
    std::fill(BrickIrradianceHistoryValid.begin(), BrickIrradianceHistoryValid.end(), false);
    std::fill(PendingBrickIrradianceWrite.begin(), PendingBrickIrradianceWrite.end(), false);
    std::fill(ProbeHistoryValid.begin(), ProbeHistoryValid.end(), false);
    std::fill(PendingProbeHistoryWrite.begin(), PendingProbeHistoryWrite.end(), false);

    uint32_t DrawSectionIndex = 0;
    auto DrawSections = Context.Owner.GetWorld().BuildSectionList();
    uint32_t StaticTriangleCount = 0;
    for (const FMeshSection& Section : DrawSections)
    {
        if (Section.IsStaticRegularMeshCandidate())
        {
            StaticTriangleCount += Section.DrawIndexCount / 3u;
        }
    }
    AddDistributedScatterInitPass(Context, StaticTriangleCount);
    for (FMeshSection& Section : DrawSections)
    {
        if (Section.IsStaticRegularMeshCandidate())
        {
            AddSectionDistributedScatterPreparePass(Context, *DrawSections.GetView(DrawSectionIndex).Object, Section, DrawSectionIndex);
        }
        ++DrawSectionIndex;
    }
    AddDistributedScatterPasses(Context, StaticTriangleCount);
    AddBuildTraceHierarchyPasses(Context);
    AddReferenceStatsPresentPass(Context);

    bSdfCacheValid = true;
    CachedSceneSignature = SceneSignature;
    CachedBuildSettingsSignature = BuildSettingsSignature;
    CachedCascadeBounds = Bounds;
    CachedStaticCandidateCount = StaticCandidateCount;
}

void FSparseSdfGI::AddDiffuseGITracePasses(FDeferredPassContext& Context) const
{
    AddRadianceCachePasses(Context);

    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGBufferHandle TraceHierarchyBottomHandle = Context.Resources.SparseSdfGI.TraceHierarchyBottomHandle;
    const FRGBufferHandle TraceHierarchyTopHandle = Context.Resources.SparseSdfGI.TraceHierarchyTopHandle;
    const FRGBufferHandle BrickRadianceHandle = Context.Resources.SparseSdfGI.BrickRadianceWriteHandle;
    const FRGResourceHandle DiffuseHandle = Context.Resources.SparseSdfGI.DiffuseGIHandle;
    const bool bWritesDenoiserInputs = DebugMode == ESparseSdfGIDebugMode::Off;
    ID3D12PipelineState* Pipeline = (DebugMode == ESparseSdfGIDebugMode::Off) ? DiffuseTracePipeline.Get() : DebugTracePipeline.Get();

    if (bUseScreenProbes && DebugMode == ESparseSdfGIDebugMode::Off)
    {
        AddScreenProbeGITracePasses(Context, BrickRadianceHandle);
        AddIrradianceCacheUpdatePasses(Context);
        return;
    }

    struct FOutputPassData
    {
        bool bEnabled = false;
        bool bWritesDenoiserInputs = false;
        FRGResourceHandle InputSHHandle{};
        FRGResourceHandle VarianceHandle{};
    };

    Graph.AddPass<FOutputPassData>("SparseSdfGI Trace", [&, DepthHandle, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, TraceHierarchyBottomHandle, TraceHierarchyTopHandle, BrickRadianceHandle, DiffuseHandle, GBufferHandles, Pipeline, bWritesDenoiserInputs](FOutputPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.bWritesDenoiserInputs = Data.bEnabled && bWritesDenoiserInputs;
        if (!Data.bEnabled)
        {
            return;
        }

        if (Data.bWritesDenoiserInputs)
        {
            const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
            const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
            Data.InputSHHandle = Builder.CreateTexture("SparseSdfGI Input SH", { FullWidth, FullHeight, DXGI_FORMAT_R32G32B32A32_UINT });
            Data.VarianceHandle = Builder.CreateTexture("SparseSdfGI Variance", { FullWidth, FullHeight, DXGI_FORMAT_R8_UNORM });
            Context.Resources.SparseSdfGI.DiffuseGIInputSHHandle = Data.InputSHHandle;
            Context.Resources.SparseSdfGI.DiffuseGIVarianceHandle = Data.VarianceHandle;
        }

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SdfAtlasHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickMetadataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(TraceHierarchyBottomHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(TraceHierarchyTopHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(DiffuseHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (Data.bWritesDenoiserInputs)
        {
            Builder.WriteTexture(Data.InputSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteTexture(Data.VarianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [&, Pipeline, BrickRadianceHandle](const FOutputPassData& Data, FDX12CommandContext& Cmd)
    {
        DispatchOutputPass(Context, Cmd, Pipeline, Data.bEnabled, BrickRadianceHandle, Data.InputSHHandle, Data.VarianceHandle);
    });

    AddIrradianceCacheUpdatePasses(Context);

    (void)Owner;
}

bool FSparseSdfGI::CreateRootSignature(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstantBufferView(1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[2].InitAsConstants(kSparseSdfGIMaxBindlessDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams),
        RootParams,
        0,
        nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
            | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RootSignature.GetAddressOf())));
    return true;
}

bool FSparseSdfGI::CreateDispatchCommandSignature(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC ArgumentDesc = {};
    ArgumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    CommandDesc.NumArgumentDescs = 1u;
    CommandDesc.pArgumentDescs = &ArgumentDesc;

    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, nullptr, IID_PPV_ARGS(DispatchCommandSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FSparseSdfGI::CreatePipelines(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> ScatterInitByteCode;
    std::vector<uint8_t> ScatterPrepareByteCode;
    std::vector<uint8_t> ScatterScanJobsByteCode;
    std::vector<uint8_t> ScatterScanGroupsByteCode;
    std::vector<uint8_t> ScatterScanGroup2ByteCode;
    std::vector<uint8_t> ScatterAddOffsetsByteCode;
    std::vector<uint8_t> ScatterBuildSampleArgsByteCode;
    std::vector<uint8_t> ScatterMarkTouchedByteCode;
    std::vector<uint8_t> ScatterAllocateBricksByteCode;
    std::vector<uint8_t> ScatterBuildBrickArgsByteCode;
    std::vector<uint8_t> ScatterClearBrickStorageByteCode;
    std::vector<uint8_t> ScatterSdfSamplesByteCode;
    std::vector<uint8_t> ScatterFinalizeBricksByteCode;
    std::vector<uint8_t> BuildTraceHierarchyBottomByteCode;
    std::vector<uint8_t> BuildTraceHierarchyTopByteCode;
    std::vector<uint8_t> ReferenceStatsPresentByteCode;
    std::vector<uint8_t> RadianceClearByteCode;
    std::vector<uint8_t> RadianceInjectByteCode;
    std::vector<uint8_t> IrradianceAccumulateByteCode;
    std::vector<uint8_t> RadianceResolveByteCode;
    std::vector<uint8_t> ProbeSpawnByteCode;
    std::vector<uint8_t> ProbeTraceByteCode;
    std::vector<uint8_t> ProbeTraceDirectionalByteCode;
    std::vector<uint8_t> ProbeInterpolateByteCode;
    std::vector<uint8_t> DebugTraceByteCode;
    std::vector<uint8_t> DiffuseTraceByteCode;

    const std::wstring ShaderPath = L"Shaders/SparseSdfGI/SparseSdfGI.hlsl";
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSInitDistributedScatterBuild", ScatterInitByteCode, { L"SPARSE_SDF_GI_SCATTER_INIT_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSPrepareScatterJobs", ScatterPrepareByteCode, { L"SPARSE_SDF_GI_SCATTER_PREPARE_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSScanScatterJobCounts", ScatterScanJobsByteCode, { L"SPARSE_SDF_GI_SCATTER_SCAN_JOBS_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSScanScatterGroupSums", ScatterScanGroupsByteCode, { L"SPARSE_SDF_GI_SCATTER_SCAN_GROUPS_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSScanScatterGroup2Sums", ScatterScanGroup2ByteCode, { L"SPARSE_SDF_GI_SCATTER_SCAN_GROUP2_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSAddScatterJobGroupOffsets", ScatterAddOffsetsByteCode, { L"SPARSE_SDF_GI_SCATTER_ADD_OFFSETS_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSBuildScatterSampleArgs", ScatterBuildSampleArgsByteCode, { L"SPARSE_SDF_GI_SCATTER_BUILD_SAMPLE_ARGS_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSMarkScatterTouchedBricks", ScatterMarkTouchedByteCode, { L"SPARSE_SDF_GI_SCATTER_MARK_TOUCHED_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSAllocateScatterBricks", ScatterAllocateBricksByteCode, { L"SPARSE_SDF_GI_SCATTER_ALLOCATE_BRICKS_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSBuildScatterBrickArgs", ScatterBuildBrickArgsByteCode, { L"SPARSE_SDF_GI_SCATTER_BUILD_BRICK_ARGS_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSClearScatterBrickStorage", ScatterClearBrickStorageByteCode, { L"SPARSE_SDF_GI_SCATTER_CLEAR_BRICK_STORAGE_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSScatterSdfSamples", ScatterSdfSamplesByteCode, { L"SPARSE_SDF_GI_SCATTER_SDF_SAMPLES_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSFinalizeScatterBricks", ScatterFinalizeBricksByteCode, { L"SPARSE_SDF_GI_SCATTER_FINALIZE_BRICKS_SHADER=1" })) { return false; }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSBuildTraceHierarchyBottom", BuildTraceHierarchyBottomByteCode, { L"SPARSE_SDF_GI_BUILD_TRACE_HIERARCHY_BOTTOM_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSBuildTraceHierarchyTop", BuildTraceHierarchyTopByteCode, { L"SPARSE_SDF_GI_BUILD_TRACE_HIERARCHY_TOP_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSStoreReferenceStatsToGpuDebug", ReferenceStatsPresentByteCode, { L"SPARSE_SDF_GI_REFERENCE_STATS_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSClearBrickRadianceAccum", RadianceClearByteCode, { L"SPARSE_SDF_GI_RADIANCE_CLEAR_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSInjectBrickRadiance", RadianceInjectByteCode, { L"SPARSE_SDF_GI_RADIANCE_INJECT_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSAccumulateBrickIrradiance", IrradianceAccumulateByteCode, { L"SPARSE_SDF_GI_IRRADIANCE_ACCUM_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSResolveBrickRadianceTemporal", RadianceResolveByteCode, { L"SPARSE_SDF_GI_RADIANCE_RESOLVE_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSSpawnScreenProbes", ProbeSpawnByteCode, { L"SPARSE_SDF_GI_PROBE_SPAWN_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSTraceScreenProbes", ProbeTraceByteCode, { L"SPARSE_SDF_GI_PROBE_TRACE_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSTraceScreenProbes", ProbeTraceDirectionalByteCode, { L"SPARSE_SDF_GI_PROBE_TRACE_SHADER=1", L"SPARSE_SDF_GI_PROBE_DIRECTIONAL_SH=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSInterpolateScreenProbes", ProbeInterpolateByteCode, { L"SPARSE_SDF_GI_PROBE_INTERPOLATE_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSDebugTrace", DebugTraceByteCode, { L"SPARSE_SDF_GI_TRACE_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSDiffuseTrace", DiffuseTraceByteCode, { L"SPARSE_SDF_GI_TRACE_SHADER=1" }))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature.Get();

    auto CreateComputePso = [Device, &PsoDesc](const std::vector<uint8_t>& ByteCode, ComPtr<ID3D12PipelineState>& OutPipeline, const char* Name)
    {
        PsoDesc.CS = { ByteCode.data(), ByteCode.size() };
        const HRESULT Hr = Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(OutPipeline.GetAddressOf()));
        if (FAILED(Hr))
        {
            std::ostringstream Oss;
            Oss << "SparseSdfGI pipeline creation failed for " << Name << ", hr=0x" << std::hex << static_cast<uint32_t>(Hr);
            LogWarning(Oss.str());
            return false;
        }
        return true;
    };

    return CreateComputePso(ScatterInitByteCode, ScatterInitPipeline, "CSInitDistributedScatterBuild")
        && CreateComputePso(ScatterPrepareByteCode, ScatterPreparePipeline, "CSPrepareScatterJobs")
        && CreateComputePso(ScatterScanJobsByteCode, ScatterScanJobsPipeline, "CSScanScatterJobCounts")
        && CreateComputePso(ScatterScanGroupsByteCode, ScatterScanGroupsPipeline, "CSScanScatterGroupSums")
        && CreateComputePso(ScatterScanGroup2ByteCode, ScatterScanGroup2Pipeline, "CSScanScatterGroup2Sums")
        && CreateComputePso(ScatterAddOffsetsByteCode, ScatterAddOffsetsPipeline, "CSAddScatterJobGroupOffsets")
        && CreateComputePso(ScatterBuildSampleArgsByteCode, ScatterBuildSampleArgsPipeline, "CSBuildScatterSampleArgs")
        && CreateComputePso(ScatterMarkTouchedByteCode, ScatterMarkTouchedPipeline, "CSMarkScatterTouchedBricks")
        && CreateComputePso(ScatterAllocateBricksByteCode, ScatterAllocateBricksPipeline, "CSAllocateScatterBricks")
        && CreateComputePso(ScatterBuildBrickArgsByteCode, ScatterBuildBrickArgsPipeline, "CSBuildScatterBrickArgs")
        && CreateComputePso(ScatterClearBrickStorageByteCode, ScatterClearBrickStoragePipeline, "CSClearScatterBrickStorage")
        && CreateComputePso(ScatterSdfSamplesByteCode, ScatterSdfSamplesPipeline, "CSScatterSdfSamples")
        && CreateComputePso(ScatterFinalizeBricksByteCode, ScatterFinalizeBricksPipeline, "CSFinalizeScatterBricks")
        && CreateComputePso(BuildTraceHierarchyBottomByteCode, BuildTraceHierarchyBottomPipeline, "CSBuildTraceHierarchyBottom")
        && CreateComputePso(BuildTraceHierarchyTopByteCode, BuildTraceHierarchyTopPipeline, "CSBuildTraceHierarchyTop")
        && CreateComputePso(ReferenceStatsPresentByteCode, ReferenceStatsPresentPipeline, "CSStoreReferenceStatsToGpuDebug")
        && CreateComputePso(RadianceClearByteCode, RadianceClearPipeline, "CSClearBrickRadianceAccum")
        && CreateComputePso(RadianceInjectByteCode, RadianceInjectPipeline, "CSInjectBrickRadiance")
        && CreateComputePso(IrradianceAccumulateByteCode, IrradianceAccumulatePipeline, "CSAccumulateBrickIrradiance")
        && CreateComputePso(RadianceResolveByteCode, RadianceResolvePipeline, "CSResolveBrickRadianceTemporal")
        && CreateComputePso(ProbeSpawnByteCode, ProbeSpawnPipeline, "CSSpawnScreenProbes")
        && CreateComputePso(ProbeTraceByteCode, ProbeTracePipeline, "CSTraceScreenProbes")
        && CreateComputePso(ProbeTraceDirectionalByteCode, ProbeTraceDirectionalPipeline, "CSTraceScreenProbes DirectionalSH")
        && CreateComputePso(ProbeInterpolateByteCode, ProbeInterpolatePipeline, "CSInterpolateScreenProbes")
        && CreateComputePso(DebugTraceByteCode, DebugTracePipeline, "CSDebugTrace")
        && CreateComputePso(DiffuseTraceByteCode, DiffuseTracePipeline, "CSDiffuseTrace");
}

bool FSparseSdfGI::CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FramesInFlight)
{
    if (!Device || Width == 0u || Height == 0u)
    {
        return false;
    }

    const DXGI_FORMAT SdfAtlasFormat = SelectSparseSdfGIAtlasFormat(Device);
    if (SdfAtlasFormat == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    const FRGTextureDesc SdfAtlasDesc =
    {
        kSparseSdfGIAtlasResolution,
        kSparseSdfGIAtlasResolution,
        SdfAtlasFormat,
        1,
        static_cast<uint16_t>(kSparseSdfGIAtlasResolution),
        D3D12_RESOURCE_DIMENSION_TEXTURE3D
    };
    CreateBindlessTexture3D(
        Device,
        L"SparseSdfGI_SdfAtlas",
        SdfAtlasDesc,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        SdfAtlas,
        true,
        true);

    const FRGBufferDesc BrickMapDesc = CreateRWStructuredBufferDesc<uint32_t>(GetBrickMapElementCount());
    CreateBindlessBuffer(
        Device,
        L"SparseSdfGI_CascadeBrickMap",
        BrickMapDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        CascadeBrickMap,
        true,
        true);

    const FRGBufferDesc BrickMetadataDesc = CreateRWStructuredBufferDesc<FBrickMetadataGpu>(GetBrickMapElementCount());
    CreateBindlessBuffer(
        Device,
        L"SparseSdfGI_BrickMetadata",
        BrickMetadataDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        BrickMetadata,
        true,
        true);

    CreateBindlessBuffer(
        Device,
        L"SparseSdfGI_TraceHierarchyBottom",
        CreateTraceHierarchyBottomDesc(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        TraceHierarchyBottom,
        true,
        true);

    CreateBindlessBuffer(
        Device,
        L"SparseSdfGI_TraceHierarchyTop",
        CreateTraceHierarchyTopDesc(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        TraceHierarchyTop,
        true,
        true);

    CreateBindlessBuffer(
        Device,
        L"SparseSdfGI_ReferenceCounterStats",
        CreateRWStructuredBufferDesc<uint32_t>(5u),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        ReferenceCounterStats,
        true,
        true);

    const uint32_t FrameCount = (std::max)(2u, FramesInFlight);
    SparseConstantBuffers.resize(FrameCount);
    SparseConstantCursors.assign(FrameCount, 0u);
    for (uint32_t FrameIndex = 0u; FrameIndex < FrameCount; ++FrameIndex)
    {
        if (!CreateMappedConstantBuffer(
            Device,
            L"SparseSdfGI_Constants_Frame" + std::to_wstring(FrameIndex),
            static_cast<uint64_t>(kSparseSdfGIConstantBufferSlotsPerFrame) * kSparseSdfGIConstantBufferStride,
            SparseConstantBuffers[FrameIndex]))
        {
            return false;
        }
    }

    const FRGBufferDesc BrickRadianceDesc = CreateBrickRadianceDesc();
    BrickRadiance.resize(FrameCount);
    BrickRadianceHistoryValid.assign(FrameCount, false);
    PendingBrickRadianceWrite.assign(FrameCount, false);
    for (uint32_t FrameIndex = 0u; FrameIndex < FrameCount; ++FrameIndex)
    {
        CreateBindlessBuffer(
            Device,
            L"SparseSdfGI_BrickRadiance_Frame" + std::to_wstring(FrameIndex),
            BrickRadianceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            BrickRadiance[FrameIndex],
            true,
            true);
    }

    BrickIrradiance.resize(FrameCount);
    BrickIrradianceHistoryValid.assign(FrameCount, false);
    PendingBrickIrradianceWrite.assign(FrameCount, false);
    for (uint32_t FrameIndex = 0u; FrameIndex < FrameCount; ++FrameIndex)
    {
        CreateBindlessBuffer(
            Device,
            L"SparseSdfGI_BrickIrradiance_Frame" + std::to_wstring(FrameIndex),
            BrickRadianceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            BrickIrradiance[FrameIndex],
            true,
            true);
    }

    ProbeHistoryCapacity = (std::max)(1u, AlignDispatch(Width, 4u) * AlignDispatch(Height, 4u));
    const FRGBufferDesc ProbeHistoryDesc = CreateScreenProbeHistoryDesc(ProbeHistoryCapacity);
    ProbeHistory.resize(FrameCount);
    ProbeHistoryValid.assign(FrameCount, false);
    PendingProbeHistoryWrite.assign(FrameCount, false);
    for (uint32_t FrameIndex = 0u; FrameIndex < FrameCount; ++FrameIndex)
    {
        CreateBindlessBuffer(
            Device,
            L"SparseSdfGI_ProbeHistory_Frame" + std::to_wstring(FrameIndex),
            ProbeHistoryDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            ProbeHistory[FrameIndex],
            true,
            true);
    }

    CreateBindlessTexture(
        Device,
        L"SparseSdfGI_Diffuse",
        { Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT },
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        DiffuseGI,
        true,
        true);

    return RefreshPersistentInputValidation();
}

bool FSparseSdfGI::RefreshPersistentInputValidation()
{
    bPersistentInputsValid =
        RootSignature &&
        DispatchCommandSignature &&
        ScatterInitPipeline &&
        ScatterPreparePipeline &&
        ScatterScanJobsPipeline &&
        ScatterScanGroupsPipeline &&
        ScatterScanGroup2Pipeline &&
        ScatterAddOffsetsPipeline &&
        ScatterBuildSampleArgsPipeline &&
        ScatterMarkTouchedPipeline &&
        ScatterAllocateBricksPipeline &&
        ScatterBuildBrickArgsPipeline &&
        ScatterClearBrickStoragePipeline &&
        ScatterSdfSamplesPipeline &&
        ScatterFinalizeBricksPipeline &&
        BuildTraceHierarchyBottomPipeline &&
        BuildTraceHierarchyTopPipeline &&
        ReferenceStatsPresentPipeline &&
        RadianceClearPipeline &&
        RadianceInjectPipeline &&
        IrradianceAccumulatePipeline &&
        RadianceResolvePipeline &&
        ProbeSpawnPipeline &&
        ProbeTracePipeline &&
        ProbeTraceDirectionalPipeline &&
        ProbeInterpolatePipeline &&
        DebugTracePipeline &&
        DiffuseTracePipeline &&
        SdfAtlas.IsFullyBound() &&
        CascadeBrickMap.IsFullyBound() &&
        BrickMetadata.IsFullyBound() &&
        TraceHierarchyBottom.IsFullyBound() &&
        TraceHierarchyTop.IsFullyBound() &&
        ReferenceCounterStats.IsFullyBound() &&
        !SparseConstantBuffers.empty() &&
        std::all_of(SparseConstantBuffers.begin(), SparseConstantBuffers.end(), [](const FMappedUploadBuffer& Buffer)
        {
            return Buffer.IsValid();
        }) &&
        !BrickRadiance.empty() &&
        std::all_of(BrickRadiance.begin(), BrickRadiance.end(), [](const FBindlessBuffer& Buffer)
        {
            return Buffer.IsFullyBound();
        }) &&
        !BrickIrradiance.empty() &&
        std::all_of(BrickIrradiance.begin(), BrickIrradiance.end(), [](const FBindlessBuffer& Buffer)
        {
            return Buffer.IsFullyBound();
        }) &&
        DiffuseGI.IsFullyBound();

    return true;
}

FSparseSdfGI::FCascadeBounds FSparseSdfGI::ComputeCascadeBounds(const FDeferredRenderer& Owner) const
{
    const float SceneRadius = (std::max)(Owner.GetSceneRadius(), kSparseSdfGIMinVoxelSize);
    const float AutoVoxelSize = (std::max)(
        (SceneRadius * 2.0f * kSparseSdfGICascadeSceneRadiusMargin) / static_cast<float>(kSparseSdfGIAtlasResolution),
        kSparseSdfGIMinVoxelSize);
    const float BaseVoxel = (BaseVoxelSize > 0.0f)
        ? (std::max)(BaseVoxelSize, kSparseSdfGIMinVoxelSize)
        : AutoVoxelSize;
    const float SamplePitch = BaseVoxel * (static_cast<float>(kSparseSdfGIBrickVoxelResolution) / static_cast<float>(kSparseSdfGIExactBrickIntervalResolution));
    const float BrickIntervalCount = static_cast<float>(kSparseSdfGIBrickGridResolution * kSparseSdfGIExactBrickIntervalResolution);
    const float ExtentValue = SamplePitch * BrickIntervalCount;
    const DirectX::XMFLOAT3 SceneCenter = Owner.GetSceneCenter();

    CachedEffectiveVoxelSize = SamplePitch;

    FCascadeBounds Bounds = {};
    Bounds.Extent = DirectX::XMFLOAT3(ExtentValue, ExtentValue, ExtentValue);
    Bounds.VoxelSize = SamplePitch;
    Bounds.Min = DirectX::XMFLOAT3(
        SceneCenter.x - Bounds.Extent.x * 0.5f,
        SceneCenter.y - Bounds.Extent.y * 0.5f,
        SceneCenter.z - Bounds.Extent.z * 0.5f);
    return Bounds;
}

uint64_t FSparseSdfGI::ComputeBuildSettingsSignature(const FCascadeBounds& Bounds) const
{
    uint64_t Hash = kSparseSdfGIHashOffsetBasis;
    HashValue(Hash, kSparseSdfGIBrickGridResolution);
    HashValue(Hash, kSparseSdfGIBrickVoxelResolution);
    HashValue(Hash, kSparseSdfGIAtlasResolution);
    HashValue(Hash, kSparseSdfGIExactBrickIntervalResolution);
    HashValue(Hash, kSparseSdfGISurfaceThicknessVoxels);
    HashValue(Hash, CascadeCount);
    HashValue(Hash, BaseVoxelSize);
    HashValue(Hash, CascadeScale);
    HashValue(Hash, MaxBrickTriangleReferences);
    HashFloat3(Hash, Bounds.Min);
    HashFloat3(Hash, Bounds.Extent);
    HashValue(Hash, Bounds.VoxelSize);
    return Hash;
}

uint64_t FSparseSdfGI::ComputeStaticSceneSignature(const FDeferredRenderer& Owner, uint32_t& OutStaticCandidateCount) const
{
    uint64_t Hash = kSparseSdfGIHashOffsetBasis;
    OutStaticCandidateCount = 0;

    auto Sections = Owner.GetWorld().BuildSectionList();
    const uint64_t TotalSectionCount = static_cast<uint64_t>(Sections.size());
    HashValue(Hash, TotalSectionCount);

    for (uint64_t DrawSectionIndex = 0; DrawSectionIndex < TotalSectionCount; ++DrawSectionIndex)
    {
        const FMeshSection& Section = Sections[static_cast<size_t>(DrawSectionIndex)];
        const bool bCandidate = Section.IsStaticRegularMeshCandidate();
        HashValue(Hash, DrawSectionIndex);
        HashValue(Hash, bCandidate);
        if (!bCandidate)
        {
            continue;
        }

        ++OutStaticCandidateCount;
        HashValue(Hash, Section.DrawIndexStart);
        HashValue(Hash, Section.DrawIndexCount);
        HashValue(Hash, Section.Geometry.IndexCount);
        HashValue(Hash, Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].SrvBindlessIndex);
        HashValue(Hash, Section.Geometry.IndexBuffer.SrvBindlessIndex);
        HashFloat4x4(Hash, Sections.GetView(static_cast<size_t>(DrawSectionIndex)).Object->GetWorldMatrix());
    }

    HashValue(Hash, OutStaticCandidateCount);
    return Hash;
}

void FSparseSdfGI::InvalidateCache() const
{
    bSdfCacheValid = false;
    CachedSceneSignature = 0;
    CachedBuildSettingsSignature = 0;
    CachedCascadeBounds = {};
    CachedStaticCandidateCount = 0;
    std::fill(BrickRadianceHistoryValid.begin(), BrickRadianceHistoryValid.end(), false);
    std::fill(PendingBrickRadianceWrite.begin(), PendingBrickRadianceWrite.end(), false);
    std::fill(BrickIrradianceHistoryValid.begin(), BrickIrradianceHistoryValid.end(), false);
    std::fill(PendingBrickIrradianceWrite.begin(), PendingBrickIrradianceWrite.end(), false);
}

void FSparseSdfGI::AddDistributedScatterInitPass(FDeferredPassContext& Context, uint32_t MaxStaticTriangleCount) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGBufferHandle ReferenceStatsHandle = Context.Resources.SparseSdfGI.ReferenceCounterStatsHandle;
    FDeferredRenderer* OwnerPtr = &Context.Owner;
    FRenderGraph* GraphPtr = &Context.Graph;
    FSparseSdfGIFrameResources* SparseResourcesPtr = &Context.Resources.SparseSdfGI;
    const uint32_t MaxJobCount = (std::max)(MaxStaticTriangleCount, 1u);
    const uint32_t JobGroupCount = GetScatterJobGroupCount(MaxJobCount);
    const uint32_t JobGroup2Count = GetScatterJobGroup2Count(MaxJobCount);
    const uint32_t ScatterBrickCapacity = (std::max)(MaxScatterBricks, 1u);

    struct FScatterInitPassData
    {
        bool bEnabled = false;
        uint32_t MaxJobCount = 0u;
        uint32_t ScatterBrickCapacity = 0u;
        FRGResourceHandle SdfAtlasHandle{};
        FRGBufferHandle BrickMapHandle{};
        FRGBufferHandle BrickMetadataHandle{};
        FRGBufferHandle ReferenceStatsHandle{};
        FRGBufferHandle JobsHandle{};
        FRGBufferHandle JobOffsetsHandle{};
        FRGBufferHandle JobGroupSumsHandle{};
        FRGBufferHandle JobGroupOffsetsHandle{};
        FRGBufferHandle JobGroup2SumsHandle{};
        FRGBufferHandle JobGroup2OffsetsHandle{};
        FRGBufferHandle TouchedBricksHandle{};
        FRGBufferHandle BrickListHandle{};
        FRGBufferHandle BrickSdfHandle{};
        FRGBufferHandle CountersHandle{};
        FRGBufferHandle SampleArgsHandle{};
        FRGBufferHandle BrickArgsHandle{};
    };

    Graph.AddPass<FScatterInitPassData>("SparseSdfGI Distributed Scatter Init", [this, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, ReferenceStatsHandle, MaxJobCount, JobGroupCount, JobGroup2Count, ScatterBrickCapacity, SparseResourcesPtr](FScatterInitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.MaxJobCount = MaxJobCount;
        Data.ScatterBrickCapacity = ScatterBrickCapacity;
        Data.SdfAtlasHandle = SdfAtlasHandle;
        Data.BrickMapHandle = BrickMapHandle;
        Data.BrickMetadataHandle = BrickMetadataHandle;
        Data.ReferenceStatsHandle = ReferenceStatsHandle;
        Data.JobsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Jobs", CreateScatterJobsDesc(MaxJobCount));
        Data.JobOffsetsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Job Offsets", CreateScatterJobOffsetsDesc(MaxJobCount));
        Data.JobGroupSumsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Job Group Sums", CreateScatterGroupDesc(JobGroupCount));
        Data.JobGroupOffsetsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Job Group Offsets", CreateScatterGroupDesc(JobGroupCount));
        Data.JobGroup2SumsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Job Group2 Sums", CreateScatterGroupDesc(JobGroup2Count));
        Data.JobGroup2OffsetsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Job Group2 Offsets", CreateScatterGroupDesc(JobGroup2Count));
        Data.TouchedBricksHandle = Builder.CreateBuffer("SparseSdfGI Scatter Touched Bricks", CreateScatterTouchedBricksDesc());
        Data.BrickListHandle = Builder.CreateBuffer("SparseSdfGI Scatter Brick List", CreateScatterBrickListDesc(ScatterBrickCapacity));
        Data.BrickSdfHandle = Builder.CreateBuffer("SparseSdfGI Scatter Brick SDF", CreateScatterBrickSdfDesc(ScatterBrickCapacity));
        Data.CountersHandle = Builder.CreateBuffer("SparseSdfGI Scatter Counters", CreateScatterCountersDesc());
        Data.SampleArgsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Sample Args", CreateScatterDispatchArgsDesc());
        Data.BrickArgsHandle = Builder.CreateBuffer("SparseSdfGI Scatter Brick Args", CreateScatterDispatchArgsDesc());

        SparseResourcesPtr->ScatterJobsHandle = Data.JobsHandle;
        SparseResourcesPtr->ScatterJobOffsetsHandle = Data.JobOffsetsHandle;
        SparseResourcesPtr->ScatterJobGroupSumsHandle = Data.JobGroupSumsHandle;
        SparseResourcesPtr->ScatterJobGroupOffsetsHandle = Data.JobGroupOffsetsHandle;
        SparseResourcesPtr->ScatterJobGroup2SumsHandle = Data.JobGroup2SumsHandle;
        SparseResourcesPtr->ScatterJobGroup2OffsetsHandle = Data.JobGroup2OffsetsHandle;
        SparseResourcesPtr->ScatterTouchedBricksHandle = Data.TouchedBricksHandle;
        SparseResourcesPtr->ScatterBrickListHandle = Data.BrickListHandle;
        SparseResourcesPtr->ScatterBrickSdfHandle = Data.BrickSdfHandle;
        SparseResourcesPtr->ScatterCountersHandle = Data.CountersHandle;
        SparseResourcesPtr->ScatterSampleDispatchArgsHandle = Data.SampleArgsHandle;
        SparseResourcesPtr->ScatterBrickDispatchArgsHandle = Data.BrickArgsHandle;

        Builder.WriteTexture(Data.SdfAtlasHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickMapHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickMetadataHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.TouchedBricksHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.CountersHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.ReferenceStatsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.SampleArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.SdfAtlasHandle);
        Builder.UavBarrier(Data.BrickMapHandle);
        Builder.UavBarrier(Data.BrickMetadataHandle);
        Builder.UavBarrier(Data.TouchedBricksHandle);
        Builder.UavBarrier(Data.CountersHandle);
        Builder.UavBarrier(Data.ReferenceStatsHandle);
    }, [this, OwnerPtr, GraphPtr](const FScatterInitPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterInitPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.MaxBrickTriangleReferences = Data.ScatterBrickCapacity;
        Constants.TrianglePoolCapacity = Data.MaxJobCount;
        Constants.CascadeMin = Bounds.Min;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.VoxelSize = Bounds.VoxelSize;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const uint32_t SdfAtlasUavIndex = GraphPtr->GetTextureUavBindlessIndex(Data.SdfAtlasHandle);
        const uint32_t BrickMapUavIndex = GraphPtr->GetBufferUavBindlessIndex(Data.BrickMapHandle);
        const uint32_t BrickMetadataUavIndex = GraphPtr->GetBufferUavBindlessIndex(Data.BrickMetadataHandle);
        const uint32_t TouchedUavIndex = GraphPtr->GetBufferUavBindlessIndex(Data.TouchedBricksHandle);
        const uint32_t CountersUavIndex = GraphPtr->GetBufferUavBindlessIndex(Data.CountersHandle);
        const uint32_t ReferenceStatsUavIndex = GraphPtr->GetBufferUavBindlessIndex(Data.ReferenceStatsHandle);
        if (!AreAllBindlessIndicesValid(SdfAtlasUavIndex, BrickMapUavIndex, BrickMetadataUavIndex, TouchedUavIndex, CountersUavIndex, ReferenceStatsUavIndex))
        {
            return;
        }

        const FSparseSdfGIScatterInitBindlessConstants Bindless =
        {
            SdfAtlasUavIndex,
            BrickMapUavIndex,
            BrickMetadataUavIndex,
            TouchedUavIndex,
            CountersUavIndex,
            ReferenceStatsUavIndex
        };
        static_assert(sizeof(FSparseSdfGIScatterInitBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterInitBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D));
    });
}

void FSparseSdfGI::AddSectionDistributedScatterPreparePass(FDeferredPassContext& Context, const FObject& Object, FMeshSection& Section, uint32_t DrawSectionIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FBindlessBuffer& PositionBuffer = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition];
    FBindlessBuffer& IndexBuffer = Section.Geometry.IndexBuffer;
    const FRGBufferHandle PositionHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Scatter Position", PositionBuffer);
    const FRGBufferHandle IndexHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Scatter Index", IndexBuffer);
    const FRGBufferHandle JobsHandle = Context.Resources.SparseSdfGI.ScatterJobsHandle;
    const FRGBufferHandle CountersHandle = Context.Resources.SparseSdfGI.ScatterCountersHandle;
    const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
    const float SectionScale = MatrixMath::ComputeMaxScale(Object.GetWorldMatrix());
    FDeferredRenderer* OwnerPtr = &Context.Owner;
    FRenderGraph* GraphPtr = &Context.Graph;

    struct FScatterPreparePassData
    {
        bool bEnabled = false;
        uint32_t TriangleCount = 0u;
        uint32_t DrawIndexStart = 0u;
        uint32_t DrawIndexCount = 0u;
        uint32_t PositionCount = 0u;
        uint32_t MaxJobCount = 0u;
        uint32_t ScatterBrickCapacity = 0u;
        uint32_t PositionBufferIndex = UINT32_MAX;
        uint32_t IndexBufferIndex = UINT32_MAX;
        DirectX::XMFLOAT3 CascadeMin{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 CascadeExtent{ 0.0f, 0.0f, 0.0f };
        float VoxelSize = 0.0f;
        DirectX::XMFLOAT4X4 World{};
        FRGBufferHandle JobsHandle{};
        FRGBufferHandle CountersHandle{};
    };

    const std::string PassName = "SparseSdfGI Prepare Scatter Jobs Section " + std::to_string(DrawSectionIndex);
    Graph.AddPass<FScatterPreparePassData>(PassName, [this, &Owner, &Object, &Section, PositionHandle, IndexHandle, JobsHandle, CountersHandle, Bounds, SectionScale](FScatterPreparePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.TriangleCount = Section.DrawIndexCount / 3u;
        Data.DrawIndexStart = Section.DrawIndexStart;
        Data.DrawIndexCount = Section.DrawIndexCount;
        Data.PositionCount = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].Desc.NumElements;
        Data.MaxJobCount = 0u;
        auto Sections = Owner.GetWorld().BuildSectionList();
        for (const FMeshSection& SceneSection : Sections)
        {
            if (SceneSection.IsStaticRegularMeshCandidate())
            {
                Data.MaxJobCount += SceneSection.DrawIndexCount / 3u;
            }
        }
        Data.MaxJobCount = (std::max)(Data.MaxJobCount, 1u);
        Data.ScatterBrickCapacity = (std::max)(MaxScatterBricks, 1u);
        Data.CascadeMin = Bounds.Min;
        Data.CascadeExtent = Bounds.Extent;
        Data.VoxelSize = Bounds.VoxelSize;
        Data.World = Object.GetWorldMatrix();
        Data.PositionBufferIndex = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].SrvBindlessIndex;
        Data.IndexBufferIndex = Section.Geometry.IndexBuffer.SrvBindlessIndex;
        Data.JobsHandle = JobsHandle;
        Data.CountersHandle = CountersHandle;
        Data.bEnabled = Data.bEnabled
            && Data.TriangleCount > 0u
            && Data.PositionCount > 0u
            && SectionScale > 0.0f
            && static_cast<bool>(Data.JobsHandle)
            && static_cast<bool>(Data.CountersHandle)
            && AreAllBindlessIndicesValid(Data.PositionBufferIndex, Data.IndexBufferIndex);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(PositionHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(IndexHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.JobsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.CountersHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.JobsHandle);
        Builder.UavBarrier(Data.CountersHandle);
    }, [this, OwnerPtr, GraphPtr](const FScatterPreparePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterPreparePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.MaxBrickTriangleReferences = Data.ScatterBrickCapacity;
        Constants.TrianglePoolCapacity = Data.MaxJobCount;
        Constants.BuildWorkOffset = Data.PositionCount;
        Constants.ModelTriangleCount = Data.TriangleCount;
        Constants.ModelDrawIndexStart = Data.DrawIndexStart;
        Constants.ModelDrawIndexCount = Data.DrawIndexCount;
        Constants.CascadeMin = Data.CascadeMin;
        Constants.CascadeExtent = Data.CascadeExtent;
        Constants.VoxelSize = Data.VoxelSize;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        Constants.World = Data.World;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const uint32_t JobsUavIndex = GraphPtr->GetBufferUavBindlessIndex(Data.JobsHandle);
        const uint32_t CountersUavIndex = GraphPtr->GetBufferUavBindlessIndex(Data.CountersHandle);
        if (!AreAllBindlessIndicesValid(JobsUavIndex, CountersUavIndex))
        {
            return;
        }

        const FSparseSdfGIScatterPrepareBindlessConstants Bindless =
        {
            Data.PositionBufferIndex,
            Data.IndexBufferIndex,
            JobsUavIndex,
            CountersUavIndex
        };
        static_assert(sizeof(FSparseSdfGIScatterPrepareBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterPrepareBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(Data.TriangleCount, kSparseSdfGIReferenceEmitGroupSize), 1u, 1u);
    });
}

void FSparseSdfGI::AddDistributedScatterPasses(FDeferredPassContext& Context, uint32_t MaxStaticTriangleCount) const
{
    FRenderGraph& Graph = Context.Graph;
    FDeferredRenderer* OwnerPtr = &Context.Owner;
    FRenderGraph* GraphPtr = &Context.Graph;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGBufferHandle ReferenceStatsHandle = Context.Resources.SparseSdfGI.ReferenceCounterStatsHandle;
    const FRGBufferHandle JobsHandle = Context.Resources.SparseSdfGI.ScatterJobsHandle;
    const FRGBufferHandle JobOffsetsHandle = Context.Resources.SparseSdfGI.ScatterJobOffsetsHandle;
    const FRGBufferHandle JobGroupSumsHandle = Context.Resources.SparseSdfGI.ScatterJobGroupSumsHandle;
    const FRGBufferHandle JobGroupOffsetsHandle = Context.Resources.SparseSdfGI.ScatterJobGroupOffsetsHandle;
    const FRGBufferHandle JobGroup2SumsHandle = Context.Resources.SparseSdfGI.ScatterJobGroup2SumsHandle;
    const FRGBufferHandle JobGroup2OffsetsHandle = Context.Resources.SparseSdfGI.ScatterJobGroup2OffsetsHandle;
    const FRGBufferHandle TouchedBricksHandle = Context.Resources.SparseSdfGI.ScatterTouchedBricksHandle;
    const FRGBufferHandle BrickListHandle = Context.Resources.SparseSdfGI.ScatterBrickListHandle;
    const FRGBufferHandle BrickSdfHandle = Context.Resources.SparseSdfGI.ScatterBrickSdfHandle;
    const FRGBufferHandle CountersHandle = Context.Resources.SparseSdfGI.ScatterCountersHandle;
    const FRGBufferHandle SampleArgsHandle = Context.Resources.SparseSdfGI.ScatterSampleDispatchArgsHandle;
    const FRGBufferHandle BrickArgsHandle = Context.Resources.SparseSdfGI.ScatterBrickDispatchArgsHandle;
    const uint32_t MaxJobCount = (std::max)(MaxStaticTriangleCount, 1u);
    const uint32_t JobGroupCount = GetScatterJobGroupCount(MaxJobCount);
    const uint32_t JobGroup2Count = GetScatterJobGroup2Count(MaxJobCount);
    const uint32_t ScatterBrickCapacity = (std::max)(MaxScatterBricks, 1u);

    auto FillCommonConstants = [this, ScatterBrickCapacity](FDeferredRenderer& Owner, uint32_t WorkCount)
    {
        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.ModelTriangleCount = WorkCount;
        Constants.MaxBrickTriangleReferences = ScatterBrickCapacity;
        Constants.TrianglePoolCapacity = WorkCount;
        Constants.CascadeMin = Bounds.Min;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        return Constants;
    };

    struct FScatterDirectPassData
    {
        bool bEnabled = false;
        uint32_t WorkCount = 0u;
        FRGBufferHandle A{};
        FRGBufferHandle B{};
        FRGBufferHandle C{};
        FRGBufferHandle D{};
        FRGBufferHandle E{};
        FRGBufferHandle F{};
        FRGResourceHandle Tex{};
    };

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Scan Scatter Jobs", [this, JobsHandle, JobOffsetsHandle, JobGroupSumsHandle, CountersHandle, MaxJobCount](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.WorkCount = MaxJobCount;
        Data.A = JobsHandle;
        Data.B = JobOffsetsHandle;
        Data.C = JobGroupSumsHandle;
        Data.D = CountersHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.D, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.C, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.B);
        Builder.UavBarrier(Data.C);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, JobGroupCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterScanJobsPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, Data.WorkCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterScanJobsBindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.A),
            GraphPtr->GetBufferUavBindlessIndex(Data.B),
            GraphPtr->GetBufferUavBindlessIndex(Data.C),
            GraphPtr->GetBufferSrvBindlessIndex(Data.D)
        };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterJobsSrvIndex, Bindless.ScatterJobOffsetsUavIndex, Bindless.ScatterGroupSumsUavIndex, Bindless.ScatterCountersSrvIndex)) { return; }
        static_assert(sizeof(FSparseSdfGIScatterScanJobsBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterScanJobsBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(JobGroupCount, 1u, 1u);
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Scan Scatter Job Groups", [this, JobGroupSumsHandle, JobGroupOffsetsHandle, JobGroup2SumsHandle, JobGroupCount](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.WorkCount = JobGroupCount;
        Data.A = JobGroupSumsHandle;
        Data.B = JobGroupOffsetsHandle;
        Data.C = JobGroup2SumsHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.C, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.B);
        Builder.UavBarrier(Data.C);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, JobGroup2Count](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterScanGroupsPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, Data.WorkCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterScanGroupsBindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.A),
            GraphPtr->GetBufferUavBindlessIndex(Data.B),
            GraphPtr->GetBufferUavBindlessIndex(Data.C)
        };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterInputGroupSumsSrvIndex, Bindless.ScatterGroupOffsetsUavIndex, Bindless.ScatterOutputGroupSumsUavIndex)) { return; }
        static_assert(sizeof(FSparseSdfGIScatterScanGroupsBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterScanGroupsBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(JobGroup2Count, 1u, 1u);
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Scan Scatter Group2", [this, JobGroup2SumsHandle, JobGroup2OffsetsHandle, CountersHandle, JobGroup2Count](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.WorkCount = JobGroup2Count;
        Data.A = JobGroup2SumsHandle;
        Data.B = JobGroup2OffsetsHandle;
        Data.C = CountersHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.C, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.B);
        Builder.UavBarrier(Data.C);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterScanGroup2Pipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, Data.WorkCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterScanGroup2BindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.A),
            GraphPtr->GetBufferUavBindlessIndex(Data.B),
            GraphPtr->GetBufferUavBindlessIndex(Data.C)
        };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterGroup2SumsSrvIndex, Bindless.ScatterGroup2OffsetsUavIndex, Bindless.ScatterCountersUavIndex)) { return; }
        static_assert(sizeof(FSparseSdfGIScatterScanGroup2BindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterScanGroup2BindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(1u, 1u, 1u);
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Add Scatter Job Offsets", [this, JobOffsetsHandle, JobGroupOffsetsHandle, JobGroup2OffsetsHandle, CountersHandle, MaxJobCount](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.WorkCount = MaxJobCount;
        Data.A = JobOffsetsHandle;
        Data.B = JobGroupOffsetsHandle;
        Data.C = JobGroup2OffsetsHandle;
        Data.D = CountersHandle;
        if (!Data.bEnabled) { return; }
        Builder.WriteBuffer(Data.A, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(Data.B, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.C, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.D, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.UavBarrier(Data.A);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterAddOffsetsPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, Data.WorkCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterAddOffsetsBindlessConstants Bindless =
        {
            GraphPtr->GetBufferUavBindlessIndex(Data.A),
            GraphPtr->GetBufferSrvBindlessIndex(Data.B),
            GraphPtr->GetBufferSrvBindlessIndex(Data.C),
            GraphPtr->GetBufferSrvBindlessIndex(Data.D)
        };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterJobOffsetsUavIndex, Bindless.ScatterGroupOffsetsSrvIndex, Bindless.ScatterGroup2OffsetsSrvIndex, Bindless.ScatterCountersSrvIndex)) { return; }
        static_assert(sizeof(FSparseSdfGIScatterAddOffsetsBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterAddOffsetsBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(Data.WorkCount, kSparseSdfGIScatterScanGroupSize), 1u, 1u);
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Build Scatter Sample Args", [this, CountersHandle, SampleArgsHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.A = CountersHandle;
        Data.B = SampleArgsHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.B);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterBuildSampleArgsPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterBuildArgsBindlessConstants Bindless = { GraphPtr->GetBufferSrvBindlessIndex(Data.A), GraphPtr->GetBufferUavBindlessIndex(Data.B) };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterCountersSrvIndex, Bindless.ScatterDispatchArgsUavIndex)) { return; }
        static_assert(sizeof(FSparseSdfGIScatterBuildArgsBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterBuildArgsBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(1u, 1u, 1u);
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Mark Scatter Touched Bricks", [this, JobsHandle, JobOffsetsHandle, TouchedBricksHandle, CountersHandle, SampleArgsHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.A = JobsHandle;
        Data.B = JobOffsetsHandle;
        Data.C = TouchedBricksHandle;
        Data.D = CountersHandle;
        Data.E = SampleArgsHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.B, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.D, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.E, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.WriteBuffer(Data.C, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.C);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterMarkTouchedPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterMarkTouchedBindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.A),
            GraphPtr->GetBufferSrvBindlessIndex(Data.B),
            GraphPtr->GetBufferUavBindlessIndex(Data.C),
            GraphPtr->GetBufferSrvBindlessIndex(Data.D)
        };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterJobsSrvIndex, Bindless.ScatterJobOffsetsSrvIndex, Bindless.ScatterTouchedBricksUavIndex, Bindless.ScatterCountersSrvIndex)) { return; }
        static_assert(sizeof(FSparseSdfGIScatterMarkTouchedBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterMarkTouchedBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        ID3D12Resource* Args = GraphPtr->GetBufferResource(Data.E);
        if (Args)
        {
            CommandList->ExecuteIndirect(DispatchCommandSignature.Get(), 1u, Args, 0u, nullptr, 0u);
        }
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Allocate Scatter Bricks", [this, TouchedBricksHandle, BrickMapHandle, BrickListHandle, CountersHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.A = TouchedBricksHandle;
        Data.B = BrickMapHandle;
        Data.C = BrickListHandle;
        Data.D = CountersHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.C, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.D, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.B);
        Builder.UavBarrier(Data.C);
        Builder.UavBarrier(Data.D);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterAllocateBricksPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterAllocateBindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.A),
            GraphPtr->GetBufferUavBindlessIndex(Data.B),
            GraphPtr->GetBufferUavBindlessIndex(Data.C),
            GraphPtr->GetBufferUavBindlessIndex(Data.D)
        };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterTouchedBricksSrvIndex, Bindless.CascadeBrickMapUavIndex, Bindless.ScatterBrickListUavIndex, Bindless.ScatterCountersUavIndex)) { return; }
        static_assert(sizeof(FSparseSdfGIScatterAllocateBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterAllocateBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetBrickMapElementCount(), kSparseSdfGIScatterScanGroupSize), 1u, 1u);
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Build Scatter Brick Args", [this, CountersHandle, BrickArgsHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.A = CountersHandle;
        Data.B = BrickArgsHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.B);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterBuildBrickArgsPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterBuildArgsBindlessConstants Bindless = { GraphPtr->GetBufferSrvBindlessIndex(Data.A), GraphPtr->GetBufferUavBindlessIndex(Data.B) };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterCountersSrvIndex, Bindless.ScatterDispatchArgsUavIndex)) { return; }
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterBuildArgsBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(1u, 1u, 1u);
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Clear Scatter Brick Storage", [this, BrickSdfHandle, BrickArgsHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.A = BrickSdfHandle;
        Data.B = BrickArgsHandle;
        if (!Data.bEnabled) { return; }
        Builder.WriteBuffer(Data.A, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(Data.B, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.UavBarrier(Data.A);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterClearBrickStoragePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterClearBrickBindlessConstants Bindless = { GraphPtr->GetBufferUavBindlessIndex(Data.A) };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterBrickSdfUavIndex)) { return; }
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterClearBrickBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        ID3D12Resource* Args = GraphPtr->GetBufferResource(Data.B);
        if (Args)
        {
            CommandList->ExecuteIndirect(DispatchCommandSignature.Get(), 1u, Args, 0u, nullptr, 0u);
        }
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Scatter SDF Samples", [this, JobsHandle, JobOffsetsHandle, BrickMapHandle, BrickSdfHandle, CountersHandle, SampleArgsHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.A = JobsHandle;
        Data.B = JobOffsetsHandle;
        Data.C = BrickMapHandle;
        Data.D = BrickSdfHandle;
        Data.E = CountersHandle;
        Data.F = SampleArgsHandle;
        if (!Data.bEnabled) { return; }
        Builder.ReadBuffer(Data.A, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.B, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.C, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.E, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.F, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.WriteBuffer(Data.D, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.D);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterSdfSamplesPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterSamplesBindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.A),
            GraphPtr->GetBufferSrvBindlessIndex(Data.B),
            GraphPtr->GetBufferSrvBindlessIndex(Data.C),
            GraphPtr->GetBufferUavBindlessIndex(Data.D),
            GraphPtr->GetBufferSrvBindlessIndex(Data.E)
        };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterJobsSrvIndex, Bindless.ScatterJobOffsetsSrvIndex, Bindless.CascadeBrickMapSrvIndex, Bindless.ScatterBrickSdfUavIndex, Bindless.ScatterCountersSrvIndex)) { return; }
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterSamplesBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        ID3D12Resource* Args = GraphPtr->GetBufferResource(Data.F);
        if (Args)
        {
            CommandList->ExecuteIndirect(DispatchCommandSignature.Get(), 1u, Args, 0u, nullptr, 0u);
        }
    });

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Finalize Scatter Bricks", [this, SdfAtlasHandle, BrickMetadataHandle, BrickListHandle, BrickSdfHandle, CountersHandle, ReferenceStatsHandle, BrickArgsHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.Tex = SdfAtlasHandle;
        Data.A = BrickMetadataHandle;
        Data.B = BrickListHandle;
        Data.C = BrickSdfHandle;
        Data.D = CountersHandle;
        Data.E = ReferenceStatsHandle;
        if (!Data.bEnabled) { return; }
        Builder.WriteTexture(Data.Tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.A, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(Data.B, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.C, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.D, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.E, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(BrickArgsHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.UavBarrier(Data.Tex);
        Builder.UavBarrier(Data.A);
        Builder.UavBarrier(Data.E);
    }, [this, OwnerPtr, GraphPtr, FillCommonConstants, BrickArgsHandle, MaxJobCount](const FScatterDirectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled) { return; }
        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ScatterFinalizeBricksPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterFinalizeBindlessConstants Bindless =
        {
            GraphPtr->GetTextureUavBindlessIndex(Data.Tex),
            GraphPtr->GetBufferUavBindlessIndex(Data.A),
            GraphPtr->GetBufferSrvBindlessIndex(Data.B),
            GraphPtr->GetBufferSrvBindlessIndex(Data.C),
            GraphPtr->GetBufferSrvBindlessIndex(Data.D),
            GraphPtr->GetBufferUavBindlessIndex(Data.E)
        };
        if (!AreAllBindlessIndicesValid(Bindless.SdfAtlasUavIndex, Bindless.BrickMetadataUavIndex, Bindless.ScatterBrickListSrvIndex, Bindless.ScatterBrickSdfSrvIndex, Bindless.ScatterCountersSrvIndex, Bindless.ReferenceStatsUavIndex)) { return; }
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterFinalizeBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        ID3D12Resource* Args = GraphPtr->GetBufferResource(BrickArgsHandle);
        if (Args)
        {
            CommandList->ExecuteIndirect(DispatchCommandSignature.Get(), 1u, Args, 0u, nullptr, 0u);
        }
    });
}

void FSparseSdfGI::AddBuildTraceHierarchyPasses(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGBufferHandle BottomHandle = Context.Resources.SparseSdfGI.TraceHierarchyBottomHandle;
    const FRGBufferHandle TopHandle = Context.Resources.SparseSdfGI.TraceHierarchyTopHandle;
    FDeferredRenderer* OwnerPtr = &Context.Owner;
    FRenderGraph* GraphPtr = &Context.Graph;

    struct FBuildBottomPassData
    {
        bool bEnabled = false;
        FRGBufferHandle BrickMapHandle{};
        FRGBufferHandle BrickMetadataHandle{};
        FRGBufferHandle BottomHandle{};
    };

    Graph.AddPass<FBuildBottomPassData>("SparseSdfGI Build Trace Hierarchy Bottom", [this, BrickMapHandle, BrickMetadataHandle, BottomHandle](FBuildBottomPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickMapHandle = BrickMapHandle;
        Data.BrickMetadataHandle = BrickMetadataHandle;
        Data.BottomHandle = BottomHandle;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && BuildTraceHierarchyBottomPipeline
            && static_cast<bool>(Data.BrickMapHandle)
            && static_cast<bool>(Data.BrickMetadataHandle)
            && static_cast<bool>(Data.BottomHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.BrickMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickMetadataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.BottomHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.BottomHandle);
    }, [this, OwnerPtr, GraphPtr](const FBuildBottomPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(BuildTraceHierarchyBottomPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIBuildTraceHierarchyBottomBindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.BrickMapHandle),
            GraphPtr->GetBufferSrvBindlessIndex(Data.BrickMetadataHandle),
            GraphPtr->GetBufferUavBindlessIndex(Data.BottomHandle)
        };
        static_assert(sizeof(FSparseSdfGIBuildTraceHierarchyBottomBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        if (!AreAllBindlessIndicesValid(Bindless.CascadeBrickMapSrvIndex, Bindless.BrickMetadataSrvIndex, Bindless.TraceHierarchyBottomUavIndex))
        {
            return;
        }

        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIBuildTraceHierarchyBottomBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetTraceHierarchyBottomNodeCount(), 64u), 1u, 1u);
    });

    struct FBuildTopPassData
    {
        bool bEnabled = false;
        FRGBufferHandle BottomHandle{};
        FRGBufferHandle TopHandle{};
    };

    Graph.AddPass<FBuildTopPassData>("SparseSdfGI Build Trace Hierarchy Top", [this, BottomHandle, TopHandle](FBuildTopPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BottomHandle = BottomHandle;
        Data.TopHandle = TopHandle;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && BuildTraceHierarchyTopPipeline
            && static_cast<bool>(Data.BottomHandle)
            && static_cast<bool>(Data.TopHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.BottomHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.TopHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.TopHandle);
    }, [this, OwnerPtr, GraphPtr](const FBuildTopPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = *OwnerPtr;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(BuildTraceHierarchyTopPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIBuildTraceHierarchyTopBindlessConstants Bindless =
        {
            GraphPtr->GetBufferSrvBindlessIndex(Data.BottomHandle),
            GraphPtr->GetBufferUavBindlessIndex(Data.TopHandle)
        };
        static_assert(sizeof(FSparseSdfGIBuildTraceHierarchyTopBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        if (!AreAllBindlessIndicesValid(Bindless.TraceHierarchyBottomSrvIndex, Bindless.TraceHierarchyTopUavIndex))
        {
            return;
        }

        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIBuildTraceHierarchyTopBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetTraceHierarchyTopNodeCount(), 64u), 1u, 1u);
    });
}

void FSparseSdfGI::AddReferenceStatsPresentPass(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGBufferHandle ReferenceStatsHandle = Context.Resources.SparseSdfGI.ReferenceCounterStatsHandle;

    struct FReferenceStatsPresentPassData
    {
        bool bEnabled = false;
        FRGBufferHandle ReferenceStatsHandle{};
        uint32_t DebugPrintStatsUavIndex = UINT32_MAX;
    };

    Graph.AddPass<FReferenceStatsPresentPassData>("SparseSdfGI Reference Stats Present", [&, ReferenceStatsHandle](FReferenceStatsPresentPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.ReferenceStatsHandle = ReferenceStatsHandle;
        Data.DebugPrintStatsUavIndex = Context.Owner.GpuDebugState.IsPrintEnabled()
            ? Context.Owner.GpuDebugState.GetPrintStatsUavBindlessIndex()
            : UINT32_MAX;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && ReferenceStatsPresentPipeline
            && static_cast<bool>(Data.ReferenceStatsHandle)
            && IsValidBindlessIndex(Data.DebugPrintStatsUavIndex);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.ReferenceStatsHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }, [this, &Context](const FReferenceStatsPresentPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ReferenceStatsPresentPipeline.Get());

        const FSparseSdfGIReferenceStatsPresentBindlessConstants Bindless =
        {
            Context.Graph.GetBufferSrvBindlessIndex(Data.ReferenceStatsHandle),
            Data.DebugPrintStatsUavIndex
        };
        static_assert(sizeof(FSparseSdfGIReferenceStatsPresentBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        if (!AreAllBindlessIndicesValid(Bindless.ReferenceStatsSrvIndex, Bindless.DebugPrintStatsUavIndex))
        {
            return;
        }

        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIReferenceStatsPresentBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(1u, 1u, 1u);
    });
}

void FSparseSdfGI::AddRadianceCachePasses(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle ShadowMaskHandle = Context.Resources.RayTracingShadow.ShadowMaskHandle;

    const FRGBufferHandle ImportedBrickIrradianceReadHandle = Context.Resources.SparseSdfGI.BrickIrradianceReadHandle;
    const bool bIrradianceReadSlotValid = CurrentBrickIrradianceReadSlot < BrickIrradianceHistoryValid.size() && BrickIrradianceHistoryValid[CurrentBrickIrradianceReadSlot];
    const bool bMultiBounceReady = bMultiBounce && bIrradianceReadSlotValid && static_cast<bool>(ImportedBrickIrradianceReadHandle);

    const FRGBufferHandle BrickIrradianceReadHandle = bMultiBounceReady ? ImportedBrickIrradianceReadHandle : FRGBufferHandle{};
    const FRGBufferHandle BrickRadianceReadHandle = Context.Resources.SparseSdfGI.BrickRadianceReadHandle;
    const FRGBufferHandle BrickRadianceWriteHandle = Context.Resources.SparseSdfGI.BrickRadianceWriteHandle;
    const bool bReadSlotValid = CurrentBrickRadianceReadSlot < BrickRadianceHistoryValid.size() && BrickRadianceHistoryValid[CurrentBrickRadianceReadSlot];
    const bool bHistoryValidForResolve = bEnableRadianceTemporalReuse && bReadSlotValid;
    const bool bRadianceCacheWillUpdate = bEnabled
        && bPersistentInputsValid
        && DebugMode == ESparseSdfGIDebugMode::Off
        && static_cast<bool>(DepthHandle)
        && static_cast<bool>(GBufferHandles[0])
        && static_cast<bool>(GBufferHandles[1])
        && static_cast<bool>(GBufferHandles[2])
        && static_cast<bool>(BrickRadianceReadHandle)
        && static_cast<bool>(BrickRadianceWriteHandle);

    struct FRadianceClearPassData
    {
        bool bEnabled = false;
        FRGBufferHandle BrickRadianceAccumHandle{};
    };

    Graph.AddPass<FRadianceClearPassData>("SparseSdfGI Radiance Clear", [&, bRadianceCacheWillUpdate](FRadianceClearPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bRadianceCacheWillUpdate;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.BrickRadianceAccumHandle = Builder.CreateBuffer("SparseSdfGI Brick Radiance Accum", CreateBrickRadianceAccumDesc());
        Context.Resources.SparseSdfGI.BrickRadianceAccumHandle = Data.BrickRadianceAccumHandle;
        Builder.WriteBuffer(Data.BrickRadianceAccumHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.BrickRadianceAccumHandle);
    }, [this, &Context](const FRadianceClearPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(RadianceClearPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.Enabled = bEnabled ? 1u : 0u;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIRadianceClearBindlessConstants Bindless =
        {
            Context.Graph.GetBufferUavBindlessIndex(Data.BrickRadianceAccumHandle)
        };
        if (!AreAllBindlessIndicesValid(Bindless.BrickRadianceAccumUavIndex))
        {
            return;
        }
        static_assert(sizeof(FSparseSdfGIRadianceClearBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIRadianceClearBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetBrickMapElementCount(), 64u), 1u, 1u);
    });

    struct FRadianceInjectPassData
    {
        bool bEnabled = false;
        FRGBufferHandle BrickRadianceAccumHandle{};
        FRGResourceHandle ShadowMaskHandle{};
        FRGBufferHandle BrickIrradianceReadHandle{};
    };

    Graph.AddPass<FRadianceInjectPassData>("SparseSdfGI Radiance Inject", [&, DepthHandle, ShadowMaskHandle, GBufferHandles, BrickIrradianceReadHandle](FRadianceInjectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickRadianceAccumHandle = Context.Resources.SparseSdfGI.BrickRadianceAccumHandle;
        Data.ShadowMaskHandle = ShadowMaskHandle;
        Data.BrickIrradianceReadHandle = BrickIrradianceReadHandle;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && DebugMode == ESparseSdfGIDebugMode::Off
            && static_cast<bool>(DepthHandle)
            && static_cast<bool>(GBufferHandles[0])
            && static_cast<bool>(GBufferHandles[1])
            && static_cast<bool>(GBufferHandles[2])
            && static_cast<bool>(Data.BrickRadianceAccumHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Data.ShadowMaskHandle)
        {
            Builder.ReadTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (Data.BrickIrradianceReadHandle)
        {
            Builder.ReadBuffer(Data.BrickIrradianceReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteBuffer(Data.BrickRadianceAccumHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.BrickRadianceAccumHandle);
    }, [this, &Context](const FRadianceInjectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        const uint32_t ShadowMaskBindlessIndex = Data.ShadowMaskHandle ? Context.Graph.GetTextureSrvBindlessIndex(Data.ShadowMaskHandle) : UINT32_MAX;
        const bool bUseShadowMask = Owner.bShadowsEnabled
            && Owner.bRayTracedShadowsEnabled
            && Owner.GetRayTracingRuntime().bRayTracingPipelineReady
            && IsValidBindlessIndex(ShadowMaskBindlessIndex);
        const uint32_t BrickRadianceAccumUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickRadianceAccumHandle);
        if (!AreAllBindlessIndicesValid(
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.GBufferC.SrvBindlessIndex,
            BrickRadianceAccumUavIndex))
        {
            return;
        }

        const uint32_t BrickIrradianceReadBufferIndex = Data.BrickIrradianceReadHandle ? Context.Graph.GetBufferSrvBindlessIndex(Data.BrickIrradianceReadHandle) : UINT32_MAX;
        const bool bUseMultiBounce = bMultiBounce && IsValidBindlessIndex(BrickIrradianceReadBufferIndex);

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(RadianceInjectPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.BounceStrength = bUseMultiBounce ? MultiBounceStrength : 0.0f;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIRadianceInjectBindlessConstants Bindless =
        {
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.GBufferC.SrvBindlessIndex,
            BrickRadianceAccumUavIndex,
            bUseShadowMask ? ShadowMaskBindlessIndex : UINT32_MAX,
            bUseShadowMask ? 1u : 0u,
            bUseMultiBounce ? BrickIrradianceReadBufferIndex : UINT32_MAX
        };
        static_assert(sizeof(FSparseSdfGIRadianceInjectBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIRadianceInjectBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(
            AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Width), kSparseSdfGIGroupSize2D),
            AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Height), kSparseSdfGIGroupSize2D),
            1u);
    });

    struct FRadianceResolvePassData
    {
        bool bEnabled = false;
        bool bHistoryValid = false;
        FRGBufferHandle BrickRadianceAccumHandle{};
        FRGBufferHandle BrickRadianceReadHandle{};
        FRGBufferHandle BrickRadianceWriteHandle{};
    };

    Graph.AddPass<FRadianceResolvePassData>("SparseSdfGI Radiance Resolve", [&, BrickRadianceReadHandle, BrickRadianceWriteHandle, bHistoryValidForResolve](FRadianceResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickRadianceAccumHandle = Context.Resources.SparseSdfGI.BrickRadianceAccumHandle;
        Data.BrickRadianceReadHandle = BrickRadianceReadHandle;
        Data.BrickRadianceWriteHandle = BrickRadianceWriteHandle;
        Data.bHistoryValid = bHistoryValidForResolve;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && DebugMode == ESparseSdfGIDebugMode::Off
            && static_cast<bool>(Data.BrickRadianceAccumHandle)
            && static_cast<bool>(Data.BrickRadianceReadHandle)
            && static_cast<bool>(Data.BrickRadianceWriteHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.BrickRadianceAccumHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickRadianceReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.BrickRadianceWriteHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.BrickRadianceWriteHandle);
    }, [this, &Context](const FRadianceResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t BrickRadianceAccumSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceAccumHandle);
        const uint32_t BrickRadianceHistorySrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceReadHandle);
        const uint32_t BrickRadianceUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickRadianceWriteHandle);
        if (!AreAllBindlessIndicesValid(BrickRadianceAccumSrvIndex, BrickRadianceHistorySrvIndex, BrickRadianceUavIndex))
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(RadianceResolvePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.Enabled = bEnabled ? 1u : 0u;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIRadianceResolveBindlessConstants Bindless =
        {
            BrickRadianceAccumSrvIndex,
            BrickRadianceHistorySrvIndex,
            BrickRadianceUavIndex,
            Data.bHistoryValid ? 1u : 0u
        };
        static_assert(sizeof(FSparseSdfGIRadianceResolveBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIRadianceResolveBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetBrickMapElementCount(), 64u), 1u, 1u);
    });

    if (bRadianceCacheWillUpdate && CurrentBrickRadianceWriteSlot < PendingBrickRadianceWrite.size())
    {
        PendingBrickRadianceWrite[CurrentBrickRadianceWriteSlot] = true;
        BrickRadianceHistoryValid[CurrentBrickRadianceWriteSlot] = false;
    }
}

void FSparseSdfGI::AddIrradianceCacheUpdatePasses(FDeferredPassContext& Context) const
{
    if (!bMultiBounce)
    {
        return;
    }

    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle DiffuseGIHandle = Context.Resources.SparseSdfGI.DiffuseGIHandle;
    const FRGBufferHandle BrickIrradianceReadHandle = Context.Resources.SparseSdfGI.BrickIrradianceReadHandle;
    const FRGBufferHandle BrickIrradianceWriteHandle = Context.Resources.SparseSdfGI.BrickIrradianceWriteHandle;
    const bool bReadSlotValid = CurrentBrickIrradianceReadSlot < BrickIrradianceHistoryValid.size() && BrickIrradianceHistoryValid[CurrentBrickIrradianceReadSlot];
    const bool bWillUpdate = bEnabled
        && bPersistentInputsValid
        && DebugMode == ESparseSdfGIDebugMode::Off
        && static_cast<bool>(DepthHandle)
        && static_cast<bool>(DiffuseGIHandle)
        && static_cast<bool>(BrickIrradianceReadHandle)
        && static_cast<bool>(BrickIrradianceWriteHandle);

    struct FIrradianceClearPassData
    {
        bool bEnabled = false;
        FRGBufferHandle AccumHandle{};
    };

    Graph.AddPass<FIrradianceClearPassData>("SparseSdfGI Irradiance Clear", [&, bWillUpdate](FIrradianceClearPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bWillUpdate;
        if (!Data.bEnabled)
        {
            return;
        }
        Data.AccumHandle = Builder.CreateBuffer("SparseSdfGI Brick Irradiance Accum", CreateBrickRadianceAccumDesc());
        Context.Resources.SparseSdfGI.BrickIrradianceAccumHandle = Data.AccumHandle;
        Builder.WriteBuffer(Data.AccumHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.AccumHandle);
    }, [this, &Context](const FIrradianceClearPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t AccumUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.AccumHandle);
        if (!AreAllBindlessIndicesValid(AccumUavIndex))
        {
            return;
        }
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(RadianceClearPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.Enabled = bEnabled ? 1u : 0u;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }
        const FSparseSdfGIRadianceClearBindlessConstants Bindless = { AccumUavIndex };
        static_assert(sizeof(FSparseSdfGIRadianceClearBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIRadianceClearBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetBrickMapElementCount(), 64u), 1u, 1u);
    });

    struct FIrradianceAccumulatePassData
    {
        bool bEnabled = false;
        FRGBufferHandle AccumHandle{};
    };

    Graph.AddPass<FIrradianceAccumulatePassData>("SparseSdfGI Irradiance Accumulate", [&, DepthHandle, DiffuseGIHandle, bWillUpdate](FIrradianceAccumulatePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.AccumHandle = Context.Resources.SparseSdfGI.BrickIrradianceAccumHandle;
        Data.bEnabled = bWillUpdate && static_cast<bool>(Data.AccumHandle);
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DiffuseGIHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.AccumHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.AccumHandle);
    }, [this, &Context, DiffuseGIHandle](const FIrradianceAccumulatePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        const uint32_t DiffuseGIIndex = Context.Graph.GetTextureSrvBindlessIndex(DiffuseGIHandle);
        const uint32_t AccumUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.AccumHandle);
        if (!AreAllBindlessIndicesValid(DepthBindlessIndex, DiffuseGIIndex, AccumUavIndex))
        {
            return;
        }
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(IrradianceAccumulatePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }
        const FSparseSdfGIIrradianceAccumulateBindlessConstants Bindless = { DepthBindlessIndex, DiffuseGIIndex, AccumUavIndex };
        static_assert(sizeof(FSparseSdfGIIrradianceAccumulateBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIIrradianceAccumulateBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(
            AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Width), kSparseSdfGIGroupSize2D),
            AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Height), kSparseSdfGIGroupSize2D),
            1u);
    });

    struct FIrradianceResolvePassData
    {
        bool bEnabled = false;
        bool bHistoryValid = false;
        FRGBufferHandle AccumHandle{};
        FRGBufferHandle ReadHandle{};
        FRGBufferHandle WriteHandle{};
    };

    Graph.AddPass<FIrradianceResolvePassData>("SparseSdfGI Irradiance Resolve", [&, BrickIrradianceReadHandle, BrickIrradianceWriteHandle, bReadSlotValid, bWillUpdate](FIrradianceResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.AccumHandle = Context.Resources.SparseSdfGI.BrickIrradianceAccumHandle;
        Data.ReadHandle = BrickIrradianceReadHandle;
        Data.WriteHandle = BrickIrradianceWriteHandle;
        Data.bHistoryValid = bReadSlotValid;
        Data.bEnabled = bWillUpdate
            && static_cast<bool>(Data.AccumHandle)
            && static_cast<bool>(Data.ReadHandle)
            && static_cast<bool>(Data.WriteHandle);
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadBuffer(Data.AccumHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.WriteHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.WriteHandle);
    }, [this, &Context](const FIrradianceResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t AccumSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.AccumHandle);
        const uint32_t HistorySrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ReadHandle);
        const uint32_t UavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.WriteHandle);
        if (!AreAllBindlessIndicesValid(AccumSrvIndex, HistorySrvIndex, UavIndex))
        {
            return;
        }
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(RadianceResolvePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.Enabled = bEnabled ? 1u : 0u;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }
        const FSparseSdfGIRadianceResolveBindlessConstants Bindless =
        {
            AccumSrvIndex,
            HistorySrvIndex,
            UavIndex,
            Data.bHistoryValid ? 1u : 0u
        };
        static_assert(sizeof(FSparseSdfGIRadianceResolveBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIRadianceResolveBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetBrickMapElementCount(), 64u), 1u, 1u);
    });

    if (bWillUpdate && CurrentBrickIrradianceWriteSlot < PendingBrickIrradianceWrite.size())
    {
        PendingBrickIrradianceWrite[CurrentBrickIrradianceWriteSlot] = true;
        BrickIrradianceHistoryValid[CurrentBrickIrradianceWriteSlot] = false;
    }
}

void FSparseSdfGI::AddScreenProbeGITracePasses(FDeferredPassContext& Context, FRGBufferHandle BrickRadianceHandle) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = bProbeMotionReproject ? Context.Resources.VelocityHandle : FRGResourceHandle{};
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGBufferHandle TraceHierarchyBottomHandle = Context.Resources.SparseSdfGI.TraceHierarchyBottomHandle;
    const FRGBufferHandle TraceHierarchyTopHandle = Context.Resources.SparseSdfGI.TraceHierarchyTopHandle;
    const FRGResourceHandle DiffuseHandle = Context.Resources.SparseSdfGI.DiffuseGIHandle;
    const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
    const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
    const uint32_t TileSize = std::clamp(ProbeTileSize, 4u, 16u);
    const uint32_t ProbeCountX = AlignDispatch(FullWidth, TileSize);
    const uint32_t ProbeCountY = AlignDispatch(FullHeight, TileSize);
    const uint32_t ProbeCount = (std::max)(1u, ProbeCountX * ProbeCountY);

    FRGBufferHandle ProbeHeaderHandle{};
    FRGBufferHandle ProbeSHHandle{};
    FRGBufferHandle ProbeVarianceHandle{};

    struct FProbeSpawnPassData
    {
        bool bEnabled = false;
        FRGBufferHandle ProbeHeaderHandle{};
        FRGResourceHandle VelocityHandle{};
    };

    Graph.AddPass<FProbeSpawnPassData>("SparseSdfGI Probe Spawn", [&, DepthHandle, GBufferHandles, VelocityHandle](FProbeSpawnPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid && ProbeSpawnPipeline && DepthHandle && GBufferHandles[0];
        if (!Data.bEnabled)
        {
            return;
        }

        Data.ProbeHeaderHandle = Builder.CreateBuffer("SparseSdfGI Probe Headers", CreateScreenProbeHeaderDesc(ProbeCount));
        ProbeHeaderHandle = Data.ProbeHeaderHandle;
        Data.VelocityHandle = VelocityHandle;
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Data.VelocityHandle)
        {
            Builder.ReadTexture(Data.VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteBuffer(Data.ProbeHeaderHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Context, ProbeCountX, ProbeCountY, TileSize](const FProbeSpawnPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        const uint32_t ProbeHeaderUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ProbeHeaderHandle);
        if (!AreAllBindlessIndicesValid(DepthBindlessIndex, Owner.GBufferA.SrvBindlessIndex, ProbeHeaderUavIndex))
        {
            return;
        }

        const uint32_t VelocityBindlessIndex = Data.VelocityHandle ? Context.Graph.GetTextureSrvBindlessIndex(Data.VelocityHandle) : UINT32_MAX;

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ProbeSpawnPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.ProbeTileSize = TileSize;
        Constants.ProbeCountX = ProbeCountX;
        Constants.ProbeCountY = ProbeCountY;
        Constants.ProbeRaysPerProbe = ProbeRaysPerProbe;
        Constants.ProbeDebugMode = ProbeDebugMode;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIProbeSpawnBindlessConstants Bindless =
        {
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            ProbeHeaderUavIndex,
            bProbeSpawnJitter ? 1u : 0u,
            Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex,
            VelocityBindlessIndex
        };
        static_assert(sizeof(FSparseSdfGIProbeSpawnBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIProbeSpawnBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(ProbeCountX, kSparseSdfGIGroupSize2D), AlignDispatch(ProbeCountY, kSparseSdfGIGroupSize2D), 1u);
    });

    struct FProbeTracePassData
    {
        bool bEnabled = false;
        FRGBufferHandle ProbeHeaderHandle{};
        FRGBufferHandle ProbeSHHandle{};
        FRGBufferHandle ProbeVarianceHandle{};
        FRGBufferHandle BrickRadianceHandle{};
        FRGBufferHandle TraceHierarchyBottomHandle{};
        FRGBufferHandle TraceHierarchyTopHandle{};
        FRGBufferHandle ProbeHistoryReadHandle{};
        FRGBufferHandle ProbeHistoryWriteHandle{};
        bool bHistoryValid = false;
    };

    const FRGBufferHandle ProbeHistoryReadHandle = Context.Resources.SparseSdfGI.ProbeHistoryReadHandle;
    const FRGBufferHandle ProbeHistoryWriteHandle = Context.Resources.SparseSdfGI.ProbeHistoryWriteHandle;
    const bool bProbeReadSlotValid = CurrentProbeHistoryReadSlot < ProbeHistoryValid.size() && ProbeHistoryValid[CurrentProbeHistoryReadSlot];
    const bool bProbeHistoryValidForTrace = bProbeTemporalReuse && bProbeReadSlotValid;

    Graph.AddPass<FProbeTracePassData>("SparseSdfGI Probe Trace", [&, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, TraceHierarchyBottomHandle, TraceHierarchyTopHandle, BrickRadianceHandle, ProbeHistoryReadHandle, ProbeHistoryWriteHandle, bProbeHistoryValidForTrace](FProbeTracePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.ProbeHeaderHandle = ProbeHeaderHandle;
        Data.BrickRadianceHandle = BrickRadianceHandle;
        Data.TraceHierarchyBottomHandle = TraceHierarchyBottomHandle;
        Data.TraceHierarchyTopHandle = TraceHierarchyTopHandle;
        Data.ProbeHistoryReadHandle = ProbeHistoryReadHandle;
        Data.ProbeHistoryWriteHandle = ProbeHistoryWriteHandle;
        Data.bHistoryValid = bProbeHistoryValidForTrace;
        const bool bTracePipelineReady = bProbeDirectionalSH ? (ProbeTraceDirectionalPipeline != nullptr) : (ProbeTracePipeline != nullptr);
        Data.bEnabled = bEnabled && bPersistentInputsValid && bTracePipelineReady && Data.ProbeHeaderHandle && SdfAtlasHandle && BrickMapHandle && BrickMetadataHandle && TraceHierarchyBottomHandle && TraceHierarchyTopHandle && BrickRadianceHandle && ProbeHistoryReadHandle && ProbeHistoryWriteHandle;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.ProbeSHHandle = Builder.CreateBuffer("SparseSdfGI Probe SH", CreateScreenProbeSHDesc(ProbeCount));
        Data.ProbeVarianceHandle = Builder.CreateBuffer("SparseSdfGI Probe Variance", CreateScreenProbeVarianceDesc(ProbeCount));
        ProbeSHHandle = Data.ProbeSHHandle;
        ProbeVarianceHandle = Data.ProbeVarianceHandle;
        Builder.ReadTexture(SdfAtlasHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickMetadataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.TraceHierarchyBottomHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.TraceHierarchyTopHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ProbeHeaderHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ProbeHistoryReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.ProbeSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.ProbeVarianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.ProbeHistoryWriteHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Context, ProbeCountX, ProbeCountY, TileSize](const FProbeTracePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t ProbeHeaderSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ProbeHeaderHandle);
        const uint32_t ProbeSHUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ProbeSHHandle);
        const uint32_t ProbeVarianceUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ProbeVarianceHandle);
        const uint32_t BrickRadianceSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceHandle);
        const uint32_t ProbeHistoryReadSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ProbeHistoryReadHandle);
        const uint32_t ProbeHistoryWriteUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ProbeHistoryWriteHandle);
        const uint32_t TraceHierarchyBottomSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.TraceHierarchyBottomHandle);
        const uint32_t TraceHierarchyTopSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.TraceHierarchyTopHandle);
        const uint32_t EnvironmentCubeIndex = Owner.GetEnvironmentCubeSrvIndex();
        const uint32_t LinearClampSamplerIndex = Owner.Device->GetLinearClampSamplerIndex();
        const uint32_t BlueNoiseSobolIndex = Owner.BlueNoiseSobolTexture.SrvBindlessIndex;
        const uint32_t BlueNoiseScramblingIndex = Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex;
        if (!AreAllBindlessIndicesValid(
            SdfAtlas.SrvBindlessIndex,
            CascadeBrickMap.SrvBindlessIndex,
            BrickMetadata.SrvBindlessIndex,
            BrickRadianceSrvIndex,
            EnvironmentCubeIndex,
            LinearClampSamplerIndex,
            ProbeHeaderSrvIndex,
            ProbeSHUavIndex,
            ProbeVarianceUavIndex,
            ProbeHistoryReadSrvIndex,
            ProbeHistoryWriteUavIndex,
            TraceHierarchyBottomSrvIndex,
            TraceHierarchyTopSrvIndex,
            BlueNoiseSobolIndex,
            BlueNoiseScramblingIndex))
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState((bProbeDirectionalSH ? ProbeTraceDirectionalPipeline : ProbeTracePipeline).Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.Intensity = Intensity;
        Constants.BounceStrength = BounceStrength;
        Constants.MaxTraceDistance = Bounds.Extent.x;
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.ProbeTileSize = TileSize;
        Constants.ProbeCountX = ProbeCountX;
        Constants.ProbeCountY = ProbeCountY;
        Constants.ProbeRaysPerProbe = std::clamp(ProbeRaysPerProbe, 4u, 64u);
        Constants.ProbeDebugMode = ProbeDebugMode;
        Constants.ProbeHistoryValid = Data.bHistoryValid ? 1u : 0u;
        Constants.SurfaceHitThresholdVoxels = SurfaceHitThresholdVoxels;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.TrianglePoolCapacity = BlueNoiseSobolIndex;
        Constants.BuildWorkOffset = BlueNoiseScramblingIndex;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIProbeTraceBindlessConstants Bindless =
        {
            SdfAtlas.SrvBindlessIndex,
            CascadeBrickMap.SrvBindlessIndex,
            BrickMetadata.SrvBindlessIndex,
            BrickRadianceSrvIndex,
            EnvironmentCubeIndex,
            LinearClampSamplerIndex,
            ProbeHeaderSrvIndex,
            ProbeSHUavIndex,
            ProbeVarianceUavIndex,
            ProbeHistoryReadSrvIndex,
            ProbeHistoryWriteUavIndex,
            TraceHierarchyBottomSrvIndex,
            TraceHierarchyTopSrvIndex
        };
        static_assert(sizeof(FSparseSdfGIProbeTraceBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIProbeTraceBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch((std::max)(1u, ProbeCountX), (std::max)(1u, ProbeCountY), 1u);

        if (CurrentProbeHistoryWriteSlot < PendingProbeHistoryWrite.size() && CurrentProbeHistoryWriteSlot < ProbeHistoryValid.size())
        {
            PendingProbeHistoryWrite[CurrentProbeHistoryWriteSlot] = true;
            ProbeHistoryValid[CurrentProbeHistoryWriteSlot] = false;
        }
    });

    struct FProbeInterpolatePassData
    {
        bool bEnabled = false;
        FRGBufferHandle ProbeHeaderHandle{};
        FRGBufferHandle ProbeSHHandle{};
        FRGBufferHandle ProbeVarianceHandle{};
        FRGResourceHandle InputSHHandle{};
        FRGResourceHandle VarianceHandle{};
    };

    Graph.AddPass<FProbeInterpolatePassData>("SparseSdfGI Probe Interpolate", [&, DepthHandle, GBufferHandles, DiffuseHandle](FProbeInterpolatePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.ProbeHeaderHandle = ProbeHeaderHandle;
        Data.ProbeSHHandle = ProbeSHHandle;
        Data.ProbeVarianceHandle = ProbeVarianceHandle;
        Data.bEnabled = bEnabled && bPersistentInputsValid && ProbeInterpolatePipeline && DepthHandle && GBufferHandles[0] && Data.ProbeHeaderHandle && Data.ProbeSHHandle && Data.ProbeVarianceHandle;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.InputSHHandle = Builder.CreateTexture("SparseSdfGI Probe Input SH", { FullWidth, FullHeight, DXGI_FORMAT_R32G32B32A32_UINT });
        Data.VarianceHandle = Builder.CreateTexture("SparseSdfGI Probe Variance FullRes", { FullWidth, FullHeight, DXGI_FORMAT_R8_UNORM });
        if (ProbeDebugMode == 0u)
        {
            Context.Resources.SparseSdfGI.DiffuseGIInputSHHandle = Data.InputSHHandle;
            Context.Resources.SparseSdfGI.DiffuseGIVarianceHandle = Data.VarianceHandle;
        }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ProbeHeaderHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ProbeSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ProbeVarianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(DiffuseHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(Data.InputSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(Data.VarianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Context, ProbeCountX, ProbeCountY, TileSize](const FProbeInterpolatePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        const uint32_t DiffuseGIUavIndex = DiffuseGI.UavBindlessIndex;
        const uint32_t ProbeHeaderSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ProbeHeaderHandle);
        const uint32_t ProbeSHSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ProbeSHHandle);
        const uint32_t ProbeVarianceSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ProbeVarianceHandle);
        const uint32_t InputSHUavIndex = Context.Graph.GetTextureUavBindlessIndex(Data.InputSHHandle);
        const uint32_t VarianceUavIndex = Context.Graph.GetTextureUavBindlessIndex(Data.VarianceHandle);
        if (!AreAllBindlessIndicesValid(
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            DiffuseGIUavIndex,
            ProbeHeaderSrvIndex,
            ProbeSHSrvIndex,
            ProbeVarianceSrvIndex,
            InputSHUavIndex,
            VarianceUavIndex))
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(ProbeInterpolatePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.ProbeTileSize = TileSize;
        Constants.ProbeCountX = ProbeCountX;
        Constants.ProbeCountY = ProbeCountY;
        Constants.ProbeRaysPerProbe = std::clamp(ProbeRaysPerProbe, 4u, 64u);
        Constants.ProbeDebugMode = ProbeDebugMode;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIProbeInterpolateBindlessConstants Bindless =
        {
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            DiffuseGIUavIndex,
            ProbeHeaderSrvIndex,
            ProbeSHSrvIndex,
            ProbeVarianceSrvIndex,
            InputSHUavIndex,
            VarianceUavIndex
        };
        static_assert(sizeof(FSparseSdfGIProbeInterpolateBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIProbeInterpolateBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Width), kSparseSdfGIGroupSize2D), AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Height), kSparseSdfGIGroupSize2D), 1u);
    });
}

void FSparseSdfGI::DispatchOutputPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, bool bPassEnabled, FRGBufferHandle BrickRadianceHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle) const
{
    if (!bPassEnabled || !PipelineState)
    {
        return;
    }

    FDeferredRenderer& Owner = Context.Owner;
    const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
    const uint32_t EnvironmentCubeIndex = Owner.GetEnvironmentCubeSrvIndex();
    const uint32_t LinearClampSamplerIndex = Owner.Device->GetLinearClampSamplerIndex();
    const uint32_t InputSHUavIndex = InputSHHandle ? Context.Graph.GetTextureUavBindlessIndex(InputSHHandle) : UINT32_MAX;
    const uint32_t VarianceUavIndex = VarianceHandle ? Context.Graph.GetTextureUavBindlessIndex(VarianceHandle) : UINT32_MAX;
    const uint32_t BrickRadianceSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(BrickRadianceHandle);
    const uint32_t BlueNoiseSobolIndex = Owner.BlueNoiseSobolTexture.SrvBindlessIndex;
    const uint32_t BlueNoiseScramblingIndex = Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex;
    const uint32_t TraceHierarchyBottomSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Context.Resources.SparseSdfGI.TraceHierarchyBottomHandle);
    const uint32_t TraceHierarchyTopSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Context.Resources.SparseSdfGI.TraceHierarchyTopHandle);
    const bool bInputsValid = AreAllBindlessIndicesValid(
        DepthBindlessIndex,
        Owner.GBufferA.SrvBindlessIndex,
        EnvironmentCubeIndex,
        LinearClampSamplerIndex,
        SdfAtlas.SrvBindlessIndex,
        CascadeBrickMap.SrvBindlessIndex,
        BrickMetadata.SrvBindlessIndex,
        BrickRadianceSrvIndex,
        DiffuseGI.UavBindlessIndex,
        BlueNoiseSobolIndex,
        BlueNoiseScramblingIndex,
        TraceHierarchyBottomSrvIndex,
        TraceHierarchyTopSrvIndex);
    const bool bDenoiserOutputsValid = (DebugMode != ESparseSdfGIDebugMode::Off) || AreAllBindlessIndicesValid(InputSHUavIndex, VarianceUavIndex);
    if (!bInputsValid)
    {
        return;
    }
    if (!bDenoiserOutputsValid)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
    ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(RootSignature.Get());
    CommandList->SetPipelineState(PipelineState);
    CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

    const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);

    FSparseSdfGIConstants Constants = {};
    Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
    Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
    Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
    Constants.DebugMode = static_cast<uint32_t>(DebugMode);
    Constants.Enabled = bEnabled ? 1u : 0u;
    Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
    Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
    Constants.Intensity = Intensity;
    Constants.BounceStrength = BounceStrength;
    Constants.MaxTraceDistance = Bounds.Extent.x;
    Constants.CascadeMin = Bounds.Min;
    Constants.VoxelSize = Bounds.VoxelSize;
    Constants.CascadeExtent = Bounds.Extent;
    Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
    Constants.SurfaceHitThresholdVoxels = SurfaceHitThresholdVoxels;
    Constants.TrianglePoolCapacity = BlueNoiseSobolIndex;
    Constants.BuildWorkOffset = BlueNoiseScramblingIndex;
    if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
    {
        return;
    }

    const FSparseSdfGITraceBindlessConstants Bindless =
    {
        SdfAtlas.SrvBindlessIndex,
        CascadeBrickMap.SrvBindlessIndex,
        BrickMetadata.SrvBindlessIndex,
        DiffuseGI.UavBindlessIndex,
        DepthBindlessIndex,
        Owner.GBufferA.SrvBindlessIndex,
        EnvironmentCubeIndex,
        LinearClampSamplerIndex,
        BrickRadianceSrvIndex,
        InputSHUavIndex,
        VarianceUavIndex,
        TraceHierarchyBottomSrvIndex,
        TraceHierarchyTopSrvIndex
    };
    static_assert(sizeof(FSparseSdfGITraceBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
    CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGITraceBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
    CommandList->Dispatch(
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Width), kSparseSdfGIGroupSize2D),
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Height), kSparseSdfGIGroupSize2D),
        1u);
}
