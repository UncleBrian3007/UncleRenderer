#include "ForwardRenderer.h"

#include "EnvironmentMap.h"
#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "SceneModelResourceLoader.h"
#include "RenderGraph.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/Camera.h"
#include "../Scene/Mesh.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12CommandContext.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../Core/RendererConfig.h"
#include <d3dx12.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <array>

constexpr uint32_t kForwardBindlessDwordCount = 9;
constexpr uint32_t kForwardPerDrawDwordCount  = 2;

FForwardRenderer::FForwardRenderer()
    : SkyAtmosphere(std::make_unique<FSkyAtmosphere>())
{
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
    FMesh::SetOptimizationStatsLoggingEnabled(Config.bLogMeshOptimizationStats);

    InitializeCommonSettings(Width, Height, Config);

    if (!GetRayTracingRuntime().CreatePipeline(*this, Device))
    {
        LogError("Forward renderer initialization failed: ray tracing pipeline creation failed");
        return false;
    }
    if (!CreateSkinningPipeline(Device))
    {
        LogError("Forward renderer initialization failed: skinning pipeline creation failed");
        return false;
    }
    if (!EnvironmentMap->InitializePipelines(*this, Device))
    {
        LogError("Forward renderer initialization failed: environment build pipeline creation failed");
        return false;
    }

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
    if (!ObjectId->InitializePipelines(Device, RootSignature.Get()))
    {
        LogError("Forward renderer initialization failed: object ID pipeline creation failed");
        return false;
    }

    LogInfo("Creating forward renderer shadow pipeline...");
    const std::vector<std::wstring> ShadowDefines;
    if (!CreateShadowPipeline(Device, RootSignature.Get(), ShadowDefines, ShadowPipelines[0], false))
    {
        LogError("Forward renderer initialization failed: shadow pipeline creation failed");
        return false;
    }
    const std::vector<std::wstring> ShadowSkinnedDefines = { L"USE_SKINNING=1" };
    if (!CreateShadowPipeline(Device, RootSignature.Get(), ShadowSkinnedDefines, ShadowPipelinesSkinned[0], false))
    {
        LogError("Forward renderer initialization failed: shadow pipeline (single-sided skinned) creation failed");
        return false;
    }

    if (!CreateShadowPipeline(Device, RootSignature.Get(), ShadowDefines, ShadowPipelines[1], true))
    {
        LogError("Forward renderer initialization failed: shadow pipeline (double-sided) creation failed");
        return false;
    }
    if (!CreateShadowPipeline(Device, RootSignature.Get(), ShadowSkinnedDefines, ShadowPipelinesSkinned[1], true))
    {
        LogError("Forward renderer initialization failed: shadow pipeline (double-sided skinned) creation failed");
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

    if (!EnvironmentMap->InitializeResources(*this, Device, Config, "Forward"))
    {
        return false;
    }

    if (!CreateDepthResourcesPerFrame(Device, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT))
    {
        LogError("Forward renderer initialization failed: depth resources creation failed");
        return false;
    }

    if (!ObjectId->InitializeResources(Device, Width, Height))
    {
        LogError("Forward renderer initialization failed: object ID resources creation failed");
        return false;
    }

    if (!CreateShadowResources(Device, ShadowMapWidth, ShadowMapHeight, ShadowMap, ShadowDSVHeap, ShadowDSVHandle))
    {
        LogError("Forward renderer initialization failed: shadow resources creation failed");
        return false;
    }

    const std::wstring SceneFilePath = Config.SceneFile.empty() ? L"Assets/Scenes/Scene.json" : Config.SceneFile;
    if (!SceneModelResourceLoader::LoadModelsFromJson(Device, SceneFilePath, SceneModels, SceneCenter, SceneRadius, &GltfScenes))
    {
        LogError("scene JSON could not be loaded.");
        return false;
    }

    if (!CreateSkinnedPositionBuffers())
    {
        LogError("Forward renderer initialization failed: skinned position buffer creation failed");
        return false;
    }

    GltfScenePoses.resize(GltfScenes.size());
    GltfSceneTimes.assign(GltfScenes.size(), 0.0f);
    for (size_t Index = 0; Index < GltfScenes.size(); ++Index)
    {
        InitializeGltfAnimationPose(GltfScenes[Index], GltfScenePoses[Index]);
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

    FSkyPipelineConfig SkyPipelineConfig;
    SkyPipelineConfig.DepthEnable = false;
    SkyPipelineConfig.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SkyPipelineConfig.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    SkyPipelineConfig.DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    const float SkySphereRadius = (std::max)(SceneRadius * 5.0f, 100.0f);
    if (!SkyAtmosphere->Initialize(Device, SkySphereRadius, BackBufferFormat, SkyPipelineConfig))
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

    if (!GpuDebugState.CreateResources(Device)
        || !GpuDebugState.CreateLinePipeline(Device, BackBufferFormat, SceneDepthFormat)
        || !GpuDebugState.CreateBoxPipeline(Device, BackBufferFormat, SceneDepthFormat))
    {
        LogError("Forward renderer initialization failed: GPU debug draw setup failed");
        return false;
    }

    if (GpuDebugState.IsPrintEnabled() && (!GpuDebugState.CreatePrintPipeline(Device, BackBufferFormat) || !GpuDebugState.CreatePrintStatsPipeline(Device)))
    {
        LogError("Forward renderer initialization failed: GPU debug print setup failed");
        return false;
    }

    LogInfo("Forward renderer initialization completed");
    return true;
}

void FForwardRenderer::RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime)
{
    FScopedPixEvent RenderEvent(CmdContext.GetCommandList(), L"ForwardRenderer");

    if (HasRenderFatalError())
    {
        const float FatalClearColor[4] = { 0.05f, 0.0f, 0.1f, 1.0f };
        CmdContext.ClearRenderTarget(RtvHandle, FatalClearColor);
        return;
    }

    RendererUtils::UpdateGltfSceneAnimation(SceneModels, GltfScenes, GltfScenePoses, GltfSceneTimes, DeltaTime);

    GpuDebugState.PreparePrint(CmdContext);
    GpuDebugState.PrepareLine(CmdContext);
    GpuDebugState.PrepareBox(CmdContext);
    ConfigureHZBOcclusion(false, UINT32_MAX, 0, 0, 0);

    FForwardFrameState FrameState;
    PrepareFrameState(Camera, FrameState);
    DispatchSkinning(CmdContext, FrameState.LightViewProjection);
    GetRayTracingRuntime().UpdateBlasRefit(*this, CmdContext);
    GetRayTracingRuntime().BuildTlas(*this, CmdContext);

    FRenderGraph Graph;
    ConfigureFrameGraph(Graph);

    FForwardFrameResources Resources;
    ImportFrameResources(Graph, Resources);

    AddGpuCullingPass(Graph, Camera, Resources.DepthHandle);
    if (!bRayTracedShadowsEnabled || !GetRayTracingRuntime().bRayTracingPipelineReady)
    {
        AddShadowPass(Graph, Camera, FrameState, Resources.ShadowHandle);
    }
    AddDepthPrepass(Graph, Camera, FrameState, Resources.DepthHandle, Resources.ShadowHandle);
    AddRayTracingShadowPass(Graph, Camera, Resources.DepthHandle, FRGResourceHandle{}, Resources.ShadowMaskHandle);
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
    OutState.bRenderShadows = bShadowsEnabled && ShadowPipelines[0] && ShadowPipelines[1] && ShadowMap;
    OutState.bDoDepthPrepass = bDepthPrepassEnabled && DepthPrepassPipelines[0] && DepthPrepassPipelines[1];
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
        &ShadowMap.State,
        { 2048, 2048, DXGI_FORMAT_D32_FLOAT });

    const FRGTextureDesc DepthDesc =
    {
        static_cast<uint32>(Viewport.Width),
        static_cast<uint32>(Viewport.Height),
        DXGI_FORMAT_D24_UNORM_S8_UINT
    };

    D3D12_RESOURCE_STATES& DepthState = GetDepthBufferState();
    OutResources.DepthHandle = Graph.ImportTexture("Depth", GetDepthBuffer(), &DepthState, DepthDesc);
    OutResources.ObjectIdHandle = ObjectId->ImportResource(
        Graph,
        static_cast<uint32_t>(Viewport.Width),
        static_cast<uint32_t>(Viewport.Height));
}

