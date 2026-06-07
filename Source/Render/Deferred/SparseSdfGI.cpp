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
    constexpr uint32_t kSparseSdfGIReferenceEmitChunkTriangles = 16u * 1024u;
    constexpr uint32_t kSparseSdfGISolveDispatchChunkGroups = 1024u;
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

    struct FTrianglePoolEntryGpu
    {
        DirectX::XMFLOAT4 P0{};
        DirectX::XMFLOAT4 P1{};
        DirectX::XMFLOAT4 P2{};
    };

    struct FBrickTriangleReferenceGpu
    {
        uint32_t TriangleId = UINT32_MAX;
        uint32_t Next = UINT32_MAX;
        uint32_t Reserved0 = 0;
        uint32_t Reserved1 = 0;
    };

    FRGBufferDesc CreateTrianglePoolDesc(uint32_t MaxTriangleCount)
    {
        return CreateRWStructuredBufferDesc<FTrianglePoolEntryGpu>((std::max)(MaxTriangleCount, 1u));
    }

    FRGBufferDesc CreateBrickReferenceHeadsDesc()
    {
        return CreateRWStructuredBufferDesc<uint32_t>(GetBrickMapElementCount());
    }

    FRGBufferDesc CreateBrickReferencesDesc(uint32_t MaxReferences)
    {
        return CreateRWStructuredBufferDesc<FBrickTriangleReferenceGpu>((std::max)(MaxReferences, 1u));
    }

    FRGBufferDesc CreateReferenceCountersDesc()
    {
        return CreateRWStructuredBufferDesc<uint32_t>(5u);
    }

    FRGBufferDesc CreateOccupiedBrickListDesc()
    {
        return CreateRWStructuredBufferDesc<uint32_t>(GetBrickMapElementCount());
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
        uint32_t SdfBuildMode = 0;
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
    if (!CreateRootSignature(Device) || !CreatePipelines(Device))
    {
        LogWarning("Deferred renderer: SparseSdfGI pipeline creation failed; feature disabled.");
        bEnabled = false;
        RootSignature.Reset();
        ReferenceBuildInitPipeline.Reset();
        ReferenceEmitPipeline.Reset();
        SolveBrickReferencesPipeline.Reset();
        ExactSolveBrickReferencesPipeline.Reset();
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
    const ESparseSdfGISdfBuildMode PreviousSdfBuildMode = SdfBuildMode;
    const uint32_t NewCascadeCount = std::clamp(Config.SparseSdfGICascadeCount, 1u, 1u);
    const float NewBaseVoxelSize = Config.SparseSdfGIBaseVoxelSize;
    const float NewCascadeScale = (std::max)(Config.SparseSdfGICascadeScale, 1.01f);
    const uint32_t NewMaxBrickTriangleReferences = std::clamp(Config.SparseSdfGIMaxBrickTriangleReferences, kSparseSdfGIMinBrickTriangleReferences, kSparseSdfGIMaxBrickTriangleReferencesLimit);
    const ESparseSdfGISdfBuildMode NewSdfBuildMode = static_cast<ESparseSdfGISdfBuildMode>(std::clamp(Config.SparseSdfGISdfBuildMode, 0u, 1u));
    const bool bBuildSettingsChanged =
        CascadeCount != NewCascadeCount ||
        PreviousSdfBuildMode != NewSdfBuildMode ||
        BaseVoxelSize != NewBaseVoxelSize ||
        CascadeScale != NewCascadeScale ||
        MaxBrickTriangleReferences != NewMaxBrickTriangleReferences ||
        DebugSolveGroupBudget != Config.SparseSdfGIDebugSolveGroupBudget ||
        DebugEmitTriangleBudget != Config.SparseSdfGIDebugEmitTriangleBudget;

    bEnabled = Config.DiffuseGISource == EDiffuseGISource::SparseSdfGI;
    DebugMode = static_cast<ESparseSdfGIDebugMode>(std::clamp(Config.SparseSdfGIDebugMode, 0u, 9u));
    SdfBuildMode = NewSdfBuildMode;
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
    DebugSolveGroupBudget = Config.SparseSdfGIDebugSolveGroupBudget;
    DebugEmitTriangleBudget = Config.SparseSdfGIDebugEmitTriangleBudget;

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
    Resources.TrianglePoolHandle = {};
    Resources.BrickReferenceHeadsHandle = {};
    Resources.BrickReferencesHandle = {};
    Resources.ReferenceCountersHandle = {};
    Resources.OccupiedBrickListHandle = {};
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
    AddReferenceBuildInitPass(Context);

    uint32_t DrawSectionIndex = 0;
    auto DrawSections = Context.Owner.GetWorld().BuildSectionList();
    for (FMeshSection& Section : DrawSections)
    {
        if (Section.IsStaticRegularMeshCandidate())
        {
            AddSectionReferenceEmitPass(Context, *DrawSections.GetView(DrawSectionIndex).Object, Section, DrawSectionIndex);
        }
        ++DrawSectionIndex;
    }

    AddSolveBrickReferencesPass(Context);
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

bool FSparseSdfGI::CreatePipelines(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> ReferenceBuildInitByteCode;
    std::vector<uint8_t> ReferenceEmitByteCode;
    std::vector<uint8_t> SolveBrickReferencesByteCode;
    std::vector<uint8_t> ExactSolveBrickReferencesByteCode;
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
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSInitReferenceBuild", ReferenceBuildInitByteCode, { L"SPARSE_SDF_GI_REFERENCE_INIT_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSEmitTriangleReferences", ReferenceEmitByteCode, { L"SPARSE_SDF_GI_REFERENCE_EMIT_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSSolveBrickReferences", SolveBrickReferencesByteCode, { L"SPARSE_SDF_GI_REFERENCE_SOLVE_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSSolveBrickReferences", ExactSolveBrickReferencesByteCode, { L"SPARSE_SDF_GI_REFERENCE_SOLVE_SHADER=1", L"SPARSE_SDF_GI_EXACT_SHARED_BORDER=1" }))
    {
        return false;
    }
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

    return CreateComputePso(ReferenceBuildInitByteCode, ReferenceBuildInitPipeline, "CSInitReferenceBuild")
        && CreateComputePso(ReferenceEmitByteCode, ReferenceEmitPipeline, "CSEmitTriangleReferences")
        && CreateComputePso(SolveBrickReferencesByteCode, SolveBrickReferencesPipeline, "CSSolveBrickReferences")
        && CreateComputePso(ExactSolveBrickReferencesByteCode, ExactSolveBrickReferencesPipeline, "CSSolveBrickReferences ExactSharedBorder")
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
        ReferenceBuildInitPipeline &&
        ReferenceEmitPipeline &&
        SolveBrickReferencesPipeline &&
        ExactSolveBrickReferencesPipeline &&
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
    const float LegacyVoxelSize = (BaseVoxelSize > 0.0f)
        ? (std::max)(BaseVoxelSize, kSparseSdfGIMinVoxelSize)
        : AutoVoxelSize;
    const bool bExactSharedBorder = SdfBuildMode == ESparseSdfGISdfBuildMode::ExactSharedBorder;
    const float SamplePitch = bExactSharedBorder
        ? LegacyVoxelSize * (static_cast<float>(kSparseSdfGIBrickVoxelResolution) / static_cast<float>(kSparseSdfGIExactBrickIntervalResolution))
        : LegacyVoxelSize;
    const float BrickIntervalCount = bExactSharedBorder
        ? static_cast<float>(kSparseSdfGIBrickGridResolution * kSparseSdfGIExactBrickIntervalResolution)
        : static_cast<float>(kSparseSdfGIAtlasResolution);
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
    HashValue(Hash, SdfBuildMode);
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

void FSparseSdfGI::AddReferenceBuildInitPass(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGBufferHandle ReferenceStatsHandle = Context.Resources.SparseSdfGI.ReferenceCounterStatsHandle;

    uint32_t MaxStaticTriangleCount = 0;
    auto Sections = Context.Owner.GetWorld().BuildSectionList();
    if (!Sections.empty())
    {
        for (const FMeshSection& Section : Sections)
        {
            if (Section.IsStaticRegularMeshCandidate())
            {
                MaxStaticTriangleCount += Section.DrawIndexCount / 3u;
            }
        }
    }

    struct FReferenceBuildInitPassData
    {
        bool bEnabled = false;
        FRGBufferHandle TrianglePoolHandle{};
        FRGBufferHandle BrickReferenceHeadsHandle{};
        FRGBufferHandle BrickReferencesHandle{};
        FRGBufferHandle ReferenceCountersHandle{};
        FRGBufferHandle OccupiedBrickListHandle{};
        FRGBufferHandle ReferenceStatsHandle{};
        uint32_t TrianglePoolCapacity = 0;
    };

    Graph.AddPass<FReferenceBuildInitPassData>("SparseSdfGI Reference Build Init", [&, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, ReferenceStatsHandle, MaxStaticTriangleCount](FReferenceBuildInitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.TrianglePoolHandle = Builder.CreateBuffer("SparseSdfGI Triangle Pool", CreateTrianglePoolDesc(MaxStaticTriangleCount));
        Data.BrickReferenceHeadsHandle = Builder.CreateBuffer("SparseSdfGI Brick Reference Heads", CreateBrickReferenceHeadsDesc());
        Data.BrickReferencesHandle = Builder.CreateBuffer("SparseSdfGI Brick References", CreateBrickReferencesDesc(MaxBrickTriangleReferences));
        Data.ReferenceCountersHandle = Builder.CreateBuffer("SparseSdfGI Reference Counters", CreateReferenceCountersDesc());
        Data.OccupiedBrickListHandle = Builder.CreateBuffer("SparseSdfGI Occupied Brick List", CreateOccupiedBrickListDesc());
        Data.ReferenceStatsHandle = ReferenceStatsHandle;

        Context.Resources.SparseSdfGI.TrianglePoolHandle = Data.TrianglePoolHandle;
        Context.Resources.SparseSdfGI.BrickReferenceHeadsHandle = Data.BrickReferenceHeadsHandle;
        Context.Resources.SparseSdfGI.BrickReferencesHandle = Data.BrickReferencesHandle;
        Context.Resources.SparseSdfGI.ReferenceCountersHandle = Data.ReferenceCountersHandle;
        Context.Resources.SparseSdfGI.OccupiedBrickListHandle = Data.OccupiedBrickListHandle;
        Data.TrianglePoolCapacity = MaxStaticTriangleCount;

        Builder.WriteTexture(SdfAtlasHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(BrickMapHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(BrickMetadataHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.TrianglePoolHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickReferenceHeadsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickReferencesHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.ReferenceCountersHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.ReferenceStatsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(SdfAtlasHandle);
        Builder.UavBarrier(BrickMetadataHandle);
        Builder.UavBarrier(Data.BrickReferenceHeadsHandle);
        Builder.UavBarrier(Data.ReferenceCountersHandle);
        Builder.UavBarrier(Data.ReferenceStatsHandle);
    }, [this, &Context](const FReferenceBuildInitPassData& Data, FDX12CommandContext& Cmd)
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
        CommandList->SetPipelineState(ReferenceBuildInitPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.DebugMode = static_cast<uint32_t>(DebugMode);
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        Constants.Intensity = Intensity;
        Constants.BounceStrength = BounceStrength;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.MaxTraceDistance = Bounds.Extent.x;
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.TrianglePoolCapacity = Data.TrianglePoolCapacity;
        Constants.CascadeMin = Bounds.Min;
        Constants.CascadeExtent = Bounds.Extent;
        static_assert(sizeof(FSparseSdfGIConstants) / sizeof(uint32_t) <= kSparseSdfGIConstantsDwordCount);
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const uint32_t BrickReferenceHeadsUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickReferenceHeadsHandle);
        const uint32_t ReferenceCountersUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ReferenceCountersHandle);
        const uint32_t ReferenceStatsUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ReferenceStatsHandle);
        if (!AreAllBindlessIndicesValid(SdfAtlas.UavBindlessIndex, CascadeBrickMap.UavBindlessIndex, BrickMetadata.UavBindlessIndex, BrickReferenceHeadsUavIndex, ReferenceCountersUavIndex, ReferenceStatsUavIndex))
        {
            return;
        }

        const FSparseSdfGIReferenceInitBindlessConstants Bindless =
        {
            SdfAtlas.UavBindlessIndex,
            CascadeBrickMap.UavBindlessIndex,
            BrickMetadata.UavBindlessIndex,
            BrickReferenceHeadsUavIndex,
            ReferenceCountersUavIndex,
            ReferenceStatsUavIndex
        };
        static_assert(sizeof(FSparseSdfGIReferenceInitBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIReferenceInitBindlessConstants) / sizeof(uint32_t), &Bindless, 0);

        CommandList->Dispatch(
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D));
    });
}

void FSparseSdfGI::AddSectionReferenceEmitPass(FDeferredPassContext& Context, const FObject& Object, FMeshSection& Section, uint32_t DrawSectionIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FBindlessBuffer& PositionBuffer = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition];
    FBindlessBuffer& IndexBuffer = Section.Geometry.IndexBuffer;
    const FRGBufferHandle PositionHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Position", PositionBuffer);
    const FRGBufferHandle IndexHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Index", IndexBuffer);
    const FRGBufferHandle TrianglePoolHandle = Context.Resources.SparseSdfGI.TrianglePoolHandle;
    const FRGBufferHandle BrickReferenceHeadsHandle = Context.Resources.SparseSdfGI.BrickReferenceHeadsHandle;
    const FRGBufferHandle BrickReferencesHandle = Context.Resources.SparseSdfGI.BrickReferencesHandle;
    const FRGBufferHandle ReferenceCountersHandle = Context.Resources.SparseSdfGI.ReferenceCountersHandle;
    const FRGBufferHandle OccupiedBrickListHandle = Context.Resources.SparseSdfGI.OccupiedBrickListHandle;

    const float SectionScale = MatrixMath::ComputeMaxScale(Object.GetWorldMatrix());
    const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);

    struct FReferenceEmitPassData
    {
        bool bEnabled = false;
        uint32_t TriangleCount = 0;
        uint32_t DrawIndexStart = 0;
        uint32_t DrawIndexCount = 0;
        uint32_t TrianglePoolCapacity = 0;
        uint32_t PositionCount = 0;
        float VoxelSize = 0.0f;
        DirectX::XMFLOAT3 CascadeMin{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 CascadeExtent{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4X4 World{};
        uint32_t PositionBufferIndex = UINT32_MAX;
        uint32_t IndexBufferIndex = UINT32_MAX;
        FRGBufferHandle TrianglePoolHandle{};
        FRGBufferHandle BrickReferenceHeadsHandle{};
        FRGBufferHandle BrickReferencesHandle{};
        FRGBufferHandle ReferenceCountersHandle{};
        FRGBufferHandle OccupiedBrickListHandle{};
    };

    const std::string PassName = "SparseSdfGI Emit References Section " + std::to_string(DrawSectionIndex);
    Graph.AddPass<FReferenceEmitPassData>(PassName, [&, PositionHandle, IndexHandle, TrianglePoolHandle, BrickReferenceHeadsHandle, BrickReferencesHandle, ReferenceCountersHandle, OccupiedBrickListHandle, SectionScale, Bounds](FReferenceEmitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.TrianglePoolHandle = TrianglePoolHandle;
        Data.BrickReferenceHeadsHandle = BrickReferenceHeadsHandle;
        Data.BrickReferencesHandle = BrickReferencesHandle;
        Data.ReferenceCountersHandle = ReferenceCountersHandle;
        Data.OccupiedBrickListHandle = OccupiedBrickListHandle;
        Data.TriangleCount = Section.DrawIndexCount / 3u;
        auto Sections = Owner.GetWorld().BuildSectionList();
        if (!Sections.empty())
        {
            for (const FMeshSection& SceneSection : Sections)
            {
                if (SceneSection.IsStaticRegularMeshCandidate())
                {
                    Data.TrianglePoolCapacity += SceneSection.DrawIndexCount / 3u;
                }
            }
        }
        Data.DrawIndexStart = Section.DrawIndexStart;
        Data.DrawIndexCount = Section.DrawIndexCount;
        Data.PositionCount = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].Desc.NumElements;
        Data.VoxelSize = Bounds.VoxelSize;
        Data.CascadeMin = Bounds.Min;
        Data.CascadeExtent = Bounds.Extent;
        Data.World = Object.GetWorldMatrix();
        Data.PositionBufferIndex = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].SrvBindlessIndex;
        Data.IndexBufferIndex = Section.Geometry.IndexBuffer.SrvBindlessIndex;
        Data.bEnabled = Data.bEnabled
            && Data.TriangleCount > 0u
            && Data.PositionCount > 0u
            && SectionScale > 0.0f
            && static_cast<bool>(Data.TrianglePoolHandle)
            && static_cast<bool>(Data.BrickReferenceHeadsHandle)
            && static_cast<bool>(Data.BrickReferencesHandle)
            && static_cast<bool>(Data.ReferenceCountersHandle)
            && static_cast<bool>(Data.OccupiedBrickListHandle)
            && AreAllBindlessIndicesValid(Data.PositionBufferIndex, Data.IndexBufferIndex);

        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(PositionHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(IndexHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.TrianglePoolHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickReferenceHeadsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickReferencesHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.ReferenceCountersHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.OccupiedBrickListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.TrianglePoolHandle);
        Builder.UavBarrier(Data.BrickReferenceHeadsHandle);
        Builder.UavBarrier(Data.BrickReferencesHandle);
        Builder.UavBarrier(Data.ReferenceCountersHandle);
        Builder.UavBarrier(Data.OccupiedBrickListHandle);
    }, [this, &Context](const FReferenceEmitPassData& Data, FDX12CommandContext& Cmd)
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
        CommandList->SetPipelineState(ReferenceEmitPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.DebugMode = static_cast<uint32_t>(DebugMode);
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.TrianglePoolCapacity = Data.TrianglePoolCapacity;
        Constants.BuildWorkOffset = Data.PositionCount;
        Constants.Intensity = Intensity;
        Constants.BounceStrength = BounceStrength;
        Constants.MaxTraceDistance = Data.CascadeExtent.x;
        Constants.CascadeMin = Data.CascadeMin;
        Constants.VoxelSize = Data.VoxelSize;
        Constants.CascadeExtent = Data.CascadeExtent;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        Constants.World = Data.World;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const uint32_t TrianglePoolUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.TrianglePoolHandle);
        const uint32_t BrickReferenceHeadsUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickReferenceHeadsHandle);
        const uint32_t BrickReferencesUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickReferencesHandle);
        const uint32_t ReferenceCountersUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ReferenceCountersHandle);
        const uint32_t OccupiedBrickListUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.OccupiedBrickListHandle);
        if (!AreAllBindlessIndicesValid(TrianglePoolUavIndex, BrickReferenceHeadsUavIndex, BrickReferencesUavIndex, ReferenceCountersUavIndex, OccupiedBrickListUavIndex))
        {
            // First point where every reference buffer (incl. OccupiedBrickList) is live; an invalid
            // index here means the dispatch would bind a null descriptor and page fault (DRED VA=0x0).
            std::ostringstream Oss;
            Oss << "SparseSdfGI emit references skipped: invalid UAV bindless index (TrianglePool="
                << TrianglePoolUavIndex << ", Heads=" << BrickReferenceHeadsUavIndex
                << ", References=" << BrickReferencesUavIndex << ", Counters=" << ReferenceCountersUavIndex
                << ", OccupiedBrickList=" << OccupiedBrickListUavIndex << ").";
            LogError(Oss.str());
            return;
        }

        const FSparseSdfGIReferenceEmitBindlessConstants Bindless =
        {
            Data.PositionBufferIndex,
            Data.IndexBufferIndex,
            TrianglePoolUavIndex,
            BrickReferenceHeadsUavIndex,
            BrickReferencesUavIndex,
            ReferenceCountersUavIndex,
            OccupiedBrickListUavIndex
        };
        static_assert(sizeof(FSparseSdfGIReferenceEmitBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIReferenceEmitBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        // Debug work cap (DebugEmitTriangleBudget): shrink/skip emit to isolate the device-removed crash.
        const uint32_t EmitTriangleCount = (std::min)(Data.TriangleCount, DebugEmitTriangleBudget);
        for (uint32_t TriangleOffset = 0u; TriangleOffset < EmitTriangleCount; TriangleOffset += kSparseSdfGIReferenceEmitChunkTriangles)
        {
            const uint32_t ChunkTriangleCount = (std::min)(kSparseSdfGIReferenceEmitChunkTriangles, EmitTriangleCount - TriangleOffset);
            Constants.ModelTriangleCount = ChunkTriangleCount;
            Constants.ModelDrawIndexStart = Data.DrawIndexStart + TriangleOffset * 3u;
            Constants.ModelDrawIndexCount = ChunkTriangleCount * 3u;
            if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
            {
                return;
            }
            CommandList->Dispatch(AlignDispatch(ChunkTriangleCount, kSparseSdfGIReferenceEmitGroupSize), 1u, 1u);
        }
    });
}

