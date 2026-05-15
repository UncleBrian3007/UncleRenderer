#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>

#include "../GpuResource.h"
#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FGtaoFrameResources
{
    FRGResourceHandle GtaoHandle{};
};

struct FRendererConfig;

class FGtao
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context);
    void AddPass(FDeferredPassContext& Context) const;
    void ApplyConfig(const FRendererConfig& Config);

    uint32_t GetSrvBindlessIndex() const { return GtaoTexture.SrvBindlessIndex; }
    bool IsEnabled() const { return bGtaoEnabled; }
    bool IsJitterEnabled() const { return bGtaoJitterEnabled; }
    float GetRadius() const { return GtaoRadius; }
    float GetIntensity() const { return GtaoIntensity; }
    float GetPower() const { return GtaoPower; }
    float GetThickness() const { return GtaoThickness; }
    uint32_t GetDirectionCount() const { return GtaoDirectionCount; }
    uint32_t GetStepCount() const { return GtaoStepCount; }
    void SetTemporalIndex(uint32_t Index) { GtaoTemporalIndex = Index; }

private:
    friend class FDeferredRenderer;

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device);
    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateHilbertLutResources(FDX12Device* Device);

private:
    bool bGtaoEnabled = true;
    bool bGtaoJitterEnabled = true;
    float GtaoRadius = 0.75f;
    float GtaoIntensity = 1.0f;
    float GtaoPower = 1.5f;
    float GtaoThickness = 0.1f;
    uint32_t GtaoDirectionCount = 6;
    uint32_t GtaoStepCount = 4;
    uint32_t GtaoTemporalIndex = 0u;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> GtaoRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> GtaoPipelines;

    FBindlessTexture GtaoTexture;
    FBindlessTexture HilbertLutTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> GtaoRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE GtaoRtvHandle{};
};
