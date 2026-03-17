#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <mutex>
#include <vector>
#include <wrl.h>

#include "../RenderGraph.h"
#include "../../Core/RendererConfig.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FSsrFrameResources
{
    FRGResourceHandle SsrHandle{};
    FRGResourceHandle SsrDenoiseHandle{};
    FRGResourceHandle SsrFallbackHandle{};
    FRGResourceHandle SsrResolveHandle{};

    FRGResourceHandle GetBaseHandle(ESSRMode Mode) const
    {
        return (Mode == ESSRMode::CS) ? SsrResolveHandle : SsrHandle;
    }

    FRGResourceHandle GetLightingHandle(ESSRMode Mode, bool bUseDenoise) const
    {
        return bUseDenoise ? SsrDenoiseHandle : GetBaseHandle(Mode);
    }
};

class FSsr
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPasses(FDeferredPassContext& Context);

    void SetSwEnabled(bool bEnabled) { bSsrSwEnabled = bEnabled; }
    void SetHwEnabled(bool bEnabled) { bSsrHwEnabled = bEnabled; }
    void SetHzbEnabled(bool bEnabled) { bSsrHzbEnabled = bEnabled; }
    void SetRefineEnabled(bool bEnabled) { bSsrRefineEnabled = bEnabled; }
    void SetDenoiseEnabled(bool bEnabled) { bSsrDenoiseEnabled = bEnabled; }
    void SetMode(ESSRMode Mode) { SsrMode = Mode; }
    void SetSamplesPerQuad(uint32_t Samples) { SsrSamplesPerQuad = Samples; }
    void SetMaxSteps(uint32_t Steps) { SsrMaxSteps = Steps; }
    void SetMaxDistance(float Distance) { SsrMaxDistance = Distance; }
    void SetThickness(float Thickness) { SsrThickness = Thickness; }
    void SetStride(float Stride) { SsrStride = Stride; }
    void SetRoughnessCutoff(float Cutoff) { SsrRoughnessCutoff = Cutoff; }
    void SetIntensity(float Intensity) { SsrIntensity = Intensity; }

    bool IsSwEnabled() const { return bSsrSwEnabled; }
    bool IsHwEnabled() const { return bSsrHwEnabled; }
    bool IsHzbEnabled() const { return bSsrHzbEnabled; }
    bool IsRefineEnabled() const { return bSsrRefineEnabled; }
    bool IsDenoiseEnabled() const { return bSsrDenoiseEnabled; }
    ESSRMode GetMode() const { return SsrMode; }
    uint32_t GetSamplesPerQuad() const { return SsrSamplesPerQuad; }
    uint32_t GetMaxSteps() const { return SsrMaxSteps; }
    float GetMaxDistance() const { return SsrMaxDistance; }
    float GetThickness() const { return SsrThickness; }
    float GetStride() const { return SsrStride; }
    float GetRoughnessCutoff() const { return SsrRoughnessCutoff; }
    float GetIntensity() const { return SsrIntensity; }

    uint32_t GetBaseOutputSrvBindlessIndex() const;
    uint32_t GetLightingSrvBindlessIndex() const;
    uint32_t GetFallbackSrvBindlessIndex() const { return SsrFallbackBindlessIndex; }

