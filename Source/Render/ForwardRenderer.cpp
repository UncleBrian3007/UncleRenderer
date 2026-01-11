#include "ForwardRenderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "RenderGraph.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/Camera.h"
#include "../Scene/Mesh.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12CommandContext.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../Core/RendererConfig.h"
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <array>

FForwardRenderer::FForwardRenderer() = default;

namespace
{
    uint32_t BuildPipelineKey(const FSceneModelResource& Model)
    {
        const uint32_t UseNormal = Model.bHasNormalMap ? 1u : 0u;
        const uint32_t UseMr = !Model.MetallicRoughnessTexturePath.empty() ? 1u : 0u;
        const uint32_t UseBase = !Model.BaseColorTexturePath.empty() ? 1u : 0u;
        const uint32_t UseEmissive = !Model.EmissiveTexturePath.empty() ? 1u : 0u;
        const uint32_t UseAlphaMask = (Model.AlphaMode == 1u) ? 1u : 0u;
        return (UseNormal) | (UseMr << 1) | (UseBase << 2) | (UseEmissive << 3) | (UseAlphaMask << 4);
    }
}

bool FForwardRenderer::Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererConfig& Config)
{
    if (Device == nullptr)
    {
        LogError("Forward renderer initialization failed: device is null");
        return false;
    }

    this->Device = Device;

    LogInfo("Forward renderer initialization started");

    InitializeCommonSettings(Width, Height, Config);

    LogInfo("Creating forward renderer root signature...");
    if (!CreateRootSignature(Device))
    {
        LogError("Forward renderer initialization failed: root signature creation failed");
        return false;
    }

    LogInfo("Creating forward renderer pipeline state...");
    if (!CreatePipelineState(Device, BackBufferFormat))
    {
        LogError("Forward renderer initialization failed: pipeline state creation failed");
        return false;
    }

    LogInfo("Creating forward renderer object ID pipeline...");
    if (!CreateObjectIdPipeline(Device))
    {
        LogError("Forward renderer initialization failed: object ID pipeline creation failed");
        return false;
    }

    LogInfo("Creating forward renderer shadow pipeline...");
    if (!CreateShadowPipeline(Device, RootSignature.Get(), ShadowPipeline))
    {
        LogError("Forward renderer initialization failed: shadow pipeline creation failed");
        return false;
    }

    TextureLoader = std::make_unique<FTextureLoader>(Device);

    if (!TextureLoader->LoadOrSolidColor(L"", 0xffffffff, NullTexture))
    {
        LogError("Forward renderer initialization failed: null texture creation failed");
        return false;
    }

    if (NullTexture)
    {
        NullTexture->SetName(L"NullTexture");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/output_pmrem.dds", EnvironmentCubeTexture))
    {
        LogError("Forward renderer initialization failed: environment cube texture loading failed");
        return false;
    }
    if (EnvironmentCubeTexture)
    {
        EnvironmentCubeTexture->SetName(L"EnvironmentCube");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/PreintegratedGF.dds", BrdfLutTexture))
    {
        LogError("Forward renderer initialization failed: BRDF LUT texture loading failed");
        return false;
    }
    if (BrdfLutTexture)
    {
        BrdfLutTexture->SetName(L"BrdfLut");
    }

    if (EnvironmentCubeTexture)
    {
        const D3D12_RESOURCE_DESC EnvDesc = EnvironmentCubeTexture->GetDesc();
        EnvironmentMipCount = static_cast<float>((std::max)(1u, static_cast<uint32_t>(EnvDesc.MipLevels)));
    }

    if (!CreateDepthResourcesPerFrame(Device, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT))
    {
        LogError("Forward renderer initialization failed: depth resources creation failed");
        return false;
    }

    if (!CreateObjectIdResources(Device, Width, Height))
    {
        LogError("Forward renderer initialization failed: object ID resources creation failed");
        return false;
    }
    if (ObjectIdTexture)
    {
        ObjectIdTexture->SetName(L"ObjectIdTexture");
    }
    if (ObjectIdRtvHeap)
    {
        ObjectIdRtvHeap->SetName(L"ObjectIdRtvHeap");
    }
    if (ObjectIdReadback)
    {
        ObjectIdReadback->SetName(L"ObjectIdReadback");
    }

    if (!CreateShadowResources(Device, ShadowMapWidth, ShadowMapHeight, ShadowMap, ShadowDSVHeap, ShadowDSVHandle, ShadowMapState))
    {
        LogError("Forward renderer initialization failed: shadow resources creation failed");
        return false;
    }

    const std::wstring SceneFilePath = Config.SceneFile.empty() ? L"Assets/Scenes/Scene.json" : Config.SceneFile;
    if (!RendererUtils::CreateSceneModelsFromJson(Device, SceneFilePath, SceneModels, SceneCenter, SceneRadius))
    {
        LogError("scene JSON could not be loaded.");
        return false;
    }

    SceneConstantBufferStride = (sizeof(FSceneConstants) + 255ULL) & ~255ULL;

    const uint64_t ConstantBufferSize = SceneConstantBufferStride * (std::max<uint64_t>(1, SceneModels.size()));

    if (!CreateSceneConstantBuffersPerFrame(Device, ConstantBufferSize))
    {
        LogError("Forward renderer initialization failed: constant buffer creation failed");
        return false;
    }
    if (!CreateCullingConstantBuffersPerFrame(Device))
    {
        LogError("Forward renderer initialization failed: culling constant buffer creation failed");
        return false;
    }

    SkySphereRadius = (std::max)(SceneRadius * 5.0f, 100.0f);
    if (!RendererUtils::CreateSkyAtmosphereResources(Device, SkySphereRadius, SkyGeometry, SkyConstantBuffer, SkyConstantBufferMapped))
    {
        LogError("Forward renderer initialization failed: sky resource creation failed");
        return false;
    }
    if (SkyConstantBuffer)
    {
        SkyConstantBuffer->SetName(L"SkyConstantBuffer");
    }

    FSkyPipelineConfig SkyPipelineConfig = {};
    SkyPipelineConfig.DepthEnable = false;
    SkyPipelineConfig.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SkyPipelineConfig.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    SkyPipelineConfig.DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    if (!RendererUtils::CreateSkyAtmospherePipeline(Device, BackBufferFormat, SkyPipelineConfig, SkyRootSignature, SkyPipelineState))
    {
        LogError("Forward renderer initialization failed: sky pipeline state creation failed");
        return false;
    }

    if (!CreateSceneTextures(Device, SceneModels))
    {
        LogError("Forward renderer initialization failed: scene texture creation failed");
        return false;
    }

    if (!CreateGpuDrivenResources(Device))
    {
        LogWarning("Forward renderer GPU-driven resources creation failed; fallback to CPU-driven draws.");
    }

    if (bEnableGpuDebugPrint)
    {
        if (!CreateGpuDebugPrintResources(Device) || !CreateGpuDebugPrintPipeline(Device, BackBufferFormat) || !CreateGpuDebugPrintStatsPipeline(Device))
        {
            LogError("Forward renderer initialization failed: GPU debug print setup failed");
            return false;
        }
    }

    LogInfo("Forward renderer initialization completed");
    return true;
}

void FForwardRenderer::RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime)
{
    FScopedPixEvent RenderEvent(CmdContext.GetCommandList(), L"ForwardRenderer");

    PrepareGpuDebugPrint(CmdContext);
    ConfigureHZBOcclusion(false, TextureDescriptorHeap.Get(), SceneTextureGpuHandle, 0, 0, 0);

    FForwardFrameState FrameState = {};
    PrepareFrameState(Camera, FrameState);

    FRenderGraph Graph;
    ConfigureFrameGraph(Graph);

    FForwardFrameResources Resources = {};
    ImportFrameResources(Graph, Resources);

    AddGpuCullingPass(Graph, Camera, Resources.DepthHandle);
    AddShadowPass(Graph, Camera, FrameState, Resources.ShadowHandle);
    AddDepthPrepass(Graph, Camera, FrameState, Resources.DepthHandle, Resources.ShadowHandle);
    AddSkyPass(Graph, Camera, FrameState, Resources.DepthHandle, RtvHandle);
    AddForwardPass(Graph, Camera, FrameState, Resources.DepthHandle, Resources.ShadowHandle, RtvHandle);
    AddObjectIdPass(Graph, Camera, FrameState, Resources.ObjectIdHandle, Resources.DepthHandle);
    AddDebugPrintPass(Graph, RtvHandle);

    Graph.Execute(CmdContext);
}

void FForwardRenderer::PrepareFrameState(const FCamera& Camera, FForwardFrameState& OutState)
{
    UpdateCullingVisibility(Camera);
    OutState.LightViewProjection = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
    OutState.bRenderShadows = bShadowsEnabled && ShadowPipeline && ShadowMap;
    OutState.bDoDepthPrepass = bDepthPrepassEnabled && DepthPrepassPipeline;
}

void FForwardRenderer::ConfigureFrameGraph(FRenderGraph& Graph) const
{
    Graph.SetDevice(Device);
    Graph.SetBarrierLoggingEnabled(bLogResourceBarriers);
    Graph.SetGraphDumpEnabled(bEnableGraphDump);
    Graph.SetGpuTimingEnabled(bEnableGpuTiming);
}

void FForwardRenderer::ImportFrameResources(FRenderGraph& Graph, FForwardFrameResources& OutResources)
{
    OutResources.ShadowHandle = Graph.ImportTexture(
        "ShadowMap",
        ShadowMap.Get(),
        &ShadowMapState,
        { 2048, 2048, DXGI_FORMAT_D32_FLOAT });

    const FRGTextureDesc DepthDesc =
    {
        static_cast<uint32>(Viewport.Width),
        static_cast<uint32>(Viewport.Height),
        DXGI_FORMAT_D24_UNORM_S8_UINT
    };

    D3D12_RESOURCE_STATES& DepthState = GetDepthBufferState();
    OutResources.DepthHandle = Graph.ImportTexture("Depth", GetDepthBuffer(), &DepthState, DepthDesc);
    OutResources.ObjectIdHandle = Graph.ImportTexture(
        "ObjectId",
        ObjectIdTexture.Get(),
        &ObjectIdState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R32_UINT });
}

