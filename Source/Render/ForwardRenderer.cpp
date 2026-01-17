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

    if (!CreateRayTracingPipeline(Device))
    {
        LogError("Forward renderer initialization failed: ray tracing pipeline creation failed");
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

    FSkyPipelineConfig SkyPipelineConfig;
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
    ConfigureHZBOcclusion(false, UINT32_MAX, 0, 0, 0);

    FForwardFrameState FrameState;
    PrepareFrameState(Camera, FrameState);
    BuildRayTracingTlas(CmdContext);

    FRenderGraph Graph;
    ConfigureFrameGraph(Graph);

    FForwardFrameResources Resources;
    ImportFrameResources(Graph, Resources);

    AddGpuCullingPass(Graph, Camera, Resources.DepthHandle);
    if (!bRayTracedShadowsEnabled || !bRayTracingPipelineReady)
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

    Graph.AddPass<FRayTracingShadowPassData>("RayTracingShadowMask", [&, ShadowMaskDesc, DepthHandle, GBufferHandle](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        if (!bRayTracedShadowsEnabled || !bRayTracingPipelineReady || !GBufferHandle)
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
        if (!bRayTracingPipelineReady || !RayTracingShaderTable || !RayTracingUavHeap)
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
        if (FrameIndex >= TlasResultBuffers.size() || !TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        const UINT DescriptorStride = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE CpuStart = RayTracingUavHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE GpuStart = RayTracingUavHeap->GetGPUDescriptorHandleForHeapStart();

        D3D12_CPU_DESCRIPTOR_HANDLE DepthCpuHandle = CpuStart;
        D3D12_GPU_DESCRIPTOR_HANDLE DepthGpuHandle = GpuStart;

        D3D12_CPU_DESCRIPTOR_HANDLE GBufferCpuHandle = CpuStart;
        D3D12_GPU_DESCRIPTOR_HANDLE GBufferGpuHandle = GpuStart;
        GBufferCpuHandle.ptr += DescriptorStride;
        GBufferGpuHandle.ptr += DescriptorStride;

        D3D12_CPU_DESCRIPTOR_HANDLE UavCpuHandle = CpuStart;
        D3D12_GPU_DESCRIPTOR_HANDLE UavGpuHandle = GpuStart;
        UavCpuHandle.ptr += DescriptorStride * 2;
        UavGpuHandle.ptr += DescriptorStride * 2;

        D3D12_SHADER_RESOURCE_VIEW_DESC DepthSrvDesc = {};
        DepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        DepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DepthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        DepthSrvDesc.Texture2D.MipLevels = 1;
        DepthSrvDesc.Texture2D.MostDetailedMip = 0;
        DepthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        Device->GetDevice()->CreateShaderResourceView(DepthBuffer, &DepthSrvDesc, DepthCpuHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferSrvDesc = {};
        GBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferSrvDesc.Format = GBufferA->GetDesc().Format;
        GBufferSrvDesc.Texture2D.MipLevels = 1;
        GBufferSrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        Device->GetDevice()->CreateShaderResourceView(GBufferA, &GBufferSrvDesc, GBufferCpuHandle);

        D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
        UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        UavDesc.Format = DXGI_FORMAT_R8_UNORM;
        UavDesc.Texture2D.MipSlice = 0;
        Device->GetDevice()->CreateUnorderedAccessView(ShadowMask, nullptr, &UavDesc, UavCpuHandle);

        if (ShadowMaskBindlessIndex == UINT32_MAX || ShadowMaskResource != ShadowMask)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
            SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SrvDesc.Format = DXGI_FORMAT_R8_UNORM;
            SrvDesc.Texture2D.MipLevels = 1;
            ShadowMaskBindlessIndex = Device->CreateBindlessSrv(ShadowMask, SrvDesc);
            ShadowMaskResource = ShadowMask;
        }

        const UINT ClearValues[4] = { 1u, 0u, 0u, 0u };
        ID3D12DescriptorHeap* Heaps[] = { RayTracingUavHeap.Get() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList4->ClearUnorderedAccessViewUint(UavGpuHandle, UavCpuHandle, ShadowMask, ClearValues, 0, nullptr);

        CommandList4->SetPipelineState1(RayTracingPipeline.GetStateObject());
        CommandList4->SetComputeRootSignature(RayTracingGlobalRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        const DirectX::XMMATRIX LightViewProjection = RendererUtils::BuildDirectionalLightViewProjection(SceneCenter, SceneRadius, LightDirection);
        UpdateSceneConstants(*Data.Camera, SceneModels.front(), ConstantBufferOffset, LightViewProjection);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);
        CommandList4->SetComputeRootDescriptorTable(2, DepthGpuHandle);
        CommandList4->SetComputeRootDescriptorTable(3, UavGpuHandle);

        D3D12_DISPATCH_RAYS_DESC DispatchDesc = {};
        DispatchDesc.RayGenerationShaderRecord.StartAddress = RayTracingShaderTable->GetGPUVirtualAddress();
        DispatchDesc.RayGenerationShaderRecord.SizeInBytes = RayTracingShaderRecordSize;

        DispatchDesc.MissShaderTable.StartAddress = RayTracingShaderTable->GetGPUVirtualAddress() + RayTracingShaderRecordSize;
        DispatchDesc.MissShaderTable.SizeInBytes = RayTracingShaderRecordSize;
        DispatchDesc.MissShaderTable.StrideInBytes = RayTracingShaderRecordSize;

        DispatchDesc.HitGroupTable.StartAddress = RayTracingShaderTable->GetGPUVirtualAddress() + RayTracingShaderRecordSize * 2;
        DispatchDesc.HitGroupTable.SizeInBytes = RayTracingShaderRecordSize;
        DispatchDesc.HitGroupTable.StrideInBytes = RayTracingShaderRecordSize;

        DispatchDesc.Width = static_cast<uint32>(Viewport.Width);
        DispatchDesc.Height = static_cast<uint32>(Viewport.Height);
        DispatchDesc.Depth = 1;

        CommandList4->DispatchRays(&DispatchDesc);
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

    Graph.AddPass<FGpuCullingPassData>("GPU Culling", [this, &Camera, DepthHandle](FGpuCullingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bEnableIndirectDraw && CullingPipeline && CullingRootSignature && GetIndirectCommandBuffer()
            && ModelBoundsBuffer && MeshletConeAxisBuffer && MeshletConeApexBuffer && Device && Device->GetBindlessDescriptorHeap();
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

        if (!Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (ShadowMapBindlessIndex == UINT32_MAX || EnvironmentCubeBindlessIndex == UINT32_MAX || BrdfLutBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
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

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t ShadowMaskEnabled = (bRayTracedShadowsEnabled && ShadowMaskBindlessIndex != UINT32_MAX) ? 1u : 0u;
            const uint32_t ResolvedShadowMaskIndex = ShadowMaskEnabled ? ShadowMaskBindlessIndex : ShadowMapBindlessIndex;
            const uint32_t ForwardBindlessIndices[] =
            {
                Model.BaseColorBindlessIndex,
                Model.MetallicRoughnessBindlessIndex,
                Model.NormalBindlessIndex,
                Model.EmissiveBindlessIndex,
                ShadowMapBindlessIndex,
                ResolvedShadowMaskIndex,
                ShadowMaskEnabled,
                EnvironmentCubeBindlessIndex,
                BrdfLutBindlessIndex
            };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(ForwardBindlessIndices), ForwardBindlessIndices, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty()
                    ? L"Model"
                    : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
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
        LocalCommandList->IASetVertexBuffers(0, SkyGeometry.VertexBufferCount, SkyGeometry.VertexBufferViews.data());
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

        if (!Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (ShadowMapBindlessIndex == UINT32_MAX || EnvironmentCubeBindlessIndex == UINT32_MAX || BrdfLutBindlessIndex == UINT32_MAX)
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
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;
            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset, Data.LightViewProjection);
        }

        ID3D12Resource* IndirectBuffer = GetIndirectCommandBuffer();
        ID3D12Resource* RunCountBuffer = GetMeshletRunCountBuffer();
        if (bEnableIndirectDraw && IndirectCommandSignature && IndirectBuffer && RunCountBuffer && !IndirectDrawRanges.empty())
        {
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
                const uint32_t ShadowMaskEnabled = (bRayTracedShadowsEnabled && ShadowMaskBindlessIndex != UINT32_MAX) ? 1u : 0u;
                const uint32_t ResolvedShadowMaskIndex = ShadowMaskEnabled ? ShadowMaskBindlessIndex : ShadowMapBindlessIndex;
                const uint32_t ForwardBindlessIndices[] =
                {
                    Range.MaterialBindlessIndices[0],
                    Range.MaterialBindlessIndices[1],
                    Range.MaterialBindlessIndices[2],
                    Range.MaterialBindlessIndices[3],
                    ShadowMapBindlessIndex,
                    ResolvedShadowMaskIndex,
                    ShadowMaskEnabled,
                    EnvironmentCubeBindlessIndex,
                    BrdfLutBindlessIndex
                };
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
                const uint32_t ShadowMaskEnabled = (bRayTracedShadowsEnabled && ShadowMaskBindlessIndex != UINT32_MAX) ? 1u : 0u;
                const uint32_t ResolvedShadowMaskIndex = ShadowMaskEnabled ? ShadowMaskBindlessIndex : ShadowMapBindlessIndex;
                const uint32_t ForwardBindlessIndices[] =
                {
                    Model.BaseColorBindlessIndex,
                    Model.MetallicRoughnessBindlessIndex,
                    Model.NormalBindlessIndex,
                    Model.EmissiveBindlessIndex,
                    ShadowMapBindlessIndex,
                    ResolvedShadowMaskIndex,
                    ShadowMaskEnabled,
                    EnvironmentCubeBindlessIndex,
                    BrdfLutBindlessIndex
                };
                LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(ForwardBindlessIndices), ForwardBindlessIndices, 0);

                if (AreModelPixEventsEnabled())
                {
                    const std::wstring ModelLabel = Model.Name.empty()
                        ? L"Model"
                        : std::wstring(Model.Name.begin(), Model.Name.end());
                    FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                }
                else
                {
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
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

            LocalCommandList->IASetVertexBuffers(0, Model.Geometry.VertexBufferCount, Model.Geometry.VertexBufferViews.data());
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
        Data.bEnabled = bEnableGpuDebugPrint && GpuDebugPrintPipeline && GpuDebugPrintRootSignature
            && Device && Device->GetBindlessDescriptorHeap();
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
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Scene constant buffer (b0), used in Shaders/ForwardVS.hlsl VSMain and Shaders/ForwardPS.hlsl PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Forward bindless indices (b1), used in Shaders/ForwardPS.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 9;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;

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
    RootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
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
    ShadowMapBindlessIndex = UINT32_MAX;
    EnvironmentCubeBindlessIndex = UINT32_MAX;
    BrdfLutBindlessIndex = UINT32_MAX;

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

    const auto CreateSceneTextureSrv = [&](ID3D12Resource* Texture)
    {
        ID3D12Resource* Resource = Texture ? Texture : NullTexture.Get();
        if (!Resource)
        {
            return UINT32_MAX;
        }

        const D3D12_RESOURCE_DESC TextureDesc = Resource->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
        SrvDesc.Format = TextureDesc.Format;
        SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Texture2D.MipLevels = TextureDesc.MipLevels;
        SrvDesc.Texture2D.MostDetailedMip = 0;
        SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        return Device->CreateBindlessSrv(Resource, SrvDesc);
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
    }

    ShadowMapBindlessIndex = Device->CreateBindlessSrv(ShadowMap.Get(), ShadowSrvDesc);
    EnvironmentCubeBindlessIndex = Device->CreateBindlessSrv(EnvironmentCubeTexture.Get(), EnvSrvDesc);
    BrdfLutBindlessIndex = Device->CreateBindlessSrv(BrdfLutTexture.Get(), BrdfSrvDesc);
    return true;
}

bool FForwardRenderer::CreateGpuDrivenResources(FDX12Device* Device)
{
    if (!Device || SceneModels.empty() || !GetSceneConstantBuffer())
    {
        return false;
    }

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

    // Step 6: Create indirect command signature
    if (!CreateIndirectCommandSignature(Device, RootSignature.Get()))
    {
        LogError("Failed to create indirect command signature");
        return false;
    }

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
