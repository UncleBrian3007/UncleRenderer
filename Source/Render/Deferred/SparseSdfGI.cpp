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
    constexpr uint32_t kSparseSdfGIConstantsDwordCount = 56u;
    constexpr uint32_t kSparseSdfGIMaxBindlessDwordCount = 14u;
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
    constexpr uint32_t kSparseSdfGIMaxCascadeCount = 4u;
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

    uint32_t GetCascadeBrickMapElementCount(uint32_t CascadeCount)
    {
        return GetBrickMapElementCount() * (std::max)(CascadeCount, 1u);
    }

    DXGI_FORMAT SelectSparseSdfGIAtlasFormat(FDX12Device* Device, uint32_t RequestedFormat)
    {
        constexpr D3D12_FORMAT_SUPPORT1 RequiredFlags = static_cast<D3D12_FORMAT_SUPPORT1>(
            D3D12_FORMAT_SUPPORT1_TEXTURE3D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
        const auto TryFormat = [Device, RequiredFlags](DXGI_FORMAT Format)
        {
            return CheckFormatSupport(Device, Format, RequiredFlags);
        };

        if (RequestedFormat == 1u)
        {
            if (TryFormat(DXGI_FORMAT_R8_UNORM))
            {
                LogInfo("SparseSdfGI atlas format selected: DXGI_FORMAT_R8_UNORM");
                return DXGI_FORMAT_R8_UNORM;
            }
            LogError("SparseSdfGI atlas format R8 requested, but DXGI_FORMAT_R8_UNORM typed UAV is unsupported.");
            return DXGI_FORMAT_UNKNOWN;
        }

        if (RequestedFormat == 2u)
        {
            if (TryFormat(DXGI_FORMAT_R16_UNORM))
            {
                LogInfo("SparseSdfGI atlas format selected: DXGI_FORMAT_R16_UNORM");
                return DXGI_FORMAT_R16_UNORM;
            }
            LogError("SparseSdfGI atlas format R16 requested, but DXGI_FORMAT_R16_UNORM typed UAV is unsupported.");
            return DXGI_FORMAT_UNKNOWN;
        }

        if (TryFormat(DXGI_FORMAT_R8_UNORM))
        {
            LogInfo("SparseSdfGI atlas format selected: DXGI_FORMAT_R8_UNORM (Auto)");
            return DXGI_FORMAT_R8_UNORM;
        }

        if (TryFormat(DXGI_FORMAT_R16_UNORM))
        {
            LogWarning("SparseSdfGI atlas format fallback: DXGI_FORMAT_R8_UNORM typed UAV unsupported; using DXGI_FORMAT_R16_UNORM");
            return DXGI_FORMAT_R16_UNORM;
        }

        LogError("SparseSdfGI requires DXGI_FORMAT_R8_UNORM or DXGI_FORMAT_R16_UNORM Texture3D typed UAV support for the SDF atlas.");
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
        return CreateRWStructuredBufferDesc<FTraceHierarchyNodeGpu>(GetTraceHierarchyBottomNodeCount() * kSparseSdfGIMaxCascadeCount);
    }

    FRGBufferDesc CreateTraceHierarchyTopDesc()
    {
        return CreateRWStructuredBufferDesc<FTraceHierarchyNodeGpu>(GetTraceHierarchyTopNodeCount() * kSparseSdfGIMaxCascadeCount);
    }

    struct FBrickShGpu
    {
        DirectX::XMUINT4 PackedSH{};
        float SampleCount = 0.0f;
        DirectX::XMFLOAT3 Reserved{};
    };

    struct FBrickShAccumGpu
    {
        DirectX::XMINT4 ShY{};
        int32_t Co = 0;
        int32_t Cg = 0;
        uint32_t SampleCount = 0;
        uint32_t Reserved = 0;
    };
    static_assert(sizeof(FBrickShGpu) == 32u);
    static_assert(sizeof(FBrickShAccumGpu) == 32u);

    FRGBufferDesc CreateBrickRadianceDesc()
    {
        return CreateRWStructuredBufferDesc<FBrickShGpu>(GetBrickMapElementCount());
    }

    FRGBufferDesc CreateBrickRadianceAccumDesc()
    {
        return CreateRWStructuredBufferDesc<FBrickShAccumGpu>(GetBrickMapElementCount());
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
        uint32_t CascadeIndex = 0;
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
        uint32_t CascadeCount = 1u;
        float CascadeScale = 2.0f;
        uint32_t PhysicalBrickBase = 0u;
        uint32_t ScatterBrickCapacity = kSparseSdfGIDefaultMaxScatterBricks;
        uint32_t CascadeDataSrvIndex = UINT32_MAX;
    };
    static_assert(offsetof(FSparseSdfGIConstants, CascadeMin) == 16u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, CascadeExtent) == 20u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, World) == 24u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, BounceStrength) == 40u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, ProbeRaysPerProbe) == 44u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, TrianglePoolCapacity) == 48u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, UseHierarchicalTrace) == 50u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, CascadeCount) == 51u * sizeof(uint32_t));
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

    struct FSparseSdfGIScatterEikonalBindlessConstants
    {
        uint32_t ScatterBrickSdfUavIndex = UINT32_MAX;
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
        uint32_t ShadowTextureIndex = UINT32_MAX;
        uint32_t ShadowMode = 0u;
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
        uint32_t BrickIrradianceReadIndex = UINT32_MAX;
    };

    constexpr uint32_t kSparseSdfGIShadowModeOff = 0u;
    constexpr uint32_t kSparseSdfGIShadowModeScreenMask = 1u;
    constexpr uint32_t kSparseSdfGIShadowModeShadowMap = 2u;

    struct FSparseSdfGIIrradianceAccumulateBindlessConstants
    {
        uint32_t DepthIndex = UINT32_MAX;
        uint32_t DiffuseGIIndex = UINT32_MAX;
        uint32_t BrickIrradianceAccumUavIndex = UINT32_MAX;
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
    };

    struct FSparseSdfGIRadianceResolveBindlessConstants
    {
        uint32_t BrickRadianceAccumSrvIndex = UINT32_MAX;
        uint32_t BrickRadianceHistorySrvIndex = UINT32_MAX;
        uint32_t BrickRadianceUavIndex = UINT32_MAX;
        uint32_t RadianceHistoryValid = 0u;
    };

    struct FSparseSdfGIBrickShPropagateBindlessConstants
    {
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
        uint32_t BrickShSourceSrvIndex = UINT32_MAX;
        uint32_t BrickShDestUavIndex = UINT32_MAX;
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
        ScatterEikonalPipeline.Reset();
        ScatterFinalizeBricksPipeline.Reset();
        BuildTraceHierarchyBottomPipeline.Reset();
        BuildTraceHierarchyTopPipeline.Reset();
        ReferenceStatsPresentPipeline.Reset();
        RadianceClearPipeline.Reset();
        RadianceInjectPipeline.Reset();
        IrradianceAccumulatePipeline.Reset();
        RadianceResolvePipeline.Reset();
        BrickShPropagatePipeline.Reset();
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
    const uint32_t NewCascadeCount = std::clamp(Config.SparseSdfGICascadeCount, 1u, kSparseSdfGIMaxCascadeCount);
    const float NewBaseVoxelSize = Config.SparseSdfGIBaseVoxelSize;
    const float NewCascadeScale = (std::max)(Config.SparseSdfGICascadeScale, 1.01f);
    const uint32_t NewMaxBrickTriangleReferences = std::clamp(Config.SparseSdfGIMaxBrickTriangleReferences, kSparseSdfGIMinBrickTriangleReferences, kSparseSdfGIMaxBrickTriangleReferencesLimit);
    const uint32_t NewMaxScatterBricks = std::clamp(Config.SparseSdfGIMaxScatterBricks, kSparseSdfGIMinScatterBricks, kSparseSdfGIMaxScatterBricksLimit);
    const uint32_t NewEffectiveMaxScatterBricks = (std::min)(NewMaxScatterBricks, GetBrickMapElementCount() / NewCascadeCount);
    const uint32_t NewSdfAtlasFormat = std::clamp(Config.SparseSdfGISdfAtlasFormat, 0u, 2u);
    const bool bBuildSettingsChanged =
        CascadeCount != NewCascadeCount ||
        BaseVoxelSize != NewBaseVoxelSize ||
        CascadeScale != NewCascadeScale ||
        MaxBrickTriangleReferences != NewMaxBrickTriangleReferences ||
        MaxScatterBricks != NewMaxScatterBricks ||
        EffectiveMaxScatterBricks != NewEffectiveMaxScatterBricks;

    bEnabled = Config.DiffuseGISource == EDiffuseGISource::SparseSdfGI;
    DebugMode = static_cast<ESparseSdfGIDebugMode>(std::clamp<uint32_t>(Config.SparseSdfGIDebugMode, SPARSE_SDF_GI_DEBUG_MODE_OFF, SPARSE_SDF_GI_DEBUG_MODE_MAX));
    CascadeCount = NewCascadeCount;
    BaseVoxelSize = NewBaseVoxelSize;
    CascadeScale = NewCascadeScale;
    bTraceHalfResolution = Config.bSparseSdfGITraceHalfResolution;
    bUseHierarchicalTrace = Config.bSparseSdfGIUseHierarchicalTrace;
    bEikonalEnabled = Config.bSparseSdfGIEikonalEnabled;
    Intensity = (std::max)(0.0f, Config.SparseSdfGIIntensity);
    BounceStrength = (std::max)(0.0f, Config.SparseSdfGIBounceStrength);
    bEnableRadianceTemporalReuse = Config.bSparseSdfGIEnableRadianceTemporalReuse;
    bUseScreenProbes = Config.bSparseSdfGIUseScreenProbes;
    ProbeTileSize = std::clamp(Config.SparseSdfGIProbeTileSize, 4u, 16u);
    ProbeRaysPerProbe = std::clamp(Config.SparseSdfGIProbeRaysPerProbe, 4u, 64u);
    ProbeDebugMode = std::clamp<uint32_t>(Config.SparseSdfGIProbeDebugMode, SPARSE_SDF_GI_PROBE_DEBUG_MODE_OFF, SPARSE_SDF_GI_PROBE_DEBUG_MODE_MAX);
    bProbeTemporalReuse = Config.bSparseSdfGIProbeTemporalReuse;
    bProbeDirectionalSH = Config.bSparseSdfGIProbeDirectionalSH;
    bProbeSpawnJitter = Config.bSparseSdfGIProbeSpawnJitter;
    bProbeMotionReproject = Config.bSparseSdfGIProbeMotionReproject;
    bMultiBounce = Config.bSparseSdfGIMultiBounce;
    MultiBounceStrength = (std::max)(0.0f, Config.SparseSdfGIMultiBounceStrength);
    SurfaceHitThresholdVoxels = std::clamp(Config.SparseSdfGISurfaceHitThresholdVoxels, 0.05f, kSparseSdfGISurfaceHitThresholdVoxels);
    MaxBrickTriangleReferences = NewMaxBrickTriangleReferences;
    MaxScatterBricks = NewMaxScatterBricks;
    EffectiveMaxScatterBricks = NewEffectiveMaxScatterBricks;
    SdfAtlasFormat = NewSdfAtlasFormat;

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
    const uint32_t CascadeDataSlotCount = static_cast<uint32_t>(CascadeDataBuffers.size());
    CurrentCascadeDataSlot = (CascadeDataSlotCount > 0u) ? (Context.FrameIndex % CascadeDataSlotCount) : 0u;
    CurrentCascadeDataSrvIndex = UINT32_MAX;
    if (CurrentCascadeDataSlot < CascadeDataBuffers.size())
    {
        Resources.CascadeDataHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Cascade Data", CascadeDataBuffers[CurrentCascadeDataSlot]);
        CurrentCascadeDataSrvIndex = CascadeDataBuffers[CurrentCascadeDataSlot].SrvBindlessIndex;
    }
    else
    {
        Resources.CascadeDataHandle = {};
    }
    Resources.TraceHierarchyBottomHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Trace Hierarchy Bottom", TraceHierarchyBottom);
    Resources.TraceHierarchyTopHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Trace Hierarchy Top", TraceHierarchyTop);
    Resources.BrickRadianceAccumHandle = {};
    Resources.BrickIrradianceAccumHandle = {};
    Resources.BrickRadianceResolvedHandle = {};
    Resources.BrickIrradianceResolvedHandle = {};
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

    const FCascadeBounds Bounds = ComputeBaseCascadeBounds(Context.Owner);
    UpdateCascadeData(Bounds);
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
    for (uint32_t CascadeIndex = 0u; CascadeIndex < CascadeCount; ++CascadeIndex)
    {
        const FCascadeBounds CascadeBounds = ComputeCascadeBounds(Bounds, CascadeIndex);
        AddDistributedScatterInitPass(Context, StaticTriangleCount, CascadeBounds, CascadeIndex);
        DrawSectionIndex = 0u;
        for (FMeshSection& Section : DrawSections)
        {
            if (Section.IsStaticRegularMeshCandidate())
            {
                AddSectionDistributedScatterPreparePass(Context, *DrawSections.GetView(DrawSectionIndex).Object, Section, DrawSectionIndex, StaticTriangleCount, CascadeBounds, CascadeIndex);
            }
            ++DrawSectionIndex;
        }
        AddDistributedScatterPasses(Context, StaticTriangleCount, CascadeBounds, CascadeIndex);
        AddBuildTraceHierarchyPasses(Context, CascadeBounds, CascadeIndex);
    }
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
    const FRGBufferHandle CascadeDataHandle = Context.Resources.SparseSdfGI.CascadeDataHandle;
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

    Graph.AddPass<FOutputPassData>("SparseSdfGI Trace", [&, DepthHandle, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, CascadeDataHandle, TraceHierarchyBottomHandle, TraceHierarchyTopHandle, BrickRadianceHandle, DiffuseHandle, GBufferHandles, Pipeline, bWritesDenoiserInputs](FOutputPassData& Data, FRGPassBuilder& Builder)
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
        Builder.ReadBuffer(CascadeDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
    std::vector<uint8_t> ScatterEikonalByteCode;
    std::vector<uint8_t> ScatterFinalizeBricksByteCode;
    std::vector<uint8_t> BuildTraceHierarchyBottomByteCode;
    std::vector<uint8_t> BuildTraceHierarchyTopByteCode;
    std::vector<uint8_t> ReferenceStatsPresentByteCode;
    std::vector<uint8_t> RadianceClearByteCode;
    std::vector<uint8_t> RadianceInjectByteCode;
    std::vector<uint8_t> IrradianceAccumulateByteCode;
    std::vector<uint8_t> RadianceResolveByteCode;
    std::vector<uint8_t> BrickShPropagateByteCode;
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
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSEikonalScatterBricks", ScatterEikonalByteCode, { L"SPARSE_SDF_GI_SCATTER_EIKONAL_SHADER=1" })) { return false; }
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
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSPropagateBrickSH", BrickShPropagateByteCode, { L"SPARSE_SDF_GI_BRICK_SH_PROPAGATE_SHADER=1" }))
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
        && CreateComputePso(ScatterEikonalByteCode, ScatterEikonalPipeline, "CSEikonalScatterBricks")
        && CreateComputePso(ScatterFinalizeBricksByteCode, ScatterFinalizeBricksPipeline, "CSFinalizeScatterBricks")
        && CreateComputePso(BuildTraceHierarchyBottomByteCode, BuildTraceHierarchyBottomPipeline, "CSBuildTraceHierarchyBottom")
        && CreateComputePso(BuildTraceHierarchyTopByteCode, BuildTraceHierarchyTopPipeline, "CSBuildTraceHierarchyTop")
        && CreateComputePso(ReferenceStatsPresentByteCode, ReferenceStatsPresentPipeline, "CSStoreReferenceStatsToGpuDebug")
        && CreateComputePso(RadianceClearByteCode, RadianceClearPipeline, "CSClearBrickRadianceAccum")
        && CreateComputePso(RadianceInjectByteCode, RadianceInjectPipeline, "CSInjectBrickRadiance")
        && CreateComputePso(IrradianceAccumulateByteCode, IrradianceAccumulatePipeline, "CSAccumulateBrickIrradiance")
        && CreateComputePso(RadianceResolveByteCode, RadianceResolvePipeline, "CSResolveBrickRadianceTemporal")
        && CreateComputePso(BrickShPropagateByteCode, BrickShPropagatePipeline, "CSPropagateBrickSH")
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

    const DXGI_FORMAT SelectedSdfAtlasFormat = SelectSparseSdfGIAtlasFormat(Device, SdfAtlasFormat);
    if (SelectedSdfAtlasFormat == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    const FRGTextureDesc SdfAtlasDesc =
    {
        kSparseSdfGIAtlasResolution,
        kSparseSdfGIAtlasResolution,
        SelectedSdfAtlasFormat,
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

    const FRGBufferDesc BrickMapDesc = CreateRWStructuredBufferDesc<uint32_t>(GetCascadeBrickMapElementCount(kSparseSdfGIMaxCascadeCount));
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

    const FRGBufferDesc CascadeDataDesc = CreateStructuredBufferDesc<FCascadeDataGpu>(kSparseSdfGIMaxCascadeCount);
    const uint32_t CascadeDataFrameCount = (std::max)(FramesInFlight, 1u);
    CascadeDataBuffers.resize(CascadeDataFrameCount);
    CascadeDataMapped.assign(CascadeDataFrameCount, nullptr);
    for (uint32_t FrameIndex = 0u; FrameIndex < CascadeDataFrameCount; ++FrameIndex)
    {
        if (!CreateMappedBindlessBuffer(
            Device,
            L"SparseSdfGI_CascadeData",
            CascadeDataDesc,
            CascadeDataBuffers[FrameIndex],
            CascadeDataMapped[FrameIndex]))
        {
            return false;
        }
        CreateBindlessBufferSrv(Device, CascadeDataBuffers[FrameIndex]);
    }

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
        ScatterEikonalPipeline &&
        ScatterFinalizeBricksPipeline &&
        BuildTraceHierarchyBottomPipeline &&
        BuildTraceHierarchyTopPipeline &&
        ReferenceStatsPresentPipeline &&
        RadianceClearPipeline &&
        RadianceInjectPipeline &&
        IrradianceAccumulatePipeline &&
        RadianceResolvePipeline &&
        BrickShPropagatePipeline &&
        ProbeSpawnPipeline &&
        ProbeTracePipeline &&
        ProbeTraceDirectionalPipeline &&
        ProbeInterpolatePipeline &&
        DebugTracePipeline &&
        DiffuseTracePipeline &&
        SdfAtlas.IsFullyBound() &&
        CascadeBrickMap.IsFullyBound() &&
        BrickMetadata.IsFullyBound() &&
        !CascadeDataBuffers.empty() &&
        std::all_of(CascadeDataBuffers.begin(), CascadeDataBuffers.end(), [](const FBindlessBuffer& Buffer)
        {
            return Buffer.HasSrv();
        }) &&
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

FSparseSdfGI::FCascadeBounds FSparseSdfGI::ComputeBaseCascadeBounds(const FDeferredRenderer& Owner) const
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

FSparseSdfGI::FCascadeBounds FSparseSdfGI::ComputeCascadeBounds(const FCascadeBounds& BaseBounds, uint32_t CascadeIndex) const
{
    FCascadeBounds Bounds = BaseBounds;
    const float Scale = std::pow((std::max)(CascadeScale, 1.01f), static_cast<float>(CascadeIndex));
    Bounds.VoxelSize = BaseBounds.VoxelSize * Scale;
    Bounds.Extent = DirectX::XMFLOAT3(BaseBounds.Extent.x * Scale, BaseBounds.Extent.y * Scale, BaseBounds.Extent.z * Scale);

    const DirectX::XMFLOAT3 Center(
        BaseBounds.Min.x + BaseBounds.Extent.x * 0.5f,
        BaseBounds.Min.y + BaseBounds.Extent.y * 0.5f,
        BaseBounds.Min.z + BaseBounds.Extent.z * 0.5f);
    Bounds.Min = DirectX::XMFLOAT3(
        Center.x - Bounds.Extent.x * 0.5f,
        Center.y - Bounds.Extent.y * 0.5f,
        Center.z - Bounds.Extent.z * 0.5f);
    return Bounds;
}

void FSparseSdfGI::UpdateCascadeData(const FCascadeBounds& BaseBounds) const
{
    if (CurrentCascadeDataSlot >= CascadeDataMapped.size() || CascadeDataMapped[CurrentCascadeDataSlot] == nullptr)
    {
        return;
    }

    auto* Data = static_cast<FCascadeDataGpu*>(CascadeDataMapped[CurrentCascadeDataSlot]);
    for (uint32_t CascadeIndex = 0u; CascadeIndex < kSparseSdfGIMaxCascadeCount; ++CascadeIndex)
    {
        const FCascadeBounds Bounds = ComputeCascadeBounds(BaseBounds, CascadeIndex);
        Data[CascadeIndex].MinVoxelSize = DirectX::XMFLOAT4(Bounds.Min.x, Bounds.Min.y, Bounds.Min.z, Bounds.VoxelSize);
        Data[CascadeIndex].Extent = DirectX::XMFLOAT4(Bounds.Extent.x, Bounds.Extent.y, Bounds.Extent.z, 0.0f);
        Data[CascadeIndex].Offsets = DirectX::XMUINT4(
            CascadeIndex * GetBrickMapElementCount(),
            CascadeIndex * GetTraceHierarchyBottomNodeCount(),
            CascadeIndex * GetTraceHierarchyTopNodeCount(),
            EffectiveMaxScatterBricks);
    }
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
    HashValue(Hash, MaxScatterBricks);
    HashValue(Hash, EffectiveMaxScatterBricks);
    HashValue(Hash, bEikonalEnabled ? 1u : 0u);
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

void FSparseSdfGI::AddDistributedScatterInitPass(FDeferredPassContext& Context, uint32_t MaxStaticTriangleCount, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const
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
    const uint32_t ScatterBrickCapacity = (std::max)(EffectiveMaxScatterBricks, 1u);
    const uint32_t PhysicalBrickBase = CascadeIndex * ScatterBrickCapacity;

    struct FScatterInitPassData
    {
        bool bEnabled = false;
        uint32_t MaxJobCount = 0u;
        uint32_t CascadeIndex = 0u;
        uint32_t PhysicalBrickBase = 0u;
        uint32_t ScatterBrickCapacity = 0u;
        FCascadeBounds Bounds{};
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

    Graph.AddPass<FScatterInitPassData>("SparseSdfGI Distributed Scatter Init", [this, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, ReferenceStatsHandle, MaxJobCount, JobGroupCount, JobGroup2Count, ScatterBrickCapacity, PhysicalBrickBase, CascadeIndex, Bounds, SparseResourcesPtr](FScatterInitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.MaxJobCount = MaxJobCount;
        Data.CascadeIndex = CascadeIndex;
        Data.PhysicalBrickBase = PhysicalBrickBase;
        Data.ScatterBrickCapacity = ScatterBrickCapacity;
        Data.Bounds = Bounds;
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

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.CascadeIndex = Data.CascadeIndex;
        Constants.MaxBrickTriangleReferences = Data.ScatterBrickCapacity;
        Constants.TrianglePoolCapacity = Data.MaxJobCount;
        Constants.CascadeMin = Data.Bounds.Min;
        Constants.CascadeExtent = Data.Bounds.Extent;
        Constants.VoxelSize = Data.Bounds.VoxelSize;
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.PhysicalBrickBase = Data.PhysicalBrickBase;
        Constants.ScatterBrickCapacity = Data.ScatterBrickCapacity;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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
        const uint32_t ClearResolution = (Data.CascadeIndex == 0u) ? kSparseSdfGIAtlasResolution : kSparseSdfGIBrickGridResolution;
        CommandList->Dispatch(
            AlignDispatch(ClearResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(ClearResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(ClearResolution, kSparseSdfGIGroupSize3D));
    });
}

void FSparseSdfGI::AddSectionDistributedScatterPreparePass(FDeferredPassContext& Context, const FObject& Object, FMeshSection& Section, uint32_t DrawSectionIndex, uint32_t MaxStaticTriangleCount, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FBindlessBuffer& PositionBuffer = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition];
    FBindlessBuffer& IndexBuffer = Section.Geometry.IndexBuffer;
    const FRGBufferHandle PositionHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Scatter Position", PositionBuffer);
    const FRGBufferHandle IndexHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Scatter Index", IndexBuffer);
    const uint32_t PositionSrvIndex = PositionBuffer.SrvBindlessIndex;
    const uint32_t IndexSrvIndex = IndexBuffer.SrvBindlessIndex;
    const FRGBufferHandle JobsHandle = Context.Resources.SparseSdfGI.ScatterJobsHandle;
    const FRGBufferHandle CountersHandle = Context.Resources.SparseSdfGI.ScatterCountersHandle;
    const float SectionScale = MatrixMath::ComputeMaxScale(Object.GetWorldMatrix());
    const uint32_t TriangleCount = Section.DrawIndexCount / 3u;
    const uint32_t DrawIndexStart = Section.DrawIndexStart;
    const uint32_t DrawIndexCount = Section.DrawIndexCount;
    const uint32_t PositionCount = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].Desc.NumElements;
    const DirectX::XMFLOAT4X4 World = Object.GetWorldMatrix();
    const uint32_t ScatterBrickCapacity = (std::max)(EffectiveMaxScatterBricks, 1u);
    const uint32_t PhysicalBrickBase = CascadeIndex * ScatterBrickCapacity;
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
        uint32_t CascadeIndex = 0u;
        uint32_t PhysicalBrickBase = 0u;
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
    Graph.AddPass<FScatterPreparePassData>(PassName, [this, PositionHandle, IndexHandle, JobsHandle, CountersHandle, Bounds, SectionScale, TriangleCount, DrawIndexStart, DrawIndexCount, PositionCount, PositionSrvIndex, IndexSrvIndex, World, MaxStaticTriangleCount, ScatterBrickCapacity, CascadeIndex, PhysicalBrickBase](FScatterPreparePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        Data.TriangleCount = TriangleCount;
        Data.DrawIndexStart = DrawIndexStart;
        Data.DrawIndexCount = DrawIndexCount;
        Data.PositionCount = PositionCount;
        Data.MaxJobCount = (std::max)(MaxStaticTriangleCount, 1u);
        Data.ScatterBrickCapacity = ScatterBrickCapacity;
        Data.CascadeIndex = CascadeIndex;
        Data.PhysicalBrickBase = PhysicalBrickBase;
        Data.CascadeMin = Bounds.Min;
        Data.CascadeExtent = Bounds.Extent;
        Data.VoxelSize = Bounds.VoxelSize;
        Data.World = World;
        Data.PositionBufferIndex = PositionSrvIndex;
        Data.IndexBufferIndex = IndexSrvIndex;
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
        Constants.CascadeIndex = Data.CascadeIndex;
        Constants.CascadeMin = Data.CascadeMin;
        Constants.CascadeExtent = Data.CascadeExtent;
        Constants.VoxelSize = Data.VoxelSize;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        Constants.World = Data.World;
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.PhysicalBrickBase = Data.PhysicalBrickBase;
        Constants.ScatterBrickCapacity = Data.ScatterBrickCapacity;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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

void FSparseSdfGI::AddDistributedScatterPasses(FDeferredPassContext& Context, uint32_t MaxStaticTriangleCount, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const
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
    const uint32_t ScatterBrickCapacity = (std::max)(EffectiveMaxScatterBricks, 1u);
    const uint32_t PhysicalBrickBase = CascadeIndex * ScatterBrickCapacity;

    auto FillCommonConstants = [this, ScatterBrickCapacity, PhysicalBrickBase, CascadeIndex, Bounds](FDeferredRenderer& Owner, uint32_t WorkCount)
    {
        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.CascadeIndex = CascadeIndex;
        Constants.ModelTriangleCount = WorkCount;
        Constants.MaxBrickTriangleReferences = ScatterBrickCapacity;
        Constants.TrianglePoolCapacity = WorkCount;
        Constants.CascadeMin = Bounds.Min;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.PhysicalBrickBase = PhysicalBrickBase;
        Constants.ScatterBrickCapacity = ScatterBrickCapacity;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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

    Graph.AddPass<FScatterDirectPassData>("SparseSdfGI Eikonal Scatter Bricks", [this, BrickSdfHandle, BrickArgsHandle](FScatterDirectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid && bEikonalEnabled;
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
        CommandList->SetPipelineState(ScatterEikonalPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        FSparseSdfGIConstants Constants = FillCommonConstants(Owner, MaxJobCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants))) { return; }
        const FSparseSdfGIScatterEikonalBindlessConstants Bindless = { GraphPtr->GetBufferUavBindlessIndex(Data.A) };
        if (!AreAllBindlessIndicesValid(Bindless.ScatterBrickSdfUavIndex)) { return; }
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIScatterEikonalBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        ID3D12Resource* Args = GraphPtr->GetBufferResource(Data.B);
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

void FSparseSdfGI::AddBuildTraceHierarchyPasses(FDeferredPassContext& Context, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const
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
        uint32_t CascadeIndex = 0u;
        FCascadeBounds Bounds{};
        FRGBufferHandle BrickMapHandle{};
        FRGBufferHandle BrickMetadataHandle{};
        FRGBufferHandle BottomHandle{};
    };

    Graph.AddPass<FBuildBottomPassData>("SparseSdfGI Build Trace Hierarchy Bottom", [this, BrickMapHandle, BrickMetadataHandle, BottomHandle, Bounds, CascadeIndex](FBuildBottomPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickMapHandle = BrickMapHandle;
        Data.BrickMetadataHandle = BrickMetadataHandle;
        Data.BottomHandle = BottomHandle;
        Data.CascadeIndex = CascadeIndex;
        Data.Bounds = Bounds;
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
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.CascadeIndex = Data.CascadeIndex;
        Constants.CascadeMin = Data.Bounds.Min;
        Constants.CascadeExtent = Data.Bounds.Extent;
        Constants.VoxelSize = Data.Bounds.VoxelSize;
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.PhysicalBrickBase = Data.CascadeIndex * EffectiveMaxScatterBricks;
        Constants.ScatterBrickCapacity = EffectiveMaxScatterBricks;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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
        uint32_t CascadeIndex = 0u;
        FCascadeBounds Bounds{};
        FRGBufferHandle BottomHandle{};
        FRGBufferHandle TopHandle{};
    };

    Graph.AddPass<FBuildTopPassData>("SparseSdfGI Build Trace Hierarchy Top", [this, BottomHandle, TopHandle, Bounds, CascadeIndex](FBuildTopPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BottomHandle = BottomHandle;
        Data.TopHandle = TopHandle;
        Data.CascadeIndex = CascadeIndex;
        Data.Bounds = Bounds;
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
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.CascadeIndex = Data.CascadeIndex;
        Constants.CascadeMin = Data.Bounds.Min;
        Constants.CascadeExtent = Data.Bounds.Extent;
        Constants.VoxelSize = Data.Bounds.VoxelSize;
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.PhysicalBrickBase = Data.CascadeIndex * EffectiveMaxScatterBricks;
        Constants.ScatterBrickCapacity = EffectiveMaxScatterBricks;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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

void FSparseSdfGI::AddBrickShPropagatePass(FDeferredPassContext& Context, const char* PassName, FRGBufferHandle SourceHandle, FRGBufferHandle DestHandle, FRGBufferHandle BrickMapHandle) const
{
    FRenderGraph& Graph = Context.Graph;

    struct FBrickShPropagatePassData
    {
        bool bEnabled = false;
        FRGBufferHandle SourceHandle{};
        FRGBufferHandle DestHandle{};
        FRGBufferHandle BrickMapHandle{};
    };

    Graph.AddPass<FBrickShPropagatePassData>(PassName, [SourceHandle, DestHandle, BrickMapHandle](FBrickShPropagatePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.SourceHandle = SourceHandle;
        Data.DestHandle = DestHandle;
        Data.BrickMapHandle = BrickMapHandle;
        Data.bEnabled = static_cast<bool>(Data.SourceHandle) && static_cast<bool>(Data.DestHandle) && static_cast<bool>(Data.BrickMapHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.SourceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.DestHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.DestHandle);
    }, [this, &Context](const FBrickShPropagatePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t BrickMapSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickMapHandle);
        const uint32_t SourceSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.SourceHandle);
        const uint32_t DestUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.DestHandle);
        if (!AreAllBindlessIndicesValid(BrickMapSrvIndex, SourceSrvIndex, DestUavIndex))
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(BrickShPropagatePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.CascadeCount = CascadeCount;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const FSparseSdfGIBrickShPropagateBindlessConstants Bindless =
        {
            BrickMapSrvIndex,
            SourceSrvIndex,
            DestUavIndex
        };
        static_assert(sizeof(FSparseSdfGIBrickShPropagateBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIBrickShPropagateBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetCascadeBrickMapElementCount(CascadeCount), 64u), 1u, 1u);
    });
}

void FSparseSdfGI::AddRadianceCachePasses(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle ShadowMaskHandle = Context.Resources.RayTracingShadow.ShadowMaskHandle;
    const FRGResourceHandle ShadowMapHandle = Context.Resources.ShadowHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle CascadeDataHandle = Context.Resources.SparseSdfGI.CascadeDataHandle;

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
        FRGResourceHandle ShadowMapHandle{};
        FRGBufferHandle BrickIrradianceReadHandle{};
        FRGBufferHandle BrickMapHandle{};
        FRGBufferHandle CascadeDataHandle{};
    };

    Graph.AddPass<FRadianceInjectPassData>("SparseSdfGI Radiance Inject", [&, DepthHandle, ShadowMaskHandle, ShadowMapHandle, GBufferHandles, BrickIrradianceReadHandle, BrickMapHandle, CascadeDataHandle](FRadianceInjectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickRadianceAccumHandle = Context.Resources.SparseSdfGI.BrickRadianceAccumHandle;
        Data.ShadowMaskHandle = ShadowMaskHandle;
        Data.ShadowMapHandle = Context.FrameState.bRenderShadows ? ShadowMapHandle : FRGResourceHandle{};
        Data.BrickIrradianceReadHandle = BrickIrradianceReadHandle;
        Data.BrickMapHandle = BrickMapHandle;
        Data.CascadeDataHandle = CascadeDataHandle;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && DebugMode == ESparseSdfGIDebugMode::Off
            && static_cast<bool>(DepthHandle)
            && static_cast<bool>(GBufferHandles[0])
            && static_cast<bool>(GBufferHandles[1])
            && static_cast<bool>(GBufferHandles[2])
            && static_cast<bool>(Data.BrickMapHandle)
            && static_cast<bool>(Data.CascadeDataHandle)
            && static_cast<bool>(Data.BrickRadianceAccumHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.CascadeDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Data.ShadowMaskHandle)
        {
            Builder.ReadTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if (Data.ShadowMapHandle)
        {
            Builder.ReadTexture(Data.ShadowMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
        const uint32_t ShadowMapBindlessIndex = Data.ShadowMapHandle ? Context.Graph.GetTextureSrvBindlessIndex(Data.ShadowMapHandle) : UINT32_MAX;
        const bool bUseShadowMask = Owner.bShadowsEnabled
            && Owner.bRayTracedShadowsEnabled
            && Owner.GetRayTracingRuntime().bRayTracingPipelineReady
            && IsValidBindlessIndex(ShadowMaskBindlessIndex);
        const bool bUseShadowMap = !bUseShadowMask
            && Owner.bShadowsEnabled
            && IsValidBindlessIndex(ShadowMapBindlessIndex);
        const uint32_t ShadowMode = bUseShadowMask
            ? kSparseSdfGIShadowModeScreenMask
            : (bUseShadowMap ? kSparseSdfGIShadowModeShadowMap : kSparseSdfGIShadowModeOff);
        const uint32_t ShadowTextureIndex = bUseShadowMask
            ? ShadowMaskBindlessIndex
            : (bUseShadowMap ? ShadowMapBindlessIndex : UINT32_MAX);
        const uint32_t BrickRadianceAccumUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickRadianceAccumHandle);
        const uint32_t BrickMapSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickMapHandle);
        if (!AreAllBindlessIndicesValid(
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.GBufferC.SrvBindlessIndex,
            BrickRadianceAccumUavIndex,
            BrickMapSrvIndex,
            CurrentCascadeDataSrvIndex))
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

        const FCascadeBounds Bounds = ComputeBaseCascadeBounds(Owner);
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
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.PhysicalBrickBase = 0u;
        Constants.ScatterBrickCapacity = EffectiveMaxScatterBricks;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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
            ShadowTextureIndex,
            ShadowMode,
            BrickMapSrvIndex,
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
        FRGBufferHandle BrickRadianceResolvedHandle{};
    };

    Graph.AddPass<FRadianceResolvePassData>("SparseSdfGI Radiance Resolve", [&, BrickRadianceReadHandle, bHistoryValidForResolve, bRadianceCacheWillUpdate](FRadianceResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickRadianceAccumHandle = Context.Resources.SparseSdfGI.BrickRadianceAccumHandle;
        Data.BrickRadianceReadHandle = BrickRadianceReadHandle;
        Data.bHistoryValid = bHistoryValidForResolve;
        Data.bEnabled = bRadianceCacheWillUpdate
            && static_cast<bool>(Data.BrickRadianceAccumHandle)
            && static_cast<bool>(Data.BrickRadianceReadHandle);
        if (!Data.bEnabled)
        {
            return;
        }
        Data.BrickRadianceResolvedHandle = Builder.CreateBuffer("SparseSdfGI Brick Radiance Resolve", CreateBrickRadianceDesc());
        Context.Resources.SparseSdfGI.BrickRadianceResolvedHandle = Data.BrickRadianceResolvedHandle;

        Builder.ReadBuffer(Data.BrickRadianceAccumHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickRadianceReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.BrickRadianceResolvedHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.BrickRadianceResolvedHandle);
    }, [this, &Context](const FRadianceResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t BrickRadianceAccumSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceAccumHandle);
        const uint32_t BrickRadianceHistorySrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceReadHandle);
        const uint32_t BrickRadianceUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickRadianceResolvedHandle);
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

    AddBrickShPropagatePass(
        Context,
        "SparseSdfGI Radiance Propagate",
        Context.Resources.SparseSdfGI.BrickRadianceResolvedHandle,
        BrickRadianceWriteHandle,
        BrickMapHandle);

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
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle CascadeDataHandle = Context.Resources.SparseSdfGI.CascadeDataHandle;
    const FRGBufferHandle BrickIrradianceReadHandle = Context.Resources.SparseSdfGI.BrickIrradianceReadHandle;
    const FRGBufferHandle BrickIrradianceWriteHandle = Context.Resources.SparseSdfGI.BrickIrradianceWriteHandle;
    const bool bReadSlotValid = CurrentBrickIrradianceReadSlot < BrickIrradianceHistoryValid.size() && BrickIrradianceHistoryValid[CurrentBrickIrradianceReadSlot];
    const bool bWillUpdate = bEnabled
        && bPersistentInputsValid
        && DebugMode == ESparseSdfGIDebugMode::Off
        && static_cast<bool>(DepthHandle)
        && static_cast<bool>(DiffuseGIHandle)
        && static_cast<bool>(BrickMapHandle)
        && static_cast<bool>(CascadeDataHandle)
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
        FRGBufferHandle BrickMapHandle{};
        FRGBufferHandle CascadeDataHandle{};
    };

    Graph.AddPass<FIrradianceAccumulatePassData>("SparseSdfGI Irradiance Accumulate", [&, DepthHandle, DiffuseGIHandle, BrickMapHandle, CascadeDataHandle, bWillUpdate](FIrradianceAccumulatePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.AccumHandle = Context.Resources.SparseSdfGI.BrickIrradianceAccumHandle;
        Data.BrickMapHandle = BrickMapHandle;
        Data.CascadeDataHandle = CascadeDataHandle;
        Data.bEnabled = bWillUpdate && static_cast<bool>(Data.AccumHandle);
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DiffuseGIHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.CascadeDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
        const uint32_t BrickMapSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickMapHandle);
        if (!AreAllBindlessIndicesValid(DepthBindlessIndex, DiffuseGIIndex, AccumUavIndex, BrickMapSrvIndex, CurrentCascadeDataSrvIndex))
        {
            return;
        }
        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(IrradianceAccumulatePipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const FCascadeBounds Bounds = ComputeBaseCascadeBounds(Owner);
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
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }
        const FSparseSdfGIIrradianceAccumulateBindlessConstants Bindless = { DepthBindlessIndex, DiffuseGIIndex, AccumUavIndex, BrickMapSrvIndex };
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
        FRGBufferHandle ResolvedHandle{};
    };

    Graph.AddPass<FIrradianceResolvePassData>("SparseSdfGI Irradiance Resolve", [&, BrickIrradianceReadHandle, bReadSlotValid, bWillUpdate](FIrradianceResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.AccumHandle = Context.Resources.SparseSdfGI.BrickIrradianceAccumHandle;
        Data.ReadHandle = BrickIrradianceReadHandle;
        Data.bHistoryValid = bReadSlotValid;
        Data.bEnabled = bWillUpdate
            && static_cast<bool>(Data.AccumHandle)
            && static_cast<bool>(Data.ReadHandle);
        if (!Data.bEnabled)
        {
            return;
        }
        Data.ResolvedHandle = Builder.CreateBuffer("SparseSdfGI Brick Irradiance Resolve", CreateBrickRadianceDesc());
        Context.Resources.SparseSdfGI.BrickIrradianceResolvedHandle = Data.ResolvedHandle;
        Builder.ReadBuffer(Data.AccumHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.ResolvedHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.ResolvedHandle);
    }, [this, &Context](const FIrradianceResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t AccumSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.AccumHandle);
        const uint32_t HistorySrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ReadHandle);
        const uint32_t UavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ResolvedHandle);
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

    AddBrickShPropagatePass(
        Context,
        "SparseSdfGI Irradiance Propagate",
        Context.Resources.SparseSdfGI.BrickIrradianceResolvedHandle,
        BrickIrradianceWriteHandle,
        BrickMapHandle);

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
    const FRGBufferHandle CascadeDataHandle = Context.Resources.SparseSdfGI.CascadeDataHandle;
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

    Graph.AddPass<FProbeTracePassData>("SparseSdfGI Probe Trace", [&, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, CascadeDataHandle, TraceHierarchyBottomHandle, TraceHierarchyTopHandle, BrickRadianceHandle, ProbeHistoryReadHandle, ProbeHistoryWriteHandle, bProbeHistoryValidForTrace](FProbeTracePassData& Data, FRGPassBuilder& Builder)
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
        Data.bEnabled = bEnabled && bPersistentInputsValid && bTracePipelineReady && Data.ProbeHeaderHandle && SdfAtlasHandle && BrickMapHandle && BrickMetadataHandle && CascadeDataHandle && TraceHierarchyBottomHandle && TraceHierarchyTopHandle && BrickRadianceHandle && ProbeHistoryReadHandle && ProbeHistoryWriteHandle;
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
        Builder.ReadBuffer(CascadeDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

        const FCascadeBounds Bounds = ComputeBaseCascadeBounds(Owner);
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
        Constants.CascadeCount = CascadeCount;
        Constants.CascadeScale = CascadeScale;
        Constants.PhysicalBrickBase = 0u;
        Constants.ScatterBrickCapacity = EffectiveMaxScatterBricks;
        Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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
        if (ProbeDebugMode == SPARSE_SDF_GI_PROBE_DEBUG_MODE_OFF)
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

        const FCascadeBounds Bounds = ComputeBaseCascadeBounds(Owner);
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

    const FCascadeBounds Bounds = ComputeBaseCascadeBounds(Owner);

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
    Constants.CascadeCount = CascadeCount;
    Constants.CascadeScale = CascadeScale;
    Constants.PhysicalBrickBase = 0u;
    Constants.ScatterBrickCapacity = EffectiveMaxScatterBricks;
    Constants.CascadeDataSrvIndex = CurrentCascadeDataSrvIndex;
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
