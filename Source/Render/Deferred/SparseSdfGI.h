#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>

#include "../GpuResource.h"
#include "../RenderGraph.h"
#include "../../Core/RendererConfig.h"
#include "../../../Shaders/SparseSdfGI/SparseSdfGIShared.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12CommandContext;
class FDX12Device;
struct FMeshSection;
class FObject;

enum class ESparseSdfGIDebugMode : uint32_t
{
    Off = SPARSE_SDF_GI_DEBUG_MODE_OFF,
    RayTrace = SPARSE_SDF_GI_DEBUG_MODE_RAY_TRACE,
    CascadeSlice = SPARSE_SDF_GI_DEBUG_MODE_CASCADE_SLICE,
    Gradient = SPARSE_SDF_GI_DEBUG_MODE_GRADIENT,
    StepCount = SPARSE_SDF_GI_DEBUG_MODE_STEP_COUNT,
    SharedSampleMismatch = SPARSE_SDF_GI_DEBUG_MODE_SHARED_SAMPLE_MISMATCH,
    BrickLocalGradient = SPARSE_SDF_GI_DEBUG_MODE_BRICK_LOCAL_GRADIENT,
    HitUVW = SPARSE_SDF_GI_DEBUG_MODE_HIT_UVW,
    BrickID = SPARSE_SDF_GI_DEBUG_MODE_BRICK_ID,
    BrickLocalGradientRounded = SPARSE_SDF_GI_DEBUG_MODE_BRICK_LOCAL_GRADIENT_ROUNDED
};

struct FSparseSdfGIFrameResources
{
    FRGResourceHandle SdfAtlasHandle{};
    FRGBufferHandle CascadeBrickMapHandle{};
    FRGBufferHandle BrickMetadataHandle{};
    FRGBufferHandle ReferenceCounterStatsHandle{};
    FRGBufferHandle CascadeDataHandle{};
    FRGBufferHandle TraceHierarchyBottomHandle{};
    FRGBufferHandle TraceHierarchyTopHandle{};
    FRGBufferHandle ScatterJobsHandle{};
    FRGBufferHandle ScatterJobOffsetsHandle{};
    FRGBufferHandle ScatterJobGroupSumsHandle{};
    FRGBufferHandle ScatterJobGroupOffsetsHandle{};
    FRGBufferHandle ScatterJobGroup2SumsHandle{};
    FRGBufferHandle ScatterJobGroup2OffsetsHandle{};
    FRGBufferHandle ScatterTouchedBricksHandle{};
    FRGBufferHandle ScatterBrickListHandle{};
    FRGBufferHandle ScatterBrickSdfHandle{};
    FRGBufferHandle ScatterCountersHandle{};
    FRGBufferHandle ScatterSampleDispatchArgsHandle{};
    FRGBufferHandle ScatterBrickDispatchArgsHandle{};
    FRGBufferHandle BrickRadianceReadHandle{};
    FRGBufferHandle BrickRadianceWriteHandle{};
    FRGBufferHandle BrickRadianceAccumHandle{};
    FRGBufferHandle BrickRadianceResolvedHandle{};
    FRGBufferHandle BrickIrradianceReadHandle{};
    FRGBufferHandle BrickIrradianceWriteHandle{};
    FRGBufferHandle BrickIrradianceAccumHandle{};
    FRGBufferHandle BrickIrradianceResolvedHandle{};
    FRGBufferHandle ProbeHistoryReadHandle{};
    FRGBufferHandle ProbeHistoryWriteHandle{};
    FRGResourceHandle DiffuseGIHandle{};
    FRGResourceHandle DiffuseGIInputSHHandle{};
    FRGResourceHandle DiffuseGIVarianceHandle{};
};

