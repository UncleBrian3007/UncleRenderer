#pragma once

#include <array>
#include <cstdint>
#include <d3d12.h>
#include <vector>
#include <wrl.h>

#include "../GpuResource.h"
#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;
class FDX12Device;

struct FHzbFrameResources
{
    FRGResourceHandle HzbHandle{};
};

class FHzb
{
public:
    bool InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device);
    bool InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height);
    void ImportPersistentResources(FDeferredPassContext& Context);
    bool CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device);
    void AddPass(FDeferredPassContext& Context);

    void SetEnabled(bool bInEnabled) { bEnabled = bInEnabled; }
    bool IsEnabled() const { return bEnabled; }

    void SetTwoPassEnabled(bool bInEnabled) { bTwoPassEnabled = bInEnabled; }
    bool IsTwoPassEnabled() const { return bTwoPassEnabled; }

    bool IsReady() const { return bReady; }
    void SetReady(bool bInReady) { bReady = bInReady; }

    uint32_t GetSrvBindlessIndex() const { return HzbTexture.SrvBindlessIndex; }
    uint32_t GetNullUavBindlessIndex() const { return HzbNullUavTexture.UavBindlessIndex; }
    const std::vector<uint32_t>& GetSrvMipBindlessIndices() const { return HzbSrvMipBindlessIndices; }
    const std::vector<uint32_t>& GetUavBindlessIndices() const { return HzbUavBindlessIndices; }
    ID3D12Resource* GetTexture() const { return HzbTexture.Get(); }
    uint32_t GetWidth() const { return Width; }
    uint32_t GetHeight() const { return Height; }
    uint32_t GetMipCount() const { return MipCount; }

    Microsoft::WRL::ComPtr<ID3D12RootSignature> GetRootSignature() const { return HzbRootSignature; }

private:
    friend class FDeferredRenderer;

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipeline(FDX12Device* Device);
    bool CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height);

private:
    bool bEnabled = true;
    bool bTwoPassEnabled = true;
    bool bReady = false;
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t MipCount = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> HzbRootSignature;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 4> HzbPipelines;

    FBindlessTexture HzbTexture;
    FBindlessTexture HzbNullUavTexture;
    std::vector<uint32_t> HzbSrvMipBindlessIndices;
    std::vector<uint32_t> HzbUavBindlessIndices;
};