void FForwardRenderer::AddRayTracingShadowPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferHandle, FRGResourceHandle& ShadowMaskHandle)
{
    struct FRayTracingShadowPassData
    {
        FRGResourceHandle ShadowMaskHandle{};
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle GBufferHandle{};
        const FCamera* Camera = nullptr;
    };

    const FRGTextureDesc ShadowMaskDesc =
    {
        static_cast<uint32>(Viewport.Width),
        static_cast<uint32>(Viewport.Height),
        DXGI_FORMAT_R8_UNORM
    };

    Graph.AddPass<FRayTracingShadowPassData>("RTShadowMask", [&, ShadowMaskDesc, DepthHandle, GBufferHandle](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        if (!bRayTracedShadowsEnabled || !GetRayTracingRuntime().bRayTracingPipelineReady || !GBufferHandle)
        {
            return;
        }

        Data.ShadowMaskHandle = Builder.CreateTexture("ShadowMask", ShadowMaskDesc);
        Data.DepthHandle = DepthHandle;
        Data.GBufferHandle = GBufferHandle;
        Data.Camera = &Camera;
        Builder.WriteTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadTexture(Data.DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.KeepAlive();
        ShadowMaskHandle = Data.ShadowMaskHandle;
    }, [this, &Graph](const FRayTracingShadowPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!GetRayTracingRuntime().bRayTracingPipelineReady || !GetRayTracingRuntime().RayQueryShadowPipeline || !GetRayTracingRuntime().RayQueryRootSignature)
        {
            return;
        }

        if (SceneModels.empty() || Data.Camera == nullptr)
        {
            return;
        }

        ID3D12Resource* ShadowMask = Graph.GetTextureResource(Data.ShadowMaskHandle);
        if (!ShadowMask)
        {
            return;
        }

        ID3D12Resource* DepthBuffer = Graph.GetTextureResource(Data.DepthHandle);
        if (!DepthBuffer)
        {
            return;
        }

        ID3D12Resource* GBufferA = Graph.GetTextureResource(Data.GBufferHandle);
        if (!GBufferA)
        {
            return;
        }

        const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
        if (FrameIndex >= GetRayTracingRuntime().TlasResultBuffers.size() || !GetRayTracingRuntime().TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FRayTracingRuntime& RayTracing = GetRayTracingRuntime();
        const uint32_t DepthBindlessIndex = RayTracing.UpdateDepthSrv(*this, FrameIndex, DepthBuffer);
        const uint32_t GBufferABindlessIndex = RayTracing.UpdateGBufferSrv(*this, FRayTracingRuntime::EGBufferSlot::A, GBufferA);
        const uint32_t ShadowMaskUavBindlessIndex = RayTracing.UpdateShadowMaskUav(*this, ShadowMask);
        const uint32_t ShadowMaskSrvBindlessIndex = RayTracing.UpdateShadowMaskSrv(*this, ShadowMask);

        if (DepthBindlessIndex == UINT32_MAX || GBufferABindlessIndex == UINT32_MAX || ShadowMaskUavBindlessIndex == UINT32_MAX || ShadowMaskSrvBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const UINT ClearValues[4] = { 1u, 0u, 0u, 0u };
        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
        const D3D12_GPU_DESCRIPTOR_HANDLE UavGpuHandle = GetBindlessGpuHandle(ShadowMaskUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE UavCpuHandle = GetBindlessCpuClearHandle(ShadowMaskUavBindlessIndex);
        CommandList4->ClearUnorderedAccessViewUint(UavGpuHandle, UavCpuHandle, ShadowMask, ClearValues, 0, nullptr);

        const uint32_t DispatchWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t DispatchHeight = static_cast<uint32_t>(Viewport.Height);
        if (DispatchWidth == 0 || DispatchHeight == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 8;
        const uint32_t GroupCountX = (DispatchWidth + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;
        const uint32_t GroupCountY = (DispatchHeight + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        CommandList4->SetPipelineState(GetRayTracingRuntime().RayQueryShadowPipeline.Get());
        CommandList4->SetComputeRootSignature(GetRayTracingRuntime().RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, GetRayTracingRuntime().TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        const DirectX::XMMATRIX LightViewProjection = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
        UpdateSceneConstants(*Data.Camera, SceneModels.front(), ConstantBufferOffset, LightViewProjection);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);
        const uint32_t BindlessIndices[] =
        {
            DepthBindlessIndex,
            GBufferABindlessIndex,
            ShadowMaskUavBindlessIndex,
            0u,
            DispatchWidth,
            DispatchHeight
        };
        static_assert(_countof(BindlessIndices) <= FRayTracingRuntime::RayQueryRootConstantDwordCount, "Ray query root constants exceed root signature capacity.");
        assert(_countof(BindlessIndices) <= FRayTracingRuntime::RayQueryRootConstantDwordCount);
        CommandList4->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        CommandList4->Dispatch(GroupCountX, GroupCountY, 1);
    });

    Graph.AddPass<FRayTracingShadowPassData>("ShadowMaskSRV", [&, ShadowMaskDesc](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.ShadowMaskHandle = ShadowMaskHandle;
        if (Data.ShadowMaskHandle)
        {
            Builder.ReadTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Builder.KeepAlive();
        }
    }, [](const FRayTracingShadowPassData&, FDX12CommandContext&)
    {
    });
}

void FForwardRenderer::AddGpuCullingPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle)
{
    struct FGpuCullingPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    const char* PassName = "GPU Culling";
    Graph.AddPass<FGpuCullingPassData>(PassName, [this, &Camera, DepthHandle](FGpuCullingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = CanDispatchGpuCulling();
        Data.Camera = &Camera;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [this, PassName](const FGpuCullingPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchGpuCulling(Cmd, *Data.Camera, PassName, ECullingMode::All, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, false);
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

        Cmd.ClearDepth(ShadowDSVHandle, 1.0f);

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
        ID3D12PipelineState* CurrentShadowPipeline = nullptr;
        const auto SetShadowPipeline = [&](bool bUseSkinning, bool bDoubleSided)
        {
            ID3D12PipelineState* Pipeline = bUseSkinning ? ShadowPipelinesSkinned[bDoubleSided ? 1u : 0u].Get() : ShadowPipelines[bDoubleSided ? 1u : 0u].Get();
            if (Pipeline != CurrentShadowPipeline)
            {
                LocalCommandList->SetPipelineState(Pipeline);
                CurrentShadowPipeline = Pipeline;
            }
        };
        SetShadowPipeline(false, false);
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
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
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);
        }

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            if (!ShadowVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBufferAddress + ConstantBufferOffset);
            LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);
            const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
            SetShadowPipeline(bUseSkinning, Model.bDoubleSided);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty()
                    ? L"Model"
                    : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
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

        Cmd.ClearDepth(GetDSVHandle());

        if (!IsValidBindlessIndex(ShadowMap.SrvBindlessIndex) || !IsValidBindlessIndex(EnvironmentCubeBindlessIndex) || !IsValidBindlessIndex(BrdfLutBindlessIndex))
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthHandle);

        ID3D12PipelineState* CurrentPipeline = nullptr;
        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            if (!SceneModelVisibility.empty() && !SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Mask)
                || Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);

            const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
            ID3D12PipelineState* DesiredPipeline = bUseSkinning ? DepthPrepassPipelinesSkinned[Model.bDoubleSided ? 1u : 0u].Get() : DepthPrepassPipelines[Model.bDoubleSided ? 1u : 0u].Get();
            if (DesiredPipeline != CurrentPipeline)
            {
                CurrentPipeline = DesiredPipeline;
                LocalCommandList->SetPipelineState(CurrentPipeline);
            }

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t ShadowMaskEnabled = (bRayTracedShadowsEnabled && IsValidBindlessIndex(ShadowMaskBindlessIndex)) ? 1u : 0u;
            const uint32_t ResolvedShadowMaskIndex = ShadowMaskEnabled ? ShadowMaskBindlessIndex : ShadowMap.SrvBindlessIndex;
            const uint32_t ForwardBindlessIndices[] =
            {
                Model.BaseColorBindlessIndex,
                Model.MetallicRoughnessBindlessIndex,
                Model.NormalBindlessIndex,
                Model.EmissiveBindlessIndex,
                ShadowMap.SrvBindlessIndex,
                ResolvedShadowMaskIndex,
                ShadowMaskEnabled,
                EnvironmentCubeBindlessIndex,
                BrdfLutBindlessIndex
            };
            static_assert(_countof(ForwardBindlessIndices) <= kForwardBindlessDwordCount);
            static_assert(1u <= kForwardPerDrawDwordCount);
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(ForwardBindlessIndices), ForwardBindlessIndices, 0);
            LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty()
                    ? L"Model"
                    : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
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
        Data.bEnabled = SkyAtmosphere && SkyAtmosphere->IsReady();
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

        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        FSkyAtmosphereFrameParameters Parameters;
        Parameters.Camera = Data.Camera;
        Parameters.Projection = Data.Camera->GetProjectionMatrix();
        Parameters.LightDirection = GetLightDirection();
        Parameters.LightColor = GetLightColor();
        Parameters.Viewport = Viewport;
        Parameters.ScissorRect = ScissorRect;
        SkyAtmosphere->Draw(Cmd, Parameters, Data.OutputHandle, DepthHandle, Data.bClearDepth);
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
        Data.bClearDepth = !FrameState.bDoDepthPrepass && !(SkyAtmosphere && SkyAtmosphere->IsReady());

        Builder.WriteTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (FrameState.bRenderShadows)
        {
            Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }, [this](const FForwardPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        Cmd.SetRenderTarget(Data.OutputHandle, &DepthHandle);

        if (Data.bClearDepth)
        {
            Cmd.ClearDepth(GetDSVHandle());
        }

        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());

        if (!IsValidBindlessIndex(ShadowMap.SrvBindlessIndex) || !IsValidBindlessIndex(EnvironmentCubeBindlessIndex) || !IsValidBindlessIndex(BrdfLutBindlessIndex))
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
		LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);
        }

        ID3D12Resource* IndirectBuffer = GetIndirectCommandBuffer();
        ID3D12Resource* RunCountBuffer = GetMeshletRunCountBuffer();
        if (bEnableIndirectDraw && IndirectCommandSignature && IndirectBuffer && RunCountBuffer && !IndirectDrawRanges.empty())
        {
            auto SelectPipelineByKey = [&](uint32_t Key) -> ID3D12PipelineState*
            {
                const bool bUseSkinning = (Key & (1u << 8)) != 0;
                const uint32_t MaterialKey = (Key & 0xFFu) | (((Key >> 9) & 1u) << 8);
                if (!EnsureBasePassPipelineOrFail(MaterialKey, bUseSkinning, "ForwardBasePass/Indirect"))
                {
                    return nullptr;
                }
                return bUseSkinning ? BasePassPipelinesSkinned[MaterialKey].Get() : BasePassPipelines[MaterialKey].Get();
            };

            for (size_t RangeIndex = 0; RangeIndex < IndirectDrawRanges.size(); ++RangeIndex)
            {
                const FIndirectDrawRange& Range = IndirectDrawRanges[RangeIndex];
                const bool bRangeSkinning = (Range.PipelineKey & (1u << 8)) != 0;
                if (bRangeSkinning && !bEnableSkinningIndirectDraw)
                {
                    continue;
                }
                ID3D12PipelineState* Pipeline = SelectPipelineByKey(Range.PipelineKey);
                if (!Pipeline)
                {
                    return;
                }
                LocalCommandList->SetPipelineState(Pipeline);
                const uint32_t ShadowMaskEnabled = (bRayTracedShadowsEnabled && IsValidBindlessIndex(ShadowMaskBindlessIndex)) ? 1u : 0u;
                const uint32_t ResolvedShadowMaskIndex = ShadowMaskEnabled ? ShadowMaskBindlessIndex : ShadowMap.SrvBindlessIndex;
                const uint32_t ForwardBindlessIndices[] =
                {
                    Range.MaterialBindlessIndices[0],
                    Range.MaterialBindlessIndices[1],
                    Range.MaterialBindlessIndices[2],
                    Range.MaterialBindlessIndices[3],
                    ShadowMap.SrvBindlessIndex,
                    ResolvedShadowMaskIndex,
                    ShadowMaskEnabled,
                    EnvironmentCubeBindlessIndex,
                    BrdfLutBindlessIndex
                };
                static_assert(_countof(ForwardBindlessIndices) <= kForwardBindlessDwordCount);
                LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(ForwardBindlessIndices), ForwardBindlessIndices, 0);

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

            if (!bEnableSkinningIndirectDraw)
            {
                for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
                {
                    if (!SceneModelVisibility.empty() && !SceneModelVisibility[ModelIndex])
                    {
                        continue;
                    }

                    const FSceneModelResource& Model = SceneModels[ModelIndex];
                    if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
                    {
                        continue;
                    }

                    const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
                    if (!bUseSkinning)
                    {
                        continue;
                    }

                    const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
                    const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                    const uint32_t PipelineKey = (ModelPipelineKey & 0xFFu) | (((ModelPipelineKey >> 9) & 1u) << 8);
                    if (!EnsureBasePassPipelineOrFail(PipelineKey, true, "ForwardBasePass/SkinningFallback"))
                    {
                        return;
                    }
                    LocalCommandList->SetPipelineState(BasePassPipelinesSkinned[PipelineKey].Get());

                    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
                    LocalCommandList->SetGraphicsRootConstantBufferView(
                        0,
                        ConstantBufferAddress + ConstantBufferOffset);
                    const uint32_t ShadowMaskEnabled = (bRayTracedShadowsEnabled && IsValidBindlessIndex(ShadowMaskBindlessIndex)) ? 1u : 0u;
                    const uint32_t ResolvedShadowMaskIndex = ShadowMaskEnabled ? ShadowMaskBindlessIndex : ShadowMap.SrvBindlessIndex;
                    const uint32_t ForwardBindlessIndices[] =
                    {
                        Model.BaseColorBindlessIndex,
                        Model.MetallicRoughnessBindlessIndex,
                        Model.NormalBindlessIndex,
                        Model.EmissiveBindlessIndex,
                        ShadowMap.SrvBindlessIndex,
                        ResolvedShadowMaskIndex,
                        ShadowMaskEnabled,
                        EnvironmentCubeBindlessIndex,
                        BrdfLutBindlessIndex
                    };
                    static_assert(_countof(ForwardBindlessIndices) <= kForwardBindlessDwordCount);
                    static_assert(1u <= kForwardPerDrawDwordCount);
                    LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(ForwardBindlessIndices), ForwardBindlessIndices, 0);
                    LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);

                    if (AreModelPixEventsEnabled())
                    {
                        const std::wstring ModelLabel = Model.Name.empty()
                            ? L"Model"
                            : std::wstring(Model.Name.begin(), Model.Name.end());
                        FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
                    }
                    else
                    {
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
                    }
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
                if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
                {
                    continue;
                }
                const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

                const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                const bool bUseSkinning = (ModelPipelineKey & (1u << 8)) != 0;
                const uint32_t PipelineKey = (ModelPipelineKey & 0xFFu) | (((ModelPipelineKey >> 9) & 1u) << 8);
                if (!EnsureBasePassPipelineOrFail(PipelineKey, bUseSkinning, "ForwardBasePass/Direct"))
                {
                    return;
                }
                LocalCommandList->SetPipelineState(bUseSkinning ? BasePassPipelinesSkinned[PipelineKey].Get() : BasePassPipelines[PipelineKey].Get());

                const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
                LocalCommandList->SetGraphicsRootConstantBufferView(
                    0,
                    ConstantBufferAddress + ConstantBufferOffset);
                const uint32_t ShadowMaskEnabled = (bRayTracedShadowsEnabled && IsValidBindlessIndex(ShadowMaskBindlessIndex)) ? 1u : 0u;
                const uint32_t ResolvedShadowMaskIndex = ShadowMaskEnabled ? ShadowMaskBindlessIndex : ShadowMap.SrvBindlessIndex;
                const uint32_t ForwardBindlessIndices[] =
                {
                    Model.BaseColorBindlessIndex,
                    Model.MetallicRoughnessBindlessIndex,
                    Model.NormalBindlessIndex,
                    Model.EmissiveBindlessIndex,
                    ShadowMap.SrvBindlessIndex,
                    ResolvedShadowMaskIndex,
                    ShadowMaskEnabled,
                    EnvironmentCubeBindlessIndex,
                    BrdfLutBindlessIndex
                };
                static_assert(_countof(ForwardBindlessIndices) <= kForwardBindlessDwordCount);
                static_assert(1u <= kForwardPerDrawDwordCount);
                LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(ForwardBindlessIndices), ForwardBindlessIndices, 0);
                LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);

                if (AreModelPixEventsEnabled())
                {
                    const std::wstring ModelLabel = Model.Name.empty()
                        ? L"Model"
                        : std::wstring(Model.Name.begin(), Model.Name.end());
                    FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
                }
                else
                {
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
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
        Data.bEnabled = ObjectId->IsReadbackRequested() && ObjectId->IsReady();
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


        LocalCommandList->SetPipelineState(ObjectId->GetPipeline());
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = GetDSVHandle();
        const D3D12_CPU_DESCRIPTOR_HANDLE ObjectIdRtvHandle = ObjectId->GetRtvHandle();
        LocalCommandList->OMSetRenderTargets(1, &ObjectIdRtvHandle, FALSE, &DepthHandle);

        const UINT ClearValue[4] = { 0, 0, 0, 0 };
        LocalCommandList->ClearRenderTargetView(ObjectId->GetRtvHandle(), reinterpret_cast<const float*>(ClearValue), 0, nullptr);

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
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            LocalCommandList->IASetVertexBuffers(0, Model.Geometry.VertexBufferCount, Model.Geometry.VertexBufferViews.data());
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);
            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBufferAddress + ConstantBufferOffset);
            LocalCommandList->SetGraphicsRoot32BitConstants(1, 1, &Model.ObjectId, 0);

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
        const uint32_t ReadX = (std::min)(ObjectId->GetReadbackX(), Width > 0 ? Width - 1 : 0);
        const uint32_t ReadY = (std::min)(ObjectId->GetReadbackY(), Height > 0 ? Height - 1 : 0);

        D3D12_RESOURCE_BARRIER Barrier = CD3DX12_RESOURCE_BARRIER::Transition(ObjectId->GetTexture(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        LocalCommandList->ResourceBarrier(1, &Barrier);

        D3D12_TEXTURE_COPY_LOCATION Src = {};
        Src.pResource = ObjectId->GetTexture();
        Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst = {};
        Dst.pResource = ObjectId->GetReadbackResource();
        Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint = ObjectId->GetFootprint();

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

        ObjectId->SetReadbackRecorded();
    });
}

