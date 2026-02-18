#pragma once

#include <array>
#include <memory>
#include <vector>
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include "../Math/MathTypes.h"
#include "Renderer.h"
#include "RendererUtils.h"
#include "TextureLoader.h"
#include "RenderGraph.h"
#include "../Scene/GltfAnimation.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;
class FForwardRenderer : public FRenderer
{
public:
    FForwardRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;

    void SetShadowsEnabled(bool bEnabled) { bShadowsEnabled = bEnabled; }
    bool IsShadowsEnabled() const { return bShadowsEnabled; }

    void SetShadowBias(float Bias) { ShadowBias = Bias; }
    float GetShadowBias() const { return ShadowBias; }

private:
    struct FForwardFrameState
    {
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
        bool bRenderShadows = false;
        bool bDoDepthPrepass = false;
    };

    struct FForwardFrameResources
    {
        FRGResourceHandle ShadowHandle{};
        FRGResourceHandle ShadowMaskHandle{};
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle ObjectIdHandle{};
    };

    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipelineState(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height);
    bool CreateObjectIdPipeline(FDX12Device* Device);
    bool CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models);
    bool CreateGpuDrivenResources(FDX12Device* Device);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset, const DirectX::XMMATRIX& LightViewProjection);
    void UpdateSkyConstants(const FCamera& Camera);
    void UpdateCullingVisibility(const FCamera& Camera);
    void PrepareFrameState(const FCamera& Camera, FForwardFrameState& OutState);
    void ConfigureFrameGraph(FRenderGraph& Graph) const;
    void ImportFrameResources(FRenderGraph& Graph, FForwardFrameResources& OutResources);
    void AddGpuCullingPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle);
    void AddShadowPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle ShadowHandle);
    void AddRayTracingShadowPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferHandle, FRGResourceHandle& ShadowMaskHandle);
    void AddDepthPrepass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle ShadowHandle);
    void AddSkyPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddForwardPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle ShadowHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddObjectIdPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle ObjectIdHandle, FRGResourceHandle DepthHandle);
    void AddDebugPrintPass(FRenderGraph& Graph, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> SkyRootSignature;
    // Base pass pipelines indexed by permutation key (bit 0: Normal, bit 1: MR, bit 2: BaseColor, bit 3: Emissive, bit 4: AlphaMask, bit 5: SheenModel, bit 6: ClearcoatModel, bit 7: AnisotropyModel, bit 8: DoubleSided)
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 512> BasePassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 512> BasePassPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> DepthPrepassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> DepthPrepassPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> ShadowPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> ShadowPipelinesSkinned;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> SkyPipelineState;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> ObjectIdPipeline;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SceneTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> SceneTexture;
    uint32_t ShadowMapBindlessIndex = UINT32_MAX;
    uint32_t EnvironmentCubeBindlessIndex = UINT32_MAX;
    uint32_t BrdfLutBindlessIndex = UINT32_MAX;
    FMeshGeometryBuffers SkyGeometry;
    float SkySphereRadius = 1000.0f;
    std::vector<FGltfScene> GltfScenes;
    std::vector<FGltfAnimationPose> GltfScenePoses;
    std::vector<float> GltfSceneTimes;

};
