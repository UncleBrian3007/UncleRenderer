#include "SparseSdfGI.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../SceneModelResource.h"
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
    constexpr uint32_t kSparseSdfGIConstantsDwordCount = 48u;
    constexpr uint32_t kSparseSdfGIBindlessDwordCount = 14u;
    constexpr uint32_t kSparseSdfGIGroupSize2D = 8u;
    constexpr uint32_t kSparseSdfGIGroupSize3D = 8u;
    constexpr uint32_t kSparseSdfGIVoxelizeGroupSize = 64u;
    constexpr float kSparseSdfGICascadeSceneRadiusMargin = 1.10f;
    constexpr float kSparseSdfGIMinVoxelSize = 0.001f;
    constexpr float kSparseSdfGISurfaceThicknessVoxels = 1.5f;
    constexpr uint32_t kSparseSdfGIDefaultMaxTriangleVoxelSpan = 32u;
    // Per-triangle voxel-span guard cap; voxelize is O(span^3) so bound it to avoid GPU hangs.
    constexpr uint32_t kSparseSdfGIMaxTriangleVoxelSpanLimit = 128u;
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

    FRGTextureDesc CreateSeedDistanceAtlasDesc()
    {
        return
        {
            kSparseSdfGIAtlasResolution,
            kSparseSdfGIAtlasResolution,
            DXGI_FORMAT_R32_UINT,
            1,
            static_cast<uint16_t>(kSparseSdfGIAtlasResolution),
            D3D12_RESOURCE_DIMENSION_TEXTURE3D
        };
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
        uint32_t MaxTriangleVoxelSpan = kSparseSdfGIDefaultMaxTriangleVoxelSpan;
        float BaseVoxelSize = 0.25f;
        float CascadeScale = 2.0f;
        float Intensity = 1.0f;
        float MaxTraceDistance = 64.0f;
        float Padding1[2] = {};
        DirectX::XMFLOAT3 CascadeMin{ 0.0f, 0.0f, 0.0f };
        float VoxelSize = 0.25f;
        DirectX::XMFLOAT3 CascadeExtent{ 1.0f, 1.0f, 1.0f };
        float SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        DirectX::XMFLOAT4X4 World{};
    };
    static_assert(offsetof(FSparseSdfGIConstants, CascadeMin) == 20u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, CascadeExtent) == 24u * sizeof(uint32_t));
    static_assert(offsetof(FSparseSdfGIConstants, World) == 28u * sizeof(uint32_t));
    static_assert(sizeof(FSparseSdfGIConstants) / sizeof(uint32_t) == 44u);

    struct FSparseSdfGIBindlessConstants
    {
        uint32_t SdfAtlasSrvIndex = UINT32_MAX;
        uint32_t SdfAtlasUavIndex = UINT32_MAX;
        uint32_t SdfSeedDistanceSrvIndex = UINT32_MAX;
        uint32_t SdfSeedDistanceUavIndex = UINT32_MAX;
        uint32_t CascadeBrickMapSrvIndex = UINT32_MAX;
        uint32_t CascadeBrickMapUavIndex = UINT32_MAX;
        uint32_t DiffuseGIUavIndex = UINT32_MAX;
        uint32_t DepthIndex = UINT32_MAX;
        uint32_t GBufferAIndex = UINT32_MAX;
        uint32_t GBufferBIndex = UINT32_MAX;
        uint32_t GBufferCIndex = UINT32_MAX;
        uint32_t PositionBufferIndex = UINT32_MAX;
        uint32_t IndexBufferIndex = UINT32_MAX;
        uint32_t LinearClampSamplerIndex = UINT32_MAX;
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
        SeedAtlasInitPipeline.Reset();
        VoxelizePipeline.Reset();
        EikonalPipeline.Reset();
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
    const uint32_t NewCascadeCount = std::clamp(Config.SparseSdfGICascadeCount, 1u, 1u);
    const float NewBaseVoxelSize = Config.SparseSdfGIBaseVoxelSize;
    const float NewCascadeScale = (std::max)(Config.SparseSdfGICascadeScale, 1.01f);
    const uint32_t NewMaxTriangleVoxelSpan = std::clamp(Config.SparseSdfGIMaxTriangleVoxelSpan, 1u, kSparseSdfGIMaxTriangleVoxelSpanLimit);
    const bool bBuildSettingsChanged =
        CascadeCount != NewCascadeCount ||
        BaseVoxelSize != NewBaseVoxelSize ||
        CascadeScale != NewCascadeScale ||
        MaxTriangleVoxelSpan != NewMaxTriangleVoxelSpan;

    bEnabled = Config.bEnableSparseSdfGI;
    DebugMode = static_cast<ESparseSdfGIDebugMode>(std::clamp(Config.SparseSdfGIDebugMode, 0u, 5u));
    CascadeCount = NewCascadeCount;
    BaseVoxelSize = NewBaseVoxelSize;
    CascadeScale = NewCascadeScale;
    bTraceHalfResolution = Config.bSparseSdfGITraceHalfResolution;
    Intensity = (std::max)(0.0f, Config.SparseSdfGIIntensity);
    MaxTriangleVoxelSpan = NewMaxTriangleVoxelSpan;

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
    Resources.SdfSeedDistanceHandle = {};
    Resources.CascadeBrickMapHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Cascade Brick Map", CascadeBrickMap);
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

    // AddSeedAtlasInitPass creates the transient seed atlas handle used by voxelize and Eikonal passes.
    AddSeedAtlasInitPass(Context);

    uint32_t ModelIndex = 0;
    std::vector<FSceneModelResource>& SceneModels = Context.Owner.GetSceneModelsMutable();
    for (FSceneModelResource& Model : SceneModels)
    {
        if (Model.IsStaticRegularMeshCandidate())
        {
            AddModelVoxelizePass(Context, Model, ModelIndex);
        }
        ++ModelIndex;
    }

    AddEikonalPass(Context);

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
    const FRGResourceHandle DiffuseHandle = Context.Resources.SparseSdfGI.DiffuseGIHandle;
    ID3D12PipelineState* Pipeline = (DebugMode == ESparseSdfGIDebugMode::Off) ? DiffuseTracePipeline.Get() : DebugTracePipeline.Get();

    struct FOutputPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FOutputPassData>("SparseSdfGI Trace", [&, DepthHandle, SdfAtlasHandle, BrickMapHandle, DiffuseHandle, GBufferHandles, Pipeline](FOutputPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid && Pipeline != nullptr;
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
    RootParams[2].InitAsConstants(kSparseSdfGIBindlessDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_ALL);

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
    std::vector<uint8_t> SeedAtlasInitByteCode;
    std::vector<uint8_t> VoxelizeByteCode;
    std::vector<uint8_t> EikonalByteCode;
    std::vector<uint8_t> DebugTraceByteCode;
    std::vector<uint8_t> DiffuseTraceByteCode;

    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/SparseSdfGI/SparseSdfGI.hlsl", L"CSSeedAtlasInit", SeedAtlasInitByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/SparseSdfGI/SparseSdfGI.hlsl", L"CSVoxelizeStaticMesh", VoxelizeByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/SparseSdfGI/SparseSdfGI.hlsl", L"CSEikonalBrickLocal", EikonalByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/SparseSdfGI/SparseSdfGI.hlsl", L"CSDebugTrace", DebugTraceByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/SparseSdfGI/SparseSdfGI.hlsl", L"CSDiffuseTrace", DiffuseTraceByteCode))
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

    return CreateComputePso(SeedAtlasInitByteCode, SeedAtlasInitPipeline, "CSSeedAtlasInit")
        && CreateComputePso(VoxelizeByteCode, VoxelizePipeline, "CSVoxelizeStaticMesh")
        && CreateComputePso(EikonalByteCode, EikonalPipeline, "CSEikonalBrickLocal")
        && CreateComputePso(DebugTraceByteCode, DebugTracePipeline, "CSDebugTrace")
        && CreateComputePso(DiffuseTraceByteCode, DiffuseTracePipeline, "CSDiffuseTrace");
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
        SeedAtlasInitPipeline &&
        VoxelizePipeline &&
        EikonalPipeline &&
        DebugTracePipeline &&
        DiffuseTracePipeline &&
        SdfAtlas.IsFullyBound() &&
        CascadeBrickMap.IsFullyBound() &&
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
    HashValue(Hash, MaxTriangleVoxelSpan);
    HashFloat3(Hash, Bounds.Min);
    HashFloat3(Hash, Bounds.Extent);
    HashValue(Hash, Bounds.VoxelSize);
    return Hash;
}

uint64_t FSparseSdfGI::ComputeStaticSceneSignature(const FDeferredRenderer& Owner, uint32_t& OutStaticCandidateCount) const
{
    uint64_t Hash = kSparseSdfGIHashOffsetBasis;
    OutStaticCandidateCount = 0;

    const std::vector<FSceneModelResource>* Models = Owner.GetSceneModels();
    const uint64_t TotalModelCount = Models ? static_cast<uint64_t>(Models->size()) : 0ull;
    HashValue(Hash, TotalModelCount);
    if (!Models)
    {
        return Hash;
    }

    for (uint64_t ModelIndex = 0; ModelIndex < TotalModelCount; ++ModelIndex)
    {
        const FSceneModelResource& Model = (*Models)[static_cast<size_t>(ModelIndex)];
        const bool bCandidate = Model.IsStaticRegularMeshCandidate();
        HashValue(Hash, ModelIndex);
        HashValue(Hash, bCandidate);
        if (!bCandidate)
        {
            continue;
        }

        ++OutStaticCandidateCount;
        HashValue(Hash, Model.DrawIndexStart);
        HashValue(Hash, Model.DrawIndexCount);
        HashValue(Hash, Model.Geometry.IndexCount);
        HashValue(Hash, Model.Geometry.VertexBuffers[0].SrvBindlessIndex);
        HashValue(Hash, Model.Geometry.IndexBuffer.SrvBindlessIndex);
        HashFloat4x4(Hash, Model.WorldMatrix);
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

void FSparseSdfGI::AddSeedAtlasInitPass(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGBufferHandle BrickMapHandle = Context.Resources.SparseSdfGI.CascadeBrickMapHandle;

    struct FSeedAtlasInitPassData
    {
        bool bEnabled = false;
        FRGResourceHandle SeedDistanceHandle{};
    };

    Graph.AddPass<FSeedAtlasInitPassData>("SparseSdfGI Seed Atlas Init", [&, BrickMapHandle](FSeedAtlasInitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid && SeedAtlasInitPipeline != nullptr;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.SeedDistanceHandle = Builder.CreateTexture("SparseSdfGI Seed Distance Atlas", CreateSeedDistanceAtlasDesc());
        Context.Resources.SparseSdfGI.SdfSeedDistanceHandle = Data.SeedDistanceHandle;
        Builder.WriteTexture(Data.SeedDistanceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(BrickMapHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Context](const FSeedAtlasInitPassData& Data, FDX12CommandContext& Cmd)
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
        CommandList->SetPipelineState(SeedAtlasInitPipeline.Get());
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
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.MaxTraceDistance = Bounds.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
        Constants.CascadeMin = Bounds.Min;
        Constants.CascadeExtent = Bounds.Extent;
        static_assert(sizeof(FSparseSdfGIConstants) / sizeof(uint32_t) <= kSparseSdfGIConstantsDwordCount);
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t SeedDistanceUavIndex = Context.Graph.GetTextureUavBindlessIndex(Data.SeedDistanceHandle);
        if (!IsValidBindlessIndex(SeedDistanceUavIndex))
        {
            return;
        }

        const FSparseSdfGIBindlessConstants Bindless =
        {
            SdfAtlas.SrvBindlessIndex,
            SdfAtlas.UavBindlessIndex,
            UINT32_MAX,
            SeedDistanceUavIndex,
            CascadeBrickMap.SrvBindlessIndex,
            CascadeBrickMap.UavBindlessIndex,
            DiffuseGI.UavBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            Owner.Device->GetLinearClampSamplerIndex()
        };
        static_assert(sizeof(FSparseSdfGIBindlessConstants) / sizeof(uint32_t) == kSparseSdfGIBindlessDwordCount);
        CommandList->SetComputeRoot32BitConstants(2, kSparseSdfGIBindlessDwordCount, &Bindless, 0);

        CommandList->Dispatch(
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D),
            AlignDispatch(kSparseSdfGIAtlasResolution, kSparseSdfGIGroupSize3D));
    });
}

void FSparseSdfGI::AddModelVoxelizePass(FDeferredPassContext& Context, FSceneModelResource& Model, uint32_t ModelIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FBindlessBuffer& PositionBuffer = Model.Geometry.VertexBuffers[0];
    FBindlessBuffer& IndexBuffer = Model.Geometry.IndexBuffer;
    const FRGBufferHandle PositionHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Position", PositionBuffer);
    const FRGBufferHandle IndexHandle = ImportBindlessBuffer(Graph, "SparseSdfGI Index", IndexBuffer);
    const FRGResourceHandle SeedDistanceHandle = Context.Resources.SparseSdfGI.SdfSeedDistanceHandle;

    const float ModelScale = MatrixMath::ComputeMaxScale(Model.WorldMatrix);
    const FCascadeBounds Bounds = ComputeCascadeBounds(Owner);

    struct FVoxelizePassData
    {
        bool bEnabled = false;
        uint32_t TriangleCount = 0;
        uint32_t DrawIndexStart = 0;
        uint32_t DrawIndexCount = 0;
        float VoxelSize = 0.0f;
        DirectX::XMFLOAT3 CascadeMin{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 CascadeExtent{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4X4 World{};
        uint32_t PositionBufferIndex = UINT32_MAX;
        uint32_t IndexBufferIndex = UINT32_MAX;
        FRGResourceHandle SeedDistanceHandle{};
    };

    const std::string PassName = "SparseSdfGI Voxelize Model " + std::to_string(ModelIndex);
    Graph.AddPass<FVoxelizePassData>(PassName, [&, PositionHandle, IndexHandle, SeedDistanceHandle, ModelScale, Bounds](FVoxelizePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.bEnabled = bEnabled && bPersistentInputsValid && VoxelizePipeline != nullptr;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.SeedDistanceHandle = SeedDistanceHandle;
        Data.TriangleCount = Model.DrawIndexCount / 3u;
        Data.DrawIndexStart = Model.DrawIndexStart;
        Data.DrawIndexCount = Model.DrawIndexCount;
        Data.VoxelSize = Bounds.VoxelSize;
        Data.CascadeMin = Bounds.Min;
        Data.CascadeExtent = Bounds.Extent;
        Data.World = Model.WorldMatrix;
        Data.PositionBufferIndex = Model.Geometry.VertexBuffers[0].SrvBindlessIndex;
        Data.IndexBufferIndex = Model.Geometry.IndexBuffer.SrvBindlessIndex;
        Data.bEnabled = Data.bEnabled
            && Data.TriangleCount > 0u
            && ModelScale > 0.0f
            && static_cast<bool>(Data.SeedDistanceHandle)
            && AreAllBindlessIndicesValid(Data.PositionBufferIndex, Data.IndexBufferIndex);

        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadBuffer(PositionHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(IndexHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SeedDistanceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(SeedDistanceHandle);
    }, [this, &Context](const FVoxelizePassData& Data, FDX12CommandContext& Cmd)
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
        CommandList->SetPipelineState(VoxelizePipeline.Get());
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
        Constants.MaxTriangleVoxelSpan = MaxTriangleVoxelSpan;
        Constants.BaseVoxelSize = Data.VoxelSize;
        Constants.CascadeScale = CascadeScale;
        Constants.Intensity = Intensity;
        Constants.MaxTraceDistance = Data.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
        Constants.CascadeMin = Data.CascadeMin;
        Constants.VoxelSize = Data.VoxelSize;
        Constants.CascadeExtent = Data.CascadeExtent;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        Constants.World = Data.World;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t SeedDistanceUavIndex = Context.Graph.GetTextureUavBindlessIndex(Data.SeedDistanceHandle);
        if (!IsValidBindlessIndex(SeedDistanceUavIndex))
        {
            return;
        }

        const FSparseSdfGIBindlessConstants Bindless =
        {
            SdfAtlas.SrvBindlessIndex,
            SdfAtlas.UavBindlessIndex,
            UINT32_MAX,
            SeedDistanceUavIndex,
            CascadeBrickMap.SrvBindlessIndex,
            CascadeBrickMap.UavBindlessIndex,
            DiffuseGI.UavBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            Data.PositionBufferIndex,
            Data.IndexBufferIndex,
            Owner.Device->GetLinearClampSamplerIndex()
        };
        CommandList->SetComputeRoot32BitConstants(2, kSparseSdfGIBindlessDwordCount, &Bindless, 0);
        CommandList->Dispatch(AlignDispatch(Data.TriangleCount, kSparseSdfGIVoxelizeGroupSize), 1u, 1u);
    });
}

void FSparseSdfGI::AddEikonalPass(FDeferredPassContext& Context) const
{
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle SdfAtlasHandle = Context.Resources.SparseSdfGI.SdfAtlasHandle;
    const FRGResourceHandle SeedDistanceHandle = Context.Resources.SparseSdfGI.SdfSeedDistanceHandle;

    struct FEikonalPassData
    {
        bool bEnabled = false;
        FRGResourceHandle SeedDistanceHandle{};
    };

    Graph.AddPass<FEikonalPassData>("SparseSdfGI Brick Eikonal", [&, SdfAtlasHandle, SeedDistanceHandle](FEikonalPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SparseSdfGI");
        Data.SeedDistanceHandle = SeedDistanceHandle;
        Data.bEnabled = bEnabled && bPersistentInputsValid && EikonalPipeline != nullptr && static_cast<bool>(Data.SeedDistanceHandle);
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(SeedDistanceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SdfAtlasHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.UavBarrier(SdfAtlasHandle);
    }, [this, &Context](const FEikonalPassData& Data, FDX12CommandContext& Cmd)
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
        CommandList->SetPipelineState(EikonalPipeline.Get());
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
        Constants.MaxTraceDistance = Bounds.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
        Constants.CascadeMin = Bounds.Min;
        Constants.VoxelSize = Bounds.VoxelSize;
        Constants.CascadeExtent = Bounds.Extent;
        Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
        CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t SeedDistanceSrvIndex = Context.Graph.GetTextureSrvBindlessIndex(Data.SeedDistanceHandle);
        if (!IsValidBindlessIndex(SeedDistanceSrvIndex))
        {
            return;
        }

        const FSparseSdfGIBindlessConstants Bindless =
        {
            SdfAtlas.SrvBindlessIndex,
            SdfAtlas.UavBindlessIndex,
            SeedDistanceSrvIndex,
            UINT32_MAX,
            CascadeBrickMap.SrvBindlessIndex,
            CascadeBrickMap.UavBindlessIndex,
            DiffuseGI.UavBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            Owner.Device->GetLinearClampSamplerIndex()
        };
        CommandList->SetComputeRoot32BitConstants(2, kSparseSdfGIBindlessDwordCount, &Bindless, 0);
        CommandList->Dispatch(
            kSparseSdfGIBrickGridResolution,
            kSparseSdfGIBrickGridResolution,
            kSparseSdfGIBrickGridResolution);
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
    const bool bInputsValid = AreAllBindlessIndicesValid(
        DepthBindlessIndex,
        Owner.GBufferA.SrvBindlessIndex,
        Owner.GBufferB.SrvBindlessIndex,
        Owner.GBufferC.SrvBindlessIndex,
        SdfAtlas.SrvBindlessIndex,
        CascadeBrickMap.SrvBindlessIndex,
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
    Constants.MaxTraceDistance = Bounds.VoxelSize * static_cast<float>(kSparseSdfGIAtlasResolution);
    Constants.CascadeMin = Bounds.Min;
    Constants.VoxelSize = Bounds.VoxelSize;
    Constants.CascadeExtent = Bounds.Extent;
    Constants.SurfaceThicknessVoxels = kSparseSdfGISurfaceThicknessVoxels;
    CommandList->SetComputeRoot32BitConstants(1, sizeof(FSparseSdfGIConstants) / sizeof(uint32_t), &Constants, 0);

    const FSparseSdfGIBindlessConstants Bindless =
    {
        SdfAtlas.SrvBindlessIndex,
        SdfAtlas.UavBindlessIndex,
        UINT32_MAX,
        UINT32_MAX,
        CascadeBrickMap.SrvBindlessIndex,
        CascadeBrickMap.UavBindlessIndex,
        DiffuseGI.UavBindlessIndex,
        DepthBindlessIndex,
        Owner.GBufferA.SrvBindlessIndex,
        Owner.GBufferB.SrvBindlessIndex,
        Owner.GBufferC.SrvBindlessIndex,
        UINT32_MAX,
        UINT32_MAX,
        Owner.Device->GetLinearClampSamplerIndex()
    };
    CommandList->SetComputeRoot32BitConstants(2, kSparseSdfGIBindlessDwordCount, &Bindless, 0);
    CommandList->Dispatch(
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Width), kSparseSdfGIGroupSize2D),
        AlignDispatch(static_cast<uint32_t>(Owner.Viewport.Height), kSparseSdfGIGroupSize2D),
        1u);
}
