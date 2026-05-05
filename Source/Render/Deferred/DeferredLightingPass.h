#pragma once

#include <cstdint>

#include "DeferredPassContext.h"

class FDX12Device;
class FDeferredRenderer;
struct FRendererConfig;
enum class EDeferredLightingVisualizationMode : uint32_t;

class FDeferredLightingPass
{
public:
    void ApplyLightingPassConfig(const FRendererConfig& Config);
    EDeferredLightingVisualizationMode GetDeferredLightingVisualizationMode() const;

    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const;

    void AddLinearDepthPass(FDeferredPassContext& Context) const;
    void AddExtractHalfDepthNormalPass(FDeferredPassContext& Context) const;
    void AddDirectLightingPass(FDeferredPassContext& Context, FRGResourceHandle& OutDirectHandle) const;
    void AddCompositeLightPass(FDeferredPassContext& Context, FRGResourceHandle DirectHandle) const;

private:
    bool CreateLightingRootSignature(FDX12Device* Device);
    bool CreateLinearDepthRootSignature(FDX12Device* Device);
    bool CreateLinearDepthPipeline(FDX12Device* Device);
    bool CreateExtractHalfDepthNormalRootSignature(FDX12Device* Device);
    bool CreateExtractHalfDepthNormalPipeline(FDX12Device* Device);
    bool CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateLinearDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height) const;

private:
    FDeferredRenderer* Owner = nullptr;
    bool bEnablePbrResearch = false;
    EDeferredLightingVisualizationMode DeferredLightingVisualizationMode{};
};