void FForwardRenderer::AddGpuCullingPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle)
{
    struct FGpuCullingPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FGpuCullingPassData>("GPU Culling", [this, &Camera, DepthHandle](FGpuCullingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bEnableIndirectDraw && CullingPipeline && CullingRootSignature && GetIndirectCommandBuffer()
            && ModelBoundsBuffer && MeshletConeAxisBuffer && MeshletConeApexBuffer;
        Data.Camera = &Camera;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [this](const FGpuCullingPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchGpuCulling(Cmd, *Data.Camera);
    });
}

void FForwardRenderer::AddShadowPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle ShadowHandle)
{
    struct FShadowPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Graph.AddPass<FShadowPassData>("ShadowMap", [&, FrameState, ShadowHandle](FShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = FrameState.bRenderShadows;
        Data.Camera = &Camera;
        Data.LightViewProjection = FrameState.LightViewProjection;

        if (FrameState.bRenderShadows)
        {
            Builder.WriteTexture(ShadowHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }, [this](const FShadowPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ShadowEvent(LocalCommandList, L"ShadowMap");
        Cmd.ClearDepth(ShadowDSVHandle, 1.0f);

        LocalCommandList->SetPipelineState(ShadowPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());
        LocalCommandList->RSSetViewports(1, &ShadowViewport);
        LocalCommandList->RSSetScissorRects(1, &ShadowScissor);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &ShadowDSVHandle);

        std::vector<bool> ShadowVisibility;
        ShadowVisibility.resize(SceneModels.size(), true);
        DirectX::XMVECTOR ShadowPlanes[6] = {};
        RendererUtils::BuildFrustumPlanesFromMatrix(Data.LightViewProjection, ShadowPlanes);
        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            ShadowVisibility[ModelIndex] = RendererUtils::IsAabbInCameraFrustum(ShadowPlanes, Model.BoundsMin, Model.BoundsMax);
        }

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);
        }

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            if (!ShadowVisibility.empty() && !ShadowVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBufferAddress + ConstantBufferOffset);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty()
                    ? L"Model"
                    : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
            else
            {
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
        }

    });
}

void FForwardRenderer::AddDepthPrepass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle ShadowHandle)
{
    struct FDepthPrepassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Graph.AddPass<FDepthPrepassData>("DepthPrepass", [&, FrameState, DepthHandle, ShadowHandle](FDepthPrepassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = FrameState.bDoDepthPrepass;
        Data.Camera = &Camera;
        Data.LightViewProjection = FrameState.LightViewProjection;

        if (FrameState.bDoDepthPrepass)
        {
            Builder.WriteTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            if (ShadowMap)
            {
                Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
    }, [this](const FDepthPrepassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent DepthEvent(LocalCommandList, L"DepthPrepass");
        Cmd.ClearDepth(GetDSVHandle());

        ID3D12DescriptorHeap* Heaps[] = { TextureDescriptorHeap.Get() };
        LocalCommandList->SetPipelineState(DepthPrepassPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthHandle);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            if (!SceneModelVisibility.empty() && !SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = SceneModels[ModelIndex];
            if (Model.AlphaMode == 1u)
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);

            LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBufferAddress + ConstantBufferOffset);
            LocalCommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty()
                    ? L"Model"
                    : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
            else
            {
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
        }
    });
}

void FForwardRenderer::AddSkyPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle)
{
    struct FSkyPassData
    {
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        const FCamera* Camera = nullptr;
        bool bEnabled = false;
        bool bClearDepth = false;
    };

    Graph.AddPass<FSkyPassData>("Sky", [&](FSkyPassData& Data, FRGPassBuilder& Builder)
    {
        Data.OutputHandle = RtvHandle;
        Data.Camera = &Camera;
        Data.bEnabled = SkyPipelineState && SkyRootSignature && SkyGeometry.IndexCount > 0;
        Data.bClearDepth = !FrameState.bDoDepthPrepass;

        if (Data.bEnabled)
        {
            Builder.WriteTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }, [this](const FSkyPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent SkyEvent(LocalCommandList, L"SkyAtmosphere");
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        Cmd.SetRenderTarget(Data.OutputHandle, &DepthHandle);

        if (Data.bClearDepth)
        {
            Cmd.ClearDepth(GetDSVHandle());
        }

        LocalCommandList->SetPipelineState(SkyPipelineState.Get());
        LocalCommandList->SetGraphicsRootSignature(SkyRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->IASetVertexBuffers(0, 1, &SkyGeometry.VertexBufferView);
        LocalCommandList->IASetIndexBuffer(&SkyGeometry.IndexBufferView);

        UpdateSkyConstants(*Data.Camera);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, SkyConstantBuffer->GetGPUVirtualAddress());
        LocalCommandList->DrawIndexedInstanced(SkyGeometry.IndexCount, 1, 0, 0, 0);
    });
}

void FForwardRenderer::AddForwardPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle DepthHandle, FRGResourceHandle ShadowHandle, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle)
{
    struct FForwardPassData
    {
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        const FCamera* Camera = nullptr;
        bool bRenderShadows = false;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
        bool bClearDepth = false;
    };

    Graph.AddPass<FForwardPassData>("Forward", [&, FrameState, DepthHandle, ShadowHandle](FForwardPassData& Data, FRGPassBuilder& Builder)
    {
        Data.OutputHandle = RtvHandle;
        Data.Camera = &Camera;
        Data.bRenderShadows = FrameState.bRenderShadows;
        Data.LightViewProjection = FrameState.LightViewProjection;
        Data.bClearDepth = !FrameState.bDoDepthPrepass && !(SkyPipelineState && SkyRootSignature && SkyGeometry.IndexCount > 0);

        Builder.WriteTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (FrameState.bRenderShadows)
        {
            Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }, [this](const FForwardPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ForwardEvent(LocalCommandList, L"ForwardPass");
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        Cmd.SetRenderTarget(Data.OutputHandle, &DepthHandle);

        if (Data.bClearDepth)
        {
            Cmd.ClearDepth(GetDSVHandle());
        }

        LocalCommandList->SetPipelineState(PipelineState.Get());
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());

        ID3D12DescriptorHeap* Heaps[] = { TextureDescriptorHeap.Get() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);
        }

        ID3D12Resource* IndirectBuffer = GetIndirectCommandBuffer();
        ID3D12Resource* RunCountBuffer = GetMeshletRunCountBuffer();
        if (bEnableIndirectDraw && IndirectCommandSignature && IndirectBuffer && RunCountBuffer && !IndirectDrawRanges.empty())
        {
            LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            auto SelectPipelineByKey = [&](uint32_t Key)
            {
                const bool UseNormal = (Key & 1u) != 0;
                const bool UseMr = (Key & 2u) != 0;
                const bool UseBaseColor = (Key & 4u) != 0;
                const bool UseEmissive = (Key & 8u) != 0;
                const bool UseAlphaMask = (Key & 16u) != 0;

                auto SelectPipeline = [&](bool bUseAlphaMask)
                {
                    if (UseNormal)
                    {
                        if (UseEmissive)
                        {
                            if (UseBaseColor)
                            {
                                return bUseAlphaMask
                                    ? (UseMr ? PipelineStateAlphaMask.Get() : PipelineStateNoMrAlphaMask.Get())
                                    : (UseMr ? PipelineState.Get() : PipelineStateNoMr.Get());
                            }

                            return bUseAlphaMask
                                ? (UseMr ? PipelineStateNoBaseColorAlphaMask.Get() : PipelineStateNoMrNoBaseColorAlphaMask.Get())
                                : (UseMr ? PipelineStateNoBaseColor.Get() : PipelineStateNoMrNoBaseColor.Get());
                        }

                        if (UseBaseColor)
                        {
                            return bUseAlphaMask
                                ? (UseMr ? PipelineStateNoEmissiveAlphaMask.Get() : PipelineStateNoMrNoEmissiveAlphaMask.Get())
                                : (UseMr ? PipelineStateNoEmissive.Get() : PipelineStateNoMrNoEmissive.Get());
                        }

                        return bUseAlphaMask
                            ? (UseMr ? PipelineStateNoBaseColorNoEmissiveAlphaMask.Get() : PipelineStateNoMrNoBaseColorNoEmissiveAlphaMask.Get())
                            : (UseMr ? PipelineStateNoBaseColorNoEmissive.Get() : PipelineStateNoMrNoBaseColorNoEmissive.Get());
                    }

                    if (UseEmissive)
                    {
                        if (UseBaseColor)
                        {
                            return bUseAlphaMask
                                ? (UseMr ? PipelineStateNoNormalAlphaMask.Get() : PipelineStateNoMrNoNormalAlphaMask.Get())
                                : (UseMr ? PipelineStateNoNormal.Get() : PipelineStateNoMrNoNormal.Get());
                        }

                        return bUseAlphaMask
                            ? (UseMr ? PipelineStateNoBaseColorNoNormalAlphaMask.Get() : PipelineStateNoMrNoBaseColorNoNormalAlphaMask.Get())
                            : (UseMr ? PipelineStateNoBaseColorNoNormal.Get() : PipelineStateNoMrNoBaseColorNoNormal.Get());
                    }

                    if (UseBaseColor)
                    {
                        return bUseAlphaMask
                            ? (UseMr ? PipelineStateNoEmissiveNoNormalAlphaMask.Get() : PipelineStateNoMrNoEmissiveNoNormalAlphaMask.Get())
                            : (UseMr ? PipelineStateNoEmissiveNoNormal.Get() : PipelineStateNoMrNoEmissiveNoNormal.Get());
                    }

                    return bUseAlphaMask
                        ? (UseMr ? PipelineStateNoBaseColorNoEmissiveNoNormalAlphaMask.Get() : PipelineStateNoMrNoBaseColorNoEmissiveNoNormalAlphaMask.Get())
                        : (UseMr ? PipelineStateNoBaseColorNoEmissiveNoNormal.Get() : PipelineStateNoMrNoBaseColorNoEmissiveNoNormal.Get());
                };

                return SelectPipeline(UseAlphaMask);
            };

            for (size_t RangeIndex = 0; RangeIndex < IndirectDrawRanges.size(); ++RangeIndex)
            {
                const FIndirectDrawRange& Range = IndirectDrawRanges[RangeIndex];
                ID3D12PipelineState* Pipeline = SelectPipelineByKey(Range.PipelineKey);
                LocalCommandList->SetPipelineState(Pipeline);
                LocalCommandList->SetGraphicsRootDescriptorTable(1, Range.TextureHandle);

                const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
                const uint64_t CountOffset = RangeIndex * sizeof(uint32_t);
                if (AreModelPixEventsEnabled())
                {
                    const wchar_t* Label = Range.Name.empty() ? L"IndirectDrawRange" : Range.Name.c_str();
                    FScopedPixEvent ModelEvent(LocalCommandList, Label);
                    LocalCommandList->ExecuteIndirect(IndirectCommandSignature.Get(), Range.Count, IndirectBuffer, Offset, RunCountBuffer, CountOffset);
                }
                else
                {
                    LocalCommandList->ExecuteIndirect(IndirectCommandSignature.Get(), Range.Count, IndirectBuffer, Offset, RunCountBuffer, CountOffset);
                }
            }
        }
        else
        {
            for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
            {
                if (!SceneModelVisibility.empty() && !SceneModelVisibility[ModelIndex])
                {
                    continue;
                }

                const FSceneModelResource& Model = SceneModels[ModelIndex];
                const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

                LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
                LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

                const bool bUseBaseColorMap = !Model.BaseColorTexturePath.empty();
                const bool bUseMetallicRoughnessMap = !Model.MetallicRoughnessTexturePath.empty();
                const bool bUseEmissiveMap = !Model.EmissiveTexturePath.empty();
                const bool bUseNormalMap = Model.bHasNormalMap;
                const bool bUseAlphaMask = Model.AlphaMode == 1u;

                auto SelectPipeline = [&](bool UseNormal, bool UseMr, bool UseBaseColor, bool UseEmissive, bool UseAlphaMask)
                {
                    if (UseNormal)
                    {
                        if (UseEmissive)
                        {
                            if (UseBaseColor)
                            {
                                return UseAlphaMask
                                    ? (UseMr ? PipelineStateAlphaMask.Get() : PipelineStateNoMrAlphaMask.Get())
                                    : (UseMr ? PipelineState.Get() : PipelineStateNoMr.Get());
                            }

                            return UseAlphaMask
                                ? (UseMr ? PipelineStateNoBaseColorAlphaMask.Get() : PipelineStateNoMrNoBaseColorAlphaMask.Get())
                                : (UseMr ? PipelineStateNoBaseColor.Get() : PipelineStateNoMrNoBaseColor.Get());
                        }

                        if (UseBaseColor)
                        {
                            return UseAlphaMask
                                ? (UseMr ? PipelineStateNoEmissiveAlphaMask.Get() : PipelineStateNoMrNoEmissiveAlphaMask.Get())
                                : (UseMr ? PipelineStateNoEmissive.Get() : PipelineStateNoMrNoEmissive.Get());
                        }

                        return UseAlphaMask
                            ? (UseMr ? PipelineStateNoBaseColorNoEmissiveAlphaMask.Get() : PipelineStateNoMrNoBaseColorNoEmissiveAlphaMask.Get())
                            : (UseMr ? PipelineStateNoBaseColorNoEmissive.Get() : PipelineStateNoMrNoBaseColorNoEmissive.Get());
                    }

                    if (UseEmissive)
                    {
                        if (UseBaseColor)
                        {
                            return UseAlphaMask
                                ? (UseMr ? PipelineStateNoNormalAlphaMask.Get() : PipelineStateNoMrNoNormalAlphaMask.Get())
                                : (UseMr ? PipelineStateNoNormal.Get() : PipelineStateNoMrNoNormal.Get());
                        }

                        return UseAlphaMask
                            ? (UseMr ? PipelineStateNoBaseColorNoNormalAlphaMask.Get() : PipelineStateNoMrNoBaseColorNoNormalAlphaMask.Get())
                            : (UseMr ? PipelineStateNoBaseColorNoNormal.Get() : PipelineStateNoMrNoBaseColorNoNormal.Get());
                    }

                    if (UseBaseColor)
                    {
                        return UseAlphaMask
                            ? (UseMr ? PipelineStateNoEmissiveNoNormalAlphaMask.Get() : PipelineStateNoMrNoEmissiveNoNormalAlphaMask.Get())
                            : (UseMr ? PipelineStateNoEmissiveNoNormal.Get() : PipelineStateNoMrNoEmissiveNoNormal.Get());
                    }

                    return UseAlphaMask
                        ? (UseMr ? PipelineStateNoBaseColorNoEmissiveNoNormalAlphaMask.Get() : PipelineStateNoMrNoBaseColorNoEmissiveNoNormalAlphaMask.Get())
                        : (UseMr ? PipelineStateNoBaseColorNoEmissiveNoNormal.Get() : PipelineStateNoMrNoBaseColorNoEmissiveNoNormal.Get());
                };

                LocalCommandList->SetPipelineState(SelectPipeline(bUseNormalMap, bUseMetallicRoughnessMap, bUseBaseColorMap, bUseEmissiveMap, bUseAlphaMask));

                const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
                LocalCommandList->SetGraphicsRootConstantBufferView(
                    0,
                    ConstantBufferAddress + ConstantBufferOffset);
                LocalCommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

                if (AreModelPixEventsEnabled())
                {
                    const std::wstring ModelLabel = Model.Name.empty()
                        ? L"Model"
                        : std::wstring(Model.Name.begin(), Model.Name.end());
                    FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                    LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
                }
                else
                {
                    LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
                }
            }
        }
    });
}

