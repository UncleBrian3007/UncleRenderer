#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "GpuResource.h"
#include "RenderGraph.h"

class FDX12Device;
struct FDeferredPassContext;

class FObjectId
{
public:
    bool InitializePipelines(FDX12Device* Device, ID3D12RootSignature* RootSignature);
    bool InitializeResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    FRGResourceHandle ImportResource(FRenderGraph& Graph, uint32_t Width, uint32_t Height);
    void AddPass(FDeferredPassContext& Context) const;

    void RequestReadback(uint32_t X, uint32_t Y);
    bool ConsumeReadback(uint32_t& OutObjectId);

    bool IsReady() const { return RenderTarget && Pipeline; }
    ID3D12Resource* GetRenderTarget() const { return RenderTarget.Get(); }
    D3D12_RESOURCE_STATES* GetStatePtr() { return &State; }
    const D3D12_CPU_DESCRIPTOR_HANDLE& GetRtvHandle() const { return RenderTarget.RtvHandle; }
    ID3D12PipelineState* GetPipeline() const { return Pipeline.Get(); }
    bool IsReadbackRequested() const { return bReadbackRequested; }
    uint32_t GetReadbackX() const { return ReadbackX; }
    uint32_t GetReadbackY() const { return ReadbackY; }
    ID3D12Resource* GetReadbackResource() const { return Readback.Get(); }
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& GetFootprint() const { return Footprint; }
    uint32_t GetRowPitch() const { return RowPitch; }
    void SetReadbackRecorded() { bReadbackRecorded = true; }

private:
    FRenderTarget RenderTarget;
    FReadbackBuffer Readback;
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bool bReadbackRequested = false;
    bool bReadbackRecorded = false;
    uint32_t ReadbackX = 0;
    uint32_t ReadbackY = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
    uint32_t RowPitch = 0;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
};
