#pragma once

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
    VoxelProjection = 3,
    BrickLocalSdfSurface = 4,
    StepCount = 5
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
    FRGBufferHandle BrickRadianceReadHandle{};
    FRGBufferHandle BrickRadianceWriteHandle{};
    FRGBufferHandle BrickRadianceAccumHandle{};
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
    float GetIntensity() const { return Intensity; }
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
    void AddRadianceCachePasses(FDeferredPassContext& Context) const;
    void AddScreenProbeGITracePasses(FDeferredPassContext& Context, FRGBufferHandle BrickRadianceHandle) const;
    void DispatchOutputPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, bool bPassEnabled, FRGBufferHandle BrickRadianceHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle) const;

private:
    bool bEnabled = false;
    ESparseSdfGIDebugMode DebugMode = ESparseSdfGIDebugMode::RayTrace;
    uint32_t CascadeCount = 1;
    float BaseVoxelSize = 0.0f;
    float CascadeScale = 2.0f;
    bool bTraceHalfResolution = false;
    float Intensity = 1.0f;
    float BounceStrength = 1.0f;
    bool bEnableRadianceTemporalReuse = true;
    bool bUseScreenProbes = false;
    uint32_t ProbeTileSize = 8;
    uint32_t ProbeRaysPerProbe = 16;
    uint32_t ProbeDebugMode = 0;
    bool bProbeTemporalReuse = false;
    bool bProbeSpawnJitter = false;
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
    mutable uint32_t CurrentProbeHistoryReadSlot = 0;
    mutable uint32_t CurrentProbeHistoryWriteSlot = 0;
    mutable std::vector<bool> ProbeHistoryValid;
    mutable std::vector<bool> PendingProbeHistoryWrite;
    uint32_t ProbeHistoryCapacity = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceBuildInitPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceEmitPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SolveBrickReferencesPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceClearPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceInjectPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceResolvePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbeSpawnPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbeTracePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ProbeInterpolatePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DebugTracePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DiffuseTracePipeline;

    FBindlessTexture SdfAtlas;
    FBindlessBuffer CascadeBrickMap;
    FBindlessBuffer BrickMetadata;
    std::vector<FBindlessBuffer> BrickRadiance;
    std::vector<FBindlessBuffer> ProbeHistory;
    FBindlessTexture DiffuseGI;
};
