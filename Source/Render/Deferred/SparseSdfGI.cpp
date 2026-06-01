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
    constexpr uint32_t kSparseSdfGIAtlasResolution = kSparseSdfGIBrickGridResolution * kSparseSdfGIBrickVoxelResolution;
    constexpr uint32_t kSparseSdfGIConstantsDwordCount = 46u;
    constexpr uint32_t kSparseSdfGIMaxBindlessDwordCount = 9u;
    constexpr uint32_t kSparseSdfGIGroupSize2D = 8u;
    constexpr uint32_t kSparseSdfGIGroupSize3D = 8u;
    constexpr uint32_t kSparseSdfGIReferenceEmitGroupSize = 64u;
    constexpr uint32_t kSparseSdfGIReferenceEmitChunkTriangles = 16u * 1024u;
    constexpr uint32_t kSparseSdfGISolveDispatchChunkGroups = 1024u;
    constexpr float kSparseSdfGICascadeSceneRadiusMargin = 1.10f;
    constexpr float kSparseSdfGIMinVoxelSize = 0.001f;
    constexpr float kSparseSdfGISurfaceThicknessVoxels = 1.5f;
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

    struct FSparseSdfGIConstants
    {
        uint32_t OutputWidth = 0;
        uint32_t OutputHeight = 0;
        uint32_t AtlasResolution = kSparseSdfGIAtlasResolution;
        uint32_t BrickGridResolution = kSparseSdfGIBrickGridResolution;
        uint32_t BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        uint32_t CascadeCount = 1;
        uint32_t FrameIndex = 0;
        uint32_t DebugMode = 0;
        uint32_t Enabled = 0;
        uint32_t TraceHalfResolution = 0;
        uint32_t ModelTriangleCount = 0;
        uint32_t ModelDrawIndexStart = 0;
        uint32_t ModelDrawIndexCount = 0;
        uint32_t MaxBrickTriangleReferences = kSparseSdfGIDefaultMaxBrickTriangleReferences;
        float BaseVoxelSize = 0.25f;
        float CascadeScale = 2.0f;
        float Intensity = 1.0f;
        float MaxTraceDistance = 64.0f;
        uint32_t TrianglePoolCapacity = 0;
        uint32_t BuildWorkOffset = 0u;
        DirectX::XMFLOAT3 CascadeMin{ 0.0f, 0.0f, 0.0f };
        float VoxelSize = 0.25f;
        DirectX::XMFLOAT3 CascadeExtent{ 1.0f, 1.0f, 1.0f };
        float SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        DirectX::XMFLOAT4X4 World{};
        float BounceStrength = 1.0f;
        uint32_t UseHitLightingVisibility = 0u;
    };
    static_assert(offsetof(FSparseSdfGIConstants, CascadeMin) == 20u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, CascadeExtent) == 24u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, World) == 28u * sizeof(uint32_t));
    static_assert(sizeof(FSparseSdfGIConstants) / sizeof(uint32_t) == 46u);

    struct FSparseSdfGIReferenceInitBindlessConstants
    {
        uint32_t SdfAtlasUavIndex = UINT32_MAX;
        uint32_t CascadeBrickMapUavIndex = UINT32_MAX;
        uint32_t BrickMetadataUavIndex = UINT32_MAX;
        uint32_t ReferenceHeadsUavIndex = UINT32_MAX;
        uint32_t ReferenceCountersUavIndex = UINT32_MAX;
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
    };

    struct FSparseSdfGIRadianceResolveBindlessConstants
    {
        uint32_t BrickRadianceAccumSrvIndex = UINT32_MAX;
        uint32_t BrickRadianceHistorySrvIndex = UINT32_MAX;
        uint32_t BrickRadianceUavIndex = UINT32_MAX;
        uint32_t RadianceHistoryValid = 0u;
    };

    struct FSparseSdfGIRadianceCopyBindlessConstants
    {
        uint32_t BrickRadianceSrvIndex = UINT32_MAX;
        uint32_t BrickRadianceHistoryUavIndex = UINT32_MAX;
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
        RadianceClearPipeline.Reset();
        RadianceInjectPipeline.Reset();
        RadianceResolvePipeline.Reset();
        RadianceCopyHistoryPipeline.Reset();
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
    return CreateResources(Device, Width, Height);
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
    const uint32_t NewCascadeCount = std::clamp(Config.SparseSdfGICascadeCount, 1u, 1u);
    const float NewBaseVoxelSize = Config.SparseSdfGIBaseVoxelSize;
    const float NewCascadeScale = (std::max)(Config.SparseSdfGICascadeScale, 1.01f);
    const uint32_t NewMaxBrickTriangleReferences = std::clamp(Config.SparseSdfGIMaxBrickTriangleReferences, kSparseSdfGIMinBrickTriangleReferences, kSparseSdfGIMaxBrickTriangleReferencesLimit);
    const bool bBuildSettingsChanged =
        CascadeCount != NewCascadeCount ||
        BaseVoxelSize != NewBaseVoxelSize ||
        CascadeScale != NewCascadeScale ||
        MaxBrickTriangleReferences != NewMaxBrickTriangleReferences ||
        DebugSolveGroupBudget != Config.SparseSdfGIDebugSolveGroupBudget ||
        DebugEmitTriangleBudget != Config.SparseSdfGIDebugEmitTriangleBudget;

    bEnabled = Config.bEnableSparseSdfGI;
    DebugMode = static_cast<ESparseSdfGIDebugMode>(std::clamp(Config.SparseSdfGIDebugMode, 0u, 5u));
    CascadeCount = NewCascadeCount;
    BaseVoxelSize = NewBaseVoxelSize;
    CascadeScale = NewCascadeScale;
    bTraceHalfResolution = Config.bSparseSdfGITraceHalfResolution;
    Intensity = (std::max)(0.0f, Config.SparseSdfGIIntensity);
    BounceStrength = (std::max)(0.0f, Config.SparseSdfGIBounceStrength);
    bUseHitLightingVisibility = Config.bSparseSdfGIUseHitLightingVisibility;
    bEnableRadianceTemporalReuse = Config.bSparseSdfGIEnableRadianceTemporalReuse;
    MaxBrickTriangleReferences = NewMaxBrickTriangleReferences;
    DebugSolveGroupBudget = Config.SparseSdfGIDebugSolveGroupBudget;
    DebugEmitTriangleBudget = Config.SparseSdfGIDebugEmitTriangleBudget;

    if ((!bPreviousEnabled && bEnabled) || bBuildSettingsChanged)
    {
        InvalidateCache();
    }
    else if (bPreviousRadianceTemporalReuse != bEnableRadianceTemporalReuse)
    {
        bBrickRadianceHistoryValid = false;
    }
}

