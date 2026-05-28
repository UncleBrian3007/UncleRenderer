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
    constexpr uint32_t kSparseSdfGIMaxBindlessDwordCount = 8u;
    constexpr uint32_t kSparseSdfGIGroupSize2D = 8u;
    constexpr uint32_t kSparseSdfGIGroupSize3D = 8u;
    constexpr uint32_t kSparseSdfGIReferenceEmitGroupSize = 64u;
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

    FRGBufferDesc CreateSolveIndirectArgsDesc()
    {
        return CreateRawBufferDesc(sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
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
        uint32_t Padding1 = 0;
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

    struct FSparseSdfGIPrepareSolveArgsBindlessConstants
    {
        uint32_t ReferenceCountersSrvIndex = UINT32_MAX;
        uint32_t SolveIndirectArgsUavIndex = UINT32_MAX;
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
    if (!CreateRootSignature(Device) || !CreatePipelines(Device) || !CreateDispatchCommandSignature(Device))
    {
        LogWarning("Deferred renderer: SparseSdfGI pipeline creation failed; feature disabled.");
        bEnabled = false;
        RootSignature.Reset();
        ReferenceBuildInitPipeline.Reset();
        ReferenceEmitPipeline.Reset();
        PrepareSolveArgsPipeline.Reset();
        SolveBrickReferencesPipeline.Reset();
        DebugTracePipeline.Reset();
        DiffuseTracePipeline.Reset();
        DispatchCommandSignature.Reset();
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
    const uint32_t NewCascadeCount = std::clamp(Config.SparseSdfGICascadeCount, 1u, 1u);
    const float NewBaseVoxelSize = Config.SparseSdfGIBaseVoxelSize;
    const float NewCascadeScale = (std::max)(Config.SparseSdfGICascadeScale, 1.01f);
    const uint32_t NewMaxBrickTriangleReferences = std::clamp(Config.SparseSdfGIMaxBrickTriangleReferences, kSparseSdfGIMinBrickTriangleReferences, kSparseSdfGIMaxBrickTriangleReferencesLimit);
    const bool bBuildSettingsChanged =
        CascadeCount != NewCascadeCount ||
        BaseVoxelSize != NewBaseVoxelSize ||
        CascadeScale != NewCascadeScale ||
        MaxBrickTriangleReferences != NewMaxBrickTriangleReferences;

    bEnabled = Config.bEnableSparseSdfGI;
    DebugMode = static_cast<ESparseSdfGIDebugMode>(std::clamp(Config.SparseSdfGIDebugMode, 0u, 5u));
    CascadeCount = NewCascadeCount;
    BaseVoxelSize = NewBaseVoxelSize;
    CascadeScale = NewCascadeScale;
    bTraceHalfResolution = Config.bSparseSdfGITraceHalfResolution;
    Intensity = (std::max)(0.0f, Config.SparseSdfGIIntensity);
    BounceStrength = (std::max)(0.0f, Config.SparseSdfGIBounceStrength);
    bUseHitLightingVisibility = Config.bSparseSdfGIUseHitLightingVisibility;
    MaxBrickTriangleReferences = NewMaxBrickTriangleReferences;

    if ((!bPreviousEnabled && bEnabled) || bBuildSettingsChanged)
    {
        InvalidateCache();
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
    Resources.SolveIndirectArgsHandle = {};
    Resources.CascadeBrickMapHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Cascade Brick Map", CascadeBrickMap);
    Resources.BrickMetadataHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Brick Metadata", BrickMetadata);
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

    AddReferenceBuildInitPass(Context);

    uint32_t DrawSectionIndex = 0;
    auto DrawSections = Context.Owner.GetWorld().BuildSectionList();
    for (FMeshSection& Section : DrawSections)
    {
        if (Section.IsStaticRegularMeshCandidate())
        {
            AddSectionReferenceEmitPass(Context, Section, DrawSectionIndex);
        }
        ++DrawSectionIndex;
    }

    AddPrepareSolveBrickReferencesArgsPass(Context);
    AddSolveBrickReferencesPass(Context);

    bSdfCacheValid = true;
    CachedSceneSignature = SceneSignature;
    CachedBuildSettingsSignature = BuildSettingsSignature;
    CachedCascadeBounds = Bounds;
    CachedStaticCandidateCount = StaticCandidateCount;
}

void FSparseSdfGI::AddDiffuseGITracePasses(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;
    const FRGBufferHandle BrickMetadataHandle = Context.Resources.SparseSdfGI.BrickMetadataHandle;
    const FRGResourceHandle DiffuseHandle = Context.Resources.SparseSdfGI.DiffuseGIHandle;
    ID3D12PipelineState* Pipeline = (DebugMode == ESparseSdfGIDebugMode::Off) ? DiffuseTracePipeline.Get() : DebugTracePipeline.Get();

    struct FOutputPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FOutputPassData>("SparseSdfGI Trace", [&, DepthHandle, SdfAtlasHandle, BrickMapHandle, BrickMetadataHandle, DiffuseHandle, GBufferHandles, Pipeline](FOutputPassData& Data, FRGPassBuilder& Builder)
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
    std::vector<uint8_t> PrepareSolveArgsByteCode;
    std::vector<uint8_t> SolveBrickReferencesByteCode;
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
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSPrepareSolveBrickReferencesArgs", PrepareSolveArgsByteCode, { L"SPARSE_SDF_GI_PREPARE_SOLVE_ARGS_SHADER=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, L"CSSolveBrickReferences", SolveBrickReferencesByteCode, { L"SPARSE_SDF_GI_REFERENCE_SOLVE_SHADER=1" }))
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
        && CreateComputePso(PrepareSolveArgsByteCode, PrepareSolveArgsPipeline, "CSPrepareSolveBrickReferencesArgs")
        && CreateComputePso(SolveBrickReferencesByteCode, SolveBrickReferencesPipeline, "CSSolveBrickReferences")
        && CreateComputePso(DebugTraceByteCode, DebugTracePipeline, "CSDebugTrace")
        && CreateComputePso(DiffuseTraceByteCode, DiffuseTracePipeline, "CSDiffuseTrace");
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
    CommandDesc.NumArgumentDescs = 1;
    CommandDesc.pArgumentDescs = &ArgumentDesc;

    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, nullptr, IID_PPV_ARGS(DispatchCommandSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FSparseSdfGI::CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (!Device || Width == 0u || Height == 0u)
    {
        return false;
    }

    const FRGTextureDesc SdfAtlasDesc =
    {
        kSparseSdfGIAtlasResolution,
        kSparseSdfGIAtlasResolution,
        DXGI_FORMAT_R16_UNORM,
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
        PrepareSolveArgsPipeline &&
        SolveBrickReferencesPipeline &&
        DebugTracePipeline &&
        DiffuseTracePipeline &&
        DispatchCommandSignature &&
        SdfAtlas.IsFullyBound() &&
        CascadeBrickMap.IsFullyBound() &&
        BrickMetadata.IsFullyBound() &&
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
        HashFloat4x4(Hash, Section.WorldMatrix);
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

void FSparseSdfGI::AddSectionReferenceEmitPass(FDeferredPassContext& Context, FMeshSection& Section, uint32_t DrawSectionIndex) const
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

    const float SectionScale = MatrixMath::ComputeMaxScale(Section.WorldMatrix);
    const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);

    struct FReferenceEmitPassData
    {
        bool bEnabled = false;
        uint32_t TriangleCount = 0;
        uint32_t DrawIndexStart = 0;
        uint32_t DrawIndexCount = 0;
        uint32_t TrianglePoolCapacity = 0;
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
        Data.VoxelSize = Bounds.VoxelSize;
        Data.CascadeMin = Bounds.Min;
        Data.CascadeExtent = Bounds.Extent;
        Data.World = Section.WorldMatrix;
        Data.PositionBufferIndex = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].SrvBindlessIndex;
        Data.IndexBufferIndex = Section.Geometry.IndexBuffer.SrvBindlessIndex;
        Data.bEnabled = Data.bEnabled
            && Data.TriangleCount > 0u
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
        Constants.ModelTriangleCount = Data.TriangleCount;
        Constants.ModelDrawIndexStart = Data.DrawIndexStart;
        Constants.ModelDrawIndexCount = Data.DrawIndexCount;
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.TrianglePoolCapacity = Data.TrianglePoolCapacity;
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
        CommandList->Dispatch(AlignDispatch(Data.TriangleCount, kSparseSdfGIReferenceEmitGroupSize), 1u, 1u);
    });
}

void FSparseSdfGI::AddPrepareSolveBrickReferencesArgsPass(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGBufferHandle ReferenceCountersHandle = Context.Resources.SparseSdfGI.ReferenceCountersHandle;

    struct FPrepareSolveArgsPassData
    {
        bool bEnabled = false;
        FRGBufferHandle ReferenceCountersHandle{};
        FRGBufferHandle SolveIndirectArgsHandle{};
    };

    Graph.AddPass<FPrepareSolveArgsPassData>("SparseSdfGI Prepare Solve Args", [&, ReferenceCountersHandle](FPrepareSolveArgsPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.ReferenceCountersHandle = ReferenceCountersHandle;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && static_cast<bool>(Data.ReferenceCountersHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Data.SolveIndirectArgsHandle = Builder.CreateBuffer("SparseSdfGI Solve Indirect Args", CreateSolveIndirectArgsDesc());
        Context.Resources.SparseSdfGI.SolveIndirectArgsHandle = Data.SolveIndirectArgsHandle;

        Builder.ReadBuffer(Data.ReferenceCountersHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(Data.SolveIndirectArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(Data.SolveIndirectArgsHandle);
    }, [this, &Context](const FPrepareSolveArgsPassData& Data, FDX12CommandContext& Cmd)
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
        CommandList->SetPipelineState(PrepareSolveArgsPipeline.Get());
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
        Constants.MaxTraceDistance = Bounds.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
        Constants.MaxBrickTriangleReferences = MaxBrickTriangleReferences;
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t ReferenceCountersSrvIndex = Context.Graph.GetBufferSrvBindlessIndex(Data.ReferenceCountersHandle);
        const uint32_t SolveIndirectArgsUavIndex = Context.Graph.GetBufferUavBindlessIndex(Data.SolveIndirectArgsHandle);
        if (!AreAllBindlessIndicesValid(ReferenceCountersSrvIndex, SolveIndirectArgsUavIndex))
        {
            return;
        }

        const FSparseSdfGIPrepareSolveArgsBindlessConstants Bindless =
        {
            ReferenceCountersSrvIndex,
            SolveIndirectArgsUavIndex
        };
        static_assert(sizeof(FSparseSdfGIPrepareSolveArgsBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIPrepareSolveArgsBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        CommandList->Dispatch(1u, 1u, 1u);
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
    const FRGBufferHandle SolveIndirectArgsHandle = Context.Resources.SparseSdfGI.SolveIndirectArgsHandle;

    struct FSolveBrickReferencesPassData
    {
        bool bEnabled = false;
        FRGBufferHandle TrianglePoolHandle{};
        FRGBufferHandle BrickReferenceHeadsHandle{};
        FRGBufferHandle BrickReferencesHandle{};
        FRGBufferHandle ReferenceCountersHandle{};
        FRGBufferHandle OccupiedBrickListHandle{};
        FRGBufferHandle SolveIndirectArgsHandle{};
    };

    Graph.AddPass<FSolveBrickReferencesPassData>("SparseSdfGI Solve Brick References", [&, SdfAtlasHandle, BrickMetadataHandle, TrianglePoolHandle, BrickReferenceHeadsHandle, BrickReferencesHandle, ReferenceCountersHandle, OccupiedBrickListHandle, SolveIndirectArgsHandle](FSolveBrickReferencesPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.TrianglePoolHandle = TrianglePoolHandle;
        Data.BrickReferenceHeadsHandle = BrickReferenceHeadsHandle;
        Data.BrickReferencesHandle = BrickReferencesHandle;
        Data.ReferenceCountersHandle = ReferenceCountersHandle;
        Data.OccupiedBrickListHandle = OccupiedBrickListHandle;
        Data.SolveIndirectArgsHandle = SolveIndirectArgsHandle;
        Data.bEnabled = bEnabled
            && bPersistentInputsValid
            && static_cast<bool>(Data.TrianglePoolHandle)
            && static_cast<bool>(Data.BrickReferenceHeadsHandle)
            && static_cast<bool>(Data.BrickReferencesHandle)
            && static_cast<bool>(Data.ReferenceCountersHandle)
            && static_cast<bool>(Data.OccupiedBrickListHandle)
            && static_cast<bool>(Data.SolveIndirectArgsHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(Data.TrianglePoolHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickReferenceHeadsHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.BrickReferencesHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.ReferenceCountersHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.OccupiedBrickListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.SolveIndirectArgsHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.WriteTexture(SdfAtlasHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(BrickMetadataHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(SdfAtlasHandle);
        Builder.UavBarrier(BrickMetadataHandle);
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
        if (!AreAllBindlessIndicesValid(TrianglePoolSrvIndex, BrickReferenceHeadsSrvIndex, BrickReferencesSrvIndex, ReferenceCountersSrvIndex, OccupiedBrickListSrvIndex))
        {
            return;
        }

        const FSparseSdfGIReferenceSolveBindlessConstants Bindless =
        {
            SdfAtlas.UavBindlessIndex,
            BrickMetadata.UavBindlessIndex,
            TrianglePoolSrvIndex,
            BrickReferenceHeadsSrvIndex,
            BrickReferencesSrvIndex,
            ReferenceCountersSrvIndex,
            OccupiedBrickListSrvIndex
        };
        static_assert(sizeof(FSparseSdfGIReferenceSolveBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGIReferenceSolveBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
        ID3D12Resource* SolveIndirectArgsBuffer = Context.Graph.GetBufferResource(Data.SolveIndirectArgsHandle);
        if (!SolveIndirectArgsBuffer)
        {
            return;
        }
        CommandList->ExecuteIndirect(DispatchCommandSignature.Get(), 1, SolveIndirectArgsBuffer, 0u, nullptr, 0u);
    });
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
        LinearClampSamplerIndex
    };
    static_assert(sizeof(FSparseSdfGITraceBindlessConstants) / sizeof(uint32_t) <= kSparseSdfGIMaxBindlessDwordCount);
    CommandList->SetComputeRoot32BitConstants(2, sizeof(FSparseSdfGITraceBindlessConstants) / sizeof(uint32_t), &Bindless, 0);
    CommandList->Dispatch(
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Width), kSparseSdfGIGroupSize2D),
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Height), kSparseSdfGIGroupSize2D),
        1u);
}