void FForwardRenderer::AddDebugPrintPass(FRenderGraph& Graph, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle)
{
    struct FDebugPrintPassData
    {
        bool bPrintEnabled = false;
        bool bBoxEnabled = false;
        bool bLineEnabled = false;
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
    };

    Graph.AddPass<FDebugPrintPassData>("GpuDebugPrint", [this, RtvHandle](FDebugPrintPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bPrintEnabled = GpuDebugState.IsPrintPassReady();
        Data.bBoxEnabled = GpuDebugState.IsBoxPassReady();
        Data.bLineEnabled = GpuDebugState.IsLinePassReady();
        Data.OutputHandle = RtvHandle;
        if (Data.bPrintEnabled || Data.bBoxEnabled || Data.bLineEnabled)
        {
            Builder.KeepAlive();
        }
    }, [this](const FDebugPrintPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bPrintEnabled && !Data.bBoxEnabled && !Data.bLineEnabled)
        {
            return;
        }

        if (Data.bPrintEnabled)
        {
            GpuDebugState.DispatchPrintStats(Device, Cmd);
            GpuDebugState.RenderPrint(Device, Viewport, ScissorRect, Cmd, Data.OutputHandle);
        }
        if (Data.bBoxEnabled)
        {
            GpuDebugState.RenderBox(Device, Viewport, ScissorRect, GetSceneConstantBufferAddress(), GetDSVHandle(), Cmd, Data.OutputHandle);
        }
        if (Data.bLineEnabled)
        {
            GpuDebugState.RenderLine(Device, Viewport, ScissorRect, GetSceneConstantBufferAddress(), GetDSVHandle(), Cmd, Data.OutputHandle);
        }
    });
}

