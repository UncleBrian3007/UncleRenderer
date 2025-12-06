#pragma once

#include <d3d12.h>
#include <DirectXMath.h>

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FRenderer
{
public:
    virtual ~FRenderer() = default;

    virtual bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat) = 0;
    virtual void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) = 0;

    virtual void SetDepthPrepassEnabled(bool bEnabled) { bDepthPrepassEnabled = bEnabled; }
    virtual bool IsDepthPrepassEnabled() const { return bDepthPrepassEnabled; }

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const { return DepthStencilHandle; }

    DirectX::XMFLOAT3 GetSceneCenter() const { return SceneCenter; }
    float GetSceneRadius() const { return SceneRadius; }

protected:
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
    DirectX::XMFLOAT3 SceneCenter{ 0.0f, 0.0f, 0.0f };
    float SceneRadius = 1.0f;

    bool bDepthPrepassEnabled = false;
};

