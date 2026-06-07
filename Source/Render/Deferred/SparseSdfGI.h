#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>

#include "../GpuResource.h"
#include "../RenderGraph.h"
#include "../../Core/RendererConfig.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12CommandContext;
class FDX12Device;
struct FMeshSection;
class FObject;

enum class ESparseSdfGIDebugMode : uint32_t
{
    Off = 0,
    RayTrace = 1,
    CascadeSlice = 2,
    Gradient = 3,
    StepCount = 4,
    SharedSampleMismatch = 5,
    BrickLocalGradient = 6,
    HitUVW = 7,
    BrickID = 8
};

enum class ESparseSdfGISdfBuildMode : uint32_t
{
    LegacyEikonal = 0,
    ExactSharedBorder = 1
};

struct FSparseSdfGIFrameResources
{
    FRGResourceHandle SdfAtlasHandle{};
    FRGBufferHandle CascadeBrickMapHandle{};
    FRGBufferHandle BrickMetadataHandle{};
    FRGBufferHandle TrianglePoolHandle{};
    FRGBufferHandle BrickReferenceHeadsHandle{};
    FRGBufferHandle BrickReferencesHandle{};
    FRGBufferHandle ReferenceCountersHandle{};
    FRGBufferHandle OccupiedBrickListHandle{};
    FRGBufferHandle ReferenceCounterStatsHandle{};
    FRGBufferHandle TraceHierarchyBottomHandle{};
    FRGBufferHandle TraceHierarchyTopHandle{};
    FRGBufferHandle BrickRadianceReadHandle{};
    FRGBufferHandle BrickRadianceWriteHandle{};
    FRGBufferHandle BrickRadianceAccumHandle{};
    FRGBufferHandle BrickIrradianceReadHandle{};
    FRGBufferHandle BrickIrradianceWriteHandle{};
    FRGBufferHandle BrickIrradianceAccumHandle{};
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
    ESparseSdfGISdfBuildMode GetSdfBuildMode() const { return SdfBuildMode; }
    uint32_t GetCurrentOutputSrvBindlessIndex() const { return bPersistentInputsValid ? DiffuseGI.SrvBindlessIndex : UINT32_MAX; }

private:
    struct FCascadeBounds
    {
        DirectX::XMFLOAT3 Min{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 Extent{ 1.0f, 1.0f, 1.0f };
        float VoxelSize = 0.25f;
    };

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipelines(FDX12Device* Device);
    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FramesInFlight);
    bool RefreshPersistentInputValidation();
    FCascadeBounds ComputeCascadeBounds(const FDeferredRenderer& Owner) const;
    uint64_t ComputeBuildSettingsSignature(const FCascadeBounds& Bounds) const;
    uint64_t ComputeStaticSceneSignature(const FDeferredRenderer& Owner, uint32_t& OutStaticCandidateCount) const;
    void InvalidateCache() const;
    void AddReferenceBuildInitPass(FDeferredPassContext& Context) const;
    void AddSectionReferenceEmitPass(FDeferredPassContext& Context, const FObject& Object, FMeshSection& Section, uint32_t DrawSectionIndex) const;
    void AddSolveBrickReferencesPass(FDeferredPassContext& Context) const;
    void AddBuildTraceHierarchyPasses(FDeferredPassContext& Context) const;
    void AddReferenceStatsPresentPass(FDeferredPassContext& Context) const;
    void AddRadianceCachePasses(FDeferredPassContext& Context) const;
    void AddIrradianceCacheUpdatePasses(FDeferredPassContext& Context) const;
    void AddScreenProbeGITracePasses(FDeferredPassContext& Context, FRGBufferHandle BrickRadianceHandle) const;
    void DispatchOutputPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, bool bPassEnabled, FRGBufferHandle BrickRadianceHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle) const;
    bool BindSparseConstants(FDeferredRenderer& Owner, ID3D12GraphicsCommandList* CommandList, const void* Constants, size_t ConstantsSize) const;
    void ResetSparseConstantCursor(uint32_t FrameIndex) const;

private:
    bool bEnabled = false;
    ESparseSdfGIDebugMode DebugMode = ESparseSdfGIDebugMode::RayTrace;
    ESparseSdfGISdfBuildMode SdfBuildMode = ESparseSdfGISdfBuildMode::LegacyEikonal;
    uint32_t CascadeCount = 1;
    float BaseVoxelSize = 0.0f;
    mutable float CachedEffectiveVoxelSize = 0.0f;
    float CascadeScale = 2.0f;
    bool bTraceHalfResolution = false;
    bool bUseHierarchicalTrace = true;
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
    uint32_t DebugSolveGroupBudget = 0xFFFFFFFFu;
    uint32_t DebugEmitTriangleBudget = 0xFFFFFFFFu;
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
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceBuildInitPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceEmitPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SolveBrickReferencesPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ExactSolveBrickReferencesPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildTraceHierarchyBottomPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BuildTraceHierarchyTopPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceStatsPresentPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceClearPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceInjectPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceResolvePipeline;
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
