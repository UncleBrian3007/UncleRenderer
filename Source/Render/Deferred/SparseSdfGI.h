#pragma once

#include <cstdint>
#include <d3d12.h>
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
    FRGBufferHandle BrickRadianceHandle{};
    FRGBufferHandle BrickRadianceHistoryHandle{};
    FRGBufferHandle BrickRadianceAccumHandle{};
    FRGResourceHandle DiffuseGIHandle{};
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
    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool RefreshPersistentInputValidation();
    FCascadeBounds ComputeCascadeBounds(const FDeferredRenderer& Owner) const;
    uint64_t ComputeBuildSettingsSignature(const FCascadeBounds& Bounds) const;
    uint64_t ComputeStaticSceneSignature(const FDeferredRenderer& Owner, uint32_t& OutStaticCandidateCount) const;
    void InvalidateCache() const;
    void AddReferenceBuildInitPass(FDeferredPassContext& Context) const;
    void AddSectionReferenceEmitPass(FDeferredPassContext& Context, const FObject& Object, FMeshSection& Section, uint32_t DrawSectionIndex) const;
    void AddSolveBrickReferencesPass(FDeferredPassContext& Context) const;
    void AddRadianceCachePasses(FDeferredPassContext& Context) const;
    void DispatchOutputPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, bool bPassEnabled) const;

private:
    bool bEnabled = false;
    ESparseSdfGIDebugMode DebugMode = ESparseSdfGIDebugMode::RayTrace;
    uint32_t CascadeCount = 1;
    float BaseVoxelSize = 0.0f;
    float CascadeScale = 2.0f;
    bool bTraceHalfResolution = false;
    float Intensity = 1.0f;
    float BounceStrength = 1.0f;
    bool bUseHitLightingVisibility = false;
    bool bEnableRadianceTemporalReuse = true;
    uint32_t MaxBrickTriangleReferences = 8u * 1024u * 1024u;
    uint32_t DebugSolveGroupBudget = 0xFFFFFFFFu;
    uint32_t DebugEmitTriangleBudget = 0xFFFFFFFFu;
    bool bPersistentInputsValid = false;
    mutable bool bSdfCacheValid = false;
    mutable uint64_t CachedSceneSignature = 0;
    mutable uint64_t CachedBuildSettingsSignature = 0;
    mutable FCascadeBounds CachedCascadeBounds{};
    mutable uint32_t CachedStaticCandidateCount = 0;
    mutable bool bBrickRadianceHistoryValid = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceBuildInitPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ReferenceEmitPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SolveBrickReferencesPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceClearPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceInjectPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceResolvePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RadianceCopyHistoryPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DebugTracePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DiffuseTracePipeline;

    FBindlessTexture SdfAtlas;
    FBindlessBuffer CascadeBrickMap;
    FBindlessBuffer BrickMetadata;
    FBindlessBuffer BrickRadiance;
    FBindlessBuffer BrickRadianceHistory;
    FBindlessTexture DiffuseGI;
};
