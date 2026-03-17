#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FCasFrameResources
{
    FRGResourceHandle TonemapOutputHandle{};
};

class FCas
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPass(FDeferredPassContext& Context) const;

    void SetEnabled(bool bEnabled) { bEnabled_ = bEnabled; }
    bool IsEnabled() const { return bEnabled_; }

    void SetSharpness(float Sharpness) { Sharpness_ = Sharpness; }
    float GetSharpness() const { return Sharpness_; }

    bool IsReady() const;

private:
    friend class FDeferredRenderer;
    friend class FTonemap;

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    uint32_t TonemapOutputBindlessIndex = UINT32_MAX;

    bool bEnabled_ = true;
    float Sharpness_ = 0.2f;
};
