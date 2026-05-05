#pragma once

#include <cstdint>
#include <vector>
#include <wrl.h>

#include "../GpuResource.h"

class FDeferredRenderer;
struct FDeferredPassContext;
struct FRendererConfig;
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

    void ApplyConfig(const FRendererConfig& Config);
    bool IsPreferred() const { return bEnabled; }
    bool IsVndfEnabled() const { return bUseVndf; }
    void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }

    void SetAccumulationEnabled(bool bEnabled);
    bool IsAccumulationEnabled() const { return bPathTracingAccumulationEnabled; }

    void SetMaxBounces(uint32_t MaxBounces);
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

    FBindlessTexture PathTracingTempTexture;
    std::vector<FBindlessTexture> PathTracingAccumulationTextures;

    bool bEnabled = false;
    bool bUseVndf = true;
    bool bPathTracingAccumulationEnabled = false;
    bool bPathTracingAccumulationUserPreference = false;
    uint32_t PathTracingAccumulationFrameCount = 0;
    std::vector<bool> PathTracingAccumulationHistoryValid;
    uint32_t PathTracingAccumulatedFrames = 0;
    uint32_t PathTracingMaxBounces = 8;
    int PathTracingDebugMode = 0;
};