class FSparseSdfGI
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void ImportPersistentResources(FDeferredPassContext& Context);
    void AddSdfUpdatePasses(FDeferredPassContext& Context) const;
    void AddDiffuseGITracePasses(FDeferredPassContext& Context) const;
    void ApplyConfig(const FRendererConfig& Config);
    void ForceInvalidateCache() const;
    void OnFrameFenceSignaled(uint32_t FrameIndex);

    bool IsEnabled() const { return bEnabled; }
    bool IsReady() const { return bPersistentInputsValid; }
    bool IsDebugVisualizationActive() const { return DebugMode != ESparseSdfGIDebugMode::Off; }
    float GetIntensity() const { return Intensity; }
    float GetEffectiveVoxelSize() const { return CachedEffectiveVoxelSize; }
    float GetEffectiveCascadeExtent() const { return CachedCascadeBounds.Extent.x; }
    uint32_t GetCurrentOutputSrvBindlessIndex() const { return bPersistentInputsValid ? DiffuseGI.SrvBindlessIndex : UINT32_MAX; }

private:
    struct FCascadeBounds
    {
        DirectX::XMFLOAT3 Min{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 Extent{ 1.0f, 1.0f, 1.0f };
        float VoxelSize = 0.25f;
    };

    struct FCascadeDataGpu
    {
        DirectX::XMFLOAT4 MinVoxelSize{};
        DirectX::XMFLOAT4 Extent{};
        DirectX::XMUINT4 Offsets{};
    };

    bool CreateRootSignature(FDX12Device* Device);
    bool CreateDispatchCommandSignature(FDX12Device* Device);
    bool CreatePipelines(FDX12Device* Device);
    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FramesInFlight);
    bool RefreshPersistentInputValidation();
    FCascadeBounds ComputeBaseCascadeBounds(const FDeferredRenderer& Owner) const;
    FCascadeBounds ComputeCascadeBounds(const FCascadeBounds& BaseBounds, uint32_t CascadeIndex) const;
    void UpdateCascadeData(const FCascadeBounds& BaseBounds) const;
    uint64_t ComputeBuildSettingsSignature(const FCascadeBounds& Bounds) const;
    uint64_t ComputeStaticSceneSignature(const FDeferredRenderer& Owner, uint32_t& OutStaticCandidateCount) const;
    void InvalidateCache() const;
    void AddDistributedScatterInitPass(FDeferredPassContext& Context, uint32_t MaxStaticTriangleCount, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const;
    void AddSectionDistributedScatterPreparePass(FDeferredPassContext& Context, const FObject& Object, FMeshSection& Section, uint32_t DrawSectionIndex, uint32_t MaxStaticTriangleCount, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const;
    void AddDistributedScatterPasses(FDeferredPassContext& Context, uint32_t MaxStaticTriangleCount, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const;
    void AddBuildTraceHierarchyPasses(FDeferredPassContext& Context, const FCascadeBounds& Bounds, uint32_t CascadeIndex) const;
    void AddReferenceStatsPresentPass(FDeferredPassContext& Context) const;
    void AddRadianceCachePasses(FDeferredPassContext& Context) const;
    void AddIrradianceCacheUpdatePasses(FDeferredPassContext& Context) const;
    void AddBrickShPropagatePass(FDeferredPassContext& Context, const char* PassName, FRGBufferHandle SourceHandle, FRGBufferHandle DestHandle, FRGBufferHandle BrickMapHandle) const;
    void AddScreenProbeGITracePasses(FDeferredPassContext& Context, FRGBufferHandle BrickRadianceHandle) const;
    void DispatchOutputPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, bool bPassEnabled, FRGBufferHandle BrickRadianceHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle) const;
    bool BindSparseConstants(FDeferredRenderer& Owner, ID3D12GraphicsCommandList* CommandList, const void* Constants, size_t ConstantsSize) const;
    void ResetSparseConstantCursor(uint32_t FrameIndex) const;

private:
    bool bEnabled = false;
    ESparseSdfGIDebugMode DebugMode = ESparseSdfGIDebugMode::RayTrace;
    uint32_t CascadeCount = 1;
    float BaseVoxelSize = 0.0f;
    mutable float CachedEffectiveVoxelSize = 0.0f;
    float CascadeScale = 2.0f;
    bool bTraceHalfResolution = false;
    bool bUseHierarchicalTrace = true;
    bool bEikonalEnabled = true;
    float Intensity = 1.0f;
    float BounceStrength = 1.0f;
    bool bEnableRadianceTemporalReuse = true;
    bool bUseScreenProbes = false;
    uint32_t ProbeTileSize = 8;
    uint32_t ProbeRaysPerProbe = 16;
    uint32_t ProbeDebugMode = 0;
    bool bProbeTemporalReuse = false;
    bool bProbeDirectionalSH = false;
    bool bProbeSpawnJitter = false;
    bool bProbeMotionReproject = true;
    bool bMultiBounce = false;
    float MultiBounceStrength = 1.0f;
    float SurfaceHitThresholdVoxels = 0.75f;
    uint32_t MaxBrickTriangleReferences = 8u * 1024u * 1024u;
    uint32_t MaxScatterBricks = 64u * 1024u;
    uint32_t EffectiveMaxScatterBricks = 64u * 1024u;
    uint32_t SdfAtlasFormat = 0;
    bool bPersistentInputsValid = false;
    mutable bool bSdfCacheValid = false;
    mutable uint64_t CachedSceneSignature = 0;
    mutable uint64_t CachedBuildSettingsSignature = 0;
    mutable FCascadeBounds CachedCascadeBounds{};
    mutable uint32_t CachedStaticCandidateCount = 0;
    mutable uint32_t CurrentBrickRadianceReadSlot = 0;
    mutable uint32_t CurrentBrickRadianceWriteSlot = 0;
    mutable std::vector<bool> BrickRadianceHistoryValid;
    mutable std::vector<bool> PendingBrickRadianceWrite;
    mutable uint32_t CurrentBrickIrradianceReadSlot = 0;
    mutable uint32_t CurrentBrickIrradianceWriteSlot = 0;
    mutable std::vector<bool> BrickIrradianceHistoryValid;
    mutable std::vector<bool> PendingBrickIrradianceWrite;
    mutable uint32_t CurrentProbeHistoryReadSlot = 0;
    mutable uint32_t CurrentProbeHistoryWriteSlot = 0;
    mutable std::vector<bool> ProbeHistoryValid;
    mutable std::vector<bool> PendingProbeHistoryWrite;
    uint32_t ProbeHistoryCapacity = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> DispatchCommandSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterInitPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterPreparePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterScanJobsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterScanGroupsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterScanGroup2Pipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterAddOffsetsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterBuildSampleArgsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterMarkTouchedPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterAllocateBricksPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterBuildBrickArgsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterClearBrickStoragePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterSdfSamplesPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterEikonalPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ScatterFinalizeBricksPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildTraceHierarchyBottomPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildTraceHierarchyTopPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceStatsPresentPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceClearPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceInjectPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceResolvePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BrickShPropagatePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> IrradianceAccumulatePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbeSpawnPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbeTracePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbeTraceDirectionalPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbeInterpolatePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DebugTracePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DiffuseTracePipeline;

    FBindlessTexture SdfAtlas;
    FBindlessBuffer CascadeBrickMap;
    FBindlessBuffer BrickMetadata;
    std::vector<FBindlessBuffer> CascadeDataBuffers;
    std::vector<void*> CascadeDataMapped;
    mutable uint32_t CurrentCascadeDataSlot = 0;
    mutable uint32_t CurrentCascadeDataSrvIndex = UINT32_MAX;
    FBindlessBuffer TraceHierarchyBottom;
    FBindlessBuffer TraceHierarchyTop;
    FBindlessBuffer ReferenceCounterStats;
    mutable std::vector<FMappedUploadBuffer> SparseConstantBuffers;
    mutable std::vector<uint32_t> SparseConstantCursors;
    std::vector<FBindlessBuffer> BrickRadiance;
    std::vector<FBindlessBuffer> BrickIrradiance;
    std::vector<FBindlessBuffer> ProbeHistory;
    FBindlessTexture DiffuseGI;
};