private:
    friend class FDeferredRenderer;

    bool CreateSsrRootSignature(FDX12Device* Device);
    bool CreateSsrPipeline(FDX12Device* Device);
    bool EnsureSsrGraphicsPipeline(FDX12Device* Device, uint32_t PipelineIndex);
    bool EnsureSsrGraphicsPipelineOrFail(FDX12Device* Device, uint32_t PipelineIndex, const char* PassContext);
    bool CompileSsrGraphicsPs(FDX12Device* Device, uint32_t PipelineIndex, std::vector<uint8_t>& OutPs);
    bool BuildSsrGraphicsPsoDesc(uint32_t PipelineIndex, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const;
    bool CreateSsrDenoiseRootSignature(FDX12Device* Device);
    bool CreateSsrDenoisePipeline(FDX12Device* Device);
    bool CreateSsrRayGatherRootSignature(FDX12Device* Device);
    bool CreateSsrRayGatherPipeline(FDX12Device* Device);
    bool CreateSsrSwTraceRootSignature(FDX12Device* Device);
    bool CreateSsrSwTracePipeline(FDX12Device* Device);
    bool EnsureSsrSwTracePipeline(FDX12Device* Device, uint32_t PipelineIndex);
    bool EnsureSsrSwTracePipelineOrFail(FDX12Device* Device, uint32_t PipelineIndex, const char* PassContext);
    bool CompileSsrSwTraceCs(FDX12Device* Device, uint32_t PipelineIndex, std::vector<uint8_t>& OutCs);
    bool CreateSsrBuildIndirectArgsRootSignature(FDX12Device* Device);
    bool CreateSsrBuildIndirectArgsPipeline(FDX12Device* Device);
    bool CreateSsrResolveRootSignature(FDX12Device* Device);
    bool CreateSsrResolvePipeline(FDX12Device* Device);
    bool CreateSsrDispatchCommandSignature(FDX12Device* Device);
    bool CreateSsrResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);

    void AddSsrRayCounterClearPass(FDeferredPassContext& Context);
    void AddSsrRayGatherPass(FDeferredPassContext& Context);
    void AddSsrBuildIndirectArgsPass(FDeferredPassContext& Context, bool bHwMiss);
    void AddSsrSwTracePass(FDeferredPassContext& Context);
    void AddSsrHwTracePass(FDeferredPassContext& Context);
    void AddSsrResolvePass(FDeferredPassContext& Context);
    void AddSsrPass(FDeferredPassContext& Context);
    void AddSsrFallbackPass(FDeferredPassContext& Context);
    void AddSsrDenoisePass(FDeferredPassContext& Context, FRGResourceHandle InputHandle);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrDenoiseRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8> SsrPipelines;
    std::vector<uint8_t> SsrGraphicsVsBytecode;
    std::array<std::vector<uint8_t>, 8> SsrGraphicsPsBytecodes;
    std::array<bool, 8> SsrGraphicsPsCompiled{};
    std::array<bool, 8> SsrGraphicsFailureLogged{};
    std::mutex SsrGraphicsPipelineMutex;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrDenoisePipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrRayGatherRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrRayGatherPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrSwTraceRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8> SsrSwTracePipelines;
    std::array<std::vector<uint8_t>, 8> SsrSwTraceCsBytecodes;
    std::array<bool, 8> SsrSwTraceCsCompiled{};
    std::array<bool, 8> SsrSwTraceFailureLogged{};
    std::mutex SsrSwTracePipelineMutex;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrBuildIndirectArgsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrBuildIndirectArgsPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SsrResolveRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SsrResolvePipeline;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> SsrDispatchCommandSignature;

    Microsoft::WRL::ComPtr<ID3D12Resource> SsrTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> SsrDenoiseTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> SsrFallbackTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> SsrResolveTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> SsrRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE SsrRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE SsrDenoiseRtvHandle{};

    uint32_t SsrBindlessIndex = UINT32_MAX;
    uint32_t SsrDenoiseBindlessIndex = UINT32_MAX;
    uint32_t SsrFallbackBindlessIndex = UINT32_MAX;
    uint32_t SsrFallbackUavBindlessIndex = UINT32_MAX;
    uint32_t SsrUavBindlessIndex = UINT32_MAX;
    uint32_t SsrResolveBindlessIndex = UINT32_MAX;
    uint32_t SsrResolveUavBindlessIndex = UINT32_MAX;

    D3D12_RESOURCE_STATES SsrState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES SsrDenoiseState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES SsrFallbackState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES SsrResolveState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayListBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayCounterBuffers;
    std::vector<uint32_t> SsrRayListSrvBindlessIndices;
    std::vector<uint32_t> SsrRayListUavBindlessIndices;
    std::vector<uint32_t> SsrRayCounterSrvBindlessIndices;
    std::vector<uint32_t> SsrRayCounterUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SsrRayListStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayCounterStates;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayListPrimaryBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayCounterPrimaryBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayListHwMissBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrRayCounterHwMissBuffers;
    std::vector<uint32_t> SsrRayListPrimarySrvBindlessIndices;
    std::vector<uint32_t> SsrRayListPrimaryUavBindlessIndices;
    std::vector<uint32_t> SsrRayCounterPrimarySrvBindlessIndices;
    std::vector<uint32_t> SsrRayCounterPrimaryUavBindlessIndices;
    std::vector<uint32_t> SsrRayListHwMissSrvBindlessIndices;
    std::vector<uint32_t> SsrRayListHwMissUavBindlessIndices;
    std::vector<uint32_t> SsrRayCounterHwMissSrvBindlessIndices;
    std::vector<uint32_t> SsrRayCounterHwMissUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SsrRayListPrimaryStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayCounterPrimaryStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayListHwMissStates;
    std::vector<D3D12_RESOURCE_STATES> SsrRayCounterHwMissStates;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrIndirectArgsPrimaryBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SsrIndirectArgsHwMissBuffers;
    std::vector<uint32_t> SsrIndirectArgsPrimaryUavBindlessIndices;
    std::vector<uint32_t> SsrIndirectArgsHwMissUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SsrIndirectArgsPrimaryStates;
    std::vector<D3D12_RESOURCE_STATES> SsrIndirectArgsHwMissStates;
    uint32_t SsrMaxRayCount = 0;

    bool bSsrSwEnabled = true;
    bool bSsrHwEnabled = true;
    bool bSsrHzbEnabled = false;
    bool bSsrRefineEnabled = false;
    bool bSsrDenoiseEnabled = false;
    uint32_t SsrMaxSteps = 32;
    float SsrMaxDistance = 50.0f;
    float SsrThickness = 1.00f;
    float SsrStride = 1.0f;
    float SsrRoughnessCutoff = 0.6f;
    float SsrIntensity = 0.3f;
    ESSRMode SsrMode = ESSRMode::PS;
    uint32_t SsrSamplesPerQuad = 1;
};