void FForwardRenderer::UpdateCullingVisibility(const FCamera& Camera)
{
    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    const bool bGpuCullingActive = CanDispatchGpuCulling();
    RendererUtils::UpdateCullingVisibility(*CullingCamera, SceneModels, SceneModelVisibility, !bGpuCullingActive);
}

bool FForwardRenderer::CreateRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    // RootParams[0]: Scene constant buffer (b0), used in Shaders/ForwardVS.hlsl VSMain and Shaders/ForwardPS.hlsl PSMain
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[1]: Forward bindless indices (b1), used in Shaders/ForwardPS.hlsl PSMain
    RootParams[1].InitAsConstants(kForwardBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    // RootParams[2]: Per-draw constants (b2): DrawIndexStart (dword0) + DrawDataIndex (dword1)
    RootParams[2].InitAsConstants(kForwardPerDrawDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    CD3DX12_STATIC_SAMPLER_DESC Samplers[3];
    CD3DX12_STATIC_SAMPLER_DESC::Init(Samplers[0], 0,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f, 4, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC::Init(Samplers[1], 1,
        D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f, 0, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC::Init(Samplers[2], 2,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 0, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
        0.0f, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootDesc;
    RootDesc.Init_1_1(
        _countof(RootParams), RootParams,
        _countof(Samplers), Samplers,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

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


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ForwardVS.hlsl", ForwardBasePassVsBytecodes[0]))
    {
        return false;
    }
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ForwardVS.hlsl", ForwardBasePassVsBytecodes[1], { L"USE_SKINNING=1" }))
    {
        return false;
    }

    ForwardBasePassBackBufferFormat = BackBufferFormat;
    for (uint32_t PipelineKey = 0; PipelineKey < 512; ++PipelineKey)
    {
        BasePassPipelines[PipelineKey].Reset();
        BasePassPipelinesSkinned[PipelineKey].Reset();
        ForwardBasePassFailureLogged[PipelineKey] = false;
    }
    for (uint32_t Permutation = 0; Permutation < 256; ++Permutation)
    {
        ForwardBasePassPsBytecodes[Permutation].clear();
        ForwardBasePassPsCompiled[Permutation] = false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    if (!BuildForwardBasePassPsoDesc(0, false, PsoDesc))
    {
        return false;
    }

    if (bDepthPrepassEnabled)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC DepthPrepassDesc = PsoDesc;
        DepthPrepassDesc.PS = { nullptr, 0 };
        DepthPrepassDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        DepthPrepassDesc.NumRenderTargets = 0;
        DepthPrepassDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;

        for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
        {
            DepthPrepassDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            DepthPrepassDesc.VS = { ForwardBasePassVsBytecodes[0].data(), ForwardBasePassVsBytecodes[0].size() };
            HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&DepthPrepassDesc, IID_PPV_ARGS(DepthPrepassPipelines[DoubleSidedVariant].GetAddressOf())));
            DepthPrepassDesc.VS = { ForwardBasePassVsBytecodes[1].data(), ForwardBasePassVsBytecodes[1].size() };
            HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&DepthPrepassDesc, IID_PPV_ARGS(DepthPrepassPipelinesSkinned[DoubleSidedVariant].GetAddressOf())));
        }
    }

    return true;
}


bool FForwardRenderer::CompileForwardBasePassPs(uint32_t PipelineKey, std::vector<uint8_t>& OutPs)
{
    FShaderCompiler Compiler;

    const bool bUseNormal = (PipelineKey & 1u) != 0;
    const bool bUseMr = (PipelineKey & 2u) != 0;
    const bool bUseBaseColor = (PipelineKey & 4u) != 0;
    const bool bUseEmissive = (PipelineKey & 8u) != 0;
    const bool bUseAlphaMask = (PipelineKey & 16u) != 0;
    const bool bUseSheenModel = (PipelineKey & 32u) != 0;
    const bool bUseClearcoatModel = (PipelineKey & 64u) != 0;
    const bool bUseAnisotropyModel = (PipelineKey & 128u) != 0;

    std::vector<std::wstring> Defines;
    Defines.push_back(bUseNormal ? L"USE_NORMAL_MAP=1" : L"USE_NORMAL_MAP=0");
    Defines.push_back(bUseMr ? L"USE_METALLIC_ROUGHNESS_MAP=1" : L"USE_METALLIC_ROUGHNESS_MAP=0");
    Defines.push_back(bUseBaseColor ? L"USE_BASE_COLOR_MAP=1" : L"USE_BASE_COLOR_MAP=0");
    Defines.push_back(bUseEmissive ? L"USE_EMISSIVE_MAP=1" : L"USE_EMISSIVE_MAP=0");
    Defines.push_back(bUseSheenModel ? L"SHADINGMODEL_SHEEN=1" : L"SHADINGMODEL_SHEEN=0");
    Defines.push_back(bUseClearcoatModel ? L"SHADINGMODEL_CLEARCOAT=1" : L"SHADINGMODEL_CLEARCOAT=0");
    Defines.push_back(bUseAnisotropyModel ? L"SHADINGMODEL_ANISOTROPY=1" : L"SHADINGMODEL_ANISOTROPY=0");
    if (bUseAlphaMask)
    {
        Defines.push_back(L"USE_ALPHA_MASK=1");
    }

    return RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/ForwardPS.hlsl", OutPs, Defines);
}

bool FForwardRenderer::BuildForwardBasePassPsoDesc(uint32_t PipelineKey, bool bUseSkinning, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const
{
    if (ForwardBasePassBackBufferFormat == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    OutDesc = {};
    OutDesc.pRootSignature = RootSignature.Get();
    OutDesc.InputLayout = { nullptr, 0 };
    const std::vector<uint8_t>& VsByteCode = ForwardBasePassVsBytecodes[bUseSkinning ? 1u : 0u];
    OutDesc.VS = { VsByteCode.data(), VsByteCode.size() };
    OutDesc.PS = { ForwardBasePassPsBytecodes[PipelineKey & 0xFFu].data(), ForwardBasePassPsBytecodes[PipelineKey & 0xFFu].size() };
    OutDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    OutDesc.SampleDesc.Count = 1;
    OutDesc.SampleMask = UINT_MAX;

    OutDesc.RasterizerState = {};
    OutDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    OutDesc.RasterizerState.CullMode = (PipelineKey & 256u) != 0 ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
    OutDesc.RasterizerState.FrontCounterClockwise = TRUE;
    OutDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    OutDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    OutDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    OutDesc.RasterizerState.DepthClipEnable = TRUE;
    OutDesc.RasterizerState.MultisampleEnable = FALSE;
    OutDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    OutDesc.RasterizerState.ForcedSampleCount = 0;
    OutDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    OutDesc.BlendState = {};
    OutDesc.BlendState.AlphaToCoverageEnable = FALSE;
    OutDesc.BlendState.IndependentBlendEnable = FALSE;
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
    OutDesc.BlendState.RenderTarget[0] = RtBlend;

    OutDesc.DepthStencilState = {};
    OutDesc.DepthStencilState.DepthEnable = TRUE;
    OutDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    OutDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    OutDesc.DepthStencilState.StencilEnable = FALSE;
    OutDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    OutDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    OutDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    OutDesc.DepthStencilState.BackFace = OutDesc.DepthStencilState.FrontFace;
    OutDesc.NumRenderTargets = 1;
    OutDesc.RTVFormats[0] = ForwardBasePassBackBufferFormat;
    OutDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    OutDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return true;
}

bool FForwardRenderer::EnsureBasePassPipeline(uint32_t PipelineKey, bool bUseSkinning)
{
    auto& TargetPipeline = bUseSkinning ? BasePassPipelinesSkinned[PipelineKey] : BasePassPipelines[PipelineKey];
    if (TargetPipeline)
    {
        return true;
    }

    std::lock_guard<std::mutex> Lock(ForwardBasePassPipelineMutex);
    if (TargetPipeline)
    {
        return true;
    }

    const uint32_t PsKey = PipelineKey & 0xFFu;
    if (!ForwardBasePassPsCompiled[PsKey])
    {
        if (!CompileForwardBasePassPs(PsKey, ForwardBasePassPsBytecodes[PsKey]))
        {
            return false;
        }
        ForwardBasePassPsCompiled[PsKey] = true;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = {};
    if (!BuildForwardBasePassPsoDesc(PipelineKey, bUseSkinning, Desc))
    {
        return false;
    }

    HRESULT Hr = Device->GetDevice()->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(TargetPipeline.GetAddressOf()));
    if (FAILED(Hr))
    {
        return false;
    }

    LogInfo(std::string("Forward BasePass pipeline created. key=") + std::to_string(PipelineKey) + ", skinned=" + (bUseSkinning ? "1" : "0"));
    return true;
}

bool FForwardRenderer::EnsureBasePassPipelineOrFail(uint32_t PipelineKey, bool bUseSkinning, const char* PassContext)
{
    if (EnsureBasePassPipeline(PipelineKey, bUseSkinning))
    {
        return true;
    }

    if (!ForwardBasePassFailureLogged[PipelineKey])
    {
        ForwardBasePassFailureLogged[PipelineKey] = true;
        LogError(std::string("Forward BasePass pipeline creation failed. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", key=" + std::to_string(PipelineKey)
            + ", skinned=" + (bUseSkinning ? "1" : "0"));
    }

    SetRenderFatalError(std::string("Forward BasePass fatal failure. context=")
        + (PassContext ? PassContext : "Unknown")
        + ", key=" + std::to_string(PipelineKey)
        + ", skinned=" + (bUseSkinning ? "1" : "0"));
    return false;
}

bool FForwardRenderer::CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models)
{
    SceneTextures.clear();
    SceneTextures.reserve(Models.size() * 10); // base color + metallic roughness + normal + emissive + sheen + clearcoat + anisotropy
    ShadowMap.SrvBindlessIndex = UINT32_MAX;
    EnvironmentCubeBindlessIndex = UINT32_MAX;
    BrdfLutBindlessIndex = UINT32_MAX;

    // Prepare parallel texture loading
    struct FTextureLoadResult
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> BaseColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> MetallicRoughness;
        Microsoft::WRL::ComPtr<ID3D12Resource> Normal;
        Microsoft::WRL::ComPtr<ID3D12Resource> Emissive;
        Microsoft::WRL::ComPtr<ID3D12Resource> SheenColor;
        Microsoft::WRL::ComPtr<ID3D12Resource> SheenRoughness;
        Microsoft::WRL::ComPtr<ID3D12Resource> Clearcoat;
        Microsoft::WRL::ComPtr<ID3D12Resource> ClearcoatRoughness;
        Microsoft::WRL::ComPtr<ID3D12Resource> ClearcoatNormal;
        Microsoft::WRL::ComPtr<ID3D12Resource> Anisotropy;
    };
    
    std::vector<FTextureLoadResult> LoadResults(Models.size());
    std::vector<FTextureLoadRequest> Requests;
    Requests.reserve(Models.size() * 10);

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

        if (!Models[Index].SheenColorTexturePath.empty())
        {
            FTextureLoadRequest SheenColorRequest;
            SheenColorRequest.Path = Models[Index].SheenColorTexturePath;
            SheenColorRequest.bUseSolidColor = false;
            SheenColorRequest.bUseSRGB = true;
            SheenColorRequest.OutTexture = &LoadResults[Index].SheenColor;
            Requests.push_back(SheenColorRequest);
        }

        if (!Models[Index].SheenRoughnessTexturePath.empty())
        {
            FTextureLoadRequest SheenRoughnessRequest;
            SheenRoughnessRequest.Path = Models[Index].SheenRoughnessTexturePath;
            SheenRoughnessRequest.bUseSolidColor = false;
            SheenRoughnessRequest.OutTexture = &LoadResults[Index].SheenRoughness;
            Requests.push_back(SheenRoughnessRequest);
        }

        if (!Models[Index].ClearcoatTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatRequest;
            ClearcoatRequest.Path = Models[Index].ClearcoatTexturePath;
            ClearcoatRequest.bUseSolidColor = false;
            ClearcoatRequest.OutTexture = &LoadResults[Index].Clearcoat;
            Requests.push_back(ClearcoatRequest);
        }

        if (!Models[Index].ClearcoatRoughnessTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatRoughnessRequest;
            ClearcoatRoughnessRequest.Path = Models[Index].ClearcoatRoughnessTexturePath;
            ClearcoatRoughnessRequest.bUseSolidColor = false;
            ClearcoatRoughnessRequest.OutTexture = &LoadResults[Index].ClearcoatRoughness;
            Requests.push_back(ClearcoatRoughnessRequest);
        }

        if (!Models[Index].ClearcoatNormalTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatNormalRequest;
            ClearcoatNormalRequest.Path = Models[Index].ClearcoatNormalTexturePath;
            ClearcoatNormalRequest.bUseSolidColor = false;
            ClearcoatNormalRequest.OutTexture = &LoadResults[Index].ClearcoatNormal;
            Requests.push_back(ClearcoatNormalRequest);
        }

        if (!Models[Index].AnisotropyTexturePath.empty())
        {
            FTextureLoadRequest AnisotropyRequest;
            AnisotropyRequest.Path = Models[Index].AnisotropyTexturePath;
            AnisotropyRequest.bUseSolidColor = false;
            AnisotropyRequest.OutTexture = &LoadResults[Index].Anisotropy;
            Requests.push_back(AnisotropyRequest);
        }
    }

    // Load all textures in parallel
    LogInfo("Loading " + std::to_string(Requests.size()) + " textures in parallel for " + std::to_string(Models.size()) + " models");
    if (!TextureLoader->LoadTexturesParallel(Requests))
    {
        LogError("Failed to load scene textures");
        return false;
    }

    const auto CreateSceneTextureSrv = [&](ID3D12Resource* Texture)
    {
        if (!Texture)
        {
            return UINT32_MAX;
        }

        const D3D12_RESOURCE_DESC TextureDesc = Texture->GetDesc();

        return Device->CreateBindlessSrv(Texture,
            CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(TextureDesc.Format, TextureDesc.MipLevels));
    };

    const auto ShadowSrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, 1);

    ID3D12Resource* EnvironmentCube = GetEnvironmentCubeTexture();
    ID3D12Resource* BrdfLut = GetBrdfLutTexture();

    const auto EnvSrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::TexCube(
        EnvironmentCube ? EnvironmentCube->GetDesc().Format : DXGI_FORMAT_UNKNOWN,
        EnvironmentCube ? EnvironmentCube->GetDesc().MipLevels : 1);

    const auto BrdfSrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(
        BrdfLut ? BrdfLut->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM,
        BrdfLut ? BrdfLut->GetDesc().MipLevels : 1);

    for (size_t Index = 0; Index < Models.size(); ++Index)
    {
        SceneTextures.push_back(LoadResults[Index].BaseColor);
        if (LoadResults[Index].BaseColor)
        {
            const std::wstring Name = L"BaseColorTexture_" + std::to_wstring(Index);
            LoadResults[Index].BaseColor->SetName(Name.c_str());
        }
        SceneModels[Index].BaseColorBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].BaseColor.Get());

        SceneTextures.push_back(LoadResults[Index].MetallicRoughness);
        if (LoadResults[Index].MetallicRoughness)
        {
            const std::wstring Name = L"MetallicRoughnessTexture_" + std::to_wstring(Index);
            LoadResults[Index].MetallicRoughness->SetName(Name.c_str());
        }
        SceneModels[Index].MetallicRoughnessBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].MetallicRoughness.Get());

        SceneTextures.push_back(LoadResults[Index].Normal);
        if (LoadResults[Index].Normal)
        {
            const std::wstring Name = L"NormalTexture_" + std::to_wstring(Index);
            LoadResults[Index].Normal->SetName(Name.c_str());
        }
        SceneModels[Index].NormalBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].Normal.Get());

        SceneTextures.push_back(LoadResults[Index].Emissive);
        if (LoadResults[Index].Emissive)
        {
            const std::wstring Name = L"EmissiveTexture_" + std::to_wstring(Index);
            LoadResults[Index].Emissive->SetName(Name.c_str());
        }
        SceneModels[Index].EmissiveBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].Emissive.Get());

        SceneTextures.push_back(LoadResults[Index].SheenColor);
        if (LoadResults[Index].SheenColor)
        {
            const std::wstring Name = L"SheenColorTexture_" + std::to_wstring(Index);
            LoadResults[Index].SheenColor->SetName(Name.c_str());
        }
        SceneModels[Index].SheenColorBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].SheenColor.Get());

        SceneTextures.push_back(LoadResults[Index].SheenRoughness);
        if (LoadResults[Index].SheenRoughness)
        {
            const std::wstring Name = L"SheenRoughnessTexture_" + std::to_wstring(Index);
            LoadResults[Index].SheenRoughness->SetName(Name.c_str());
        }
        SceneModels[Index].SheenRoughnessBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].SheenRoughness.Get());

        SceneTextures.push_back(LoadResults[Index].Clearcoat);
        if (LoadResults[Index].Clearcoat)
        {
            const std::wstring Name = L"ClearcoatTexture_" + std::to_wstring(Index);
            LoadResults[Index].Clearcoat->SetName(Name.c_str());
        }
        SceneModels[Index].ClearcoatBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].Clearcoat.Get());

        SceneTextures.push_back(LoadResults[Index].ClearcoatRoughness);
        if (LoadResults[Index].ClearcoatRoughness)
        {
            const std::wstring Name = L"ClearcoatRoughnessTexture_" + std::to_wstring(Index);
            LoadResults[Index].ClearcoatRoughness->SetName(Name.c_str());
        }
        SceneModels[Index].ClearcoatRoughnessBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].ClearcoatRoughness.Get());

        SceneTextures.push_back(LoadResults[Index].ClearcoatNormal);
        if (LoadResults[Index].ClearcoatNormal)
        {
            const std::wstring Name = L"ClearcoatNormalTexture_" + std::to_wstring(Index);
            LoadResults[Index].ClearcoatNormal->SetName(Name.c_str());
        }
        SceneModels[Index].ClearcoatNormalBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].ClearcoatNormal.Get());

        SceneTextures.push_back(LoadResults[Index].Anisotropy);
        if (LoadResults[Index].Anisotropy)
        {
            const std::wstring Name = L"AnisotropyTexture_" + std::to_wstring(Index);
            LoadResults[Index].Anisotropy->SetName(Name.c_str());
        }
        SceneModels[Index].AnisotropyBindlessIndex = CreateSceneTextureSrv(LoadResults[Index].Anisotropy.Get());
    }

    if (ShadowMap.SrvBindlessIndex == UINT32_MAX)
    {
        ShadowMap.SrvBindlessIndex = Device->CreateBindlessSrv(ShadowMap.Get(), ShadowSrvDesc);
    }
    else
    {
        Device->WriteBindlessSrv(ShadowMap.SrvBindlessIndex, ShadowMap.Get(), ShadowSrvDesc);
    }
    EnvironmentCubeBindlessIndex = Device->CreateBindlessSrv(EnvironmentCube, EnvSrvDesc);
    BrdfLutBindlessIndex = Device->CreateBindlessSrv(BrdfLut, BrdfSrvDesc);
    return true;
}