void FForwardRenderer::AddObjectIdPass(FRenderGraph& Graph, const FCamera& Camera, const FForwardFrameState& FrameState, FRGResourceHandle ObjectIdHandle, FRGResourceHandle DepthHandle)
{
    struct FObjectIdPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Graph.AddPass<FObjectIdPassData>("ObjectId", [this, &Camera, FrameState, ObjectIdHandle, DepthHandle](FObjectIdPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bObjectIdReadbackRequested && ObjectIdPipeline && ObjectIdTexture;
        Data.Camera = &Camera;
        Data.LightViewProjection = FrameState.LightViewProjection;

        if (Data.bEnabled)
        {
            Builder.WriteTexture(ObjectIdHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
        }
    }, [this](const FObjectIdPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ObjectIdEvent(LocalCommandList, L"ObjectIdPass");

        LocalCommandList->SetPipelineState(ObjectIdPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(1, &ObjectIdRtvHandle, FALSE, &DepthHandle);

        const UINT ClearValue[4] = { 0, 0, 0, 0 };
        LocalCommandList->ClearRenderTargetView(ObjectIdRtvHandle, reinterpret_cast<const float*>(ClearValue), 0, nullptr);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);
        }

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            if (!SceneModelVisibility.empty() && !SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);
            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBufferAddress + ConstantBufferOffset);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty()
                    ? L"Model"
                    : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
            else
            {
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
        }

        const uint32_t Width = static_cast<uint32_t>(Viewport.Width);
        const uint32_t Height = static_cast<uint32_t>(Viewport.Height);
        const uint32_t ReadX = (std::min)(ObjectIdReadbackX, Width > 0 ? Width - 1 : 0);
        const uint32_t ReadY = (std::min)(ObjectIdReadbackY, Height > 0 ? Height - 1 : 0);

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = ObjectIdTexture.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        LocalCommandList->ResourceBarrier(1, &Barrier);

        D3D12_TEXTURE_COPY_LOCATION Src = {};
        Src.pResource = ObjectIdTexture.Get();
        Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst = {};
        Dst.pResource = ObjectIdReadback.Get();
        Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint = ObjectIdFootprint;

        D3D12_BOX SourceBox = {};
        SourceBox.left = ReadX;
        SourceBox.top = ReadY;
        SourceBox.front = 0;
        SourceBox.right = ReadX + 1;
        SourceBox.bottom = ReadY + 1;
        SourceBox.back = 1;

        LocalCommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &SourceBox);

        std::swap(Barrier.Transition.StateBefore, Barrier.Transition.StateAfter);
        LocalCommandList->ResourceBarrier(1, &Barrier);

        bObjectIdReadbackRecorded = true;
    });
}

void FForwardRenderer::AddDebugPrintPass(FRenderGraph& Graph, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle)
{
    struct FDebugPrintPassData
    {
        bool bEnabled = false;
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
    };

    Graph.AddPass<FDebugPrintPassData>("GpuDebugPrint", [this, RtvHandle](FDebugPrintPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bEnableGpuDebugPrint && GpuDebugPrintPipeline && GpuDebugPrintRootSignature && GpuDebugPrintDescriptorHeap;
        Data.OutputHandle = RtvHandle;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [this](const FDebugPrintPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchGpuDebugPrintStats(Cmd);
        RenderGpuDebugPrint(Cmd, Data.OutputHandle);
    });
}

void FForwardRenderer::UpdateCullingVisibility(const FCamera& Camera)
{
    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    const bool bGpuCullingActive = bEnableIndirectDraw && CullingPipeline && CullingRootSignature && GetIndirectCommandBuffer()
        && ModelBoundsBuffer && MeshletConeAxisBuffer && MeshletConeApexBuffer;
    RendererUtils::UpdateCullingVisibility(*CullingCamera, SceneModels, SceneModelVisibility, !bGpuCullingActive);
}

bool FForwardRenderer::CreateRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 DescriptorRange = {};
    DescriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    DescriptorRange.NumDescriptors = 7;
    DescriptorRange.BaseShaderRegister = 0;
    DescriptorRange.RegisterSpace = 0;
    DescriptorRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Scene constant buffer (b0), used in Shaders/ForwardVS.hlsl VSMain and Shaders/ForwardPS.hlsl PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Material/scene texture SRV table (t0..t6), used in Shaders/ForwardPS.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &DescriptorRange;

    D3D12_STATIC_SAMPLER_DESC Samplers[3] = {};
    Samplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    Samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[0].MipLODBias = 0.0f;
    Samplers[0].MaxAnisotropy = 4;
    Samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    Samplers[0].MinLOD = 0.0f;
    Samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[0].ShaderRegister = 0;
    Samplers[0].RegisterSpace = 0;
    Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    Samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    Samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    Samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    Samplers[1].MinLOD = 0.0f;
    Samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[1].ShaderRegister = 1;
    Samplers[1].RegisterSpace = 0;
    Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    Samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    Samplers[2].MinLOD = 0.0f;
    Samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[2].ShaderRegister = 2;
    Samplers[2].RegisterSpace = 0;
    Samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootDesc = {};
    RootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootDesc.Desc_1_1.pParameters = RootParams;
    RootDesc.Desc_1_1.NumStaticSamplers = _countof(Samplers);
    RootDesc.Desc_1_1.pStaticSamplers = Samplers;
    RootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob && ErrorBlob->GetBufferSize() > 0)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RootSignature.GetAddressOf())));
    return true;
}

