#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>

#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12CommandContext;
class FDX12Device;

struct FPathTracingFrameResources
{
    FRGResourceHandle TempHandle{};
    std::vector<FRGResourceHandle> AccumulationHandles{};
};

class FPathTracing
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPasses(FDeferredPassContext& Context);
    void PrepareFrameState(uint32_t FrameIndex, bool bCameraMoved, bool& bAccumulationActive, bool& bHistoryReady, uint32_t& ReadIndex, uint32_t& WriteIndex);
    void OnFrameFenceSignaled(uint32_t FrameIndex);
    void ResetAccumulation();

    void SetAccumulationEnabled(bool bEnabled);
    bool IsAccumulationEnabled() const { return bPathTracingAccumulationEnabled; }

    void SetMaxBounces(uint32_t MaxBounces) { PathTracingMaxBounces = MaxBounces; }
    uint32_t GetMaxBounces() const { return PathTracingMaxBounces; }

    void SetDebugMode(int Mode);
    int GetDebugMode() const { return PathTracingDebugMode; }

    uint32_t GetAccumulatedFrames() const { return PathTracingAccumulatedFrames; }

private:
    friend class FDeferredRenderer;

    bool CreateAccumulationRootSignature(FDX12Device* Device);
    bool CreateAccumulationPipeline(FDX12Device* Device);
    bool CreateAccumulationResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    void AddPathTracingPass(FDeferredPassContext& Context);
    void AddPathTracingAccumulationPass(FDeferredPassContext& Context);

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PathTracingAccumulationPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> PathTracingAccumulationRootSignature;

    Microsoft::WRL::ComPtr<ID3D12Resource> PathTracingTempTexture;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> PathTracingAccumulationTextures;

    uint32_t PathTracingTempBindlessIndex = UINT32_MAX;
    std::vector<uint32_t> PathTracingAccumulationSrvBindlessIndices;
    std::vector<uint32_t> PathTracingAccumulationUavBindlessIndices;

    D3D12_RESOURCE_STATES PathTracingTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    std::vector<D3D12_RESOURCE_STATES> PathTracingAccumulationStates;

    bool bPathTracingAccumulationEnabled = false;
    bool bPathTracingAccumulationUserPreference = false;
    uint32_t PathTracingAccumulationFrameCount = 0;
    std::vector<bool> PathTracingAccumulationHistoryValid;
    uint32_t PathTracingAccumulatedFrames = 0;
    uint32_t PathTracingMaxBounces = 8;
    int PathTracingDebugMode = 0;
};