void FSparseSdfGI::ForceInvalidateCache() const
{
    InvalidateCache();
}

void FSparseSdfGI::ImportPersistentResources(FDeferredPassContext& Context)
{
    FRenderGraph& Graph = Context.Graph;
    FSparseSdfGIFrameResources& Resources = Context.Resources.SparseSdfGI;

    Resources.SdfAtlasHandle = ImportBindlessTexture(Graph, "SparseSdfGI SDF Atlas", SdfAtlas);
    Resources.TrianglePoolHandle = {};
    Resources.BrickReferenceHeadsHandle = {};
    Resources.BrickReferencesHandle = {};
    Resources.ReferenceCountersHandle = {};
    Resources.OccupiedBrickListHandle = {};
    Resources.BrickRadianceAccumHandle = {};
    Resources.CascadeBrickMapHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Cascade Brick Map", CascadeBrickMap);
    Resources.BrickMetadataHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Metadata", BrickMetadata);
    Resources.BrickRadianceHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Radiance", BrickRadiance);
    Resources.BrickRadianceHistoryHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Radiance History", BrickRadianceHistory);
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
        return;
    }

    bBrickRadianceHistoryValid = false;
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
    const FRGBufferHandle BrickRadianceHandle = Context.Resources.SparseSdfGI.BrickRadianceHandle;
    const FRGResourceHandle DiffuseHandle = Context.Resources.SparseSdfGI.DiffuseGIHandle;
    ID3D12PipelineState* Pipeline = (DebugMode == ESparseSdfGIDebugMode::Off) ? DiffuseTracePipeline.Get() : DebugTracePipeline.Get();

    struct FOutputPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FOutputPassData>("SparseSdfGI Trace", [&, DepthHandle, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, BrickRadianceHandle, DiffuseHandle, GBufferHandles, Pipeline](FOutputPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid;
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SdfAtlasHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickMapHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickMetadataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(BrickRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(DiffuseHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, Pipeline](const FOutputPassData& Data, FDX12CommandContext& Cmd)
    {
        DispatchOutputPass(Context, Cmd, Pipeline, Data.bEnabled);
    });

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
    RootParams[1].InitAsConstants(kSparseSdfGIConstantsDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
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
    std::vector<uint8_t> RadianceClearByteCode;
    std::vector<uint8_t> RadianceInjectByteCode;
    std::vector<uint8_t> RadianceResolveByteCode;
    std::vector<uint8_t> RadianceCopyHistoryByteCode;
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
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSClearBrickRadianceAccum", RadianceClearByteCode, { L"SPARSE_SDF_GI_RADIANCE_CLEAR_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSInjectBrickRadiance", RadianceInjectByteCode, { L"SPARSE_SDF_GI_RADIANCE_INJECT_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSResolveBrickRadianceTemporal", RadianceResolveByteCode, { L"SPARSE_SDF_GI_RADIANCE_RESOLVE_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSCopyBrickRadianceToHistory", RadianceCopyHistoryByteCode, { L"SPARSE_SDF_GI_RADIANCE_COPY_SHADER=1" }))
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
        && CreateComputePso(RadianceClearByteCode, RadianceClearPipeline, "CSClearBrickRadianceAccum")
        && CreateComputePso(RadianceInjectByteCode, RadianceInjectPipeline, "CSInjectBrickRadiance")
        && CreateComputePso(RadianceResolveByteCode, RadianceResolvePipeline, "CSResolveBrickRadianceTemporal")
        && CreateComputePso(RadianceCopyHistoryByteCode, RadianceCopyHistoryPipeline, "CSCopyBrickRadianceToHistory")
        && CreateComputePso(DebugTraceByteCode, DebugTracePipeline, "CSDebugTrace")
        && CreateComputePso(DiffuseTraceByteCode, DiffuseTracePipeline, "CSDiffuseTrace");
}

bool FSparseSdfGI::CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
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

    const FRGBufferDesc BrickRadianceDesc = CreateBrickRadianceDesc();
    CreateBindlessBuffer(
        Device,
        L"SparseSdfGI_BrickRadiance",
        BrickRadianceDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        BrickRadiance,
        true,
        true);

    CreateBindlessBuffer(
        Device,
        L"SparseSdfGI_BrickRadianceHistory",
        BrickRadianceDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        BrickRadianceHistory,
        true,
        true);

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
        RadianceClearPipeline &&
        RadianceInjectPipeline &&
        RadianceResolvePipeline &&
        RadianceCopyHistoryPipeline &&
        DebugTracePipeline &&
        DiffuseTracePipeline &&
        SdfAtlas.IsFullyBound() &&
        CascadeBrickMap.IsFullyBound() &&
        BrickMetadata.IsFullyBound() &&
        BrickRadiance.IsFullyBound() &&
        BrickRadianceHistory.IsFullyBound() &&
        DiffuseGI.IsFullyBound();

    return true;
}

FSparseSdfGI::FCascadeBounds FSparseSdfGI::ComputeCascadeBounds(const FDeferredRenderer& Owner) const
{
    const float SceneRadius = (std::max)(Owner.GetSceneRadius(), kSparseSdfGIMinVoxelSize);
    const float AutoVoxelSize = (std::max)(
        (SceneRadius * 2.0f * kSparseSdfGICascadeSceneRadiusMargin) / static_cast<float>(kSparseSdfGIAtlasResolution),
        kSparseSdfGIMinVoxelSize);
    const float VoxelSize = (BaseVoxelSize > 0.0f)
        ? (std::max)(BaseVoxelSize, kSparseSdfGIMinVoxelSize)
        : AutoVoxelSize;
    const float ExtentValue = VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
    const DirectX::XMFLOAT3 SceneCenter = Owner.GetSceneCenter();

    FCascadeBounds Bounds = {};
    Bounds.Extent = DirectX::XMFLOAT3(ExtentValue, ExtentValue, ExtentValue);
    Bounds.VoxelSize = VoxelSize;
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
    bBrickRadianceHistoryValid = false;
}

void FSparseSdfGI::AddReferenceBuildInitPass(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;

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
        uint32_t TrianglePoolCapacity = 0;
    };

    Graph.AddPass<FReferenceBuildInitPassData>("SparseSdfGI Reference Build Init", [&, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, MaxStaticTriangleCount](FReferenceBuildInitPassData& Data, FRGPassBuilder& Builder)
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
        Builder.UavBarrier(SdfAtlasHandle);
        Builder.UavBarrier(BrickMetadataHandle);
        Builder.UavBarrier(Data.BrickReferenceHeadsHandle);
        Builder.UavBarrier(Data.ReferenceCountersHandle);
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
        Constants.CascadeCount = CascadeCount;
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.DebugMode = static_cast<uint32_t>(DebugMode);
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);
        Constants.BaseVoxelSize = Bounds.VoxelSize;
        Constants.CascadeScale = CascadeScale;
        Constants.Intensity = Intensity;
        Constants.BounceStrength = BounceStrength;
        Constants.UseHitLightingVisibility = bUseHitLightingVisibility ? 1u : 0u;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.MaxTraceDistance = Bounds.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.TrianglePoolCapacity = Data.TrianglePoolCapacity;
        Constants.CascadeMin = Bounds.Min;
        Constants.CascadeExtent = Bounds.Extent;
        static_assert(sizeof(FSparseSdfGIConstants) / sizeof(uint32_t) <= kSparseSdfGIConstantsDwordCount);
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t BrickReferenceHeadsUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickReferenceHeadsHandle);
        const uint32_t ReferenceCountersUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.ReferenceCountersHandle);
        if (!AreAllBindlessIndicesValid(SdfAtlas.UavBindlessIndex, CascadeBrickMap.UavBindlessIndex, BrickMetadata.UavBindlessIndex, BrickReferenceHeadsUavIndex, ReferenceCountersUavIndex))
        {
            return;
        }

        const FSparseSdfGIReferenceInitBindlessConstants Bindless =
        {
            SdfAtlas.UavBindlessIndex,
            CascadeBrickMap.UavBindlessIndex,
            BrickMetadata.UavBindlessIndex,
            BrickReferenceHeadsUavIndex,
            ReferenceCountersUavIndex
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
        Constants.CascadeCount = CascadeCount;
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.DebugMode = static_cast<uint32_t>(DebugMode);
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.TrianglePoolCapacity = Data.TrianglePoolCapacity;
        Constants.BuildWorkOffset = Data.PositionCount;
        Constants.BaseVoxelSize = Data.VoxelSize;
        Constants.CascadeScale = CascadeScale;
        Constants.Intensity = Intensity;
        Constants.BounceStrength = BounceStrength;
        Constants.UseHitLightingVisibility = bUseHitLightingVisibility ? 1u : 0u;
        Constants.MaxTraceDistance = Data.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
        Constants.CascadeMin = Data.CascadeMin;
        Constants.VoxelSize = Data.VoxelSize;
        Constants.CascadeExtent = Data.CascadeExtent;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        Constants.World = Data.World;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

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
            CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);
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
        uint32_t TrianglePoolCapacity = 0;
    };

    Graph.AddPass<FSolveBrickReferencesPassData>("SparseSdfGI Solve Brick References", [&, SdfAtlasHandle, BrickMetadataHandle, TrianglePoolHandle, BrickReferenceHeadsHandle, BrickReferencesHandle, ReferenceCountersHandle, OccupiedBrickListHandle](FSolveBrickReferencesPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.SdfAtlasHandle = SdfAtlasHandle;
        Data.BrickMetadataHandle = BrickMetadataHandle;
        Data.TrianglePoolHandle = TrianglePoolHandle;
        Data.BrickReferenceHeadsHandle = BrickReferenceHeadsHandle;
        Data.BrickReferencesHandle = BrickReferencesHandle;
        Data.ReferenceCountersHandle = ReferenceCountersHandle;
        Data.OccupiedBrickListHandle = OccupiedBrickListHandle;
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
            && static_cast<bool>(Data.OccupiedBrickListHandle);
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
        Builder.UavBarrier(Data.SdfAtlasHandle);
        Builder.UavBarrier(Data.BrickMetadataHandle);
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
        CommandList->SetPipelineState(SolveBrickReferencesPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.CascadeCount = CascadeCount;
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.DebugMode = static_cast<uint32_t>(DebugMode);
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
        Constants.BaseVoxelSize = Bounds.VoxelSize;
        Constants.CascadeScale = CascadeScale;
        Constants.Intensity = Intensity;
        Constants.BounceStrength = BounceStrength;
        Constants.UseHitLightingVisibility = bUseHitLightingVisibility ? 1u : 0u;
        Constants.MaxTraceDistance = Bounds.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.TrianglePoolCapacity = Data.TrianglePoolCapacity;
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t TrianglePoolSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.TrianglePoolHandle);
        const uint32_t BrickReferenceHeadsSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickReferenceHeadsHandle);
        const uint32_t BrickReferencesSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickReferencesHandle);
        const uint32_t ReferenceCountersSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ReferenceCountersHandle);
        const uint32_t OccupiedBrickListSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.OccupiedBrickListHandle);
        const uint32_t SdfAtlasUavIndex = Context.Graph.GetTextureUavBindlessIndex(Data.SdfAtlasHandle);
        const uint32_t BrickMetadataUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickMetadataHandle);
        if (!AreAllBindlessIndicesValid(SdfAtlasUavIndex, BrickMetadataUavIndex, TrianglePoolSrvIndex, BrickReferenceHeadsSrvIndex, BrickReferencesSrvIndex, ReferenceCountersSrvIndex, OccupiedBrickListSrvIndex))
        {
            std::ostringstream Oss;
            Oss << "SparseSdfGI solve skipped: invalid bindless index (SdfAtlasUav=" << SdfAtlasUavIndex
                << ", BrickMetadataUav=" << BrickMetadataUavIndex << ", TrianglePoolSrv=" << TrianglePoolSrvIndex
                << ", HeadsSrv=" << BrickReferenceHeadsSrvIndex << ", ReferencesSrv=" << BrickReferencesSrvIndex
                << ", CountersSrv=" << ReferenceCountersSrvIndex << ", OccupiedBrickListSrv=" << OccupiedBrickListSrvIndex << ").";
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
            OccupiedBrickListSrvIndex
        };
        static_assert(sizeof(FSparseSdfGIReferenceSolveBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIReferenceSolveBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        // Debug work cap (DebugSolveGroupBudget): shrink/skip solve to isolate the device-removed crash.
        const uint32_t SolveGroupBudget = (std::min)(GetBrickMapElementCount(), DebugSolveGroupBudget);
        for (uint32_t SolveDispatchOffset = 0u; SolveDispatchOffset < SolveGroupBudget; SolveDispatchOffset += kSparseSdfGISolveDispatchChunkGroups)
        {
            Constants.BuildWorkOffset = SolveDispatchOffset;
            CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);
            const uint32_t ChunkGroupCount = (std::min)(kSparseSdfGISolveDispatchChunkGroups, SolveGroupBudget - SolveDispatchOffset);
            CommandList->Dispatch(ChunkGroupCount, 1u, 1u);
        }
    });
}