bool FForwardRenderer::CreatePipelineState(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;
    std::vector<uint8_t> PSByteCodeNoBaseColor;
    std::vector<uint8_t> PSByteCodeNoMr;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColor;
    std::vector<uint8_t> PSByteCodeNoEmissive;
    std::vector<uint8_t> PSByteCodeNoBaseColorNoEmissive;
    std::vector<uint8_t> PSByteCodeNoMrNoEmissive;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColorNoEmissive;
    std::vector<uint8_t> PSByteCodeNoNormal;
    std::vector<uint8_t> PSByteCodeNoBaseColorNoNormal;
    std::vector<uint8_t> PSByteCodeNoMrNoNormal;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColorNoNormal;
    std::vector<uint8_t> PSByteCodeNoEmissiveNoNormal;
    std::vector<uint8_t> PSByteCodeNoBaseColorNoEmissiveNoNormal;
    std::vector<uint8_t> PSByteCodeNoMrNoEmissiveNoNormal;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColorNoEmissiveNoNormal;
    std::vector<uint8_t> PSByteCodeAlphaMask;
    std::vector<uint8_t> PSByteCodeNoBaseColorAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColorAlphaMask;
    std::vector<uint8_t> PSByteCodeNoEmissiveAlphaMask;
    std::vector<uint8_t> PSByteCodeNoBaseColorNoEmissiveAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrNoEmissiveAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColorNoEmissiveAlphaMask;
    std::vector<uint8_t> PSByteCodeNoNormalAlphaMask;
    std::vector<uint8_t> PSByteCodeNoBaseColorNoNormalAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrNoNormalAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColorNoNormalAlphaMask;
    std::vector<uint8_t> PSByteCodeNoEmissiveNoNormalAlphaMask;
    std::vector<uint8_t> PSByteCodeNoBaseColorNoEmissiveNoNormalAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrNoEmissiveNoNormalAlphaMask;
    std::vector<uint8_t> PSByteCodeNoMrNoBaseColorNoEmissiveNoNormalAlphaMask;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/ForwardVS.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const auto MakeAlphaDefines = [](const std::vector<std::wstring>& Defines)
    {
        std::vector<std::wstring> Result = Defines;
        Result.push_back(L"USE_ALPHA_MASK=1");
        return Result;
    };

    const std::vector<std::wstring> DefaultDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCode, DefaultDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeAlphaMask, MakeAlphaDefines(DefaultDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColor, NoBaseColorDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorAlphaMask, MakeAlphaDefines(NoBaseColorDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMr, NoMrDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrAlphaMask, MakeAlphaDefines(NoMrDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColor, NoMrNoBaseColorDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorAlphaMask, MakeAlphaDefines(NoMrNoBaseColorDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoEmissiveDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoEmissive, NoEmissiveDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoEmissiveAlphaMask, MakeAlphaDefines(NoEmissiveDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorNoEmissiveDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoEmissive, NoBaseColorNoEmissiveDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoEmissiveAlphaMask, MakeAlphaDefines(NoBaseColorNoEmissiveDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoEmissiveDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoEmissive, NoMrNoEmissiveDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoEmissiveAlphaMask, MakeAlphaDefines(NoMrNoEmissiveDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorNoEmissiveDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoEmissive, NoMrNoBaseColorNoEmissiveDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoEmissiveAlphaMask, MakeAlphaDefines(NoMrNoBaseColorNoEmissiveDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoNormal, NoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoNormalAlphaMask, MakeAlphaDefines(NoNormalDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoNormal, NoBaseColorNoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoNormalAlphaMask, MakeAlphaDefines(NoBaseColorNoNormalDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoNormal, NoMrNoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoNormalAlphaMask, MakeAlphaDefines(NoMrNoNormalDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoNormal, NoMrNoBaseColorNoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoNormalAlphaMask, MakeAlphaDefines(NoMrNoBaseColorNoNormalDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoEmissiveNoNormal, NoEmissiveNoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoEmissiveNoNormalAlphaMask, MakeAlphaDefines(NoEmissiveNoNormalDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorNoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoEmissiveNoNormal, NoBaseColorNoEmissiveNoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoEmissiveNoNormalAlphaMask, MakeAlphaDefines(NoBaseColorNoEmissiveNoNormalDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoEmissiveNoNormal, NoMrNoEmissiveNoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoEmissiveNoNormalAlphaMask, MakeAlphaDefines(NoMrNoEmissiveNoNormalDefines)))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorNoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoEmissiveNoNormal, NoMrNoBaseColorNoEmissiveNoNormalDefines))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoEmissiveNoNormalAlphaMask, MakeAlphaDefines(NoMrNoBaseColorNoEmissiveNoNormalDefines)))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature.Get();
    PsoDesc.InputLayout = { InputLayout, _countof(InputLayout) };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PsoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;
    PsoDesc.RasterizerState.MultisampleEnable = FALSE;
    PsoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    PsoDesc.RasterizerState.ForcedSampleCount = 0;
    PsoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    PsoDesc.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC RtBlend = {};
    RtBlend.BlendEnable = FALSE;
    RtBlend.LogicOpEnable = FALSE;
    RtBlend.SrcBlend = D3D12_BLEND_ONE;
    RtBlend.DestBlend = D3D12_BLEND_ZERO;
    RtBlend.BlendOp = D3D12_BLEND_OP_ADD;
    RtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    RtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    RtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    RtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    RtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    PsoDesc.BlendState.RenderTarget[0] = RtBlend;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    PsoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    PsoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    PsoDesc.DepthStencilState.BackFace = PsoDesc.DepthStencilState.FrontFace;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineState.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColor.data(), PSByteCodeNoBaseColor.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColor.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMr.data(), PSByteCodeNoMr.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMr.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColor.data(), PSByteCodeNoMrNoBaseColor.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColor.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoEmissive.data(), PSByteCodeNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColorNoEmissive.data(), PSByteCodeNoBaseColorNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColorNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoEmissive.data(), PSByteCodeNoMrNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColorNoEmissive.data(), PSByteCodeNoMrNoBaseColorNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColorNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoNormal.data(), PSByteCodeNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColorNoNormal.data(), PSByteCodeNoBaseColorNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColorNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoNormal.data(), PSByteCodeNoMrNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColorNoNormal.data(), PSByteCodeNoMrNoBaseColorNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColorNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoEmissiveNoNormal.data(), PSByteCodeNoEmissiveNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoEmissiveNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColorNoEmissiveNoNormal.data(), PSByteCodeNoBaseColorNoEmissiveNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColorNoEmissiveNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoEmissiveNoNormal.data(), PSByteCodeNoMrNoEmissiveNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoEmissiveNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColorNoEmissiveNoNormal.data(), PSByteCodeNoMrNoBaseColorNoEmissiveNoNormal.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColorNoEmissiveNoNormal.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeAlphaMask.data(), PSByteCodeAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColorAlphaMask.data(), PSByteCodeNoBaseColorAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColorAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrAlphaMask.data(), PSByteCodeNoMrAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColorAlphaMask.data(), PSByteCodeNoMrNoBaseColorAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColorAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoEmissiveAlphaMask.data(), PSByteCodeNoEmissiveAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoEmissiveAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColorNoEmissiveAlphaMask.data(), PSByteCodeNoBaseColorNoEmissiveAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColorNoEmissiveAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoEmissiveAlphaMask.data(), PSByteCodeNoMrNoEmissiveAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoEmissiveAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColorNoEmissiveAlphaMask.data(), PSByteCodeNoMrNoBaseColorNoEmissiveAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColorNoEmissiveAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoNormalAlphaMask.data(), PSByteCodeNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoNormalAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColorNoNormalAlphaMask.data(), PSByteCodeNoBaseColorNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColorNoNormalAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoNormalAlphaMask.data(), PSByteCodeNoMrNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoNormalAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColorNoNormalAlphaMask.data(), PSByteCodeNoMrNoBaseColorNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColorNoNormalAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoEmissiveNoNormalAlphaMask.data(), PSByteCodeNoEmissiveNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoEmissiveNoNormalAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoBaseColorNoEmissiveNoNormalAlphaMask.data(), PSByteCodeNoBaseColorNoEmissiveNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoBaseColorNoEmissiveNoNormalAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoEmissiveNoNormalAlphaMask.data(), PSByteCodeNoMrNoEmissiveNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoEmissiveNoNormalAlphaMask.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeNoMrNoBaseColorNoEmissiveNoNormalAlphaMask.data(), PSByteCodeNoMrNoBaseColorNoEmissiveNoNormalAlphaMask.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PipelineStateNoMrNoBaseColorNoEmissiveNoNormalAlphaMask.GetAddressOf())));

    if (bDepthPrepassEnabled)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC DepthPrepassDesc = PsoDesc;
        DepthPrepassDesc.PS = { nullptr, 0 };
        DepthPrepassDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        DepthPrepassDesc.NumRenderTargets = 0;
        DepthPrepassDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;

        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&DepthPrepassDesc, IID_PPV_ARGS(DepthPrepassPipeline.GetAddressOf())));
    }

    return true;
}

bool FForwardRenderer::CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models)
{
    if (!TextureLoader)
    {
        return false;
    }

    SceneTextures.clear();
    SceneTextures.reserve(Models.size() * 4); // 4 textures per model (base color + metallic roughness + normal + emissive)

    if (!ShadowMap)
    {
        return false;
    }

    // Prepare parallel texture loading
    struct FTextureLoadResult
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> BaseColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> MetallicRoughness;
        Microsoft::WRL::ComPtr<ID3D12Resource> Normal;
        Microsoft::WRL::ComPtr<ID3D12Resource> Emissive;
    };
    
    std::vector<FTextureLoadResult> LoadResults(Models.size());
    std::vector<FTextureLoadRequest> Requests;
    Requests.reserve(Models.size() * 4);

    const auto ClampToByte = [](float Value)
    {
        const float Clamped = (std::max)(0.0f, (std::min)(1.0f, Value));
        return static_cast<uint32_t>(std::round(Clamped * 255.0f));
    };

    const auto PackColor = [&ClampToByte](const DirectX::XMFLOAT3& Color)
    {
        const uint32_t R = ClampToByte(Color.x);
        const uint32_t G = ClampToByte(Color.y);
        const uint32_t B = ClampToByte(Color.z);
        return 0xff000000 | (B << 16) | (G << 8) | R;
    };

    // Build load requests for all textures
    for (size_t Index = 0; Index < Models.size(); ++Index)
    {
        if (!Models[Index].BaseColorTexturePath.empty())
        {
            FTextureLoadRequest BaseColorRequest;
            BaseColorRequest.Path = Models[Index].BaseColorTexturePath;
            BaseColorRequest.bUseSolidColor = false;
            BaseColorRequest.bUseSRGB = true;
            BaseColorRequest.OutTexture = &LoadResults[Index].BaseColor;
            Requests.push_back(BaseColorRequest);
        }

        if (!Models[Index].MetallicRoughnessTexturePath.empty())
        {
            FTextureLoadRequest MetallicRoughnessRequest;
            MetallicRoughnessRequest.Path = Models[Index].MetallicRoughnessTexturePath;
            MetallicRoughnessRequest.bUseSolidColor = false;
            MetallicRoughnessRequest.OutTexture = &LoadResults[Index].MetallicRoughness;
            Requests.push_back(MetallicRoughnessRequest);
        }

        if (!Models[Index].NormalTexturePath.empty())
        {
            FTextureLoadRequest NormalRequest;
            NormalRequest.Path = Models[Index].NormalTexturePath;
            NormalRequest.bUseSolidColor = false;
            NormalRequest.OutTexture = &LoadResults[Index].Normal;
            Requests.push_back(NormalRequest);
        }

        if (!Models[Index].EmissiveTexturePath.empty())
        {
            FTextureLoadRequest EmissiveRequest;
            EmissiveRequest.Path = Models[Index].EmissiveTexturePath;
            EmissiveRequest.bUseSolidColor = false;
            EmissiveRequest.bUseSRGB = true;
            EmissiveRequest.OutTexture = &LoadResults[Index].Emissive;
            Requests.push_back(EmissiveRequest);
        }
    }

    // Load all textures in parallel
    LogInfo("Loading " + std::to_string(Requests.size()) + " textures in parallel for " + std::to_string(Models.size()) + " models");
    if (!TextureLoader->LoadTexturesParallel(Requests))
    {
        LogError("Failed to load scene textures");
        return false;
    }

    // Create descriptor heap and SRVs
    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = static_cast<UINT>(Models.size() * 7);
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(TextureDescriptorHeap.GetAddressOf())));
    if (TextureDescriptorHeap)
    {
        TextureDescriptorHeap->SetName(L"TextureDescriptorHeap");
    }

    const UINT DescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = TextureDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle = TextureDescriptorHeap->GetGPUDescriptorHandleForHeapStart();

    const auto CreateSceneTextureSrv = [&](ID3D12Resource* Texture)
    {
        ID3D12Resource* Resource = Texture ? Texture : NullTexture.Get();
        if (!Resource)
        {
            return;
        }

        const D3D12_RESOURCE_DESC TextureDesc = Resource->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
        SrvDesc.Format = TextureDesc.Format;
        SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Texture2D.MipLevels = TextureDesc.MipLevels;
        SrvDesc.Texture2D.MostDetailedMip = 0;
        SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        Device->GetDevice()->CreateShaderResourceView(Resource, &SrvDesc, CpuHandle);
    };

    D3D12_SHADER_RESOURCE_VIEW_DESC ShadowSrvDesc = {};
    ShadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ShadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ShadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ShadowSrvDesc.Texture2D.MipLevels = 1;
    ShadowSrvDesc.Texture2D.MostDetailedMip = 0;
    ShadowSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_SHADER_RESOURCE_VIEW_DESC EnvSrvDesc = {};
    EnvSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    EnvSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    EnvSrvDesc.Format = EnvironmentCubeTexture ? EnvironmentCubeTexture->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
    EnvSrvDesc.TextureCube.MipLevels = EnvironmentCubeTexture ? EnvironmentCubeTexture->GetDesc().MipLevels : 1;
    EnvSrvDesc.TextureCube.MostDetailedMip = 0;
    EnvSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    D3D12_SHADER_RESOURCE_VIEW_DESC BrdfSrvDesc = {};
    BrdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    BrdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    BrdfSrvDesc.Format = BrdfLutTexture ? BrdfLutTexture->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
    BrdfSrvDesc.Texture2D.MipLevels = BrdfLutTexture ? BrdfLutTexture->GetDesc().MipLevels : 1;
    BrdfSrvDesc.Texture2D.MostDetailedMip = 0;
    BrdfSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    for (size_t Index = 0; Index < Models.size(); ++Index)
    {
        SceneTextures.push_back(LoadResults[Index].BaseColor);
        if (LoadResults[Index].BaseColor)
        {
            const std::wstring Name = L"BaseColorTexture_" + std::to_wstring(Index);
            LoadResults[Index].BaseColor->SetName(Name.c_str());
        }
        CreateSceneTextureSrv(LoadResults[Index].BaseColor.Get());
        SceneModels[Index].TextureHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        SceneTextures.push_back(LoadResults[Index].MetallicRoughness);
        if (LoadResults[Index].MetallicRoughness)
        {
            const std::wstring Name = L"MetallicRoughnessTexture_" + std::to_wstring(Index);
            LoadResults[Index].MetallicRoughness->SetName(Name.c_str());
        }
        CreateSceneTextureSrv(LoadResults[Index].MetallicRoughness.Get());

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        SceneTextures.push_back(LoadResults[Index].Normal);
        if (LoadResults[Index].Normal)
        {
            const std::wstring Name = L"NormalTexture_" + std::to_wstring(Index);
            LoadResults[Index].Normal->SetName(Name.c_str());
        }
        CreateSceneTextureSrv(LoadResults[Index].Normal.Get());

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        SceneTextures.push_back(LoadResults[Index].Emissive);
        if (LoadResults[Index].Emissive)
        {
            const std::wstring Name = L"EmissiveTexture_" + std::to_wstring(Index);
            LoadResults[Index].Emissive->SetName(Name.c_str());
        }
        CreateSceneTextureSrv(LoadResults[Index].Emissive.Get());

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        Device->GetDevice()->CreateShaderResourceView(ShadowMap.Get(), &ShadowSrvDesc, CpuHandle);

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        Device->GetDevice()->CreateShaderResourceView(EnvironmentCubeTexture.Get(), &EnvSrvDesc, CpuHandle);

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        Device->GetDevice()->CreateShaderResourceView(BrdfLutTexture.Get(), &BrdfSrvDesc, CpuHandle);

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    SceneTextureGpuHandle = TextureDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    return true;
}

