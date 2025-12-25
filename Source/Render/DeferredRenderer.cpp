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

using Microsoft::WRL::ComPtr;

FDeferredRenderer::FDeferredRenderer() = default;

namespace
{
    std::wstring BuildShaderTarget(const wchar_t* StagePrefix, D3D_SHADER_MODEL ShaderModel)
    {
        uint32_t ShaderModelValue = static_cast<uint32_t>(ShaderModel);
        uint32_t Major = (ShaderModelValue >> 4) & 0xF;
        uint32_t Minor = ShaderModelValue & 0xF;

        return std::wstring(StagePrefix) + L"_" + std::to_wstring(Major) + L"_" + std::to_wstring(Minor);
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

    bDepthPrepassEnabled = Options.bUseDepthPrepass;
    bShadowsEnabled = Options.bEnableShadows;
    ShadowBias = Options.ShadowBias;
    bTonemapEnabled = Options.bEnableTonemap;
    TonemapExposure = Options.TonemapExposure;
    TonemapWhitePoint = Options.TonemapWhitePoint;
    TonemapGamma = Options.TonemapGamma;
    bLogResourceBarriers = Options.bLogResourceBarriers;
    bEnableGraphDump = Options.bEnableGraphDump;
    bEnableGpuTiming = Options.bEnableGpuTiming;
    bHZBEnabled = Options.bEnableHZB;

    Viewport.TopLeftX = 0.0f;
    Viewport.TopLeftY = 0.0f;
    Viewport.Width = static_cast<float>(Width);
    Viewport.Height = static_cast<float>(Height);
    Viewport.MinDepth = 0.0f;
    Viewport.MaxDepth = 1.0f;

    ScissorRect.left = 0;
    ScissorRect.top = 0;
    ScissorRect.right = static_cast<LONG>(Width);
    ScissorRect.bottom = static_cast<LONG>(Height);

    ShadowViewport.TopLeftX = 0.0f;
    ShadowViewport.TopLeftY = 0.0f;
    ShadowViewport.Width = 2048.0f;
    ShadowViewport.Height = 2048.0f;
    ShadowViewport.MinDepth = 0.0f;
    ShadowViewport.MaxDepth = 1.0f;

    ShadowScissor.left = 0;
    ShadowScissor.top = 0;
    ShadowScissor.right = 2048;
    ShadowScissor.bottom = 2048;

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

    LogInfo("Creating deferred renderer depth prepass pipeline...");
    if (!CreateDepthPrepassPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: depth prepass pipeline creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer shadow pipeline...");
    if (!CreateShadowPipeline(Device))
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

    LogInfo("Creating deferred renderer tonemap root signature and pipeline...");
    if (!CreateTonemapRootSignature(Device) || !CreateTonemapPipeline(Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: tonemap pipeline creation failed");
        return false;
    }

    TextureLoader = std::make_unique<FTextureLoader>(Device);

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

    if (!CreateShadowResources(Device))
    {
        LogError("Deferred renderer initialization failed: shadow resources creation failed");
        return false;
    }

    if (!CreateGBufferResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: GBuffer resource creation failed");
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
        const FGltfMaterialTextureSet DefaultTextureSet = DefaultTextures.PerMesh.empty() ? FGltfMaterialTextureSet{} : DefaultTextures.PerMesh.front();
        DefaultModel.BaseColorTexturePath = DefaultTextureSet.BaseColor;
        DefaultModel.MetallicRoughnessTexturePath = DefaultTextureSet.MetallicRoughness;
        DefaultModel.NormalTexturePath = DefaultTextureSet.Normal;
        DefaultModel.BaseColorFactor = { 1.0f, 1.0f, 1.0f };
        DefaultModel.MetallicFactor = 0.0f;
        DefaultModel.RoughnessFactor = 1.0f;
        DefaultModel.EmissiveFactor = { 0.0f, 0.0f, 0.0f };
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

    LogInfo("Deferred renderer initialization completed");
    return true;
}

void FDeferredRenderer::RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime)
{
    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

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
        DXGI_FORMAT_R24G8_TYPELESS
    };

    FRGResourceHandle DepthHandle = Graph.ImportTexture("Depth", DepthBuffer.Get(), &DepthBufferState, DepthDesc);
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

    FRGResourceHandle HZBHandle = Graph.ImportTexture(
        "HZB",
        HierarchicalZBuffer.Get(),
        &HZBState,
        { HZBWidth, HZBHeight, DXGI_FORMAT_R32_FLOAT });

    struct FShadowPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FShadowPassData>("ShadowMap", [&, bRenderShadows](FShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRenderShadows;
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

        PixSetMarker(LocalCommandList, L"ShadowMap");
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

            LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
            LocalCommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

            LocalCommandList->DrawIndexedInstanced(Model.Geometry.IndexCount, 1, 0, 0, 0);
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

        PixSetMarker(LocalCommandList, L"DepthPrepass");

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

            LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            LocalCommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
            LocalCommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

            LocalCommandList->DrawIndexedInstanced(Model.Geometry.IndexCount, 1, 0, 0, 0);
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

        PixSetMarker(LocalCommandList, L"GBuffer");

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

        LocalCommandList->RSSetViewports(1, &Viewport);
        LocalCommandList->RSSetScissorRects(1, &ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(_countof(BasePassRTVs), BasePassRTVs, FALSE, &DepthStencilHandle);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            UpdateSceneConstants(*Data.Camera, Model, ConstantBufferOffset);

            LocalCommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
            LocalCommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

            const bool bUseNormalMap = Model.bHasNormalMap;

            LocalCommandList->SetPipelineState(bUseNormalMap ? BasePassPipelineWithNormalMap.Get() : BasePassPipelineWithoutNormalMap.Get());

            LocalCommandList->DrawIndexedInstanced(Model.Geometry.IndexCount, 1, 0, 0, 0);
        }

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

    if (bHZBEnabled)
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
            if (!HZBPipeline || !HZBRootSignature || Data.MipCount == 0)
            {
                return;
            }

            ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

            PixSetMarker(LocalCommandList, L"BuildHZB");

            ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
            LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
            LocalCommandList->SetPipelineState(HZBPipeline.Get());
            LocalCommandList->SetComputeRootSignature(HZBRootSignature.Get());

            struct FHZBConstants
            {
                uint32_t SourceWidth;
                uint32_t SourceHeight;
                uint32_t DestWidth;
                uint32_t DestHeight;
                uint32_t DestWidth1;
                uint32_t DestHeight1;
                uint32_t SourceMip;
                uint32_t HasSecondMip;
            };

            uint32_t CurrentWidth = Data.Width;
            uint32_t CurrentHeight = Data.Height;

            for (uint32_t MipIndex = 0; MipIndex < Data.MipCount; MipIndex += 2)
            {
                const uint32_t SourceWidth = (MipIndex == 0) ? Data.SourceWidth : (std::max)(1u, CurrentWidth);
                const uint32_t SourceHeight = (MipIndex == 0) ? Data.SourceHeight : (std::max)(1u, CurrentHeight);

                const uint32_t DestWidth = (MipIndex == 0) ? CurrentWidth : (std::max)(1u, CurrentWidth / 2);
                const uint32_t DestHeight = (MipIndex == 0) ? CurrentHeight : (std::max)(1u, CurrentHeight / 2);
                const bool bHasSecondMip = (MipIndex + 1) < Data.MipCount;
                const uint32_t DestWidth1 = bHasSecondMip ? (std::max)(1u, DestWidth / 2) : 0u;
                const uint32_t DestHeight1 = bHasSecondMip ? (std::max)(1u, DestHeight / 2) : 0u;

                FHZBConstants Constants = {};
                Constants.SourceWidth = SourceWidth;
                Constants.SourceHeight = SourceHeight;
                Constants.DestWidth = DestWidth;
                Constants.DestHeight = DestHeight;
                Constants.DestWidth1 = DestWidth1;
                Constants.DestHeight1 = DestHeight1;
                Constants.SourceMip = 0u;
                Constants.HasSecondMip = bHasSecondMip ? 1u : 0u;

                D3D12_GPU_DESCRIPTOR_HANDLE SourceHandle = Data.DepthSrv;
                if (MipIndex > 0)
                {
                    const uint32_t SourceMipIndex = MipIndex - 1;
                    SourceHandle = (SourceMipIndex < Data.HZBSrvMips.size()) ? Data.HZBSrvMips[SourceMipIndex] : D3D12_GPU_DESCRIPTOR_HANDLE{};
                }
                const D3D12_GPU_DESCRIPTOR_HANDLE DestHandle0 = (MipIndex < Data.HZBUavs.size()) ? Data.HZBUavs[MipIndex] : D3D12_GPU_DESCRIPTOR_HANDLE{};
                const D3D12_GPU_DESCRIPTOR_HANDLE DestHandle1 = (bHasSecondMip && (MipIndex + 1) < Data.HZBUavs.size())
                    ? Data.HZBUavs[MipIndex + 1]
                    : Data.HZBNullUav;

                if (SourceHandle.ptr == 0 || DestHandle0.ptr == 0 || DestHandle1.ptr == 0)
                {
                    break;
                }

                LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
                LocalCommandList->SetComputeRootDescriptorTable(1, SourceHandle);
                LocalCommandList->SetComputeRootDescriptorTable(2, DestHandle0);
                LocalCommandList->SetComputeRootDescriptorTable(3, DestHandle1);

                const uint32_t GroupX = (Constants.DestWidth + 7) / 8;
                const uint32_t GroupY = (Constants.DestHeight + 7) / 8;
                LocalCommandList->Dispatch(GroupX, GroupY, 1);

                CurrentWidth = bHasSecondMip ? DestWidth1 : DestWidth;
                CurrentHeight = bHasSecondMip ? DestHeight1 : DestHeight;

                if (MipIndex + 1 < Data.MipCount)
                {
                    D3D12_RESOURCE_BARRIER Barriers[3] = {};
                    uint32 BarrierCount = 0;

                    Barriers[BarrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    Barriers[BarrierCount].UAV.pResource = HierarchicalZBuffer.Get();
                    BarrierCount++;

                    Barriers[BarrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    Barriers[BarrierCount].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    Barriers[BarrierCount].Transition.pResource = HierarchicalZBuffer.Get();
                    Barriers[BarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    Barriers[BarrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    Barriers[BarrierCount].Transition.Subresource = D3D12CalcSubresource(MipIndex, 0, 0, Data.MipCount, 1);
                    BarrierCount++;

                    if (bHasSecondMip && (MipIndex + 2 < Data.MipCount))
                    {
                        Barriers[BarrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        Barriers[BarrierCount].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                        Barriers[BarrierCount].Transition.pResource = HierarchicalZBuffer.Get();
                        Barriers[BarrierCount].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        Barriers[BarrierCount].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        Barriers[BarrierCount].Transition.Subresource = D3D12CalcSubresource(MipIndex + 1, 0, 0, Data.MipCount, 1);
                        BarrierCount++;
                    }

                    LocalCommandList->ResourceBarrier(BarrierCount, Barriers);
                }
            }

            if (Data.MipCount > 1)
            {
                std::vector<D3D12_RESOURCE_BARRIER> RestoreBarriers;
                RestoreBarriers.reserve(Data.MipCount);

                for (uint32_t MipIndex = 0; MipIndex + 1 < Data.MipCount; ++MipIndex)
                {
                    D3D12_RESOURCE_BARRIER Barrier = {};
                    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    Barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    Barrier.Transition.pResource = HierarchicalZBuffer.Get();
                    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    Barrier.Transition.Subresource = D3D12CalcSubresource(MipIndex, 0, 0, Data.MipCount, 1);

                    RestoreBarriers.push_back(Barrier);
                }

                if (!RestoreBarriers.empty())
                {
                    LocalCommandList->ResourceBarrier(static_cast<UINT>(RestoreBarriers.size()), RestoreBarriers.data());
                }
            }
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

        PixSetMarker(LocalCommandList, L"Lighting");

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

        PixSetMarker(LocalCommandList, L"SkyAtmosphere");
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

    struct FTonemapPassData
    {
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
    };

    Graph.AddPass<FTonemapPassData>("Tonemap", [&](FTonemapPassData& Data, FRGPassBuilder& Builder)
    {
        Data.OutputHandle = RtvHandle;
        Builder.ReadTexture(LightingHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        for (int i = 0; i < 3; ++i)
        {
            Builder.WriteTexture(GBufferHandles[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [this](const FTonemapPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        PixSetMarker(LocalCommandList, L"Tonemap");
        Cmd.SetRenderTarget(Data.OutputHandle, nullptr);

        struct FTonemapConstants
        {
            uint32_t Enabled;
            float Exposure;
            float WhitePoint;
            float Gamma;
        };

        const FTonemapConstants TonemapConstants =
        {
            bTonemapEnabled ? 1u : 0u,
            TonemapExposure,
            TonemapWhitePoint,
            TonemapGamma
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
        LocalCommandList->DrawInstanced(3, 1, 0, 0);

        Cmd.TransitionResource(LightingBuffer.Get(), LightingBufferState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        LightingBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    });

    Graph.Execute(CmdContext);
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
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

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
    D3D12_DESCRIPTOR_RANGE1 DescriptorRanges[4] = {};
    for (int i = 0; i < 4; ++i)
    {
        DescriptorRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DescriptorRanges[i].NumDescriptors = 1;
        DescriptorRanges[i].BaseShaderRegister = static_cast<UINT>(i);
        DescriptorRanges[i].RegisterSpace = 0;
        DescriptorRanges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        DescriptorRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = _countof(DescriptorRanges);
    RootParams[1].DescriptorTable.pDescriptorRanges = DescriptorRanges;

    D3D12_STATIC_SAMPLER_DESC Samplers[2] = {};
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

    Samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    Samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    Samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[1].MinLOD = 0.0f;
    Samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[1].ShaderRegister = 1;
    Samplers[1].RegisterSpace = 0;
    Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> WithNormalDefines = { L"USE_NORMAL_MAP=1" };
    const std::vector<std::wstring> WithoutNormalDefines = { L"USE_NORMAL_MAP=0" };

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMap, WithNormalDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMap, WithoutNormalDefines))
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
    return true;
}

bool FDeferredRenderer::CreateDepthPrepassPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);

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

bool FDeferredRenderer::CreateShadowPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/ShadowMap.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
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
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(ShadowPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = BuildShaderTarget(L"ps", ShaderModel);

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
    D3D12_DESCRIPTOR_RANGE1 DescriptorRanges[3] = {};

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

    D3D12_ROOT_PARAMETER1 RootParams[4] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 8;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &DescriptorRanges[0];

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[2].DescriptorTable.pDescriptorRanges = &DescriptorRanges[1];

    RootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[3].DescriptorTable.pDescriptorRanges = &DescriptorRanges[2];

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
    std::vector<uint8_t> CSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = BuildShaderTarget(L"cs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/BuildHZB.hlsl", L"BuildHZB", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = HZBRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };

    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(HZBPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateTonemapRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 DescriptorRange = {};
    DescriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    DescriptorRange.NumDescriptors = 1;
    DescriptorRange.BaseShaderRegister = 0;
    DescriptorRange.RegisterSpace = 0;
    DescriptorRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &DescriptorRange;

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
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = BuildShaderTarget(L"ps", ShaderModel);

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

bool FDeferredRenderer::CreateShadowResources(FDX12Device* Device)
{
    if (Device == nullptr)
    {
        return false;
    }

    constexpr uint32_t ShadowSize = 2048;

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = ShadowSize;
    Desc.Height = ShadowSize;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = DXGI_FORMAT_R32_TYPELESS;
    Desc.SampleDesc.Count = 1;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    ClearValue.DepthStencil.Depth = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &ClearValue,
        IID_PPV_ARGS(ShadowMap.GetAddressOf())));

    ShadowMap->SetName(L"ShadowMap");

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = 1;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(ShadowDSVHeap.GetAddressOf())));

    ShadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle = ShadowDSVHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc = {};
    DsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    DsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    DsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    Device->GetDevice()->CreateDepthStencilView(ShadowMap.Get(), &DsvDesc, DsvHandle);

    ShadowDSVHandle = DsvHandle;

    return true;
}

bool FDeferredRenderer::CreateDescriptorHeap(FDX12Device* Device)
{
    const UINT TextureCount = static_cast<UINT>(SceneTextures.size());
    const UINT MipCount = HZBMipCount == 0 ? 1u : static_cast<UINT>(HZBMipCount);
    const UINT HZBDescriptorCount = 3 + (MipCount * 2);

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = TextureCount * 4 + 5 + HZBDescriptorCount;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(DescriptorHeap.GetAddressOf())));

    const UINT DescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle = DescriptorHeap->GetGPUDescriptorHandleForHeapStart();

    const auto CreateSceneTextureSrv = [&](ID3D12Resource* Texture)
    {
        if (!Texture)
        {
            return;
        }

        const D3D12_RESOURCE_DESC TextureDesc = Texture->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC SceneSrvDesc = {};
        SceneSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SceneSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SceneSrvDesc.Format = TextureDesc.Format;
        SceneSrvDesc.Texture2D.MipLevels = TextureDesc.MipLevels;
        SceneSrvDesc.Texture2D.MostDetailedMip = 0;
        SceneSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        Device->GetDevice()->CreateShaderResourceView(Texture, &SceneSrvDesc, CpuHandle);
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

    uint32_t PackMetallicRoughness(float Metallic, float Roughness)
    {
        const uint32_t M = ClampToByte(Metallic);
        const uint32_t R = ClampToByte(Roughness);
        return 0xff000000 | (R << 8) | M;
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

        // Base color texture
        const uint32_t BaseColorValue = PackColor(Model.BaseColorFactor);
        FTextureLoadRequest BaseColorRequest;
        BaseColorRequest.Path = Model.BaseColorTexturePath;
        BaseColorRequest.SolidColor = BaseColorValue;
        BaseColorRequest.bUseSolidColor = Model.BaseColorTexturePath.empty();
        BaseColorRequest.OutTexture = &TextureSet.BaseColor;
        Requests.push_back(BaseColorRequest);

        // Metallic roughness texture - use solid color if path is empty
        const uint32_t MetallicRoughnessValue = PackMetallicRoughness(Model.MetallicFactor, Model.RoughnessFactor);
        FTextureLoadRequest MetallicRoughnessRequest;
        MetallicRoughnessRequest.Path = Model.MetallicRoughnessTexturePath;
        MetallicRoughnessRequest.SolidColor = MetallicRoughnessValue;
        MetallicRoughnessRequest.bUseSolidColor = Model.MetallicRoughnessTexturePath.empty();
        MetallicRoughnessRequest.OutTexture = &TextureSet.MetallicRoughness;
        Requests.push_back(MetallicRoughnessRequest);

        // Normal texture - use default normal if path is empty
        FTextureLoadRequest NormalRequest;
        NormalRequest.Path = Model.NormalTexturePath;
        NormalRequest.SolidColor = 0xff8080ff;
        NormalRequest.bUseSolidColor = Model.NormalTexturePath.empty();
        NormalRequest.OutTexture = &TextureSet.Normal;
        Requests.push_back(NormalRequest);

        // Emissive texture
        const uint32_t EmissiveValue = PackColor(Model.EmissiveFactor);
        FTextureLoadRequest EmissiveRequest;
        EmissiveRequest.Path = Model.EmissiveTexturePath;
        EmissiveRequest.SolidColor = EmissiveValue;
        EmissiveRequest.bUseSolidColor = Model.EmissiveTexturePath.empty();
        EmissiveRequest.OutTexture = &TextureSet.Emissive;
        Requests.push_back(EmissiveRequest);
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
