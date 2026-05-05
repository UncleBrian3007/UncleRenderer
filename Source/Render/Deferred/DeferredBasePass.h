#pragma once

#include <array>
#include <mutex>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <cstdint>
#include "DeferredPassContext.h"

class FDX12Device;
class FDeferredRenderer;

class FDeferredBasePass
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT LightingBufferFormat);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const;

    void AddShadowPass(FDeferredPassContext& Context) const;
    void AddDepthPrepass(FDeferredPassContext& Context) const;
    void AddBasePass(FDeferredPassContext& Context, bool bClearTargets, bool bClearDepth, const char* PassName, bool bAllowSkinningFallback) const;
    void AddVelocityPass(FDeferredPassContext& Context) const;

    ID3D12RootSignature* GetBasePassRootSignature() const { return BasePassRootSignature.Get(); }
    bool HasShadowPipelines() const { return ShadowPipelines[0] && ShadowPipelines[1]; }
    bool HasDepthPrepassPipelines() const { return DepthPrepassPipelines[0] && DepthPrepassPipelines[1]; }

private:
    FDeferredRenderer* Owner = nullptr;
    bool CreateBasePassRootSignature(FDX12Device* Device);
    bool CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat);
    bool CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height) const;
    bool CreateVelocityResources(FDX12Device* Device, uint32_t Width, uint32_t Height) const;
    bool EnsureBasePassPipeline(uint32_t PipelineKey, bool bUseSkinning) const;
    bool EnsureBasePassPipelineOrFail(uint32_t PipelineKey, bool bUseSkinning, const char* PassContext) const;
    bool CompileDeferredBasePassPs(uint32_t PipelineKey, std::vector<uint8_t>& OutPs) const;
    bool BuildDeferredBasePassPsoDesc(uint32_t PipelineKey, bool bUseSkinning, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const;
    bool CreateVelocityRootSignature(FDX12Device* Device);
    bool CreateVelocityPipeline(FDX12Device* Device);
    bool CreateDepthPrepassPipeline(FDX12Device* Device);

    FDX12Device* Device = nullptr;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> BasePassRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> VelocityRootSignature;
    // Base pass pipelines indexed by permutation key (bit 0: Normal, bit 1: MR, bit 2: BaseColor, bit 3: Emissive,
    // bit 4: AlphaMask, bit 5: SheenModel, bit 6: ClearcoatModel, bit 7: AnisotropyModel, bit 8: DoubleSided, bit 9: ClusterDagDebugLightingView)
    mutable std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 1024> BasePassPipelines;
    mutable std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 1024> BasePassPipelinesSkinned;
    std::array<std::vector<uint8_t>, 4> DeferredBasePassVsBytecodes;
    mutable std::array<std::vector<uint8_t>, 1024> DeferredBasePassPsBytecodes;
    mutable std::array<bool, 1024> DeferredBasePassPsCompiled{};
    mutable std::array<bool, 1024> DeferredBasePassFailureLogged{};
    mutable std::mutex DeferredBasePassPipelineMutex;
    DXGI_FORMAT DeferredBasePassLightingFormat = DXGI_FORMAT_UNKNOWN;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> DepthPrepassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> DepthPrepassPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> ShadowPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> ShadowPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> VelocityPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> VelocityPipelinesSkinned;
};
