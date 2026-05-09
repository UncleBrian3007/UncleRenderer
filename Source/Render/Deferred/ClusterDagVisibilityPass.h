#pragma once

#include <cstdint>
#include <d3d12.h>
#include <array>
#include <wrl.h>

#include "../GpuResource.h"
#include "../RenderGraph.h"

class FDeferredRenderer;
class FDX12Device;
struct FDeferredPassContext;

struct FClusterDagVisibilityFrameResources
{
    FRGResourceHandle VisibilityHandle0{};
    FRGResourceHandle VisibilityHandle1{};
};

class FClusterDagVisibilityPass
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context);
    void AddPasses(FDeferredPassContext& Context) const;
    void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }

    bool IsReady() const;
    uint32_t GetVisibilitySrvBindlessIndex0() const { return VisibilityTexture0.SrvBindlessIndex; }
    uint32_t GetVisibilitySrvBindlessIndex1() const { return VisibilityTexture1.SrvBindlessIndex; }

private:
    bool CreateVisibilityRootSignature(FDX12Device* Device);
    bool CreateVisibilityPipeline(FDX12Device* Device);
    bool CreateResolveRootSignature(FDX12Device* Device);
    bool CreateResolvePipeline(FDX12Device* Device);
    bool CreateCommandSignature(FDX12Device* Device);
    bool CreateVisibilityResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    void AddVisibilityPass(FDeferredPassContext& Context) const;
    void AddResolvePass(FDeferredPassContext& Context) const;

private:
    FDeferredRenderer* Owner = nullptr;
    FDX12Device* Device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> VisibilityRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> ResolveRootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> CommandSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> VisibilityPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ResolvePipeline;
    FBindlessTexture VisibilityTexture0;
    FBindlessTexture VisibilityTexture1;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> VisibilityRtvHeap;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> VisibilityRtvHandles{};
    bool bFeatureSupported = false;
    bool bEnabled = true;
    bool bPipelinesReady = false;
    bool bResourcesReady = false;
};
