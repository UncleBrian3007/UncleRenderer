#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <wrl.h>

#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FAutoExposureFrameResources
{
    std::array<FRGResourceHandle, 2> LuminanceHandles{};
};

class FAutoExposure
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPass(FDeferredPassContext& Context) const;
    void FinalizeFrame();

    void SetEnabled(bool bEnabled) { bEnabled_ = bEnabled; }
    bool IsEnabled() const { return bEnabled_; }

    void SetKey(float Key) { ExposureKey = Key; }
    float GetKey() const { return ExposureKey; }

    void SetMinExposure(float MinExposure) { MinExposure_ = MinExposure; }
    float GetMinExposure() const { return MinExposure_; }

    void SetMaxExposure(float MaxExposure) { MaxExposure_ = MaxExposure; }
    float GetMaxExposure() const { return MaxExposure_; }

    void SetSpeedUp(float Speed) { SpeedUp = Speed; }
    float GetSpeedUp() const { return SpeedUp; }

    void SetSpeedDown(float Speed) { SpeedDown = Speed; }
    float GetSpeedDown() const { return SpeedDown; }

    uint32_t GetCurrentLuminanceSrvBindlessIndex() const { return LuminanceSrvBindlessIndices[LuminanceWriteIndex]; }

private:
    friend class FDeferredRenderer;
    friend class FTonemap;

    bool CreateResources(FDX12Device* Device);
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device);

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> LuminanceTextures;

    std::array<uint32_t, 2> LuminanceSrvBindlessIndices{ { UINT32_MAX, UINT32_MAX } };
    std::array<uint32_t, 2> LuminanceUavBindlessIndices{ { UINT32_MAX, UINT32_MAX } };
    std::array<D3D12_RESOURCE_STATES, 2> LuminanceStates = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };

    bool bEnabled_ = false;
    float ExposureKey = 0.18f;
    float MinExposure_ = 0.1f;
    float MaxExposure_ = 5.0f;
    float SpeedUp = 3.0f;
    float SpeedDown = 1.0f;
    uint32_t LuminanceWriteIndex = 0;
    bool bHistoryValid = false;
};
