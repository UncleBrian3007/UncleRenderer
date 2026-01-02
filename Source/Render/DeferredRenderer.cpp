#include "DeferredRenderer.h"

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
#include <d3dx12.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <array>

using Microsoft::WRL::ComPtr;

FDeferredRenderer::FDeferredRenderer() = default;

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

    constexpr DXGI_FORMAT GBufferFormats[3] =
    {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM
    };

    constexpr DXGI_FORMAT LightingBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool FDeferredRenderer::Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options)
{
    if (Device == nullptr)
    {
        LogError("Deferred renderer initialization failed: device is null");
        return false;
    }

    this->Device = Device;

    LogInfo("Deferred renderer initialization started");

    this->BackBufferFormat = BackBufferFormat;

    bTonemapEnabled = Options.bEnableTonemap;
    TonemapExposure = Options.TonemapExposure;
    TonemapWhitePoint = Options.TonemapWhitePoint;
    TonemapGamma = Options.TonemapGamma;
    bAutoExposureEnabled = Options.bEnableAutoExposure;
    AutoExposureKey = Options.AutoExposureKey;
    AutoExposureMin = Options.AutoExposureMin;
    AutoExposureMax = Options.AutoExposureMax;
    AutoExposureSpeedUp = Options.AutoExposureSpeedUp;
    AutoExposureSpeedDown = Options.AutoExposureSpeedDown;
    bHZBEnabled = Options.bEnableHZB;
    bHZBReady = false;

    InitializeCommonSettings(Width, Height, Options);

    LogInfo("Creating deferred renderer base pass root signature...");
    if (!CreateBasePassRootSignature(Device))
    {
        LogError("Deferred renderer initialization failed: base pass root signature creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting root signature...");
    if (!CreateLightingRootSignature(Device))
    {
        LogError("Deferred renderer initialization failed: lighting root signature creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer base pass pipeline...");
    if (!CreateBasePassPipeline(Device, LightingBufferFormat))
    {
        LogError("Deferred renderer initialization failed: base pass pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer object ID pipeline...");
    if (!CreateObjectIdPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: object ID pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer depth prepass pipeline...");
    if (!CreateDepthPrepassPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: depth prepass pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer shadow pipeline...");
    if (!CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowPipeline))
    {
        LogError("Deferred renderer initialization failed: shadow pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting pipeline...");
    if (!CreateLightingPipeline(Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: lighting pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer hierarchical Z-buffer root signature and pipeline...");
    if (!CreateHZBRootSignature(Device) || !CreateHZBPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: HZB pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer auto exposure root signature and pipeline...");
    if (!CreateAutoExposureRootSignature(Device) || !CreateAutoExposurePipeline(Device))
    {
        LogError("Deferred renderer initialization failed: auto exposure pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer tonemap root signature and pipeline...");
    if (!CreateTonemapRootSignature(Device) || !CreateTonemapPipeline(Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: tonemap pipeline creation failed");
        return false;
    }

    TextureLoader = std::make_unique<FTextureLoader>(Device);

    if (!TextureLoader->LoadOrSolidColor(L"", 0xffffffff, NullTexture))
    {
        LogError("Deferred renderer initialization failed: null texture creation failed");
        return false;
    }

    if (NullTexture)
    {
        NullTexture->SetName(L"NullTexture");
    }

    FDepthResources DepthResources = {};
    if (!RendererUtils::CreateDepthResources(Device, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT, DepthResources))
    {
        LogError("Deferred renderer initialization failed: depth resources creation failed");
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
        LogError("Deferred renderer initialization failed: object ID resources creation failed");
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
        LogError("Deferred renderer initialization failed: shadow resources creation failed");
        return false;
    }

    if (!CreateGBufferResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: GBuffer resource creation failed");
        return false;
    }

    if (!CreateLuminanceResources(Device))
    {
        LogError("Deferred renderer initialization failed: luminance resource creation failed");
        return false;
    }

    if (!CreateHZBResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: HZB resource creation failed");
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
            LogError("Deferred renderer initialization failed: default scene geometry creation failed");
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

    SceneWorldMatrix = SceneModels.front().WorldMatrix;

    SceneConstantBufferStride = (sizeof(FSceneConstants) + 255ULL) & ~255ULL;
    const uint64_t ConstantBufferSize = SceneConstantBufferStride * (std::max<uint64_t>(1, SceneModels.size()));

    FMappedConstantBuffer ConstantBufferResource = {};
    if (!RendererUtils::CreateMappedConstantBuffer(Device, ConstantBufferSize, ConstantBufferResource))
    {
        LogError("Deferred renderer initialization failed: constant buffer creation failed");
        return false;
    }
    ConstantBuffer = ConstantBufferResource.Resource;
    ConstantBufferMapped = ConstantBufferResource.MappedData;
    ConstantBuffer->SetName(L"SceneConstantBuffer");

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/output_pmrem.dds", EnvironmentCubeTexture))
    {
        LogError("Deferred renderer initialization failed: environment cube texture loading failed");
        return false;
    }
    if (EnvironmentCubeTexture)
    {
        EnvironmentCubeTexture->SetName(L"EnvironmentCube");
    }

    if (!TextureLoader->LoadOrDefault(L"Assets/Textures/PreintegratedGF.dds", BrdfLutTexture))
    {
        LogError("Deferred renderer initialization failed: BRDF LUT texture loading failed");
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

    if (!CreateSceneTextures(Device, SceneModels))
    {
        LogError("Deferred renderer initialization failed: scene texture creation failed");
        return false;
    }

    if (!CreateDescriptorHeap(Device))
    {
        LogError("Deferred renderer initialization failed: descriptor heap creation failed");
        return false;
    }

    if (!CreateGpuDrivenResources(Device))
    {
        LogWarning("Deferred renderer GPU-driven resources creation failed; fallback to CPU-driven draws.");
    }

    SkySphereRadius = (std::max)(SceneRadius * 5.0f, 100.0f);
    if (!RendererUtils::CreateSkyAtmosphereResources(Device, SkySphereRadius, SkyGeometry, SkyConstantBuffer, SkyConstantBufferMapped))
    {
        LogError("Deferred renderer initialization failed: sky resource creation failed");
        return false;
    }

    if (SkyConstantBuffer)
    {
        SkyConstantBuffer->SetName(L"SkyConstantBuffer");
    }

    FSkyPipelineConfig SkyPipelineConfig = {};
    SkyPipelineConfig.DepthEnable = true;
    SkyPipelineConfig.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    SkyPipelineConfig.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    SkyPipelineConfig.DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    if (!RendererUtils::CreateSkyAtmospherePipeline(Device, LightingBufferFormat, SkyPipelineConfig, SkyRootSignature, SkyPipelineState))
    {
        LogError("Deferred renderer initialization failed: sky pipeline state creation failed");
        return false;
    }

    if (bEnableGpuDebugPrint)
    {
        if (!CreateGpuDebugPrintResources(Device) || !CreateGpuDebugPrintPipeline(Device, BackBufferFormat) || !CreateGpuDebugPrintStatsPipeline(Device))
        {
            LogError("Deferred renderer initialization failed: GPU debug print setup failed");
            return false;
        }
    }

    LogInfo("Deferred renderer initialization completed");
    return true;
}

void FDeferredRenderer::RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime)
{
    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    PrepareGpuDebugPrint(CmdContext);

    UpdateCullingVisibility(Camera);

    const bool bRenderShadows = bShadowsEnabled && ShadowPipeline && ShadowMap;
    const bool bDoDepthPrepass = bDepthPrepassEnabled && DepthPrepassPipeline;
    if (!bDoDepthPrepass)
    {
        bHZBReady = false;
    }

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
        DXGI_FORMAT_R24G8_TYPELESS
    };

    FRGResourceHandle DepthHandle = Graph.ImportTexture("Depth", DepthBuffer.Get(), &DepthBufferState, DepthDesc);
    FRGResourceHandle ObjectIdHandle = Graph.ImportTexture(
        "ObjectId",
        ObjectIdTexture.Get(),
        &ObjectIdState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), DXGI_FORMAT_R32_UINT });
    FRGResourceHandle GBufferHandles[3] =
    {
        Graph.ImportTexture("GBufferA", GBufferA.Get(), &GBufferStates[0], { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), GBufferFormats[0] }),
        Graph.ImportTexture("GBufferB", GBufferB.Get(), &GBufferStates[1], { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), GBufferFormats[1] }),
        Graph.ImportTexture("GBufferC", GBufferC.Get(), &GBufferStates[2], { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), GBufferFormats[2] }),
    };

    FRGResourceHandle LightingHandle = Graph.ImportTexture(
        "Lighting",
        LightingBuffer.Get(),
        &LightingBufferState,
        { static_cast<uint32>(Viewport.Width), static_cast<uint32>(Viewport.Height), LightingBufferFormat });

    FRGResourceHandle LuminanceHandles[2] =
    {
        Graph.ImportTexture(
            "LuminanceA",
            LuminanceTextures[0].Get(),
            &LuminanceStates[0],
            { 1u, 1u, DXGI_FORMAT_R32_FLOAT }),
        Graph.ImportTexture(
            "LuminanceB",
            LuminanceTextures[1].Get(),
            &LuminanceStates[1],
            { 1u, 1u, DXGI_FORMAT_R32_FLOAT })
    };

    FRGResourceHandle HZBHandle = Graph.ImportTexture(
        "HZB",
        HierarchicalZBuffer.Get(),
        &HZBState,
        { HZBWidth, HZBHeight, DXGI_FORMAT_R32_FLOAT });

    struct FGpuCullingPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    if (!bHZBEnabled)
    {
        bHZBReady = false;
    }

    const bool bUseHZBOcclusion = bHZBEnabled && bHZBReady && HZBSrvHandle.ptr != 0;
    ConfigureHZBOcclusion(bUseHZBOcclusion, DescriptorHeap.Get(), HZBSrvHandle, HZBWidth, HZBHeight, HZBMipCount);

    Graph.AddPass<FGpuCullingPassData>("GPU Culling", [this, &Camera, DepthHandle, HZBHandle, bUseHZBOcclusion](FGpuCullingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bEnableIndirectDraw && CullingPipeline && CullingRootSignature && IndirectCommandBuffer && ModelBoundsBuffer;
        Data.Camera = &Camera;
        if (Data.bEnabled)
        {
            if (bUseHZBOcclusion)
            {
                Builder.ReadTexture(HZBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
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
    };

    Graph.AddPass<FShadowPassData>("ShadowMap", [&, bRenderShadows](FShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRenderShadows;
        Data.bUseIndirect = bEnableIndirectDraw && IndirectCommandSignature && IndirectCommandBuffer && IndirectCommandCount > 0;
        Data.Camera = &Camera;

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

        ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
        LocalCommandList->SetPipelineState(ShadowPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &ShadowViewport);
        LocalCommandList->RSSetScissorRects(1, &ShadowScissor);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &ShadowDSVHandle);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset);
        }

        if (Data.bUseIndirect)
        {
            LocalCommandList->SetGraphicsRootDescriptorTable(1, DescriptorHeap->GetGPUDescriptorHandleForHeapStart());
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
            LocalCommandList->SetGraphicsRootDescriptorTable(1, DescriptorHeap->GetGPUDescriptorHandleForHeapStart());

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
    };

    Graph.AddPass<FDepthPrepassData>("DepthPrepass", [&, bDoDepthPrepass](FDepthPrepassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bDoDepthPrepass;
        Data.Camera = &Camera;

        if (bDoDepthPrepass)
        {
            Builder.WriteTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
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

        ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
        LocalCommandList->SetPipelineState(DepthPrepassPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthStencilHandle);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset);
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
            LocalCommandList->SetGraphicsRootDescriptorTable(1, DescriptorHeap->GetGPUDescriptorHandleForHeapStart());

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

    struct FBasePassData
    {
        bool bDoDepthPrepass = false;
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FBasePassData>("GBuffer", [&](FBasePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bDoDepthPrepass = bDoDepthPrepass;
        Data.Camera = &Camera;

        for (int i = 0; i < 3; ++i)
        {
            Builder.WriteTexture(GBufferHandles[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Builder.WriteTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }, [this](const FBasePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent GBufferEvent(LocalCommandList, L"GBuffer");

        D3D12_CPU_DESCRIPTOR_HANDLE BasePassRTVs[4] =
        {
            GBufferRTVHandles[0],
            GBufferRTVHandles[1],
            GBufferRTVHandles[2],
            LightingRTVHandle
        };

        if (!Data.bDoDepthPrepass)
        {
            Cmd.ClearDepth(DepthStencilHandle);
        }

        for (const D3D12_CPU_DESCRIPTOR_HANDLE& Handle : GBufferRTVHandles)
        {
            const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            Cmd.ClearRenderTarget(Handle, ClearValue);
        }

        const float SceneClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        Cmd.ClearRenderTarget(LightingRTVHandle, SceneClear);

        LocalCommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());

        ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootDescriptorTable(1, DescriptorHeap->GetGPUDescriptorHandleForHeapStart());

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(_countof(BasePassRTVs), BasePassRTVs, FALSE, &DepthStencilHandle);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset);
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

                if (UseEmissive)
                {
                    if (UseBaseColor)
                    {
                        return UseNormal
                            ? (UseMr ? BasePassPipelineWithNormalMap.Get() : BasePassPipelineWithNormalMapNoMr.Get())
                            : (UseMr ? BasePassPipelineWithoutNormalMap.Get() : BasePassPipelineWithoutNormalMapNoMr.Get());
                    }

                    return UseNormal
                        ? (UseMr ? BasePassPipelineWithNormalMapNoBaseColor.Get() : BasePassPipelineWithNormalMapNoMrNoBaseColor.Get())
                        : (UseMr ? BasePassPipelineWithoutNormalMapNoBaseColor.Get() : BasePassPipelineWithoutNormalMapNoMrNoBaseColor.Get());
                }

                if (UseBaseColor)
                {
                    return UseNormal
                        ? (UseMr ? BasePassPipelineWithNormalMapNoEmissive.Get() : BasePassPipelineWithNormalMapNoMrNoEmissive.Get())
                        : (UseMr ? BasePassPipelineWithoutNormalMapNoEmissive.Get() : BasePassPipelineWithoutNormalMapNoMrNoEmissive.Get());
                }

                return UseNormal
                    ? (UseMr ? BasePassPipelineWithNormalMapNoBaseColorNoEmissive.Get() : BasePassPipelineWithNormalMapNoMrNoBaseColorNoEmissive.Get())
                    : (UseMr ? BasePassPipelineWithoutNormalMapNoBaseColorNoEmissive.Get() : BasePassPipelineWithoutNormalMapNoMrNoBaseColorNoEmissive.Get());
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

                LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
                LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

                LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
                LocalCommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

                const bool bUseNormalMap = Model.bHasNormalMap;
                const bool bUseMetallicRoughnessMap = !Model.MetallicRoughnessTexturePath.empty();
                const bool bUseBaseColorMap = !Model.BaseColorTexturePath.empty();
                const bool bUseEmissiveMap = !Model.EmissiveTexturePath.empty();

                auto SelectPipeline = [&](bool UseNormal, bool UseMr, bool UseBaseColor, bool UseEmissive)
                {
                    if (UseEmissive)
                    {
                        if (UseBaseColor)
                        {
                            return UseNormal
                                ? (UseMr ? BasePassPipelineWithNormalMap.Get() : BasePassPipelineWithNormalMapNoMr.Get())
                                : (UseMr ? BasePassPipelineWithoutNormalMap.Get() : BasePassPipelineWithoutNormalMapNoMr.Get());
                        }

                        return UseNormal
                            ? (UseMr ? BasePassPipelineWithNormalMapNoBaseColor.Get() : BasePassPipelineWithNormalMapNoMrNoBaseColor.Get())
                            : (UseMr ? BasePassPipelineWithoutNormalMapNoBaseColor.Get() : BasePassPipelineWithoutNormalMapNoMrNoBaseColor.Get());
                    }

                    if (UseBaseColor)
                    {
                        return UseNormal
                            ? (UseMr ? BasePassPipelineWithNormalMapNoEmissive.Get() : BasePassPipelineWithNormalMapNoMrNoEmissive.Get())
                            : (UseMr ? BasePassPipelineWithoutNormalMapNoEmissive.Get() : BasePassPipelineWithoutNormalMapNoMrNoEmissive.Get());
                    }

                    return UseNormal
                        ? (UseMr ? BasePassPipelineWithNormalMapNoBaseColorNoEmissive.Get() : BasePassPipelineWithNormalMapNoMrNoBaseColorNoEmissive.Get())
                        : (UseMr ? BasePassPipelineWithoutNormalMapNoBaseColorNoEmissive.Get() : BasePassPipelineWithoutNormalMapNoMrNoBaseColorNoEmissive.Get());
                };

                LocalCommandList->SetPipelineState(SelectPipeline(bUseNormalMap, bUseMetallicRoughnessMap, bUseBaseColorMap, bUseEmissiveMap));

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
    };

    Graph.AddPass<FObjectIdPassData>("ObjectId", [this, &Camera, ObjectIdHandle, DepthHandle](FObjectIdPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bObjectIdReadbackRequested && ObjectIdPipeline && ObjectIdTexture;
        Data.Camera = &Camera;
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
        LocalCommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());
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
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset);
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

    struct FHZBPassData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipCount = 0;
        uint32_t SourceWidth = 0;
        uint32_t SourceHeight = 0;
        D3D12_GPU_DESCRIPTOR_HANDLE DepthSrv{};
        D3D12_GPU_DESCRIPTOR_HANDLE HZBSrv{};
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> HZBSrvMips;
        std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> HZBUavs;
        D3D12_GPU_DESCRIPTOR_HANDLE HZBNullUav{};
    };

    if (bHZBEnabled && bDoDepthPrepass)
    {
        Graph.AddPass<FHZBPassData>("Build HZB", [&](FHZBPassData& Data, FRGPassBuilder& Builder)
        {
            Data.Width = HZBWidth;
            Data.Height = HZBHeight;
            Data.MipCount = HZBMipCount;
            const D3D12_RESOURCE_DESC DepthDesc = DepthBuffer->GetDesc();
            Data.SourceWidth = static_cast<uint32_t>(DepthDesc.Width);
            Data.SourceHeight = DepthDesc.Height;
            Data.DepthSrv = DepthBufferHandle;
            Data.HZBSrv = HZBSrvHandle;
            Data.HZBSrvMips = HZBSrvMipHandles;
            Data.HZBUavs = HZBUavHandles;
            Data.HZBNullUav = HZBNullUavHandle;

            Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteTexture(HZBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }, [this](const FHZBPassData& Data, FDX12CommandContext& Cmd)
        {
            if (!HZBRootSignature || Data.MipCount == 0)
            {
                return;
            }

            ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

            FScopedPixEvent HZBEvent(LocalCommandList, L"BuildHZB");

            ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
            LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            LocalCommandList->SetComputeRootSignature(HZBRootSignature.Get());

            struct FHZBConstants
            {
                uint32_t SourceWidth;
                uint32_t SourceHeight;
                uint32_t DestWidth;
                uint32_t DestHeight;
                uint32_t DestWidth1;
                uint32_t DestHeight1;
                uint32_t DestWidth2;
                uint32_t DestHeight2;
                uint32_t DestWidth3;
                uint32_t DestHeight3;
                uint32_t SourceMip;
            };

            uint32_t CurrentWidth = Data.Width;
            uint32_t CurrentHeight = Data.Height;
            std::vector<D3D12_RESOURCE_STATES> MipStates(Data.MipCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            uint32_t MipIndex = 0;
            while (MipIndex < Data.MipCount)
            {
                const uint32_t RemainingMips = Data.MipCount - MipIndex;
                const uint32_t MipsThisDispatch = (std::min)(4u, RemainingMips);
                const bool bHasSecondMip = MipsThisDispatch > 1;
                const bool bHasThirdMip = MipsThisDispatch > 2;
                const bool bHasFourthMip = MipsThisDispatch > 3;

                const uint32_t SourceWidth = (MipIndex == 0) ? Data.SourceWidth : (std::max)(1u, CurrentWidth);
                const uint32_t SourceHeight = (MipIndex == 0) ? Data.SourceHeight : (std::max)(1u, CurrentHeight);

                const uint32_t DestWidth = (MipIndex == 0) ? CurrentWidth : (std::max)(1u, CurrentWidth / 2);
                const uint32_t DestHeight = (MipIndex == 0) ? CurrentHeight : (std::max)(1u, CurrentHeight / 2);
                const uint32_t DestWidth1 = bHasSecondMip ? (std::max)(1u, DestWidth / 2) : 0u;
                const uint32_t DestHeight1 = bHasSecondMip ? (std::max)(1u, DestHeight / 2) : 0u;
                const uint32_t DestWidth2 = bHasThirdMip ? (std::max)(1u, DestWidth1 / 2) : 0u;
                const uint32_t DestHeight2 = bHasThirdMip ? (std::max)(1u, DestHeight1 / 2) : 0u;
                const uint32_t DestWidth3 = bHasFourthMip ? (std::max)(1u, DestWidth2 / 2) : 0u;
                const uint32_t DestHeight3 = bHasFourthMip ? (std::max)(1u, DestHeight2 / 2) : 0u;

                FHZBConstants Constants = {};
                Constants.SourceWidth = SourceWidth;
                Constants.SourceHeight = SourceHeight;
                Constants.DestWidth = DestWidth;
                Constants.DestHeight = DestHeight;
                Constants.DestWidth1 = DestWidth1;
                Constants.DestHeight1 = DestHeight1;
                Constants.DestWidth2 = DestWidth2;
                Constants.DestHeight2 = DestHeight2;
                Constants.DestWidth3 = DestWidth3;
                Constants.DestHeight3 = DestHeight3;
                Constants.SourceMip = 0u;

                D3D12_GPU_DESCRIPTOR_HANDLE SourceHandle = Data.DepthSrv;
                if (MipIndex > 0)
                {
                    const uint32_t SourceMipIndex = MipIndex - 1;
                    SourceHandle = (SourceMipIndex < Data.HZBSrvMips.size()) ? Data.HZBSrvMips[SourceMipIndex] : D3D12_GPU_DESCRIPTOR_HANDLE{};

                    if (SourceMipIndex < MipStates.size() && MipStates[SourceMipIndex] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                    {
                        D3D12_RESOURCE_BARRIER ToSrvBarrier = {};
                        ToSrvBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        ToSrvBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        ToSrvBarrier.Transition.pResource = HierarchicalZBuffer.Get();
                        ToSrvBarrier.Transition.StateBefore = MipStates[SourceMipIndex];
                        ToSrvBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        ToSrvBarrier.Transition.Subresource = D3D12CalcSubresource(SourceMipIndex, 0, 0, Data.MipCount, 1);
                        if (bLogResourceBarriers)
                        {
							LogInfo("HZB Barrier: Mip " + std::to_string(SourceMipIndex) + " "
								+ RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateBefore) + " -> "
								+ RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateAfter));
                        }
                        LocalCommandList->ResourceBarrier(1, &ToSrvBarrier);
                        MipStates[SourceMipIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    }
                }
                const D3D12_GPU_DESCRIPTOR_HANDLE DestHandle0 = (MipIndex < Data.HZBUavs.size()) ? Data.HZBUavs[MipIndex] : D3D12_GPU_DESCRIPTOR_HANDLE{};
                const D3D12_GPU_DESCRIPTOR_HANDLE DestHandle1 = (bHasSecondMip && (MipIndex + 1) < Data.HZBUavs.size())
                    ? Data.HZBUavs[MipIndex + 1]
                    : Data.HZBNullUav;
                const D3D12_GPU_DESCRIPTOR_HANDLE DestHandle2 = (bHasThirdMip && (MipIndex + 2) < Data.HZBUavs.size())
                    ? Data.HZBUavs[MipIndex + 2]
                    : Data.HZBNullUav;
                const D3D12_GPU_DESCRIPTOR_HANDLE DestHandle3 = (bHasFourthMip && (MipIndex + 3) < Data.HZBUavs.size())
                    ? Data.HZBUavs[MipIndex + 3]
                    : Data.HZBNullUav;

                if (SourceHandle.ptr == 0 || DestHandle0.ptr == 0 || DestHandle1.ptr == 0 || DestHandle2.ptr == 0 || DestHandle3.ptr == 0)
                {
                    break;
                }

                ID3D12PipelineState* SelectedPipeline = HZBPipelines[MipsThisDispatch - 1].Get();
                if (!SelectedPipeline)
                {
                    break;
                }

                LocalCommandList->SetPipelineState(SelectedPipeline);
                LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
                LocalCommandList->SetComputeRootDescriptorTable(1, SourceHandle);
                LocalCommandList->SetComputeRootDescriptorTable(2, DestHandle0);
                LocalCommandList->SetComputeRootDescriptorTable(3, DestHandle1);
                LocalCommandList->SetComputeRootDescriptorTable(4, DestHandle2);
                LocalCommandList->SetComputeRootDescriptorTable(5, DestHandle3);

                const uint32_t GroupX = (Constants.DestWidth + 7) / 8;
                const uint32_t GroupY = (Constants.DestHeight + 7) / 8;
                LocalCommandList->Dispatch(GroupX, GroupY, 1);

                if (bHasFourthMip)
                {
                    CurrentWidth = DestWidth3;
                    CurrentHeight = DestHeight3;
                }
                else if (bHasThirdMip)
                {
                    CurrentWidth = DestWidth2;
                    CurrentHeight = DestHeight2;
                }
                else if (bHasSecondMip)
                {
                    CurrentWidth = DestWidth1;
                    CurrentHeight = DestHeight1;
                }
                else
                {
                    CurrentWidth = DestWidth;
                    CurrentHeight = DestHeight;
                }

                std::vector<D3D12_RESOURCE_BARRIER> Barriers;
                Barriers.reserve(MipsThisDispatch + 1);

                D3D12_RESOURCE_BARRIER UavBarrier = {};
                UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                UavBarrier.UAV.pResource = HierarchicalZBuffer.Get();
				if (bLogResourceBarriers)
				{
                    LogInfo("HZB Barrier: UAV sync");
                }
                Barriers.push_back(UavBarrier);

                for (uint32_t LocalMip = 0; LocalMip < MipsThisDispatch; ++LocalMip)
                {
                    const uint32_t TargetMip = MipIndex + LocalMip;
                    if (TargetMip >= Data.MipCount)
                    {
                        break;
                    }

                    D3D12_RESOURCE_BARRIER Barrier = {};
                    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    Barrier.Transition.pResource = HierarchicalZBuffer.Get();
                    Barrier.Transition.StateBefore = MipStates[TargetMip];
                    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    Barrier.Transition.Subresource = D3D12CalcSubresource(TargetMip, 0, 0, Data.MipCount, 1);
                    if (bLogResourceBarriers)
                    {
						LogInfo("HZB Barrier: Mip " + std::to_string(TargetMip) + " "
							+ RendererUtils::ResourceStateToString(Barrier.Transition.StateBefore) + " -> "
							+ RendererUtils::ResourceStateToString(Barrier.Transition.StateAfter));
                    }
                    Barriers.push_back(Barrier);
                    MipStates[TargetMip] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }

                if (!Barriers.empty())
                {
                    LocalCommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
                }

                MipIndex += MipsThisDispatch;
            }

            HZBState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            bHZBReady = true;
        });
    }

    struct FLightingPassData
    {
        bool bUseShadows = false;
    };

    Graph.AddPass<FLightingPassData>("Lighting", [&](FLightingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseShadows = bRenderShadows;

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (Data.bUseShadows)
        {
            Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [this](const FLightingPassData&, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent LightingEvent(LocalCommandList, L"Lighting");

        ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(LightingRTVHandle, nullptr);

        LocalCommandList->SetPipelineState(LightingPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(LightingRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());
        LocalCommandList->SetGraphicsRootDescriptorTable(1, GBufferGpuHandles[0]);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });

    struct FSkyPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSkyPassData>("Sky", [&](FSkyPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = SkyPipelineState && SkyRootSignature && SkyGeometry.IndexCount > 0;
        Data.Camera = &Camera;

        if (Data.bEnabled)
        {
            Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
            Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [this](const FSkyPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent SkyEvent(LocalCommandList, L"SkyAtmosphere");
        LocalCommandList->SetPipelineState(SkyPipelineState.Get());
        LocalCommandList->SetGraphicsRootSignature(SkyRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->IASetVertexBuffers(0, 1, &SkyGeometry.VertexBufferView);
        LocalCommandList->IASetIndexBuffer(&SkyGeometry.IndexBufferView);
        LocalCommandList->OMSetRenderTargets(1, &LightingRTVHandle, FALSE, &DepthStencilHandle);

        UpdateSkyConstants(*Data.Camera);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, SkyConstantBuffer->GetGPUVirtualAddress());
        LocalCommandList->DrawIndexedInstanced(SkyGeometry.IndexCount, 1, 0, 0, 0);
    });

    struct FAutoExposurePassData
    {
        bool bEnabled = false;
        DirectX::XMFLOAT2 InputSize{};
        float DeltaTime = 0.0f;
        float AdaptationSpeedUp = 3.0f;
        float AdaptationSpeedDown = 1.0f;
        uint32_t UseHistory = 0;
        uint32_t ReadIndex = 0;
        uint32_t WriteIndex = 0;
    };

    Graph.AddPass<FAutoExposurePassData>("AutoExposure", [&](FAutoExposurePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bAutoExposureEnabled && AutoExposurePipeline && AutoExposureRootSignature;
        if (Data.bEnabled)
        {
            Data.ReadIndex = 1u - LuminanceWriteIndex;
            Data.WriteIndex = LuminanceWriteIndex;
            Data.InputSize = DirectX::XMFLOAT2(Viewport.Width, Viewport.Height);
            Data.DeltaTime = DeltaTime;
            Data.AdaptationSpeedUp = AutoExposureSpeedUp;
            Data.AdaptationSpeedDown = AutoExposureSpeedDown;
            Data.UseHistory = bLuminanceHistoryValid ? 1u : 0u;
            Builder.ReadTexture(LightingHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadTexture(LuminanceHandles[Data.ReadIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteTexture(LuminanceHandles[Data.WriteIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [this](const FAutoExposurePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent AutoExposureEvent(LocalCommandList, L"AutoExposure");

        struct FAutoExposureConstants
        {
            DirectX::XMFLOAT2 InputSize;
            float DeltaTime;
            float AdaptationSpeedUp;
            float AdaptationSpeedDown;
            uint32_t UseHistory;
            float AutoExposureKey;
            float AutoExposureMin;
            float AutoExposureMax;
        };

        const FAutoExposureConstants Constants =
        {
            Data.InputSize,
            Data.DeltaTime,
            Data.AdaptationSpeedUp,
            Data.AdaptationSpeedDown,
            Data.UseHistory,
            AutoExposureKey,
            AutoExposureMin,
            AutoExposureMax
        };

        ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
        LocalCommandList->SetPipelineState(AutoExposurePipeline.Get());
        LocalCommandList->SetComputeRootSignature(AutoExposureRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRootDescriptorTable(1, LightingBufferHandle);
        LocalCommandList->SetComputeRootDescriptorTable(2, LuminanceSrvHandles[Data.ReadIndex]);
        LocalCommandList->SetComputeRootDescriptorTable(3, LuminanceUavHandles[Data.WriteIndex]);
        LocalCommandList->Dispatch(1, 1, 1);
    });

    struct FTonemapPassData
    {
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        bool bUseAutoExposure = false;
        uint32_t LuminanceIndex = 0;
    };

    Graph.AddPass<FTonemapPassData>("Tonemap", [&](FTonemapPassData& Data, FRGPassBuilder& Builder)
    {
        Data.OutputHandle = RtvHandle;
        Data.bUseAutoExposure = bAutoExposureEnabled;
        Data.LuminanceIndex = LuminanceWriteIndex;
        Builder.ReadTexture(LightingHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LuminanceHandles[Data.LuminanceIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        for (int i = 0; i < 3; ++i)
        {
            Builder.WriteTexture(GBufferHandles[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [this](const FTonemapPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent TonemapEvent(LocalCommandList, L"Tonemap");
        Cmd.SetRenderTarget(Data.OutputHandle, nullptr);

        struct FTonemapConstants
        {
            uint32_t Enabled;
            uint32_t AutoExposureEnabled;
            float Exposure;
            float WhitePoint;
            float Gamma;
            float AutoExposureKey;
            float AutoExposureMin;
            float AutoExposureMax;
        };

        const FTonemapConstants TonemapConstants =
        {
            bTonemapEnabled ? 1u : 0u,
            bAutoExposureEnabled ? 1u : 0u,
            TonemapExposure,
            TonemapWhitePoint,
            TonemapGamma,
            AutoExposureKey,
            AutoExposureMin,
            AutoExposureMax
        };

        ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
        LocalCommandList->SetPipelineState(TonemapPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(TonemapRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(TonemapConstants) / sizeof(uint32_t), &TonemapConstants, 0);
        LocalCommandList->SetGraphicsRootDescriptorTable(1, LightingBufferHandle);
        LocalCommandList->SetGraphicsRootDescriptorTable(2, LuminanceSrvHandles[Data.LuminanceIndex]);
        LocalCommandList->DrawInstanced(3, 1, 0, 0);

        Cmd.TransitionResource(LightingBuffer.Get(), LightingBufferState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        LightingBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
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

    if (bAutoExposureEnabled)
    {
        bLuminanceHistoryValid = true;
        LuminanceWriteIndex = 1u - LuminanceWriteIndex;
    }
    else
    {
        bLuminanceHistoryValid = false;
    }
}

bool FDeferredRenderer::CreateBasePassRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 DescriptorRange = {};
    DescriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    DescriptorRange.NumDescriptors = 4;
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

    // RootParams[1]: Base pass material texture SRV table (t0..t3)
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &DescriptorRange;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(BasePassRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 DescriptorRanges[6] = {};
    for (int i = 0; i < 6; ++i)
    {
        DescriptorRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DescriptorRanges[i].NumDescriptors = 1;
        DescriptorRanges[i].BaseShaderRegister = static_cast<UINT>(i);
        DescriptorRanges[i].RegisterSpace = 0;
        DescriptorRanges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        DescriptorRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Lighting constants (b0)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: GBuffer/IBL/shadow SRV table (t0..t5)
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = _countof(DescriptorRanges);
    RootParams[1].DescriptorTable.pDescriptorRanges = DescriptorRanges;

    D3D12_STATIC_SAMPLER_DESC Samplers[3] = {};
    Samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    Samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    Samplers[0].MinLOD = 0.0f;
    Samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[0].ShaderRegister = 0;
    Samplers[0].RegisterSpace = 0;
    Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    Samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
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

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = _countof(Samplers);
    RootSigDesc.Desc_1_1.pStaticSamplers = Samplers;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(LightingRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCodeWithNormalMap;
    std::vector<uint8_t> PSByteCodeWithoutNormalMap;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> WithNormalDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithoutNormalDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithNormalNoMrDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithoutNormalNoMrDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithNormalNoBaseColorDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithoutNormalNoBaseColorDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithNormalNoMrNoBaseColorDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithoutNormalNoMrNoBaseColorDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=1" };
    const std::vector<std::wstring> WithNormalNoEmissiveDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=0" };
    const std::vector<std::wstring> WithoutNormalNoEmissiveDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=0" };
    const std::vector<std::wstring> WithNormalNoMrNoEmissiveDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=0" };
    const std::vector<std::wstring> WithoutNormalNoMrNoEmissiveDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=1", L"USE_EMISSIVE_MAP=0" };
    const std::vector<std::wstring> WithNormalNoBaseColorNoEmissiveDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=0" };
    const std::vector<std::wstring> WithoutNormalNoBaseColorNoEmissiveDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=1", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=0" };
    const std::vector<std::wstring> WithNormalNoMrNoBaseColorNoEmissiveDefines = { L"USE_NORMAL_MAP=1", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=0" };
    const std::vector<std::wstring> WithoutNormalNoMrNoBaseColorNoEmissiveDefines = { L"USE_NORMAL_MAP=0", L"USE_METALLIC_ROUGHNESS_MAP=0", L"USE_BASE_COLOR_MAP=0", L"USE_EMISSIVE_MAP=0" };

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMap, WithNormalDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMap, WithoutNormalDefines))
    {
        return false;
    }

    std::vector<uint8_t> PSByteCodeWithNormalMapNoMr;
    std::vector<uint8_t> PSByteCodeWithoutNormalMapNoMr;
    std::vector<uint8_t> PSByteCodeWithNormalMapNoBaseColor;
    std::vector<uint8_t> PSByteCodeWithoutNormalMapNoBaseColor;
    std::vector<uint8_t> PSByteCodeWithNormalMapNoMrNoBaseColor;
    std::vector<uint8_t> PSByteCodeWithoutNormalMapNoMrNoBaseColor;
    std::vector<uint8_t> PSByteCodeWithNormalMapNoEmissive;
    std::vector<uint8_t> PSByteCodeWithoutNormalMapNoEmissive;
    std::vector<uint8_t> PSByteCodeWithNormalMapNoMrNoEmissive;
    std::vector<uint8_t> PSByteCodeWithoutNormalMapNoMrNoEmissive;
    std::vector<uint8_t> PSByteCodeWithNormalMapNoBaseColorNoEmissive;
    std::vector<uint8_t> PSByteCodeWithoutNormalMapNoBaseColorNoEmissive;
    std::vector<uint8_t> PSByteCodeWithNormalMapNoMrNoBaseColorNoEmissive;
    std::vector<uint8_t> PSByteCodeWithoutNormalMapNoMrNoBaseColorNoEmissive;

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMapNoMr, WithNormalNoMrDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMapNoMr, WithoutNormalNoMrDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMapNoBaseColor, WithNormalNoBaseColorDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMapNoBaseColor, WithoutNormalNoBaseColorDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMapNoMrNoBaseColor, WithNormalNoMrNoBaseColorDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMapNoMrNoBaseColor, WithoutNormalNoMrNoBaseColorDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMapNoEmissive, WithNormalNoEmissiveDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMapNoEmissive, WithoutNormalNoEmissiveDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMapNoMrNoEmissive, WithNormalNoMrNoEmissiveDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMapNoMrNoEmissive, WithoutNormalNoMrNoEmissiveDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMapNoBaseColorNoEmissive, WithNormalNoBaseColorNoEmissiveDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMapNoBaseColorNoEmissive, WithoutNormalNoBaseColorNoEmissiveDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMapNoMrNoBaseColorNoEmissive, WithNormalNoMrNoBaseColorNoEmissiveDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMapNoMrNoBaseColorNoEmissive, WithoutNormalNoMrNoBaseColorNoEmissiveDefines))
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

    auto InitializeBasePassDesc = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc)
    {
        Desc = {};
        Desc.pRootSignature = BasePassRootSignature.Get();
        Desc.InputLayout = { InputLayout, _countof(InputLayout) };
        Desc.VS = { VSByteCode.data(), VSByteCode.size() };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.SampleDesc.Count = 1;
        Desc.SampleMask = UINT_MAX;

        Desc.RasterizerState = {};
        Desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        Desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        Desc.RasterizerState.FrontCounterClockwise = TRUE;
        Desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        Desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        Desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        Desc.RasterizerState.DepthClipEnable = TRUE;
        Desc.RasterizerState.MultisampleEnable = FALSE;
        Desc.RasterizerState.AntialiasedLineEnable = FALSE;
        Desc.RasterizerState.ForcedSampleCount = 0;
        Desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        Desc.BlendState = {};
        Desc.BlendState.AlphaToCoverageEnable = FALSE;
        Desc.BlendState.IndependentBlendEnable = TRUE;
        for (int i = 0; i < 4; ++i)
        {
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
            Desc.BlendState.RenderTarget[i] = RtBlend;
        }

        Desc.DepthStencilState = {};
        Desc.DepthStencilState.DepthEnable = TRUE;
        Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        Desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        Desc.DepthStencilState.StencilEnable = FALSE;
        Desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        Desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        Desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        Desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        Desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        Desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        Desc.DepthStencilState.BackFace = Desc.DepthStencilState.FrontFace;
        Desc.NumRenderTargets = 4;
        Desc.RTVFormats[0] = GBufferFormats[0];
        Desc.RTVFormats[1] = GBufferFormats[1];
        Desc.RTVFormats[2] = GBufferFormats[2];
        Desc.RTVFormats[3] = LightingFormat;
        Desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        Desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    InitializeBasePassDesc(PsoDesc);
    PsoDesc.PS = { PSByteCodeWithNormalMap.data(), PSByteCodeWithNormalMap.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMap.GetAddressOf())));

    InitializeBasePassDesc(PsoDesc);
    PsoDesc.PS = { PSByteCodeWithoutNormalMap.data(), PSByteCodeWithoutNormalMap.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMap.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithNormalMapNoMr.data(), PSByteCodeWithNormalMapNoMr.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMapNoMr.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithoutNormalMapNoMr.data(), PSByteCodeWithoutNormalMapNoMr.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMapNoMr.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithNormalMapNoBaseColor.data(), PSByteCodeWithNormalMapNoBaseColor.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMapNoBaseColor.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithoutNormalMapNoBaseColor.data(), PSByteCodeWithoutNormalMapNoBaseColor.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMapNoBaseColor.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithNormalMapNoMrNoBaseColor.data(), PSByteCodeWithNormalMapNoMrNoBaseColor.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMapNoMrNoBaseColor.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithoutNormalMapNoMrNoBaseColor.data(), PSByteCodeWithoutNormalMapNoMrNoBaseColor.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMapNoMrNoBaseColor.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithNormalMapNoEmissive.data(), PSByteCodeWithNormalMapNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMapNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithoutNormalMapNoEmissive.data(), PSByteCodeWithoutNormalMapNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMapNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithNormalMapNoMrNoEmissive.data(), PSByteCodeWithNormalMapNoMrNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMapNoMrNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithoutNormalMapNoMrNoEmissive.data(), PSByteCodeWithoutNormalMapNoMrNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMapNoMrNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithNormalMapNoBaseColorNoEmissive.data(), PSByteCodeWithNormalMapNoBaseColorNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMapNoBaseColorNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithoutNormalMapNoBaseColorNoEmissive.data(), PSByteCodeWithoutNormalMapNoBaseColorNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMapNoBaseColorNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithNormalMapNoMrNoBaseColorNoEmissive.data(), PSByteCodeWithNormalMapNoMrNoBaseColorNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMapNoMrNoBaseColorNoEmissive.GetAddressOf())));

    PsoDesc.PS = { PSByteCodeWithoutNormalMapNoMrNoBaseColorNoEmissive.data(), PSByteCodeWithoutNormalMapNoMrNoBaseColorNoEmissive.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMapNoMrNoBaseColorNoEmissive.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateDepthPrepassPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
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
    PsoDesc.pRootSignature = BasePassRootSignature.Get();
    PsoDesc.InputLayout = { InputLayout, _countof(InputLayout) };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Render back faces into the shadow map using clockwise winding to capture silhouettes
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
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;

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
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LightingRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = LightingBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(LightingPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateHZBRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 DescriptorRanges[5] = {};

    DescriptorRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    DescriptorRanges[0].NumDescriptors = 1;
    DescriptorRanges[0].BaseShaderRegister = 0;
    DescriptorRanges[0].RegisterSpace = 0;
    DescriptorRanges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRanges[0].OffsetInDescriptorsFromTableStart = 0;

    DescriptorRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    DescriptorRanges[1].NumDescriptors = 1;
    DescriptorRanges[1].BaseShaderRegister = 0;
    DescriptorRanges[1].RegisterSpace = 0;
    DescriptorRanges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRanges[1].OffsetInDescriptorsFromTableStart = 0;

    DescriptorRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    DescriptorRanges[2].NumDescriptors = 1;
    DescriptorRanges[2].BaseShaderRegister = 1;
    DescriptorRanges[2].RegisterSpace = 0;
    DescriptorRanges[2].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRanges[2].OffsetInDescriptorsFromTableStart = 0;

    DescriptorRanges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    DescriptorRanges[3].NumDescriptors = 1;
    DescriptorRanges[3].BaseShaderRegister = 2;
    DescriptorRanges[3].RegisterSpace = 0;
    DescriptorRanges[3].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRanges[3].OffsetInDescriptorsFromTableStart = 0;

    DescriptorRanges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    DescriptorRanges[4].NumDescriptors = 1;
    DescriptorRanges[4].BaseShaderRegister = 3;
    DescriptorRanges[4].RegisterSpace = 0;
    DescriptorRanges[4].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRanges[4].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 RootParams[6] = {};

    // RootParams[0]: HZB constants (mip counts, dimensions, source mip)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 11;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: HZB source texture SRV (t0)
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &DescriptorRanges[0];

    // RootParams[2]: HZB output UAV 0 (u0)
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[2].DescriptorTable.pDescriptorRanges = &DescriptorRanges[1];

    // RootParams[3]: HZB output UAV 1 (u1)
    RootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[3].DescriptorTable.pDescriptorRanges = &DescriptorRanges[2];

    // RootParams[4]: HZB output UAV 2 (u2)
    RootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[4].DescriptorTable.pDescriptorRanges = &DescriptorRanges[3];

    // RootParams[5]: HZB output UAV 3 (u3)
    RootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[5].DescriptorTable.pDescriptorRanges = &DescriptorRanges[4];

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(HZBRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateHZBPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    for (size_t PipelineIndex = 0; PipelineIndex < HZBPipelines.size(); ++PipelineIndex)
    {
        std::vector<uint8_t> CSByteCode;
        const std::wstring Define = L"HZB_MIPS_PER_DISPATCH=" + std::to_wstring(PipelineIndex + 1);
        const std::vector<std::wstring> Defines = { Define };

        if (!Compiler.CompileFromFile(L"Shaders/BuildHZB.hlsl", L"BuildHZB", CSTarget, CSByteCode, Defines))
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
        PsoDesc.pRootSignature = HZBRootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };

        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(HZBPipelines[PipelineIndex].GetAddressOf())));
    }
    return true;
}

bool FDeferredRenderer::CreateAutoExposureRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 SceneSrvRange = {};
    SceneSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    SceneSrvRange.NumDescriptors = 1;
    SceneSrvRange.BaseShaderRegister = 0;
    SceneSrvRange.RegisterSpace = 0;
    SceneSrvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    SceneSrvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE1 HistorySrvRange = {};
    HistorySrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    HistorySrvRange.NumDescriptors = 1;
    HistorySrvRange.BaseShaderRegister = 1;
    HistorySrvRange.RegisterSpace = 0;
    HistorySrvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    HistorySrvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE1 UavRange = {};
    UavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    UavRange.NumDescriptors = 1;
    UavRange.BaseShaderRegister = 0;
    UavRange.RegisterSpace = 0;
    UavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    UavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 RootParams[4] = {};
    // RootParams[0]: Auto exposure constants (input size, delta time, adaptation)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 9;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: Scene color SRV (t0)
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &SceneSrvRange;

    // RootParams[2]: Luminance history SRV (t1)
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[2].DescriptorTable.pDescriptorRanges = &HistorySrvRange;

    // RootParams[3]: Luminance output UAV (u0)
    RootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[3].DescriptorTable.pDescriptorRanges = &UavRange;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(AutoExposureRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateAutoExposurePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/AutoExposure.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = AutoExposureRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(AutoExposurePipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateTonemapRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 LightingRange = {};
    LightingRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    LightingRange.NumDescriptors = 1;
    LightingRange.BaseShaderRegister = 0;
    LightingRange.RegisterSpace = 0;
    LightingRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    LightingRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE1 LuminanceRange = {};
    LuminanceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    LuminanceRange.NumDescriptors = 1;
    LuminanceRange.BaseShaderRegister = 1;
    LuminanceRange.RegisterSpace = 0;
    LuminanceRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    LuminanceRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 RootParams[3] = {};

    // RootParams[0]: Tonemap constants (exposure/gamma/auto-exposure)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Constants.Num32BitValues = 8;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: Lighting buffer SRV (t0)
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &LightingRange;

    // RootParams[2]: Luminance SRV (t1)
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[2].DescriptorTable.pDescriptorRanges = &LuminanceRange;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(TonemapRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateTonemapPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/Tonemap.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/Tonemap.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = TonemapRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(TonemapPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    Microsoft::WRL::ComPtr<ID3D12Resource>* Targets[3] = { &GBufferA, &GBufferB, &GBufferC };
    const wchar_t* GBufferNames[3] = { L"GBufferA", L"GBufferB", L"GBufferC" };

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    const UINT RtvDescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = {};
    RtvHandle.ptr = 0;

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 4;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(GBufferRTVHeap.GetAddressOf())));
    if (GBufferRTVHeap)
    {
        GBufferRTVHeap->SetName(L"GBufferRTVHeap");
    }

    RtvHandle = GBufferRTVHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < 3; ++i)
    {
        Desc.Width = Width;
        Desc.Height = Height;
        Desc.Format = GBufferFormats[i];

        D3D12_CLEAR_VALUE ClearValue = {};
        ClearValue.Format = Desc.Format;
        ClearValue.Color[0] = 0.0f;
        ClearValue.Color[1] = 0.0f;
        ClearValue.Color[2] = 0.0f;
        ClearValue.Color[3] = 1.0f;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &ClearValue,
            IID_PPV_ARGS(Targets[i]->GetAddressOf())));

        Targets[i]->Get()->SetName(GBufferNames[i]);

        GBufferRTVHandles[i] = RtvHandle;
        D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
        RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        RtvDesc.Format = GBufferFormats[i];
        Device->GetDevice()->CreateRenderTargetView(Targets[i]->Get(), &RtvDesc, RtvHandle);
        RtvHandle.ptr += RtvDescriptorSize;

        GBufferStates[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    Desc.Width = Width;
    Desc.Height = Height;
    Desc.Format = LightingBufferFormat;

    D3D12_CLEAR_VALUE LightingClear = {};
    LightingClear.Format = Desc.Format;
    LightingClear.Color[0] = 0.0f;
    LightingClear.Color[1] = 0.0f;
    LightingClear.Color[2] = 0.0f;
    LightingClear.Color[3] = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        LightingBufferState,
        &LightingClear,
        IID_PPV_ARGS(LightingBuffer.GetAddressOf())));

    LightingBuffer->SetName(L"LightingBuffer");

    LightingRTVHandle = RtvHandle;
    D3D12_RENDER_TARGET_VIEW_DESC LightingRtvDesc = {};
    LightingRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    LightingRtvDesc.Format = LightingBufferFormat;
    Device->GetDevice()->CreateRenderTargetView(LightingBuffer.Get(), &LightingRtvDesc, RtvHandle);

    return true;
}

bool FDeferredRenderer::CreateLuminanceResources(FDX12Device* Device)
{
    if (Device == nullptr)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = 1;
    Desc.Height = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = DXGI_FORMAT_R32_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(LuminanceTextures[0].GetAddressOf())));

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(LuminanceTextures[1].GetAddressOf())));

    if (LuminanceTextures[0])
    {
        LuminanceTextures[0]->SetName(L"LogAverageLuminanceA");
    }
    if (LuminanceTextures[1])
    {
        LuminanceTextures[1]->SetName(L"LogAverageLuminanceB");
    }
    LuminanceStates = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    return true;
}

bool FDeferredRenderer::CreateHZBResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t BaseWidth = (std::max)(1u, (Width + 1) / 2);
    const uint32_t BaseHeight = (std::max)(1u, (Height + 1) / 2);

    HZBWidth = BaseWidth;
    HZBHeight = BaseHeight;
    HZBMipCount = 1;

    uint32_t MipWidth = BaseWidth;
    uint32_t MipHeight = BaseHeight;
    while (MipWidth > 1 || MipHeight > 1)
    {
        MipWidth = (std::max)(1u, MipWidth / 2);
        MipHeight = (std::max)(1u, MipHeight / 2);
        ++HZBMipCount;
    }

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Alignment = 0;
    Desc.Width = BaseWidth;
    Desc.Height = BaseHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = static_cast<UINT16>(HZBMipCount);
    Desc.Format = DXGI_FORMAT_R32_FLOAT;
    Desc.SampleDesc.Count = 1;
    Desc.SampleDesc.Quality = 0;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        HZBState,
        nullptr,
        IID_PPV_ARGS(HierarchicalZBuffer.GetAddressOf())));

    HierarchicalZBuffer->SetName(L"HierarchicalZBuffer");

    {
        D3D12_RESOURCE_DESC NullDesc = {};
        NullDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        NullDesc.Alignment = 0;
        NullDesc.Width = 1;
        NullDesc.Height = 1;
        NullDesc.DepthOrArraySize = 1;
        NullDesc.MipLevels = 1;
        NullDesc.Format = DXGI_FORMAT_R32_FLOAT;
        NullDesc.SampleDesc.Count = 1;
        NullDesc.SampleDesc.Quality = 0;
        NullDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        NullDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &NullDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(HZBNullUavResource.GetAddressOf())));

        HZBNullUavResource->SetName(L"HZBNullUavResource");
    }

    return true;
}

bool FDeferredRenderer::CreateDescriptorHeap(FDX12Device* Device)
{
    const UINT TextureCount = static_cast<UINT>(SceneTextures.size());
    const UINT MipCount = HZBMipCount == 0 ? 1u : static_cast<UINT>(HZBMipCount);
    const UINT HZBDescriptorCount = 3 + (MipCount * 2);

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = TextureCount * 4 + 11 + HZBDescriptorCount;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(DescriptorHeap.GetAddressOf())));
    if (DescriptorHeap)
    {
        DescriptorHeap->SetName(L"DescriptorHeap");
    }

    const UINT DescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle = DescriptorHeap->GetGPUDescriptorHandleForHeapStart();

    const auto CreateSceneTextureSrv = [&](ID3D12Resource* Texture)
    {
        ID3D12Resource* Resource = Texture ? Texture : NullTexture.Get();
        if (!Resource)
        {
            return;
        }

        const D3D12_RESOURCE_DESC TextureDesc = Resource->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC SceneSrvDesc = {};
        SceneSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SceneSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SceneSrvDesc.Format = TextureDesc.Format;
        SceneSrvDesc.Texture2D.MipLevels = TextureDesc.MipLevels;
        SceneSrvDesc.Texture2D.MostDetailedMip = 0;
        SceneSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        Device->GetDevice()->CreateShaderResourceView(Resource, &SceneSrvDesc, CpuHandle);
    };

    for (size_t Index = 0; Index < SceneTextures.size(); ++Index)
    {
        CreateSceneTextureSrv(SceneTextures[Index].BaseColor.Get());
        SceneModels[Index].TextureHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        CreateSceneTextureSrv(SceneTextures[Index].MetallicRoughness.Get());

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        CreateSceneTextureSrv(SceneTextures[Index].Normal.Get());

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        CreateSceneTextureSrv(SceneTextures[Index].Emissive.Get());

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    ID3D12Resource* Buffers[3] = { GBufferA.Get(), GBufferB.Get(), GBufferC.Get() };
    for (int i = 0; i < 3; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
        SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Format = GBufferFormats[i];
        SrvDesc.Texture2D.MipLevels = 1;
        Device->GetDevice()->CreateShaderResourceView(Buffers[i], &SrvDesc, CpuHandle);
        GBufferGpuHandles[i] = GpuHandle;
        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC ShadowSrvDesc = {};
    ShadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ShadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ShadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ShadowSrvDesc.Texture2D.MipLevels = 1;
    Device->GetDevice()->CreateShaderResourceView(ShadowMap.Get(), &ShadowSrvDesc, CpuHandle);
    ShadowMapHandle = GpuHandle;

    CpuHandle.ptr += DescriptorSize;
    GpuHandle.ptr += DescriptorSize;

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC EnvSrvDesc = {};
        EnvSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        EnvSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        EnvSrvDesc.Format = EnvironmentCubeTexture->GetDesc().Format;
        EnvSrvDesc.TextureCube.MipLevels = EnvironmentCubeTexture->GetDesc().MipLevels;
        EnvSrvDesc.TextureCube.MostDetailedMip = 0;
        EnvSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        Device->GetDevice()->CreateShaderResourceView(EnvironmentCubeTexture.Get(), &EnvSrvDesc, CpuHandle);
        EnvironmentCubeHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC BrdfSrvDesc = {};
        BrdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        BrdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        BrdfSrvDesc.Format = BrdfLutTexture->GetDesc().Format;
        BrdfSrvDesc.Texture2D.MipLevels = BrdfLutTexture->GetDesc().MipLevels;
        BrdfSrvDesc.Texture2D.MostDetailedMip = 0;
        BrdfSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        Device->GetDevice()->CreateShaderResourceView(BrdfLutTexture.Get(), &BrdfSrvDesc, CpuHandle);
        BrdfLutHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LightingSrvDesc = {};
        LightingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LightingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LightingSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        LightingSrvDesc.Texture2D.MipLevels = 1;
        Device->GetDevice()->CreateShaderResourceView(LightingBuffer.Get(), &LightingSrvDesc, CpuHandle);
        LightingBufferHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    for (uint32_t Index = 0; Index < LuminanceTextures.size(); ++Index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LuminanceSrvDesc = {};
        LuminanceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LuminanceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LuminanceSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        LuminanceSrvDesc.Texture2D.MipLevels = 1;
        Device->GetDevice()->CreateShaderResourceView(LuminanceTextures[Index].Get(), &LuminanceSrvDesc, CpuHandle);
        LuminanceSrvHandles[Index] = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        D3D12_UNORDERED_ACCESS_VIEW_DESC LuminanceUavDesc = {};
        LuminanceUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        LuminanceUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        LuminanceUavDesc.Texture2D.MipSlice = 0;
        LuminanceUavDesc.Texture2D.PlaneSlice = 0;
        Device->GetDevice()->CreateUnorderedAccessView(LuminanceTextures[Index].Get(), nullptr, &LuminanceUavDesc, CpuHandle);
        LuminanceUavHandles[Index] = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC DepthSrvDesc = {};
        DepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        DepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DepthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        DepthSrvDesc.Texture2D.MipLevels = 1;
        DepthSrvDesc.Texture2D.MostDetailedMip = 0;
        DepthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        Device->GetDevice()->CreateShaderResourceView(DepthBuffer.Get(), &DepthSrvDesc, CpuHandle);
        DepthBufferHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC HZBSrvDesc = {};
        HZBSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        HZBSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        HZBSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        HZBSrvDesc.Texture2D.MipLevels = HZBMipCount;
        HZBSrvDesc.Texture2D.MostDetailedMip = 0;
        HZBSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        Device->GetDevice()->CreateShaderResourceView(HierarchicalZBuffer.Get(), &HZBSrvDesc, CpuHandle);
        HZBSrvHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    HZBSrvMipHandles.clear();
    HZBSrvMipHandles.reserve(HZBMipCount);
    for (uint32_t Mip = 0; Mip < HZBMipCount; ++Mip)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC HZBMipSrvDesc = {};
        HZBMipSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        HZBMipSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        HZBMipSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        HZBMipSrvDesc.Texture2D.MipLevels = 1;
        HZBMipSrvDesc.Texture2D.MostDetailedMip = Mip;
        HZBMipSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        Device->GetDevice()->CreateShaderResourceView(HierarchicalZBuffer.Get(), &HZBMipSrvDesc, CpuHandle);
        HZBSrvMipHandles.push_back(GpuHandle);

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    HZBUavHandles.clear();
    HZBUavHandles.reserve(HZBMipCount);
    for (uint32_t Mip = 0; Mip < HZBMipCount; ++Mip)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
        UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        UavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        UavDesc.Texture2D.MipSlice = Mip;
        UavDesc.Texture2D.PlaneSlice = 0;

        Device->GetDevice()->CreateUnorderedAccessView(HierarchicalZBuffer.Get(), nullptr, &UavDesc, CpuHandle);
        HZBUavHandles.push_back(GpuHandle);

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC NullUavDesc = {};
        NullUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        NullUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        NullUavDesc.Texture2D.MipSlice = 0;
        NullUavDesc.Texture2D.PlaneSlice = 0;

        Device->GetDevice()->CreateUnorderedAccessView(HZBNullUavResource.Get(), nullptr, &NullUavDesc, CpuHandle);
        HZBNullUavHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    return true;
}

bool FDeferredRenderer::CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
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

bool FDeferredRenderer::CreateObjectIdPipeline(FDX12Device* Device)
{
    return RendererUtils::CreateObjectIdPipeline(Device, BasePassRootSignature.Get(), ObjectIdPipeline);
}

namespace
{
    uint32_t ClampToByte(float Value)
    {
        const float Clamped = (std::max)(0.0f, (std::min)(1.0f, Value));
        return static_cast<uint32_t>(std::round(Clamped * 255.0f));
    }

    uint32_t PackColor(const DirectX::XMFLOAT3& Color)
    {
        const uint32_t R = ClampToByte(Color.x);
        const uint32_t G = ClampToByte(Color.y);
        const uint32_t B = ClampToByte(Color.z);
        return 0xff000000 | (B << 16) | (G << 8) | R;
    }

}

bool FDeferredRenderer::CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models)
{
    if (!TextureLoader)
    {
        return false;
    }

    SceneTextures.clear();
    SceneTextures.reserve(Models.size());

    // Prepare all texture load requests
    std::vector<FTextureLoadRequest> Requests;
    Requests.reserve(Models.size() * 4); // 4 textures per model

    // Pre-allocate texture sets
    for (const FSceneModelResource& Model : Models)
    {
        FModelTextureSet TextureSet;
        SceneTextures.push_back(TextureSet);
    }

    // Build load requests for all textures
    for (size_t i = 0; i < Models.size(); ++i)
    {
        const FSceneModelResource& Model = Models[i];
        FModelTextureSet& TextureSet = SceneTextures[i];

        // Base color texture - skip when missing
        if (!Model.BaseColorTexturePath.empty())
        {
            FTextureLoadRequest BaseColorRequest;
            BaseColorRequest.Path = Model.BaseColorTexturePath;
            BaseColorRequest.bUseSolidColor = false;
            BaseColorRequest.OutTexture = &TextureSet.BaseColor;
            Requests.push_back(BaseColorRequest);
        }

        // Metallic roughness texture - skip when missing
        if (!Model.MetallicRoughnessTexturePath.empty())
        {
            FTextureLoadRequest MetallicRoughnessRequest;
            MetallicRoughnessRequest.Path = Model.MetallicRoughnessTexturePath;
            MetallicRoughnessRequest.bUseSolidColor = false;
            MetallicRoughnessRequest.OutTexture = &TextureSet.MetallicRoughness;
            Requests.push_back(MetallicRoughnessRequest);
        }

        // Normal texture - use default normal if path is empty
        FTextureLoadRequest NormalRequest;
        NormalRequest.Path = Model.NormalTexturePath;
        NormalRequest.SolidColor = 0xff8080ff;
        NormalRequest.bUseSolidColor = Model.NormalTexturePath.empty();
        NormalRequest.OutTexture = &TextureSet.Normal;
        Requests.push_back(NormalRequest);

        // Emissive texture - skip when missing
        if (!Model.EmissiveTexturePath.empty())
        {
            FTextureLoadRequest EmissiveRequest;
            EmissiveRequest.Path = Model.EmissiveTexturePath;
            EmissiveRequest.bUseSolidColor = false;
            EmissiveRequest.OutTexture = &TextureSet.Emissive;
            Requests.push_back(EmissiveRequest);
        }
    }

    // Load all textures in parallel
    LogInfo("Loading " + std::to_string(Requests.size()) + " textures in parallel for " + std::to_string(Models.size()) + " models");
    const bool bSuccess = TextureLoader->LoadTexturesParallel(Requests);

    if (!bSuccess)
    {
        LogError("Failed to load scene textures");
    }

    return bSuccess;
}

bool FDeferredRenderer::CreateGpuDrivenResources(FDX12Device* Device)
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

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(IndirectCommandUpload.GetAddressOf())));

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

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &BoundsUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(ModelBoundsUpload.GetAddressOf())));

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
    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, BasePassRootSignature.Get(), IID_PPV_ARGS(IndirectCommandSignature.GetAddressOf())));

    return true;
}

void FDeferredRenderer::UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset)
{
    const DirectX::XMVECTOR LightDir = DirectX::XMLoadFloat3(&LightDirection);
    const DirectX::XMMATRIX LightVP = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
    DirectX::XMStoreFloat4x4(&LightViewProjection, LightVP);

    RendererUtils::UpdateSceneConstants(
        Camera,
        Model,
        LightIntensity,
        LightDir,
        LightColor,
        LightVP,
        bShadowsEnabled ? ShadowStrength : 0.0f,
        ShadowBias,
        static_cast<float>(ShadowMapWidth),
        static_cast<float>(ShadowMapHeight),
        EnvironmentMipCount,
        ConstantBufferMapped,
        ConstantBufferOffset);
}

void FDeferredRenderer::UpdateSkyConstants(const FCamera& Camera)
{
    using namespace DirectX;

    const FFloat3 CameraPosition = Camera.GetPosition();
    const XMMATRIX Scale = XMMatrixScaling(SkySphereRadius, SkySphereRadius, SkySphereRadius);
    const XMMATRIX Translation = XMMatrixTranslation(CameraPosition.x, CameraPosition.y, CameraPosition.z);
    const XMMATRIX World = Scale * Translation;

    const XMVECTOR LightDir = XMLoadFloat3(&LightDirection);
    RendererUtils::UpdateSkyConstants(Camera, World, LightDir, LightColor, SkyConstantBufferMapped);
}

void FDeferredRenderer::UpdateCullingVisibility(const FCamera& Camera)
{
    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    RendererUtils::UpdateCullingVisibility(*CullingCamera, SceneModels, SceneModelVisibility);
}
