#pragma once

#include <array>
#include <mutex>
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
#include "SkyAtmosphere.h"
#include "TextureLoader.h"
#include "RenderGraph.h"
#include "../Scene/GltfAnimation.h"

class FDX12Device;
class FDX12CommandContext;
class FCamera;
class FForwardRenderer : public FRenderer
{
public:
    // Lifecycle
    FForwardRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config) override;
    void RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime) override;

private:
    // Per-frame state structs
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

    // Initialization
    bool CreateRootSignature(FDX12Device* Device);
    bool CreatePipelineState(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreateSceneTextures(FDX12Device* Device, std::vector<FSceneModelResource>& Models);
    bool CreateGpuDrivenResources(FDX12Device* Device);

    // Pipeline management
    bool EnsureBasePassPipeline(uint32_t PipelineKey, bool bUseSkinning);
    bool EnsureBasePassPipelineOrFail(uint32_t PipelineKey, bool bUseSkinning, const char* PassContext);
    bool CompileForwardBasePassPs(uint32_t PipelineKey, std::vector<uint8_t>& OutPs);
    bool BuildForwardBasePassPsoDesc(uint32_t PipelineKey, bool bUseSkinning, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const;

    // Frame rendering
    void PrepareFrameState(const FCamera& Camera, FForwardFrameState& OutState);
    void ConfigureFrameGraph(FRenderGraph& Graph) const;
    void ImportFrameResources(FRenderGraph& Graph, FForwardFrameResources& OutResources);
    void UpdateCullingVisibility(const FCamera& Camera);
    void UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset, const DirectX::XMMATRIX& LightViewProjection);

    // Render passes
    void AddGpuCullingPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle);
    void AddShadowPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle ShadowHandle);
    void AddRayTracingShadowPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferHandle, FRGResourceHandle& ShadowMaskHandle);
    void AddDepthPrepass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle ShadowHandle);
    void AddSkyPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddForwardPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle ShadowHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);
    void AddObjectIdPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle ObjectIdHandle, FRGResourceHandle DepthHandle);
    void AddDebugPrintPass(FRenderGraph& Graph, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle);

private:
    // Pipeline state
    Microsoft::WRL::ComPtr<ID3D12RootSignature>              RootSignature;
    // Base pass pipelines indexed by permutation key
    // (bit 0: Normal, bit 1: MR, bit 2: BaseColor, bit 3: Emissive, bit 4: AlphaMask,
    //  bit 5: SheenModel, bit 6: ClearcoatModel, bit 7: AnisotropyModel, bit 8: DoubleSided)
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 512> BasePassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 512> BasePassPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2>   DepthPrepassPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2>   DepthPrepassPipelinesSkinned;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2>   ShadowPipelines;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2>   ShadowPipelinesSkinned;
    std::array<std::vector<uint8_t>, 2>   ForwardBasePassVsBytecodes;
    std::array<std::vector<uint8_t>, 256> ForwardBasePassPsBytecodes;
    std::array<bool, 256> ForwardBasePassPsCompiled{};
    std::array<bool, 512> ForwardBasePassFailureLogged{};
    std::mutex            ForwardBasePassPipelineMutex;
    DXGI_FORMAT           ForwardBasePassBackBufferFormat = DXGI_FORMAT_UNKNOWN;

    // Scene resources
    std::unique_ptr<FSkyAtmosphere>                              SkyAtmosphere;
    Microsoft::WRL::ComPtr<ID3D12Resource>                       SceneTexture;
    // Scene and animation data
    std::vector<FGltfScene> GltfScenes;
};
