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

    void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
    bool IsEnabled() const { return bEnabled; }

    void SetSharpness(float InSharpness) { Sharpness = InSharpness; }
    float GetSharpness() const { return Sharpness; }

    bool IsReady() const;

private:
    friend class FDeferredRenderer;
    friend class FTonemap;

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;

    bool bEnabled = true;
    float Sharpness = 0.2f;
};
