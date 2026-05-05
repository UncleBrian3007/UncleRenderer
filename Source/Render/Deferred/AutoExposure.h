#pragma once

#include <array>
#include <cstdint>
#include <wrl.h>

#include "../GpuResource.h"

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

    void SetEnabled(bool bEnabled) { this->bEnabled = bEnabled; }
    bool IsEnabled() const { return bEnabled; }

    void SetKey(float Key) { ExposureKey = Key; }
    float GetKey() const { return ExposureKey; }

    void SetMinExposure(float MinExposure) { this->MinExposure = MinExposure; }
    float GetMinExposure() const { return MinExposure; }

    void SetMaxExposure(float MaxExposure) { this->MaxExposure = MaxExposure; }
    float GetMaxExposure() const { return MaxExposure; }

    void SetSpeedUp(float Speed) { SpeedUp = Speed; }
    float GetSpeedUp() const { return SpeedUp; }

    void SetSpeedDown(float Speed) { SpeedDown = Speed; }
    float GetSpeedDown() const { return SpeedDown; }

    uint32_t GetCurrentLuminanceSrvBindlessIndex() const { return LuminanceTextures[LuminanceWriteIndex].SrvBindlessIndex; }

private:
    friend class FDeferredRenderer;
    friend class FTonemap;

    bool CreateResources(FDX12Device* Device);
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device);

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    std::array<FBindlessTexture, 2> LuminanceTextures;

    bool bEnabled = false;
    float ExposureKey = 0.18f;
    float MinExposure = 0.1f;
    float MaxExposure = 5.0f;
    float SpeedUp = 3.0f;
    float SpeedDown = 1.0f;
    uint32_t LuminanceWriteIndex = 0;
    bool bHistoryValid = false;
};