bool FForwardRenderer::CreateGpuDrivenResources(FDX12Device* Device)
{
    if (!Device || SceneModels.empty() || !GetSceneConstantBuffer())
    {
        return false;
    }

    IndirectDrawRanges.clear();

    std::vector<uint32_t> SortedIndices(SceneModels.size());
    for (uint32_t Index = 0; Index < SortedIndices.size(); ++Index)
    {
        SortedIndices[Index] = Index;
    }

    std::sort(SortedIndices.begin(), SortedIndices.end(), [&](uint32_t A, uint32_t B)
    {
        const FSceneModelResource& ModelA = SceneModels[A];
        const FSceneModelResource& ModelB = SceneModels[B];
        const uint32_t KeyA = BuildPipelineKey(ModelA);
        const uint32_t KeyB = BuildPipelineKey(ModelB);
        if (KeyA != KeyB)
        {
            return KeyA < KeyB;
        }
        return ModelA.TextureHandle.ptr < ModelB.TextureHandle.ptr;
    });

    size_t TotalCommandCount = 0;
    for (const FSceneModelResource& Model : SceneModels)
    {
        if (Model.bUseMeshletCulling && !Model.Meshlets.empty())
        {
            TotalCommandCount += Model.Meshlets.size();
        }
        else
        {
            TotalCommandCount += 1;
        }
    }

    std::vector<FIndirectDrawCommand> Commands;
    Commands.reserve(TotalCommandCount);

    std::vector<FMeshletDrawData> MeshletDrawData;
    MeshletDrawData.reserve(TotalCommandCount);

    std::vector<DirectX::XMFLOAT4> Bounds;
    Bounds.reserve(TotalCommandCount);

    std::vector<DirectX::XMFLOAT4> MeshletConeAxisCutoff;
    std::vector<DirectX::XMFLOAT4> MeshletConeApex;
    MeshletConeAxisCutoff.reserve(TotalCommandCount);
    MeshletConeApex.reserve(TotalCommandCount);

    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = GetSceneConstantBufferAddress();
    uint32_t GroupIndex = 0;
    auto AppendIndirectDrawData = [&](uint32_t SortedIndex)
    {
        const FSceneModelResource& Model = SceneModels[SortedIndex];
        const uint32_t PipelineKey = BuildPipelineKey(Model);

        if (IndirectDrawRanges.empty()
            || IndirectDrawRanges.back().PipelineKey != PipelineKey
            || IndirectDrawRanges.back().TextureHandle.ptr != Model.TextureHandle.ptr)
        {
            FIndirectDrawRange Range;
            Range.Start = static_cast<uint32_t>(Commands.size());
            Range.Count = 0;
            Range.PipelineKey = PipelineKey;
            Range.TextureHandle = Model.TextureHandle;
            if (!Model.Name.empty())
            {
                Range.Name.assign(Model.Name.begin(), Model.Name.end());
            }
            IndirectDrawRanges.push_back(Range);
        }

        const uint32_t RangeIndex = static_cast<uint32_t>(IndirectDrawRanges.size() - 1);
        if (Model.bUseMeshletCulling && !Model.Meshlets.empty() && !Model.MeshletBounds.empty())
        {
            const DirectX::XMMATRIX World = DirectX::XMLoadFloat4x4(&Model.WorldMatrix);
            const DirectX::XMMATRIX NormalMatrix = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, World));
            const float ScaleX = std::sqrt(Model.WorldMatrix._11 * Model.WorldMatrix._11 + Model.WorldMatrix._21 * Model.WorldMatrix._21 + Model.WorldMatrix._31 * Model.WorldMatrix._31);
            const float ScaleY = std::sqrt(Model.WorldMatrix._12 * Model.WorldMatrix._12 + Model.WorldMatrix._22 * Model.WorldMatrix._22 + Model.WorldMatrix._32 * Model.WorldMatrix._32);
            const float ScaleZ = std::sqrt(Model.WorldMatrix._13 * Model.WorldMatrix._13 + Model.WorldMatrix._23 * Model.WorldMatrix._23 + Model.WorldMatrix._33 * Model.WorldMatrix._33);
            const float ModelScale = (std::max)((std::max)(ScaleX, ScaleY), ScaleZ);

            const size_t MeshletCount = (std::min)(Model.Meshlets.size(), Model.MeshletBounds.size());
            for (size_t MeshletIndex = 0; MeshletIndex < MeshletCount; ++MeshletIndex)
            {
                const FMesh::FMeshlet& Meshlet = Model.Meshlets[MeshletIndex];
                const FMesh::FMeshletBounds& BoundsData = Model.MeshletBounds[MeshletIndex];

                FIndirectDrawCommand Command = {};
                Command.VertexBufferView = Model.Geometry.VertexBufferView;
                Command.IndexBufferView = Model.Geometry.IndexBufferView;
                Command.ConstantBufferAddress = ConstantBufferBase + SceneConstantBufferStride * SortedIndex;
                Command.DrawArguments.IndexCountPerInstance = Meshlet.IndexCount;
                Command.DrawArguments.InstanceCount = 1;
                Command.DrawArguments.StartIndexLocation = Meshlet.IndexOffset;
                Command.DrawArguments.BaseVertexLocation = 0;
                Command.DrawArguments.StartInstanceLocation = SortedIndex;
                Commands.push_back(Command);

                FMeshletDrawData DrawData{};
                DrawData.StartIndex = Meshlet.IndexOffset;
                DrawData.IndexCount = Meshlet.IndexCount;
                DrawData.RangeIndex = RangeIndex;
                DrawData.GroupIndex = GroupIndex;
                MeshletDrawData.push_back(DrawData);

                const DirectX::XMVECTOR LocalCenter = DirectX::XMLoadFloat3(&BoundsData.Center);
                const DirectX::XMVECTOR WorldCenter = DirectX::XMVector3TransformCoord(LocalCenter, World);
                DirectX::XMFLOAT3 Center{};
                DirectX::XMStoreFloat3(&Center, WorldCenter);
                const float Radius = BoundsData.Radius * ModelScale;
                Bounds.emplace_back(Center.x, Center.y, Center.z, Radius);

                const DirectX::XMVECTOR LocalAxis = DirectX::XMLoadFloat3(&BoundsData.ConeAxis);
                const DirectX::XMVECTOR WorldAxis = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(LocalAxis, NormalMatrix));
                DirectX::XMFLOAT3 Axis{};
                DirectX::XMStoreFloat3(&Axis, WorldAxis);
                MeshletConeAxisCutoff.emplace_back(Axis.x, Axis.y, Axis.z, BoundsData.ConeCutoff);

                const DirectX::XMVECTOR LocalApex = DirectX::XMLoadFloat3(&BoundsData.ConeApex);
                const DirectX::XMVECTOR WorldApex = DirectX::XMVector3TransformCoord(LocalApex, World);
                DirectX::XMFLOAT3 Apex{};
                DirectX::XMStoreFloat3(&Apex, WorldApex);
                MeshletConeApex.emplace_back(Apex.x, Apex.y, Apex.z, 0.0f);

                IndirectDrawRanges.back().Count += 1;
            }
        }
        else
        {
            FIndirectDrawCommand Command = {};
            Command.VertexBufferView = Model.Geometry.VertexBufferView;
            Command.IndexBufferView = Model.Geometry.IndexBufferView;
            Command.ConstantBufferAddress = ConstantBufferBase + SceneConstantBufferStride * SortedIndex;
            Command.DrawArguments.IndexCountPerInstance = Model.DrawIndexCount;
            Command.DrawArguments.InstanceCount = 1;
            Command.DrawArguments.StartIndexLocation = Model.DrawIndexStart;
            Command.DrawArguments.BaseVertexLocation = 0;
            Command.DrawArguments.StartInstanceLocation = SortedIndex;
            Commands.push_back(Command);

            FMeshletDrawData DrawData{};
            DrawData.StartIndex = Model.DrawIndexStart;
            DrawData.IndexCount = Model.DrawIndexCount;
            DrawData.RangeIndex = RangeIndex;
            DrawData.GroupIndex = GroupIndex;
            MeshletDrawData.push_back(DrawData);

            const DirectX::XMFLOAT3 Center{
                (Model.BoundsMin.x + Model.BoundsMax.x) * 0.5f,
                (Model.BoundsMin.y + Model.BoundsMax.y) * 0.5f,
                (Model.BoundsMin.z + Model.BoundsMax.z) * 0.5f
            };
            const float Radius = std::sqrt(
                (Model.BoundsMax.x - Center.x) * (Model.BoundsMax.x - Center.x) +
                (Model.BoundsMax.y - Center.y) * (Model.BoundsMax.y - Center.y) +
                (Model.BoundsMax.z - Center.z) * (Model.BoundsMax.z - Center.z));
            Bounds.emplace_back(Center.x, Center.y, Center.z, Radius);
            MeshletConeAxisCutoff.emplace_back(0.0f, 0.0f, 1.0f, -1.0f);
            MeshletConeApex.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
            IndirectDrawRanges.back().Count += 1;
        }

        GroupIndex += 1;
    };

    for (uint32_t SortedIndex : SortedIndices)
    {
        AppendIndirectDrawData(SortedIndex);
    }

    IndirectCommandCount = static_cast<uint32_t>(Commands.size());

    std::vector<uint32_t> RangeOffsets;
    RangeOffsets.reserve(IndirectDrawRanges.size());
    for (const FIndirectDrawRange& Range : IndirectDrawRanges)
    {
        RangeOffsets.push_back(Range.Start);
    }

    const uint64_t CommandBufferSize = sizeof(FIndirectDrawCommand) * Commands.size();
    const uint64_t MeshletDrawDataSize = sizeof(FMeshletDrawData) * MeshletDrawData.size();
    const uint64_t RangeOffsetSize = sizeof(uint32_t) * RangeOffsets.size();

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    UploadHeap.CreationNodeMask = 1;
    UploadHeap.VisibleNodeMask = 1;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC BufferDesc = {};
    BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufferDesc.Width = CommandBufferSize;
    BufferDesc.Height = 1;
    BufferDesc.DepthOrArraySize = 1;
    BufferDesc.MipLevels = 1;
    BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    BufferDesc.SampleDesc.Count = 1;
    BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    BufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC UploadDesc = BufferDesc;
    UploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_DESC TemplateDesc = BufferDesc;
    TemplateDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_RANGE EmptyRange = { 0, 0 };
    IndirectCommandBuffers.clear();
    IndirectCommandTemplateBuffers.clear();
    IndirectCommandStates.clear();
    MeshletVisibilityBuffers.clear();
    MeshletRunCountBuffers.clear();
    MeshletVisibilityStates.clear();
    MeshletRunCountStates.clear();
    IndirectCommandBuffers.resize(GetFramesInFlight());
    IndirectCommandTemplateBuffers.resize(GetFramesInFlight());
    IndirectCommandStates.resize(GetFramesInFlight(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    MeshletVisibilityBuffers.resize(GetFramesInFlight());
    MeshletRunCountBuffers.resize(GetFramesInFlight());
    MeshletVisibilityStates.resize(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    MeshletRunCountStates.resize(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> IndirectCommandUploads;
    IndirectCommandUploads.resize(GetFramesInFlight());
    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &BufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(IndirectCommandBuffers[FrameIndex].GetAddressOf())));
        if (IndirectCommandBuffers[FrameIndex])
        {
            const std::wstring Name = L"IndirectCommandBuffer_Frame" + std::to_wstring(FrameIndex);
            IndirectCommandBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &TemplateDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(IndirectCommandTemplateBuffers[FrameIndex].GetAddressOf())));
        if (IndirectCommandTemplateBuffers[FrameIndex])
        {
            const std::wstring Name = L"IndirectCommandTemplateBuffer_Frame" + std::to_wstring(FrameIndex);
            IndirectCommandTemplateBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &UploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &UploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(IndirectCommandUploads[FrameIndex].GetAddressOf())));

        if (IndirectCommandUploads[FrameIndex])
        {
            void* UploadData = nullptr;
            HR_CHECK(IndirectCommandUploads[FrameIndex]->Map(0, &EmptyRange, &UploadData));
            std::vector<FIndirectDrawCommand> FrameCommands = Commands;
            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = SceneConstantBuffers[FrameIndex]
                ? SceneConstantBuffers[FrameIndex]->GetGPUVirtualAddress()
                : 0;
            for (uint32_t CommandIndex = 0; CommandIndex < FrameCommands.size(); ++CommandIndex)
            {
                const uint32_t InstanceIndex = FrameCommands[CommandIndex].DrawArguments.StartInstanceLocation;
                FrameCommands[CommandIndex].ConstantBufferAddress =
                    ConstantBufferBase + SceneConstantBufferStride * InstanceIndex;
            }
            std::memcpy(UploadData, FrameCommands.data(), CommandBufferSize);
            IndirectCommandUploads[FrameIndex]->Unmap(0, nullptr);
        }

        D3D12_RESOURCE_DESC VisibilityDesc = BufferDesc;
        VisibilityDesc.Width = sizeof(uint32_t) * IndirectCommandCount;
        VisibilityDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &VisibilityDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(MeshletVisibilityBuffers[FrameIndex].GetAddressOf())));
        if (MeshletVisibilityBuffers[FrameIndex])
        {
            const std::wstring Name = L"MeshletVisibilityBuffer_Frame" + std::to_wstring(FrameIndex);
            MeshletVisibilityBuffers[FrameIndex]->SetName(Name.c_str());
        }

        D3D12_RESOURCE_DESC RunCountDesc = BufferDesc;
        RunCountDesc.Width = sizeof(uint32_t) * RangeOffsets.size();
        RunCountDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RunCountDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(MeshletRunCountBuffers[FrameIndex].GetAddressOf())));
        if (MeshletRunCountBuffers[FrameIndex])
        {
            const std::wstring Name = L"MeshletRunCountBuffer_Frame" + std::to_wstring(FrameIndex);
            MeshletRunCountBuffers[FrameIndex]->SetName(Name.c_str());
        }
    }

    const uint64_t BoundsBufferSize = sizeof(DirectX::XMFLOAT4) * Bounds.size();
    D3D12_RESOURCE_DESC BoundsDesc = BufferDesc;
    BoundsDesc.Width = BoundsBufferSize;
    BoundsDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_DESC BoundsUploadDesc = BoundsDesc;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &BoundsDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(ModelBoundsBuffer.GetAddressOf())));
    if (ModelBoundsBuffer)
    {
        ModelBoundsBuffer->SetName(L"ModelBoundsBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &BoundsUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(ModelBoundsUpload.GetAddressOf())));
    if (ModelBoundsUpload)
    {
        ModelBoundsUpload->SetName(L"ModelBoundsUpload");
    }

    void* UploadData = nullptr;
    HR_CHECK(ModelBoundsUpload->Map(0, &EmptyRange, &UploadData));
    std::memcpy(UploadData, Bounds.data(), BoundsBufferSize);
    ModelBoundsUpload->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC DrawDataDesc = BoundsDesc;
    DrawDataDesc.Width = MeshletDrawDataSize;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &DrawDataDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletDrawDataBuffer.GetAddressOf())));
    if (MeshletDrawDataBuffer)
    {
        MeshletDrawDataBuffer->SetName(L"MeshletDrawDataBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &DrawDataDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletDrawDataUpload.GetAddressOf())));
    if (MeshletDrawDataUpload)
    {
        MeshletDrawDataUpload->SetName(L"MeshletDrawDataUpload");
    }

    HR_CHECK(MeshletDrawDataUpload->Map(0, &EmptyRange, &UploadData));
    std::memcpy(UploadData, MeshletDrawData.data(), MeshletDrawDataSize);
    MeshletDrawDataUpload->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC RangeOffsetDesc = BoundsDesc;
    RangeOffsetDesc.Width = RangeOffsetSize;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &RangeOffsetDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletRangeOffsetBuffer.GetAddressOf())));
    if (MeshletRangeOffsetBuffer)
    {
        MeshletRangeOffsetBuffer->SetName(L"MeshletRangeOffsetBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &RangeOffsetDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletRangeOffsetUpload.GetAddressOf())));
    if (MeshletRangeOffsetUpload)
    {
        MeshletRangeOffsetUpload->SetName(L"MeshletRangeOffsetUpload");
    }

    HR_CHECK(MeshletRangeOffsetUpload->Map(0, &EmptyRange, &UploadData));
    std::memcpy(UploadData, RangeOffsets.data(), RangeOffsetSize);
    MeshletRangeOffsetUpload->Unmap(0, nullptr);

    const uint64_t ConeBufferSize = sizeof(DirectX::XMFLOAT4) * MeshletConeAxisCutoff.size();
    D3D12_RESOURCE_DESC ConeDesc = BoundsDesc;
    ConeDesc.Width = ConeBufferSize;

    D3D12_RESOURCE_DESC ConeUploadDesc = ConeDesc;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletConeAxisBuffer.GetAddressOf())));
    if (MeshletConeAxisBuffer)
    {
        MeshletConeAxisBuffer->SetName(L"MeshletConeAxisBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletConeAxisUpload.GetAddressOf())));
    if (MeshletConeAxisUpload)
    {
        MeshletConeAxisUpload->SetName(L"MeshletConeAxisUpload");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletConeApexBuffer.GetAddressOf())));
    if (MeshletConeApexBuffer)
    {
        MeshletConeApexBuffer->SetName(L"MeshletConeApexBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletConeApexUpload.GetAddressOf())));
    if (MeshletConeApexUpload)
    {
        MeshletConeApexUpload->SetName(L"MeshletConeApexUpload");
    }

    void* ConeAxisData = nullptr;
    HR_CHECK(MeshletConeAxisUpload->Map(0, &EmptyRange, &ConeAxisData));
    std::memcpy(ConeAxisData, MeshletConeAxisCutoff.data(), ConeBufferSize);
    MeshletConeAxisUpload->Unmap(0, nullptr);

    void* ConeApexData = nullptr;
    HR_CHECK(MeshletConeApexUpload->Map(0, &EmptyRange, &ConeApexData));
    std::memcpy(ConeApexData, MeshletConeApex.data(), ConeBufferSize);
    MeshletConeApexUpload->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC DebugDesc = BufferDesc;
    DebugDesc.Width = GpuDebugPrintBufferSize;
    DebugDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &DebugDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintBuffer.GetAddressOf())));
    if (GpuDebugPrintBuffer)
    {
        GpuDebugPrintBuffer->SetName(L"GpuDebugPrintBuffer");
    }

    D3D12_RESOURCE_DESC DebugUploadDesc = {};
    DebugUploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    DebugUploadDesc.Width = sizeof(uint32_t);
    DebugUploadDesc.Height = 1;
    DebugUploadDesc.DepthOrArraySize = 1;
    DebugUploadDesc.MipLevels = 1;
    DebugUploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    DebugUploadDesc.SampleDesc.Count = 1;
    DebugUploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DebugUploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &DebugUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintUpload.GetAddressOf())));
    if (GpuDebugPrintUpload)
    {
        GpuDebugPrintUpload->SetName(L"GpuDebugPrintUpload");
        void* DebugUploadData = nullptr;
        HR_CHECK(GpuDebugPrintUpload->Map(0, &EmptyRange, &DebugUploadData));
        if (DebugUploadData)
        {
            std::memset(DebugUploadData, 0, sizeof(uint32_t));
        }
        GpuDebugPrintUpload->Unmap(0, nullptr);
    }

    D3D12_RESOURCE_DESC StatsDesc = {};
    StatsDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    StatsDesc.Width = sizeof(uint32_t) * 3;
    StatsDesc.Height = 1;
    StatsDesc.DepthOrArraySize = 1;
    StatsDesc.MipLevels = 1;
    StatsDesc.Format = DXGI_FORMAT_UNKNOWN;
    StatsDesc.SampleDesc.Count = 1;
    StatsDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    StatsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &StatsDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintStatsBuffer.GetAddressOf())));
    if (GpuDebugPrintStatsBuffer)
    {
        GpuDebugPrintStatsBuffer->SetName(L"GpuDebugPrintStatsBuffer");
    }

    D3D12_RESOURCE_DESC StatsUploadDesc = StatsDesc;
    StatsUploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &StatsUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintStatsUpload.GetAddressOf())));
    if (GpuDebugPrintStatsUpload)
    {
        GpuDebugPrintStatsUpload->SetName(L"GpuDebugPrintStatsUpload");
        void* StatsUploadData = nullptr;
        HR_CHECK(GpuDebugPrintStatsUpload->Map(0, &EmptyRange, &StatsUploadData));
        if (StatsUploadData)
        {
            std::memset(StatsUploadData, 0, sizeof(uint32_t) * 3);
        }
        GpuDebugPrintStatsUpload->Unmap(0, nullptr);
    }

    if (!GpuDebugPrintBuffer || !GpuDebugPrintUpload || !GpuDebugPrintStatsBuffer || !GpuDebugPrintStatsUpload)
    {
        LogError("Failed to create GPU debug print resources");
        return false;
    }

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

    std::vector<D3D12_RESOURCE_BARRIER> PreCopyBarriers;
    PreCopyBarriers.reserve(GetFramesInFlight() * 2 + 7);
    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = IndirectCommandTemplateBuffers[FrameIndex].Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        PreCopyBarriers.push_back(Barrier);
    }
    D3D12_RESOURCE_BARRIER DrawDataBarrier = {};
    DrawDataBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    DrawDataBarrier.Transition.pResource = MeshletDrawDataBuffer.Get();
    DrawDataBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    DrawDataBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    DrawDataBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(DrawDataBarrier);

    D3D12_RESOURCE_BARRIER RangeOffsetBarrier = {};
    RangeOffsetBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    RangeOffsetBarrier.Transition.pResource = MeshletRangeOffsetBuffer.Get();
    RangeOffsetBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    RangeOffsetBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    RangeOffsetBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(RangeOffsetBarrier);
    D3D12_RESOURCE_BARRIER BoundsBarrier = {};
    BoundsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    BoundsBarrier.Transition.pResource = ModelBoundsBuffer.Get();
    BoundsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    BoundsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    BoundsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(BoundsBarrier);

    D3D12_RESOURCE_BARRIER ConeAxisBarrier = {};
    ConeAxisBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ConeAxisBarrier.Transition.pResource = MeshletConeAxisBuffer.Get();
    ConeAxisBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ConeAxisBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    ConeAxisBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(ConeAxisBarrier);

    D3D12_RESOURCE_BARRIER ConeApexBarrier = {};
    ConeApexBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ConeApexBarrier.Transition.pResource = MeshletConeApexBuffer.Get();
    ConeApexBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ConeApexBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    ConeApexBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(ConeApexBarrier);

    D3D12_RESOURCE_BARRIER DebugBarrier = {};
    DebugBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    DebugBarrier.Transition.pResource = GpuDebugPrintBuffer.Get();
    DebugBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    DebugBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    DebugBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(DebugBarrier);

    D3D12_RESOURCE_BARRIER StatsBarrier = {};
    StatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    StatsBarrier.Transition.pResource = GpuDebugPrintStatsBuffer.Get();
    StatsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    StatsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    StatsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(StatsBarrier);

    UploadList->ResourceBarrier(static_cast<UINT>(PreCopyBarriers.size()), PreCopyBarriers.data());

    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        UploadList->CopyBufferRegion(
            IndirectCommandTemplateBuffers[FrameIndex].Get(),
            0,
            IndirectCommandUploads[FrameIndex].Get(),
            0,
            CommandBufferSize);
    }
    UploadList->CopyBufferRegion(ModelBoundsBuffer.Get(), 0, ModelBoundsUpload.Get(), 0, BoundsBufferSize);
    UploadList->CopyBufferRegion(MeshletDrawDataBuffer.Get(), 0, MeshletDrawDataUpload.Get(), 0, MeshletDrawDataSize);
    UploadList->CopyBufferRegion(MeshletRangeOffsetBuffer.Get(), 0, MeshletRangeOffsetUpload.Get(), 0, RangeOffsetSize);
    UploadList->CopyBufferRegion(MeshletConeAxisBuffer.Get(), 0, MeshletConeAxisUpload.Get(), 0, ConeBufferSize);
    UploadList->CopyBufferRegion(MeshletConeApexBuffer.Get(), 0, MeshletConeApexUpload.Get(), 0, ConeBufferSize);
    if (GpuDebugPrintBuffer && GpuDebugPrintUpload)
    {
        UploadList->CopyBufferRegion(GpuDebugPrintBuffer.Get(), 0, GpuDebugPrintUpload.Get(), 0, sizeof(uint32_t));
    }
    if (GpuDebugPrintStatsBuffer && GpuDebugPrintStatsUpload)
    {
        UploadList->CopyBufferRegion(GpuDebugPrintStatsBuffer.Get(), 0, GpuDebugPrintStatsUpload.Get(), 0, sizeof(uint32_t) * 3);
    }

    std::vector<D3D12_RESOURCE_BARRIER> PostCopyBarriers;
    PostCopyBarriers.reserve(GetFramesInFlight() * 2 + 7);
    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = IndirectCommandTemplateBuffers[FrameIndex].Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        PostCopyBarriers.push_back(Barrier);
    }

    D3D12_RESOURCE_BARRIER PostBoundsBarrier = {};
    PostBoundsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostBoundsBarrier.Transition.pResource = ModelBoundsBuffer.Get();
    PostBoundsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostBoundsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostBoundsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostBoundsBarrier);

    D3D12_RESOURCE_BARRIER PostDrawDataBarrier = {};
    PostDrawDataBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostDrawDataBarrier.Transition.pResource = MeshletDrawDataBuffer.Get();
    PostDrawDataBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostDrawDataBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostDrawDataBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostDrawDataBarrier);

    D3D12_RESOURCE_BARRIER PostRangeOffsetBarrier = {};
    PostRangeOffsetBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostRangeOffsetBarrier.Transition.pResource = MeshletRangeOffsetBuffer.Get();
    PostRangeOffsetBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostRangeOffsetBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostRangeOffsetBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostRangeOffsetBarrier);

    D3D12_RESOURCE_BARRIER PostConeAxisBarrier = {};
    PostConeAxisBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostConeAxisBarrier.Transition.pResource = MeshletConeAxisBuffer.Get();
    PostConeAxisBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostConeAxisBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostConeAxisBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostConeAxisBarrier);

    D3D12_RESOURCE_BARRIER PostConeApexBarrier = {};
    PostConeApexBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostConeApexBarrier.Transition.pResource = MeshletConeApexBuffer.Get();
    PostConeApexBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostConeApexBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostConeApexBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostConeApexBarrier);

    D3D12_RESOURCE_BARRIER PostDebugBarrier = {};
    PostDebugBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostDebugBarrier.Transition.pResource = GpuDebugPrintBuffer.Get();
    PostDebugBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostDebugBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostDebugBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PostCopyBarriers.push_back(PostDebugBarrier);

    D3D12_RESOURCE_BARRIER PostStatsBarrier = {};
    PostStatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostStatsBarrier.Transition.pResource = GpuDebugPrintStatsBuffer.Get();
    PostStatsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostStatsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostStatsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PostCopyBarriers.push_back(PostStatsBarrier);

    UploadList->ResourceBarrier(static_cast<UINT>(PostCopyBarriers.size()), PostCopyBarriers.data());

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    IndirectCommandStates.assign(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    MeshletVisibilityStates.assign(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    MeshletRunCountStates.assign(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    GpuDebugPrintState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_ROOT_PARAMETER1 RootParams[8] = {};
    // RootParams[0]: CullingConstants CBV (b0), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[1]: ModelBounds SRV (t0), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RootParams[1].Descriptor.ShaderRegister = 0;
    RootParams[1].Descriptor.RegisterSpace = 0;
    RootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[2]: MeshletConeAxisCutoff SRV (t2), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RootParams[2].Descriptor.ShaderRegister = 2;
    RootParams[2].Descriptor.RegisterSpace = 0;
    RootParams[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[3]: MeshletConeApex SRV (t3), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RootParams[3].Descriptor.ShaderRegister = 3;
    RootParams[3].Descriptor.RegisterSpace = 0;
    RootParams[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[4]: VisibleMeshlets UAV (u0), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RootParams[4].Descriptor.ShaderRegister = 0;
    RootParams[4].Descriptor.RegisterSpace = 0;
    RootParams[4].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[5]: DebugPrintBuffer UAV (u1), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RootParams[5].Descriptor.ShaderRegister = 1;
    RootParams[5].Descriptor.RegisterSpace = 0;
    RootParams[5].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[6]: DebugPrintStats UAV (u2), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RootParams[6].Descriptor.ShaderRegister = 2;
    RootParams[6].Descriptor.RegisterSpace = 0;
    RootParams[6].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE1 HZBRange = {};
    HZBRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    HZBRange.NumDescriptors = 1;
    HZBRange.BaseShaderRegister = 1;
    HZBRange.RegisterSpace = 0;
    HZBRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    HZBRange.OffsetInDescriptorsFromTableStart = 0;

    // RootParams[7]: HZB SRV table (t1), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[7].DescriptorTable.pDescriptorRanges = &HZBRange;
    RootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 RootDesc = {};
    RootDesc.NumParameters = _countof(RootParams);
    RootDesc.pParameters = RootParams;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc = {};
    VersionedRootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    VersionedRootDesc.Desc_1_1 = RootDesc;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(CullingRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/CullMeshletVisibility.hlsl", L"CSMain", L"cs_6_0", CsByteCode))
    {
        LogError("Failed to compile culling compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC CsDesc = {};
    CsDesc.pRootSignature = CullingRootSignature.Get();
    CsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&CsDesc, IID_PPV_ARGS(CullingPipeline.GetAddressOf())));

    D3D12_ROOT_PARAMETER1 RunRootParams[7] = {};
    // RunRootParams[0]: CullingConstants CBV (b0), used in Shaders/ClearMeshletRunCounts.hlsl CSMain and Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RunRootParams[0].Descriptor.ShaderRegister = 0;
    RunRootParams[0].Descriptor.RegisterSpace = 0;
    RunRootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RunRootParams[1]: VisibleMeshlets SRV (t0), used in Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RunRootParams[1].Descriptor.ShaderRegister = 0;
    RunRootParams[1].Descriptor.RegisterSpace = 0;
    RunRootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RunRootParams[2]: MeshletDrawData SRV (t1), used in Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RunRootParams[2].Descriptor.ShaderRegister = 1;
    RunRootParams[2].Descriptor.RegisterSpace = 0;
    RunRootParams[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RunRootParams[3]: RangeOffsets SRV (t2), used in Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RunRootParams[3].Descriptor.ShaderRegister = 2;
    RunRootParams[3].Descriptor.RegisterSpace = 0;
    RunRootParams[3].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RunRootParams[4]: CommandTemplates SRV (t3), used in Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RunRootParams[4].Descriptor.ShaderRegister = 3;
    RunRootParams[4].Descriptor.RegisterSpace = 0;
    RunRootParams[4].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RunRootParams[5]: RunCounts UAV (u1), used in Shaders/ClearMeshletRunCounts.hlsl CSMain and Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RunRootParams[5].Descriptor.ShaderRegister = 1;
    RunRootParams[5].Descriptor.RegisterSpace = 0;
    RunRootParams[5].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RunRootParams[6]: OutputCommands UAV (u0), used in Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RunRootParams[6].Descriptor.ShaderRegister = 0;
    RunRootParams[6].Descriptor.RegisterSpace = 0;
    RunRootParams[6].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 RunRootDesc = {};
    RunRootDesc.NumParameters = _countof(RunRootParams);
    RunRootDesc.pParameters = RunRootParams;
    RunRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RunVersionedDesc = {};
    RunVersionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RunVersionedDesc.Desc_1_1 = RunRootDesc;

    ComPtr<ID3DBlob> RunSerializedSig;
    ComPtr<ID3DBlob> RunErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RunVersionedDesc, RunSerializedSig.GetAddressOf(), RunErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, RunSerializedSig->GetBufferPointer(), RunSerializedSig->GetBufferSize(), IID_PPV_ARGS(MeshletRunRootSignature.GetAddressOf())));

    std::vector<uint8_t> ClearByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/ClearMeshletRunCounts.hlsl", L"CSMain", L"cs_6_0", ClearByteCode))
    {
        LogError("Failed to compile meshlet run clear compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ClearDesc = {};
    ClearDesc.pRootSignature = MeshletRunRootSignature.Get();
    ClearDesc.CS = { ClearByteCode.data(), ClearByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&ClearDesc, IID_PPV_ARGS(MeshletRunClearPipeline.GetAddressOf())));

    std::vector<uint8_t> RunByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/BuildMeshletRunsAppend.hlsl", L"CSMain", L"cs_6_0", RunByteCode))
    {
        LogError("Failed to compile meshlet run append compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC RunDesc = {};
    RunDesc.pRootSignature = MeshletRunRootSignature.Get();
    RunDesc.CS = { RunByteCode.data(), RunByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&RunDesc, IID_PPV_ARGS(MeshletRunAppendPipeline.GetAddressOf())));

    D3D12_INDIRECT_ARGUMENT_DESC IndirectArgs[4] = {};
    IndirectArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    IndirectArgs[0].VertexBuffer.Slot = 0;
    IndirectArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    IndirectArgs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    IndirectArgs[2].ConstantBufferView.RootParameterIndex = 0;
    IndirectArgs[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.pArgumentDescs = IndirectArgs;
    CommandDesc.NumArgumentDescs = _countof(IndirectArgs);
    CommandDesc.ByteStride = sizeof(FIndirectDrawCommand);
    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, RootSignature.Get(), IID_PPV_ARGS(IndirectCommandSignature.GetAddressOf())));

    return true;
}

bool FForwardRenderer::CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    const bool bCreated = RendererUtils::CreateObjectIdResources(
        Device,
        Width,
        Height,
        ObjectIdTexture,
        ObjectIdRtvHeap,
        ObjectIdRtvHandle,
        ObjectIdReadback,
        ObjectIdFootprint,
        ObjectIdRowPitch);
    if (bCreated)
    {
        ObjectIdState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return bCreated;
}

bool FForwardRenderer::CreateObjectIdPipeline(FDX12Device* Device)
{
    return RendererUtils::CreateObjectIdPipeline(Device, RootSignature.Get(), ObjectIdPipeline);
}

void FForwardRenderer::UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset, const DirectX::XMMATRIX& LightViewProjection)
{
    const DirectX::XMVECTOR LightDir = DirectX::XMLoadFloat3(&LightDirection);

    RendererUtils::UpdateSceneConstants(
        Camera,
        Model,
        LightIntensity,
        LightDir,
        LightColor,
        LightViewProjection,
        Camera.GetProjectionMatrix(),
        bShadowsEnabled ? ShadowStrength : 0.0f,
        ShadowBias,
        static_cast<float>(ShadowMapWidth),
        static_cast<float>(ShadowMapHeight),
        EnvironmentMipCount,
        DirectX::XMFLOAT2(0.0f, 0.0f),
        0u,
        bGtaoEnabled,
        GtaoRadius,
        GtaoIntensity,
        GtaoPower,
        GtaoThickness,
        GtaoDirectionCount,
        GtaoStepCount,
        GetSceneConstantBufferMapped(),
        ConstantBufferOffset);
}

void FForwardRenderer::UpdateSkyConstants(const FCamera& Camera)
{
    using namespace DirectX;

    const FFloat3 CameraPosition = Camera.GetPosition();
    const XMMATRIX Scale = XMMatrixScaling(SkySphereRadius, SkySphereRadius, SkySphereRadius);
    const XMMATRIX Translation = XMMatrixTranslation(CameraPosition.x, CameraPosition.y, CameraPosition.z);
    const XMMATRIX World = Scale * Translation;

    const XMVECTOR LightDir = XMLoadFloat3(&LightDirection);
    RendererUtils::UpdateSkyConstants(Camera, World, Camera.GetProjectionMatrix(), LightDir, LightColor, SkyConstantBufferMapped);
}