void FSparseSdfGI::AddRadianceCachePasses(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle ShadowMaskHandle = Context.Resources.RayTracingShadow.ShadowMaskHandle;
    const FRGBufferHandle BrickRadianceHandle = Context.Resources.SparseSdfGI.BrickRadianceHandle;
    const FRGBufferHandle BrickRadianceHistoryHandle = Context.Resources.SparseSdfGI.BrickRadianceHistoryHandle;
    const bool bHistoryValidForResolve = bEnableRadianceTemporalReuse && bBrickRadianceHistoryValid;
    const bool bRadianceCacheWillUpdate = bEnabled
        && bPersistentInputsValid
        && DebugMode == ESparseSdfGIDebugMode::Off
        && static_cast<bool>(DepthHandle)
        && static_cast<bool>(GBufferHandles[0])
        && static_cast<bool>(GBufferHandles[1])
        && static_cast<bool>(GBufferHandles[2])
        && static_cast<bool>(BrickRadianceHandle)
        && static_cast<bool>(BrickRadianceHistoryHandle);

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
        Constants.CascadeCount = CascadeCount;
        Constants.Enabled = bEnabled ? 1u : 0u;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

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
    };

    Graph.AddPass<FRadianceInjectPassData>("SparseSdfGI Radiance Inject", [&, DepthHandle, ShadowMaskHandle, GBufferHandles](FRadianceInjectPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickRadianceAccumHandle = Context.Resources.SparseSdfGI.BrickRadianceAccumHandle;
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
        if (ShadowMaskHandle)
        {
            Builder.ReadTexture(ShadowMaskHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
        const uint32_t ShadowMaskBindlessIndex = Owner.ShadowMaskBindlessIndex;
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
        Constants.CascadeCount = CascadeCount;
        Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
        Constants.Enabled = bEnabled ? 1u : 0u;
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const FSparseSdfGIRadianceInjectBindlessConstants Bindless =
        {
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.GBufferC.SrvBindlessIndex,
            BrickRadianceAccumUavIndex,
            bUseShadowMask ? ShadowMaskBindlessIndex : UINT32_MAX,
            bUseShadowMask ? 1u : 0u
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
        FRGBufferHandle BrickRadianceHandle{};
        FRGBufferHandle BrickRadianceHistoryHandle{};
    };

    Graph.AddPass<FRadianceResolvePassData>("SparseSdfGI Radiance Resolve", [&, BrickRadianceHandle, BrickRadianceHistoryHandle, bHistoryValidForResolve](FRadianceResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickRadianceAccumHandle = Context.Resources.SparseSdfGI.BrickRadianceAccumHandle;
        Data.BrickRadianceHandle = BrickRadianceHandle;
        Data.BrickRadianceHistoryHandle = BrickRadianceHistoryHandle;
        Data.bHistoryValid = bHistoryValidForResolve;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && DebugMode == ESparseSdfGIDebugMode::Off
            && static_cast<bool>(Data.BrickRadianceAccumHandle)
            && static_cast<bool>(Data.BrickRadianceHandle)
            && static_cast<bool>(Data.BrickRadianceHistoryHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.BrickRadianceAccumHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickRadianceHistoryHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.BrickRadianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.BrickRadianceHandle);
    }, [this, &Context](const FRadianceResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t BrickRadianceAccumSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceAccumHandle);
        const uint32_t BrickRadianceHistorySrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceHistoryHandle);
        const uint32_t BrickRadianceUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickRadianceHandle);
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
        Constants.CascadeCount = CascadeCount;
        Constants.Enabled = bEnabled ? 1u : 0u;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

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

    struct FRadianceCopyHistoryPassData
    {
        bool bEnabled = false;
        FRGBufferHandle BrickRadianceHandle{};
        FRGBufferHandle BrickRadianceHistoryHandle{};
    };

    Graph.AddPass<FRadianceCopyHistoryPassData>("SparseSdfGI Radiance Copy History", [&, BrickRadianceHandle, BrickRadianceHistoryHandle, bRadianceCacheWillUpdate](FRadianceCopyHistoryPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.BrickRadianceHandle = BrickRadianceHandle;
        Data.BrickRadianceHistoryHandle = BrickRadianceHistoryHandle;
        Data.bEnabled = bRadianceCacheWillUpdate && bEnableRadianceTemporalReuse;
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.BrickRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.BrickRadianceHistoryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.BrickRadianceHistoryHandle);
    }, [this, &Context](const FRadianceCopyHistoryPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        FDeferredRenderer& Owner = Context.Owner;
        const uint32_t BrickRadianceSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.BrickRadianceHandle);
        const uint32_t BrickRadianceHistoryUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.BrickRadianceHistoryHandle);
        if (!AreAllBindlessIndicesValid(BrickRadianceSrvIndex, BrickRadianceHistoryUavIndex))
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(RootSignature.Get());
        CommandList->SetPipelineState(RadianceCopyHistoryPipeline.Get());
        CommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        FSparseSdfGIConstants Constants = {};
        Constants.OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.AtlasResolution = kSparseSdfGIAtlasResolution;
        Constants.BrickGridResolution = kSparseSdfGIBrickGridResolution;
        Constants.BrickVoxelResolution = kSparseSdfGIBrickVoxelResolution;
        Constants.CascadeCount = CascadeCount;
        Constants.Enabled = bEnabled ? 1u : 0u;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const FSparseSdfGIRadianceCopyBindlessConstants Bindless =
        {
            BrickRadianceSrvIndex,
            BrickRadianceHistoryUavIndex
        };
        static_assert(sizeof(FSparseSdfGIRadianceCopyBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIRadianceCopyBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(GetBrickMapElementCount(), 64u), 1u, 1u);
    });

    if (bRadianceCacheWillUpdate && bEnableRadianceTemporalReuse)
    {
        bBrickRadianceHistoryValid = true;
    }
}

