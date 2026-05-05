#pragma once

#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>
#include <DirectXMath.h>

#include "../GpuResource.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;
class FCamera;

struct FTaaFrameResources
{
    std::vector<FRGResourceHandle> HistoryHandles{};
};

class FTaa
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPass(FDeferredPassContext& Context) const;

    void PrepareFrameState(FDeferredRenderer& Owner, const FCamera& Camera, bool bUseGtaoJitter,
        bool& bTaaActive, bool& bTaaHistoryReady, uint32_t& TaaFrameIndex, uint32_t& TaaReadIndex, uint32_t& TaaWriteIndex);
    void FinalizeFrameState(bool bTaaActive, bool bGtaoJitterActive);
    void OnFrameFenceSignaled(uint32_t FrameIndex);

    void SetEnabled(bool bEnabled);
    bool IsEnabled() const { return bEnabled; }

    void SetHistoryWeight(float Weight) { HistoryWeight = Weight; }
    float GetHistoryWeight() const { return HistoryWeight; }

    bool IsReady() const;
    bool UsesJitter() const { return bUseJitter; }
    DirectX::XMFLOAT2 GetJitter() const { return Jitter; }
    DirectX::XMMATRIX GetProjection() const { return Projection; }
    uint32_t GetSampleIndex() const { return SampleIndex; }
    uint32_t GetHistorySrvBindlessIndex(uint32_t Index) const;
    uint32_t GetHistoryUavBindlessIndex(uint32_t Index) const;

private:
    friend class FDeferredRenderer;
    friend class FSsr;
    friend class FTonemap;

    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount);
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device);
    void ResetHistoryState();

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    std::vector<FBindlessTexture> HistoryTextures;
    std::vector<bool> HistoryValid;

    bool bEnabled = false;
    float HistoryWeight = 0.9f;
    uint32_t SampleIndex = 0;
    DirectX::XMFLOAT2 Jitter{ 0.0f, 0.0f };
    DirectX::XMMATRIX Projection = DirectX::XMMatrixIdentity();
    bool bUseJitter = false;
};
