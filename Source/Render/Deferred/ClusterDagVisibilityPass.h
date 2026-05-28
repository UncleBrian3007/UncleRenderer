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
    FRGResourceHandle Visibility64Handle{};
};

class FClusterDagVisibilityPass
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context);
    void AddPasses(FDeferredPassContext& Context) const;
    void SetSoftwareRasterHzbRejectEnabled(bool bInEnabled) { bSoftwareRasterHzbRejectEnabled = bInEnabled; }

    bool IsReady() const;
    uint32_t GetVisibilitySrvBindlessIndex() const { return VisibilityTexture64.SrvBindlessIndex; }

private:
    bool CreateVisibilityRootSignature(FDX12Device* Device);
    bool CreateVisibilityPipeline(FDX12Device* Device);
    bool CreateSoftwareRasterPipeline(FDX12Device* Device);
    bool CreateDepthExportRootSignature(FDX12Device* Device);
    bool CreateDepthExportPipeline(FDX12Device* Device);
    bool CreateResolveRootSignature(FDX12Device* Device);
    bool CreateResolvePipeline(FDX12Device* Device);
    bool CreateCommandSignature(FDX12Device* Device);
    bool CreateVisibilityResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    void AddVisibilityPass(FDeferredPassContext& Context) const;
    void AddSoftwareRasterPass(FDeferredPassContext& Context) const;
    void AddDepthExportPass(FDeferredPassContext& Context) const;
    void AddResolvePass(FDeferredPassContext& Context) const;

private:
    FDeferredRenderer* Owner = nullptr;
    FDX12Device* Device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> VisibilityRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SoftwareRasterRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> DepthExportRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> ResolveRootSignature;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> CommandSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> VisibilityPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PrepareSoftwareRasterArgsPipeline;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> SoftwareRasterPipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> DepthExportPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ResolvePipeline;
    FBindlessTexture VisibilityTexture64;
    bool bSoftwareRasterHzbRejectEnabled = true;
    bool bPipelinesReady = false;
    bool bResourcesReady = false;
};
