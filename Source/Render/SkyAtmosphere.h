#pragma once

#include "RendererUtils.h"

#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>

class FDX12Device;
class FDX12CommandContext;
class FCamera;
struct FDeferredPassContext;
struct FMeshGeometryBuffers;

struct FSkyAtmosphereConstants
{
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4X4 View;
    DirectX::XMFLOAT4X4 Projection;
    DirectX::XMFLOAT3 CameraPosition;
    float Padding0 = 0.0f;
    DirectX::XMFLOAT3 LightDirection{ 0.0f, -1.0f, 0.0f };
    float Padding1 = 0.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    float Padding2 = 0.0f;
};

struct FSkyPipelineConfig
{
    bool DepthEnable = false;
    D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_DEPTH_WRITE_MASK DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DXGI_FORMAT DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
};

struct FSkyAtmosphereFrameParameters
{
    const FCamera* Camera = nullptr;
    DirectX::XMMATRIX Projection = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 LightDirection{ 0.0f, -1.0f, 0.0f };
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    D3D12_VIEWPORT Viewport{};
    D3D12_RECT ScissorRect{};
};

class FSkyAtmosphere
{
public:
    bool Initialize(FDX12Device* Device, float InSphereRadius, DXGI_FORMAT RenderTargetFormat, const FSkyPipelineConfig& Config);
    bool IsReady() const;
    void AddPass(FDeferredPassContext& Context);
    void Draw(
        FDX12CommandContext& Cmd,
        const FSkyAtmosphereFrameParameters& Parameters,
        const D3D12_CPU_DESCRIPTOR_HANDLE& RenderTargetHandle,
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthStencilHandle,
        bool bClearDepth = false);

private:
    void UpdateConstants(const FSkyAtmosphereFrameParameters& Parameters);

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PipelineState;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12Resource> ConstantBuffer;
    uint8_t* ConstantBufferMapped = nullptr;
    FMeshGeometryBuffers Geometry;
    float SphereRadius = 100.0f;
};

namespace SkyAtmosphere
{
    bool CreateResources(
        FDX12Device* Device,
        float SphereRadius,
        FMeshGeometryBuffers& OutGeometry,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutConstantBuffer,
        uint8_t*& OutConstantBufferMapped);

    bool CreatePipeline(
        FDX12Device* Device,
        DXGI_FORMAT BackBufferFormat,
        const FSkyPipelineConfig& Config,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& OutRootSignature,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState);

    void UpdateConstants(
        const FCamera& Camera,
        const DirectX::XMMATRIX& WorldMatrix,
        const DirectX::XMMATRIX& Projection,
        const DirectX::XMVECTOR& LightDirection,
        const DirectX::XMFLOAT3& LightColor,
        uint8_t* ConstantBufferMapped);
}