bool FForwardRenderer::CreateGpuDrivenResources(FDX12Device* Device)
{
    // Step 1: Prepare indirect draw data
    FGpuDrivenPreparedData PreparedData;
    if (!PrepareGpuDrivenDrawData(PreparedData))
    {
        LogError("Failed to prepare GPU-driven draw data");
        return false;
    }

    // Step 2: Create per-frame indirect buffers
    if (!CreatePerFrameIndirectBuffers(Device, PreparedData))
    {
        LogError("Failed to create per-frame indirect buffers");
        return false;
    }

    // Step 3: Create shared GPU-driven buffers
    if (!CreateSharedGpuDrivenBuffers(Device, PreparedData))
    {
        LogError("Failed to create shared GPU-driven buffers");
        return false;
    }

    // Step 4: Upload buffers to GPU
    if (!UploadGpuDrivenBuffers(Device, PreparedData))
    {
        LogError("Failed to upload GPU-driven buffers");
        return false;
    }

    // Step 5: Create culling pipelines
    if (!CreateCullingPipelines(Device))
    {
        LogError("Failed to create culling pipelines");
        return false;
    }

    if (!GpuDrivenCullingState.CreateVisibilityListPipelines(Device))
    {
        LogError("Failed to create visibility list pipelines");
        return false;
    }
    RefreshGpuDrivenPersistentValidation();

    // Step 6: Create indirect command signature
    if (!CreateIndirectCommandSignature(Device, RootSignature.Get()))
    {
        LogError("Failed to create indirect command signature");
        return false;
    }

    return true;
}

void FForwardRenderer::UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset, const DirectX::XMMATRIX& LightViewProjection)
{
    const DirectX::XMVECTOR LightDir = DirectX::XMLoadFloat3(&LightDirection);

    RendererUtils::FUpdateSceneConstantsParams Params;
    Params.Camera = &Camera;
    Params.Model = &Model;
    Params.LightIntensity = LightIntensity;
    Params.LightDirection = LightDir;
    Params.LightColor = LightColor;
    Params.LightViewProjection = LightViewProjection;
    Params.Projection = Camera.GetProjectionMatrix();
    Params.ShadowStrength = bShadowsEnabled ? ShadowStrength : 0.0f;
    Params.ShadowBias = ShadowBias;
    Params.ShadowMapWidth = static_cast<float>(ShadowMapWidth);
    Params.ShadowMapHeight = static_cast<float>(ShadowMapHeight);
    Params.EnvMapMipCount = GetEnvironmentMipCount();
    Params.ConstantBufferMapped = GetSceneConstantBufferMapped();
    Params.ConstantBufferOffset = ConstantBufferOffset;
    RendererUtils::UpdateSceneConstants(Params);
}