void FSparseSdfGI::DispatchOutputPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, bool bPassEnabled) const
{
    if (!bPassEnabled || !PipelineState)
    {
        return;
    }

    FDeferredRenderer& Owner = Context.Owner;
    const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
    const uint32_t EnvironmentCubeIndex = Owner.GetEnvironmentCubeSrvIndex();
    const uint32_t LinearClampSamplerIndex = Owner.Device->GetLinearClampSamplerIndex();
    const bool bInputsValid = AreAllBindlessIndicesValid(
        DepthBindlessIndex,
        Owner.GBufferA.SrvBindlessIndex,
        EnvironmentCubeIndex,
        LinearClampSamplerIndex,
        SdfAtlas.SrvBindlessIndex,
        CascadeBrickMap.SrvBindlessIndex,
        BrickMetadata.SrvBindlessIndex,
        BrickRadiance.SrvBindlessIndex,
        DiffuseGI.UavBindlessIndex);
    if (!bInputsValid)
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
    Constants.CascadeCount = CascadeCount;
    Constants.FrameIndex = static_cast<uint32_t>(Owner.GetFrameNumber());
    Constants.DebugMode = static_cast<uint32_t>(DebugMode);
    Constants.Enabled = bEnabled ? 1u : 0u;
    Constants.TraceHalfResolution = bTraceHalfResolution ? 1u : 0u;
    Constants.BaseVoxelSize = Bounds.VoxelSize;
    Constants.CascadeScale = CascadeScale;
    Constants.Intensity = Intensity;
    Constants.BounceStrength = BounceStrength;
    Constants.UseHitLightingVisibility = bUseHitLightingVisibility ? 1u : 0u;
    Constants.MaxTraceDistance = Bounds.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
    Constants.CascadeMin = Bounds.Min;
    Constants.VoxelSize = Bounds.VoxelSize;
    Constants.CascadeExtent = Bounds.Extent;
    Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
    CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

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
        BrickRadiance.SrvBindlessIndex
    };
    static_assert(sizeof(FSparseSdfGITraceBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
    CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGITraceBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
    CommandList->Dispatch(
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Width), kSparseSdfGIGroupSize2D),
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Height), kSparseSdfGIGroupSize2D),
        1u);
}
