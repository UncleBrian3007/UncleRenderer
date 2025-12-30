#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <string>
#include <vector>

struct FSceneModelResource;

struct FRendererOptions
{
    std::wstring SceneFilePath = L"Assets/Scenes/Scene.json";
    bool bUseDepthPrepass = false;
    bool bEnableShadows = true;
    float ShadowBias = 0.0f;
    bool bEnableTonemap = true;
    float TonemapExposure = 0.5f;
    float TonemapWhitePoint = 4.0f;
    float TonemapGamma = 1.0f;
    bool bLogResourceBarriers = false;
    bool bEnableGraphDump = false;
    bool bEnableGpuTiming = false;
    bool bEnableHZB = true;
    bool bEnableIndirectDraw = false;
};

class FDX12Device;
class FDX12CommandContext;
class FCamera;

class FRenderer
{
public:
    virtual ~FRenderer() = default;

    virtual bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options) = 0;
    virtual void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) = 0;

    virtual void SetDepthPrepassEnabled(bool bEnabled) { bDepthPrepassEnabled = bEnabled; }
    virtual bool IsDepthPrepassEnabled() const { return bDepthPrepassEnabled; }

    const D3D12_CPU_DESCRIPTOR_HANDLE& GetDSVHandle() const { return DepthStencilHandle; }

    DirectX::XMFLOAT3 GetSceneCenter() const { return SceneCenter; }
    float GetSceneRadius() const { return SceneRadius; }

    void SetLightDirection(const DirectX::XMFLOAT3& Direction) { LightDirection = Direction; }
    DirectX::XMFLOAT3 GetLightDirection() const { return LightDirection; }

    void SetLightIntensity(float Intensity) { LightIntensity = Intensity; }
    float GetLightIntensity() const { return LightIntensity; }

    void SetLightColor(const DirectX::XMFLOAT3& Color) { LightColor = Color; }
    DirectX::XMFLOAT3 GetLightColor() const { return LightColor; }

    void SetCullingCameraOverride(const FCamera* Camera) { CullingCameraOverride = Camera; }
    const FCamera* GetCullingCameraOverride() const { return CullingCameraOverride; }
    virtual const std::vector<FSceneModelResource>* GetSceneModels() const { return nullptr; }
    virtual bool GetSceneModelStats(size_t& OutTotal, size_t& OutCulled) const { return false; }
    virtual void RequestObjectIdReadback(uint32_t X, uint32_t Y) {}
    virtual bool ConsumeObjectIdReadback(uint32_t& OutObjectId) { return false; }

protected:
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
    DirectX::XMFLOAT3 SceneCenter{ 0.0f, 0.0f, 0.0f };
    float SceneRadius = 1.0f;

    DirectX::XMFLOAT3 LightDirection{ -0.5f, -1.0f, 0.2f };
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };

    bool bDepthPrepassEnabled = false;
    const FCamera* CullingCameraOverride = nullptr;
};
