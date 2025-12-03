#pragma once

#include <d3d12.h>

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FRenderer
{
public:
    virtual ~FRenderer() = default;

    virtual bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat) = 0;
    virtual void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) = 0;

    virtual const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const = 0;
};

