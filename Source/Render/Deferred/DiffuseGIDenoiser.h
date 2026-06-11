#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>

#include "../GpuResource.h"
#include "../RenderGraph.h"
#include "GBufferLayout.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FDiffuseGIDenoiserFrameResources
{
    FRGResourceHandle PreBlurSHHandle{};
    FRGResourceHandle TemporalSHHandle{};
    FRGResourceHandle HistoryIrradianceHandle{};
    FRGResourceHandle HistorySHReadHandle{};
    FRGResourceHandle HistorySHWriteHandle{};
    FRGResourceHandle HistoryCountReadHandle{};
    FRGResourceHandle HistoryCountWriteHandle{};
    FRGResourceHandle PrevLinearDepthReadHandle{};
    FRGResourceHandle PrevLinearDepthWriteHandle{};
    FRGResourceHandle PrevNormalReadHandle{};
    FRGResourceHandle PrevNormalWriteHandle{};
    FRGResourceHandle ShMipHandle{};
    FRGResourceHandle LinearDepthMipHandle{};
    FRGBufferHandle SpdAtomicCounterHandle{};
};

class FDiffuseGIDenoiser
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPasses(FDeferredPassContext& Context, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle) const;
    void FinalizeFrame(FDeferredRenderer& Owner);
    void OnFrameFenceSignaled(uint32_t FrameIndex);
    void InvalidateHistory();

    void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
    bool IsEnabled() const { return bEnabled; }

    void SetSeparableFinalBlur(bool bInSeparableFinalBlur) { bSeparableFinalBlur = bInSeparableFinalBlur; }

    void SetFreezeHistoryResetPeriod(uint32_t InPeriod) { FreezeHistoryResetPeriod = InPeriod; }
    uint32_t GetFreezeHistoryResetPeriod() const { return FreezeHistoryResetPeriod; }

    bool IsHistoryValid() const { return bHistoryValid; }
    bool HasCurrentFrameOutput() const { return bPassesSubmittedThisFrame; }
    ID3D12Resource* GetCurrentOutputTexture() const;
    uint32_t GetCurrentOutputSrvBindlessIndex() const;
    uint32_t GetCurrentOutputUavBindlessIndex() const;
    uint32_t GetPrevLinearDepthSrvBindlessIndex() const;

private:
    friend class FDeferredRenderer;

    bool ShouldResetHistoryForFreeze(const FDeferredRenderer& Owner) const;
    void RefreshPersistentInputValidation();
    bool IsReady() const;
    void AddPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle& PreBlurSHHandle) const;
    void AddTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle& TemporalSHHandle, FRGResourceHandle HistorySHReadHandle, FRGResourceHandle HistoryCountReadHandle, FRGResourceHandle HistoryCountWriteHandle, FRGResourceHandle PrevLinearDepthReadHandle, FRGResourceHandle PrevLinearDepthWriteHandle, FRGResourceHandle PrevNormalReadHandle, FRGResourceHandle PrevNormalWriteHandle) const;
    void AddShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const;
    void AddHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const;
    void AddFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const;
    void AddSeparableFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, kDeferredGBufferCount>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle SourceSHHandle, FRGResourceHandle& InOutDestSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistoryCountHandle, bool bDirectionY) const;
    uint32_t GetFrameSlot(uint32_t FrameIndex) const;

private:
    bool bEnabled = true;
    bool bSeparableFinalBlur = true;
    uint32_t FreezeHistoryResetPeriod = 3;
    mutable bool bHistoryValid = false;
    mutable bool bPassesSubmittedThisFrame = false;
    mutable uint32_t CurrentOutputSlot = 0;
    mutable uint32_t CurrentReadSlot = 0;
    bool bPersistentInputsValid = false;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PreBlurPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> TemporalAccumulationPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GenerateShMipsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> GenerateLinearDepthMipsPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> HistoryReconstructionPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> FinalBlurPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SeparableFinalBlurPipeline;

    std::vector<FBindlessTexture> HistoryIrradiance;
    std::vector<FBindlessTexture> HistorySH;
    std::vector<FBindlessTexture> HistoryCount;
    std::vector<FBindlessTexture> PrevLinearDepth;
    std::vector<FBindlessTexture> PrevNormal;
    mutable std::vector<bool> HistoryValid;
    mutable std::vector<bool> PendingHistoryWrite;
};
