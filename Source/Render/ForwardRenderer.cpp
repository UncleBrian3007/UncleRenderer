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
        return (UseNormal) | (UseMr << 1) | (UseBase << 2) | (UseEmissive << 3);
    }
}

bool FForwardRenderer::Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options)
{
    if (Device == nullptr)
    {
        LogError("Forward renderer initialization failed: device is null");
        return false;
    }

    this->Device = Device;

    LogInfo("Forward renderer initialization started");

    InitializeCommonSettings(Width, Height, Options);

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

    FDepthResources DepthResources = {};
    if (!RendererUtils::CreateDepthResources(Device, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT, DepthResources))
    {
        LogError("Forward renderer initialization failed: depth resources creation failed");
        return false;
    }
    DepthBuffer = DepthResources.DepthBuffer;
    DSVHeap = DepthResources.DSVHeap;
    DepthStencilHandle = DepthResources.DepthStencilHandle;
    DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (DepthBuffer)
    {
        DepthBuffer->SetName(L"DepthBuffer");
    }
    if (DSVHeap)
    {
        DSVHeap->SetName(L"DepthDSVHeap");
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

    const std::wstring SceneFilePath = Options.SceneFilePath.empty() ? L"Assets/Scenes/Scene.json" : Options.SceneFilePath;
    if (!RendererUtils::CreateSceneModelsFromJson(Device, SceneFilePath, SceneModels, SceneCenter, SceneRadius))
    {
        LogWarning("Falling back to default geometry; scene JSON could not be loaded.");

        FSceneModelResource DefaultModel;
        FGltfMaterialTextures DefaultTextures;
        if (!RendererUtils::CreateDefaultSceneGeometry(Device, DefaultModel.Geometry, SceneCenter, SceneRadius, &DefaultTextures))
        {
            LogError("Forward renderer initialization failed: default scene geometry creation failed");
            return false;
        }

        const DirectX::XMMATRIX DefaultWorld = DirectX::XMMatrixTranslation(-SceneCenter.x, -SceneCenter.y, -SceneCenter.z);
        DirectX::XMStoreFloat4x4(&DefaultModel.WorldMatrix, DefaultWorld);
        DefaultModel.Center = SceneCenter;
        DefaultModel.Name = "DefaultMesh";
        DefaultModel.BoundsMin = DirectX::XMFLOAT3(SceneCenter.x - SceneRadius, SceneCenter.y - SceneRadius, SceneCenter.z - SceneRadius);
        DefaultModel.BoundsMax = DirectX::XMFLOAT3(SceneCenter.x + SceneRadius, SceneCenter.y + SceneRadius, SceneCenter.z + SceneRadius);
        DefaultModel.ObjectId = 1;
        DefaultModel.DrawIndexStart = 0;
        DefaultModel.DrawIndexCount = DefaultModel.Geometry.IndexCount;
        const FGltfMaterialTextureSet DefaultTextureSet = DefaultTextures.PerPrimitive.empty() ? FGltfMaterialTextureSet{} : DefaultTextures.PerPrimitive.front();
        DefaultModel.BaseColorTexturePath = DefaultTextureSet.BaseColor;
        DefaultModel.MetallicRoughnessTexturePath = DefaultTextureSet.MetallicRoughness;
        DefaultModel.NormalTexturePath = DefaultTextureSet.Normal;
        DefaultModel.BaseColorFactor = { 1.0f, 1.0f, 1.0f };
        DefaultModel.BaseColorAlpha = 1.0f;
        DefaultModel.MetallicFactor = 0.0f;
        DefaultModel.RoughnessFactor = 1.0f;
        DefaultModel.EmissiveFactor = { 0.0f, 0.0f, 0.0f };
        DefaultModel.AlphaCutoff = 0.5f;
        DefaultModel.AlphaMode = 0;
        DefaultModel.bHasNormalMap = !DefaultTextureSet.Normal.empty();
        SceneModels.push_back(std::move(DefaultModel));
    }

    SceneConstantBufferStride = (sizeof(FSceneConstants) + 255ULL) & ~255ULL;

    const uint64_t ConstantBufferSize = SceneConstantBufferStride * (std::max<uint64_t>(1, SceneModels.size()));

    FMappedConstantBuffer ConstantBufferResource = {};
    if (!RendererUtils::CreateMappedConstantBuffer(Device, ConstantBufferSize, ConstantBufferResource))
    {
        LogError("Forward renderer initialization failed: constant buffer creation failed");
        return false;
    }
    ConstantBuffer = ConstantBufferResource.Resource;
    ConstantBufferMapped = ConstantBufferResource.MappedData;
    if (ConstantBuffer)
    {
        ConstantBuffer->SetName(L"SceneConstantBuffer");
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

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    PrepareGpuDebugPrint(CmdContext);

    UpdateCullingVisibility(Camera);

    const DirectX::XMMATRIX LightViewProjection = RendererUtils::BuildDirectionalLightViewProjection(
        SceneCenter,
        SceneRadius,
        LightDirection);

    const bool bRenderShadows = bShadowsEnabled && ShadowPipeline && ShadowMap;
    const bool bDoDepthPrepass = bDepthPrepassEnabled && DepthPrepassPipeline;

    FRenderGraph Graph;
    Graph.SetDevice(Device);
    Graph.SetBarrierLoggingEnabled(bLogResourceBarriers);
    Graph.SetGraphDumpEnabled(bEnableGraphDump);
    Graph.SetGpuTimingEnabled(bEnableGpuTiming);

    FRGResourceHandle ShadowHandle = Graph.ImportTexture(
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

    FRGResourceHandle DepthHandle = Graph.ImportTexture("Depth", DepthBuffer.Get(), &DepthBufferState, DepthDesc);
    FRGResourceHandle ObjectIdHandle = Graph.ImportTexture(
        "ObjectId",
        ObjectIdTexture.Get(),
        &ObjectIdState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R32_UINT });

    struct FGpuCullingPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    ConfigureHZBOcclusion(false, TextureDescriptorHeap.Get(), SceneTextureGpuHandle, 0, 0, 0);

    Graph.AddPass<FGpuCullingPassData>("GPU Culling", [this, &Camera, DepthHandle](FGpuCullingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bEnableIndirectDraw && CullingPipeline && CullingRootSignature && IndirectCommandBuffer && ModelBoundsBuffer;
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

    struct FShadowPassData
    {
        bool bEnabled = false;
        bool bUseIndirect = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Graph.AddPass<FShadowPassData>("ShadowMap", [&, bRenderShadows](FShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRenderShadows;
        Data.bUseIndirect = bEnableIndirectDraw && IndirectCommandSignature && IndirectCommandBuffer && IndirectCommandCount > 0;
        Data.Camera = &Camera;
        Data.LightViewProjection = LightViewProjection;

        if (bRenderShadows)
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

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);
        }

        if (Data.bUseIndirect)
        {
            LocalCommandList->ExecuteIndirect(
                IndirectCommandSignature.Get(),
                IndirectCommandCount,
                IndirectCommandBuffer.Get(),
                0,
                nullptr,
                0);
            return;
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

            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);

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

    struct FDepthPrepassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Graph.AddPass<FDepthPrepassData>("DepthPrepass", [&, bDoDepthPrepass](FDepthPrepassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bDoDepthPrepass;
        Data.Camera = &Camera;
        Data.LightViewProjection = LightViewProjection;

        if (bDoDepthPrepass)
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
        Cmd.ClearDepth(DepthStencilHandle);

        ID3D12DescriptorHeap* Heaps[] = { TextureDescriptorHeap.Get() };
        LocalCommandList->SetPipelineState(DepthPrepassPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthStencilHandle);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            if (!SceneModelVisibility.empty() && !SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);

            LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
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
        Data.bClearDepth = !bDoDepthPrepass;

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
        Cmd.SetRenderTarget(Data.OutputHandle, &DepthStencilHandle);

        if (Data.bClearDepth)
        {
            Cmd.ClearDepth(DepthStencilHandle);
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

    struct FForwardPassData
    {
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        const FCamera* Camera = nullptr;
        bool bRenderShadows = false;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
        bool bClearDepth = false;
    };

    Graph.AddPass<FForwardPassData>("Forward", [&, bRenderShadows](FForwardPassData& Data, FRGPassBuilder& Builder)
    {
        Data.OutputHandle = RtvHandle;
        Data.Camera = &Camera;
        Data.bRenderShadows = bRenderShadows;
        Data.LightViewProjection = LightViewProjection;
        Data.bClearDepth = !bDoDepthPrepass && !(SkyPipelineState && SkyRootSignature && SkyGeometry.IndexCount > 0);

        Builder.WriteTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (bRenderShadows)
        {
            Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }, [this](const FForwardPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ForwardEvent(LocalCommandList, L"ForwardPass");
        Cmd.SetRenderTarget(Data.OutputHandle, &DepthStencilHandle);

        if (Data.bClearDepth)
        {
            Cmd.ClearDepth(DepthStencilHandle);
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

        if (bEnableIndirectDraw && IndirectCommandSignature && IndirectCommandBuffer && !IndirectDrawRanges.empty())
        {
            LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            auto SelectPipelineByKey = [&](uint32_t Key)
            {
                const bool UseNormal = (Key & 1u) != 0;
                const bool UseMr = (Key & 2u) != 0;
                const bool UseBaseColor = (Key & 4u) != 0;
                const bool UseEmissive = (Key & 8u) != 0;

                if (UseNormal)
                {
                    if (UseEmissive)
                    {
                        if (UseBaseColor)
                        {
                            return UseMr ? PipelineState.Get() : PipelineStateNoMr.Get();
                        }

                        return UseMr ? PipelineStateNoBaseColor.Get() : PipelineStateNoMrNoBaseColor.Get();
                    }

                    if (UseBaseColor)
                    {
                        return UseMr ? PipelineStateNoEmissive.Get() : PipelineStateNoMrNoEmissive.Get();
                    }

                    return UseMr ? PipelineStateNoBaseColorNoEmissive.Get() : PipelineStateNoMrNoBaseColorNoEmissive.Get();
                }

                if (UseEmissive)
                {
                    if (UseBaseColor)
                    {
                        return UseMr ? PipelineStateNoNormal.Get() : PipelineStateNoMrNoNormal.Get();
                    }

                    return UseMr ? PipelineStateNoBaseColorNoNormal.Get() : PipelineStateNoMrNoBaseColorNoNormal.Get();
                }

                if (UseBaseColor)
                {
                    return UseMr ? PipelineStateNoEmissiveNoNormal.Get() : PipelineStateNoMrNoEmissiveNoNormal.Get();
                }

                return UseMr ? PipelineStateNoBaseColorNoEmissiveNoNormal.Get() : PipelineStateNoMrNoBaseColorNoEmissiveNoNormal.Get();
            };

            for (const FIndirectDrawRange& Range : IndirectDrawRanges)
            {
                ID3D12PipelineState* Pipeline = SelectPipelineByKey(Range.PipelineKey);
                LocalCommandList->SetPipelineState(Pipeline);
                LocalCommandList->SetGraphicsRootDescriptorTable(1, Range.TextureHandle);

                const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
                if (AreModelPixEventsEnabled())
                {
                    const wchar_t* Label = Range.Name.empty() ? L"IndirectDrawRange" : Range.Name.c_str();
                    FScopedPixEvent ModelEvent(LocalCommandList, Label);
                    LocalCommandList->ExecuteIndirect(IndirectCommandSignature.Get(), Range.Count, IndirectCommandBuffer.Get(), Offset, nullptr, 0);
                }
                else
                {
                    LocalCommandList->ExecuteIndirect(IndirectCommandSignature.Get(), Range.Count, IndirectCommandBuffer.Get(), Offset, nullptr, 0);
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

                auto SelectPipeline = [&](bool UseNormal, bool UseMr, bool UseBaseColor, bool UseEmissive)
                {
                    if (UseNormal)
                    {
                        if (UseEmissive)
                        {
                            if (UseBaseColor)
                            {
                                return UseMr ? PipelineState.Get() : PipelineStateNoMr.Get();
                            }

                            return UseMr ? PipelineStateNoBaseColor.Get() : PipelineStateNoMrNoBaseColor.Get();
                        }

                        if (UseBaseColor)
                        {
                            return UseMr ? PipelineStateNoEmissive.Get() : PipelineStateNoMrNoEmissive.Get();
                        }

                        return UseMr ? PipelineStateNoBaseColorNoEmissive.Get() : PipelineStateNoMrNoBaseColorNoEmissive.Get();
                    }

                    if (UseEmissive)
                    {
                        if (UseBaseColor)
                        {
                            return UseMr ? PipelineStateNoNormal.Get() : PipelineStateNoMrNoNormal.Get();
                        }

                        return UseMr ? PipelineStateNoBaseColorNoNormal.Get() : PipelineStateNoMrNoBaseColorNoNormal.Get();
                    }

                    if (UseBaseColor)
                    {
                        return UseMr ? PipelineStateNoEmissiveNoNormal.Get() : PipelineStateNoMrNoEmissiveNoNormal.Get();
                    }

                    return UseMr ? PipelineStateNoBaseColorNoEmissiveNoNormal.Get() : PipelineStateNoMrNoBaseColorNoEmissiveNoNormal.Get();
                };

                LocalCommandList->SetPipelineState(SelectPipeline(bUseNormalMap, bUseMetallicRoughnessMap, bUseBaseColorMap, bUseEmissiveMap));

                LocalCommandList->SetGraphicsRootConstantBufferView(
                    0,
                    ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
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

    struct FObjectIdPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Graph.AddPass<FObjectIdPassData>("ObjectId", [this, &Camera, LightViewProjection, ObjectIdHandle, DepthHandle](FObjectIdPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bObjectIdReadbackRequested && ObjectIdPipeline && ObjectIdTexture;
        Data.Camera = &Camera;
        Data.LightViewProjection = LightViewProjection;

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
        LocalCommandList->OMSetRenderTargets(1, &ObjectIdRtvHandle, FALSE, &DepthStencilHandle);

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
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);

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

    Graph.Execute(CmdContext);
}

void FForwardRenderer::UpdateCullingVisibility(const FCamera& Camera)
{
    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    RendererUtils::UpdateCullingVisibility(*CullingCamera, SceneModels, SceneModelVisibility);
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
    // RootParams[0]: Scene constant buffer (b0)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Material/scene texture SRV table (t0..t6)
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &DescriptorRange;

    D3D12_STATIC_SAMPLER_DESC Samplers[3] = {};
    Samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    Samplers[0].MipLODBias = 0.0f;
    Samplers[0].MaxAnisotropy = 1;
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

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/ForwardVS.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> DefaultDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCode, DefaultDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColor, NoBaseColorDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMr, NoMrDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColor, NoMrNoBaseColorDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoEmissiveDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoEmissive, NoEmissiveDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorNoEmissiveDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoEmissive, NoBaseColorNoEmissiveDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoEmissiveDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoEmissive, NoMrNoEmissiveDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorNoEmissiveDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=1" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoEmissive, NoMrNoBaseColorNoEmissiveDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoNormal, NoNormalDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoNormal, NoBaseColorNoNormalDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoNormal, NoMrNoNormalDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=1", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoNormal, NoMrNoBaseColorNoNormalDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoEmissiveNoNormal, NoEmissiveNoNormalDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoBaseColorNoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoBaseColorNoEmissiveNoNormal, NoBaseColorNoEmissiveNoNormalDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoEmissiveNoNormal, NoMrNoEmissiveNoNormalDefines))
    {
        return false;
    }

    const std::vector<std::wstring> NoMrNoBaseColorNoEmissiveNoNormalDefines = { L"USE_BASE_COLOR_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_EMISSIVE_MAP=0", L"USE_NORMAL_MAP=0" };
    if (!Compiler.CompileFromFile(L"Shaders/ForwardPS.hlsl", L"PSMain", PSTarget, PSByteCodeNoMrNoBaseColorNoEmissiveNoNormal, NoMrNoBaseColorNoEmissiveNoNormalDefines))
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
    if (!Device || SceneModels.empty() || !ConstantBuffer)
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

    std::vector<FIndirectDrawCommand> Commands;
    Commands.reserve(SceneModels.size());

    std::vector<DirectX::XMFLOAT4> Bounds;
    Bounds.reserve(SceneModels.size() * 2);

    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = ConstantBuffer->GetGPUVirtualAddress();
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
        Bounds.emplace_back(Model.BoundsMin.x, Model.BoundsMin.y, Model.BoundsMin.z, 0.0f);
        Bounds.emplace_back(Model.BoundsMax.x, Model.BoundsMax.y, Model.BoundsMax.z, 0.0f);
        IndirectDrawRanges.back().Count += 1;
    };

    for (uint32_t SortedIndex : SortedIndices)
    {
        AppendIndirectDrawData(SortedIndex);
    }

    IndirectCommandCount = static_cast<uint32_t>(Commands.size());

    const uint64_t CommandBufferSize = sizeof(FIndirectDrawCommand) * Commands.size();

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

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &BufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(IndirectCommandBuffer.GetAddressOf())));
    if (IndirectCommandBuffer)
    {
        IndirectCommandBuffer->SetName(L"IndirectCommandBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(IndirectCommandUpload.GetAddressOf())));
    if (IndirectCommandUpload)
    {
        IndirectCommandUpload->SetName(L"IndirectCommandUpload");
    }

    const D3D12_RANGE EmptyRange = { 0, 0 };
    void* UploadData = nullptr;
    HR_CHECK(IndirectCommandUpload->Map(0, &EmptyRange, &UploadData));
    std::memcpy(UploadData, Commands.data(), CommandBufferSize);
    IndirectCommandUpload->Unmap(0, nullptr);

    const uint64_t BoundsBufferSize = sizeof(DirectX::XMFLOAT4) * Bounds.size();
    D3D12_RESOURCE_DESC BoundsDesc = BufferDesc;
    BoundsDesc.Width = BoundsBufferSize;
    BoundsDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_DESC BoundsUploadDesc = BoundsDesc;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &BoundsDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
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

    UploadData = nullptr;
    HR_CHECK(ModelBoundsUpload->Map(0, &EmptyRange, &UploadData));
    std::memcpy(UploadData, Bounds.data(), BoundsBufferSize);
    ModelBoundsUpload->Unmap(0, nullptr);

    D3D12_RESOURCE_DESC DebugDesc = BufferDesc;
    DebugDesc.Width = GpuDebugPrintBufferSize;
    DebugDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &DebugDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
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
    StatsDesc.Width = sizeof(uint32_t) * 2;
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
        D3D12_RESOURCE_STATE_COPY_DEST,
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
            std::memset(StatsUploadData, 0, sizeof(uint32_t) * 2);
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

    UploadList->CopyBufferRegion(IndirectCommandBuffer.Get(), 0, IndirectCommandUpload.Get(), 0, CommandBufferSize);
    UploadList->CopyBufferRegion(ModelBoundsBuffer.Get(), 0, ModelBoundsUpload.Get(), 0, BoundsBufferSize);
    if (GpuDebugPrintBuffer && GpuDebugPrintUpload)
    {
        UploadList->CopyBufferRegion(GpuDebugPrintBuffer.Get(), 0, GpuDebugPrintUpload.Get(), 0, sizeof(uint32_t));
    }
    if (GpuDebugPrintStatsBuffer && GpuDebugPrintStatsUpload)
    {
        UploadList->CopyBufferRegion(GpuDebugPrintStatsBuffer.Get(), 0, GpuDebugPrintStatsUpload.Get(), 0, sizeof(uint32_t) * 2);
    }

    D3D12_RESOURCE_BARRIER Barriers[4] = {};
    Barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barriers[0].Transition.pResource = IndirectCommandBuffer.Get();
    Barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    Barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barriers[1].Transition.pResource = ModelBoundsBuffer.Get();
    Barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barriers[2].Transition.pResource = GpuDebugPrintBuffer.Get();
    Barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barriers[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barriers[3].Transition.pResource = GpuDebugPrintStatsBuffer.Get();
    Barriers[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    UploadList->ResourceBarrier(4, Barriers);

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    IndirectCommandState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    GpuDebugPrintState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    D3D12_ROOT_PARAMETER RootParams[6] = {};
    // RootParams[0]: cbuffer CullingConstants (frustum planes, view-projection, HZB settings, debug toggle)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].Constants.ShaderRegister = 0;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.Num32BitValues = 46;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[1]: SRV ModelBounds buffer (t0)
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RootParams[1].Descriptor.ShaderRegister = 0;
    RootParams[1].Descriptor.RegisterSpace = 0;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[2]: UAV IndirectArgs buffer (u0)
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RootParams[2].Descriptor.ShaderRegister = 0;
    RootParams[2].Descriptor.RegisterSpace = 0;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[3]: UAV DebugPrintBuffer for text entries (u1)
    RootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RootParams[3].Descriptor.ShaderRegister = 1;
    RootParams[3].Descriptor.RegisterSpace = 0;
    RootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[4]: UAV DebugPrintStatsBuffer for culling counters (u2)
    RootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    RootParams[4].Descriptor.ShaderRegister = 2;
    RootParams[4].Descriptor.RegisterSpace = 0;
    RootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE HZBRange = {};
    HZBRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    HZBRange.NumDescriptors = 1;
    HZBRange.BaseShaderRegister = 1;
    HZBRange.RegisterSpace = 0;
    HZBRange.OffsetInDescriptorsFromTableStart = 0;

    // RootParams[5]: HZB SRV table (t1)
    RootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[5].DescriptorTable.pDescriptorRanges = &HZBRange;
    RootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC RootDesc = {};
    RootDesc.NumParameters = _countof(RootParams);
    RootDesc.pParameters = RootParams;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(CullingRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/CullIndirectArgs.hlsl", L"CSMain", L"cs_6_0", CsByteCode))
    {
        LogError("Failed to compile culling compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC CsDesc = {};
    CsDesc.pRootSignature = CullingRootSignature.Get();
    CsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&CsDesc, IID_PPV_ARGS(CullingPipeline.GetAddressOf())));

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

void FForwardRenderer::UpdateIndirectCommandBuffer()
{
    if (!IndirectCommandBuffer || !ConstantBuffer)
    {
        return;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = ConstantBuffer->GetGPUVirtualAddress();
    const uint64_t CommandBufferSize = sizeof(FIndirectDrawCommand) * SceneModels.size();

    std::vector<FIndirectDrawCommand> Commands;
    Commands.reserve(SceneModels.size());
    for (uint32_t Index = 0; Index < SceneModels.size(); ++Index)
    {
        const FSceneModelResource& Model = SceneModels[Index];
        FIndirectDrawCommand Command = {};
        Command.VertexBufferView = Model.Geometry.VertexBufferView;
        Command.IndexBufferView = Model.Geometry.IndexBufferView;
        Command.ConstantBufferAddress = ConstantBufferBase + SceneConstantBufferStride * Index;
        Command.DrawArguments.IndexCountPerInstance = Model.DrawIndexCount;
        Command.DrawArguments.InstanceCount = 1;
        Command.DrawArguments.StartIndexLocation = Model.DrawIndexStart;
        Command.DrawArguments.BaseVertexLocation = 0;
        Command.DrawArguments.StartInstanceLocation = Index;
        Commands.push_back(Command);
    }

    if (!IndirectCommandUpload)
    {
        return;
    }

    const D3D12_RANGE EmptyRange = { 0, 0 };
    void* UploadData = nullptr;
    HR_CHECK(IndirectCommandUpload->Map(0, &EmptyRange, &UploadData));
    std::memcpy(UploadData, Commands.data(), CommandBufferSize);
    IndirectCommandUpload->Unmap(0, nullptr);
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
        bShadowsEnabled ? ShadowStrength : 0.0f,
        ShadowBias,
        static_cast<float>(ShadowMapWidth),
        static_cast<float>(ShadowMapHeight),
        EnvironmentMipCount,
        ConstantBufferMapped,
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
    RendererUtils::UpdateSkyConstants(Camera, World, LightDir, LightColor, SkyConstantBufferMapped);
}
