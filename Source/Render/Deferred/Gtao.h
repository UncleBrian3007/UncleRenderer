#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FGtaoFrameResources
{
    FRGResourceHandle GtaoHandle{};
};

class FGtao
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPass(FDeferredPassContext& Context) const;

    uint32_t GetSrvBindlessIndex() const { return GtaoBindlessIndex; }

private:
    friend class FDeferredRenderer;

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device);
    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateHilbertLutResources(FDX12Device* Device);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> GtaoRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> GtaoPipelines;

    Microsoft::WRL::ComPtr<ID3D12Resource> GtaoTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> HilbertLutTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GtaoRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE GtaoRtvHandle{};

    uint32_t HilbertLutBindlessIndex = UINT32_MAX;
    uint32_t GtaoBindlessIndex = UINT32_MAX;

    D3D12_RESOURCE_STATES GtaoState = D3D12_RESOURCE_STATE_RENDER_TARGET;
};