void FSparseSdfGI::AddSolveBrickReferencesPass(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGBufferHandle TrianglePoolHandle = Context.Resources.SparseSdfGI.TrianglePoolHandle;
    const FRGBufferHandle BrickReferenceHeadsHandle = Context.Resources.SparseSdfGI.BrickReferenceHeadsHandle;
    const FRGBufferHandle BrickReferencesHandle = Context.Resources.SparseSdfGI.BrickReferencesHandle;
    const FRGBufferHandle ReferenceCountersHandle = Context.Resources.SparseSdfGI.ReferenceCountersHandle;
    const FRGBufferHandle OccupiedBrickListHandle = Context.Resources.SparseSdfGI.OccupiedBrickListHandle;
    const FRGBufferHandle ReferenceStatsHandle = Context.Resources.SparseSdfGI.ReferenceCounterStatsHandle;
    struct FSolveBrickReferencesPassData
    {
        bool bEnabled = false;
        FRGResourceHandle SdfAtlasHandle{};
        FRGBufferHandle BrickMetadataHandle{};
        FRGBufferHandle TrianglePoolHandle{};
        FRGBufferHandle BrickReferenceHeadsHandle{};
        FRGBufferHandle BrickReferencesHandle{};
        FRGBufferHandle ReferenceCountersHandle{};
        FRGBufferHandle OccupiedBrickListHandle{};
        FRGBufferHandle ReferenceStatsHandle{};
        uint32_t TrianglePoolCapacity = 0;
    };

    Graph.AddPass<FSolveBrickReferencesPassData>("SparseSdfGI Solve Brick References", [&, SdfAtlasHandle, BrickMetadataHandle, TrianglePoolHandle, BrickReferenceHeadsHandle, BrickReferencesHandle, ReferenceCountersHandle, OccupiedBrickListHandle, ReferenceStatsHandle](FSolveBrickReferencesPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.SdfAtlasHandle = SdfAtlasHandle;
        Data.BrickMetadataHandle = BrickMetadataHandle;
        Data.TrianglePoolHandle = TrianglePoolHandle;
        Data.BrickReferenceHeadsHandle = BrickReferenceHeadsHandle;
        Data.BrickReferencesHandle = BrickReferencesHandle;
        Data.ReferenceCountersHandle = ReferenceCountersHandle;
        Data.OccupiedBrickListHandle = OccupiedBrickListHandle;
        Data.ReferenceStatsHandle = ReferenceStatsHandle;
        auto Sections = Context.Owner.GetWorld().BuildSectionList();
        for (const FMeshSection& Section : Sections)
        {
            if (Section.IsStaticRegularMeshCandidate())
            {
                Data.TrianglePoolCapacity += Section.DrawIndexCount / 3u;
            }
        }
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && Data.TrianglePoolCapacity > 0u
            && static_cast<bool>(Data.SdfAtlasHandle)
            && static_cast<bool>(Data.BrickMetadataHandle)
            && static_cast<bool>(Data.TrianglePoolHandle)
            && static_cast<bool>(Data.BrickReferenceHeadsHandle)
            && static_cast<bool>(Data.BrickReferencesHandle)
            && static_cast<bool>(Data.ReferenceCountersHandle)
            && static_cast<bool>(Data.OccupiedBrickListHandle)
            && static_cast<bool>(Data.ReferenceStatsHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.TrianglePoolHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickReferenceHeadsHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickReferencesHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ReferenceCountersHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.OccupiedBrickListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Data.SdfAtlasHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.BrickMetadataHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.ReferenceStatsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.SdfAtlasHandle);
        Builder.UavBarrier(Data.BrickMetadataHandle);
        Builder.UavBarrier(Data.ReferenceStatsHandle);
    }, [this, &Context](const FSolveBrickReferencesPassData& Data, FDX12CommandContext& Cmd)
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
        ID3D12PipelineState* SolvePipeline = (SdfBuildMode == ESparseSdfGISdfBuildMode::ExactSharedBorder)
            ? ExactSolveBrickReferencesPipeline.Get()
            : SolveBrickReferencesPipeline.Get();
        CommandList->SetPipelineState(SolvePipeline);
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.DebugMode = static_cast<uint32_t>(DebugMode);
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
        Constants.UseHierarchicalTrace = bUseHierarchicalTrace ? 1u : 0u;
        Constants.Intensity = Intensity;
        Constants.BounceStrength = BounceStrength;
        Constants.MaxTraceDistance = Bounds.Extent.x;
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.TrianglePoolCapacity = Data.TrianglePoolCapacity;
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
        {
            return;
        }

        const uint32_t TrianglePoolSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.TrianglePoolHandle);
        const uint32_t BrickReferenceHeadsSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickReferenceHeadsHandle);
        const uint32_t BrickReferencesSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickReferencesHandle);
        const uint32_t ReferenceCountersSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ReferenceCountersHandle);
        const uint32_t OccupiedBrickListSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.OccupiedBrickListHandle);
        const uint32_t SdfAtlasUavIndex = Context.Graph.GetTextureUavBindlessIndex(Data.SdfAtlasHandle);
        const uint32_t BrickMetadataUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickMetadataHandle);
        const uint32_t ReferenceStatsUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ReferenceStatsHandle);
        if (!AreAllBindlessIndicesValid(SdfAtlasUavIndex, BrickMetadataUavIndex, TrianglePoolSrvIndex, BrickReferenceHeadsSrvIndex, BrickReferencesSrvIndex, ReferenceCountersSrvIndex, OccupiedBrickListSrvIndex, ReferenceStatsUavIndex))
        {
            std::ostringstream Oss;
            Oss << "SparseSdfGI solve skipped: invalid bindless index (SdfAtlasUav=" << SdfAtlasUavIndex
                << ", BrickMetadataUav=" << BrickMetadataUavIndex << ", TrianglePoolSrv=" << TrianglePoolSrvIndex
                << ", HeadsSrv=" << BrickReferenceHeadsSrvIndex << ", ReferencesSrv=" << BrickReferencesSrvIndex
                << ", CountersSrv=" << ReferenceCountersSrvIndex << ", OccupiedBrickListSrv=" << OccupiedBrickListSrvIndex
                << ", ReferenceStatsUav=" << ReferenceStatsUavIndex << ").";
            LogError(Oss.str());
            return;
        }

        const FSparseSdfGIReferenceSolveBindlessConstants Bindless =
        {
            SdfAtlasUavIndex,
            BrickMetadataUavIndex,
            TrianglePoolSrvIndex,
            BrickReferenceHeadsSrvIndex,
            BrickReferencesSrvIndex,
            ReferenceCountersSrvIndex,
            OccupiedBrickListSrvIndex,
            ReferenceStatsUavIndex
        };
        static_assert(sizeof(FSparseSdfGIReferenceSolveBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIReferenceSolveBindlessConstants) / sizeof(uint32_t), &Bindless, 0);

        const uint32_t SolveGroupBudget = (std::min)(GetBrickMapElementCount(), DebugSolveGroupBudget);
        for (uint32_t SolveDispatchOffset = 0u; SolveDispatchOffset < SolveGroupBudget; SolveDispatchOffset += kSparseSdfGISolveDispatchChunkGroups)
        {
            Constants.BuildWorkOffset = SolveDispatchOffset;
            if (!BindSparseConstants(Owner, CommandList, &Constants, sizeof(Constants)))
            {
                return;
            }
            const uint32_t ChunkGroupCount = (std::min)(kSparseSdfGISolveDispatchChunkGroups, SolveGroupBudget - SolveDispatchOffset);
            CommandList->Dispatch(ChunkGroupCount, 1u, 1u);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
        Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
    Constants.SdfBuildMode = static_cast<uint32_t>(SdfBuildMode);
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
