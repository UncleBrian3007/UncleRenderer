#pragma once

#include <d3d12.h>
#include "GpuResource.h"

class FDX12Device;
class FRenderer;
class FTextureLoader;
struct FRendererConfig;

class FEnvironmentMap
{
public:
    bool InitializePipelines(FRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FRenderer& Owner, FDX12Device* Device, const FRendererConfig& Config, const char* LogPrefix);

    ID3D12Resource* GetCubeTexture() const { return EnvironmentCubeTexture.Get(); }
    ID3D12Resource* GetBrdfLutTexture() const { return BrdfLutTexture.Get(); }
    uint32_t GetCubeSrvIndex() const { return EnvironmentCubeTexture.SrvBindlessIndex; }
    uint32_t GetBrdfLutSrvIndex() const { return BrdfLutTexture.SrvBindlessIndex; }
    float GetMipCount() const { return EnvironmentMipCount; }

private:
    DXGI_FORMAT ResolveBuildFormat(FDX12Device* Device) const;
    bool BuildFromEquirect(FRenderer& Owner, FDX12Device* Device, FTextureLoader& TextureLoader, const FRendererConfig& Config);

    FBindlessCubeTexture EnvironmentCubeTexture;
    FBindlessTexture BrdfLutTexture;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> BuildRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> EquirectToCubePipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> CubeMipGenPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SpecularPrefilterPipeline;
    float EnvironmentMipCount = 1.0f;
};
