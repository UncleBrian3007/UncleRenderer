#include "DeferredResourceImporter.h"

#include "../DeferredRenderer.h"
#include "Gtao.h"
#include "Hzb.h"
#include "Ssr.h"
#include "RestirGI.h"
#include "AutoExposure.h"
#include "Cas.h"
#include "Taa.h"
#include "Tonemap.h"
#include "PathTracing.h"
#include "ClusterDagVisibilityPass.h"
#include "../../RHI/DX12Device.h"
#include <d3dx12.h>
#include <algorithm>
#include <string>

void FDeferredResourceImporter::ImportFrameResources(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FDeferredRenderer::FDeferredFrameResources& OutResources = Context.Resources;

    OutResources.ShadowHandle = Graph.ImportTexture(
        "ShadowMap",
        Owner.ShadowMap.Get(),
        &Owner.ShadowMap.State,
        { 2048, 2048, DXGI_FORMAT_D32_FLOAT });

    const FRGTextureDesc DepthDesc =
    {
        static_cast<uint32>(Owner.Viewport.Width),
        static_cast<uint32>(Owner.Viewport.Height),
        DXGI_FORMAT_R24G8_TYPELESS
    };

    D3D12_RESOURCE_STATES& DepthState = Owner.GetDepthBufferState();
    OutResources.DepthHandle = Graph.ImportTexture("Depth", Owner.GetDepthBuffer(), &DepthState, DepthDesc);
    OutResources.ObjectIdHandle = Owner.ObjectId->ImportResource(
        Graph,
        static_cast<uint32_t>(Owner.Viewport.Width),
        static_cast<uint32_t>(Owner.Viewport.Height));
    OutResources.VelocityHandle = Graph.ImportTexture(
        "Velocity",
        Owner.VelocityTexture.Get(),
        &Owner.VelocityTexture.State,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });
    OutResources.GBufferHandles =
    {
        Graph.ImportTexture("GBufferA", Owner.GBufferA.Get(), &Owner.GBufferStates[0], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::GBufferFormats[0] }),
        Graph.ImportTexture("GBufferB", Owner.GBufferB.Get(), &Owner.GBufferStates[1], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::GBufferFormats[1] }),
        Graph.ImportTexture("GBufferC", Owner.GBufferC.Get(), &Owner.GBufferStates[2], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::GBufferFormats[2] }),
        Graph.ImportTexture("GBufferD", Owner.GBufferD.Get(), &Owner.GBufferStates[3], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::GBufferFormats[3] }),
    };
    Owner.ClusterDagVisibilityPass->ImportPersistentResources(Context);

    OutResources.LinearDepthHandle = Graph.ImportTexture(
        "LinearDepth",
        Owner.LinearDepthTexture.Get(),
        &Owner.LinearDepthTexture.State,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16_FLOAT });

    Owner.Gtao->ImportPersistentResources(Context);
    Owner.RestirGI->ImportPersistentResources(Context);
    Owner.RestirGIDenoiser->ImportPersistentResources(Context);
    Owner.Ssr->ImportPersistentResources(Context);

    OutResources.LightingHandle = Graph.ImportTexture(
        "Lighting",
        Owner.LightingBuffer.Get(),
        &Owner.LightingBuffer.State,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::LightingBufferFormat },
        Owner.LightingBuffer.SrvBindlessIndex);

    Owner.AutoExposure->ImportPersistentResources(Context);
    Owner.Cas->ImportPersistentResources(Context);
    Owner.Tonemap->ImportPersistentResources(Context);
    Owner.Taa->ImportPersistentResources(Context);
    Owner.PathTracing->ImportPersistentResources(Context);

    Owner.Hzb->ImportPersistentResources(Context);
}

bool FDeferredRenderer::CreateDescriptorHeap(FDX12Device* Device)
{
    const auto CreateSceneTextureSrv = [&](ID3D12Resource* Texture) -> uint32_t
    {
        if (!Texture)
        {
            return UINT32_MAX;
        }

        const D3D12_RESOURCE_DESC TextureDesc = Texture->GetDesc();

        return Device->CreateBindlessSrv(Texture,
            CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(TextureDesc.Format, TextureDesc.MipLevels));
    };

    const auto CreateSceneTextureDescriptors = [&]()
    {
        for (size_t Index = 0; Index < SceneTextures.size(); ++Index)
        {
            SceneModels[Index].BaseColorBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].BaseColor.Get());
            SceneModels[Index].MetallicRoughnessBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].MetallicRoughness.Get());
            SceneModels[Index].NormalBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Normal.Get());
            SceneModels[Index].EmissiveBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Emissive.Get());
            SceneModels[Index].SheenColorBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].SheenColor.Get());
            SceneModels[Index].SheenRoughnessBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].SheenRoughness.Get());
            SceneModels[Index].ClearcoatBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Clearcoat.Get());
            SceneModels[Index].ClearcoatRoughnessBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].ClearcoatRoughness.Get());
            SceneModels[Index].ClearcoatNormalBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].ClearcoatNormal.Get());
            SceneModels[Index].AnisotropyBindlessIndex = CreateSceneTextureSrv(SceneTextures[Index].Anisotropy.Get());
        }
    };
    CreateSceneTextureDescriptors();

    ID3D12Resource* Buffers[4] = { GBufferA.Get(), GBufferB.Get(), GBufferC.Get(), GBufferD.Get() };
    for (int i = 0; i < 4; ++i)
    {
        GBufferBindlessIndices[i] = Device->CreateBindlessSrv(Buffers[i],
            CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(FDeferredRenderer::GBufferFormats[i], 1));
    }

    const auto ShadowSrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, 1);
    if (ShadowMap.SrvBindlessIndex == UINT32_MAX)
    {
        ShadowMap.SrvBindlessIndex = Device->CreateBindlessSrv(ShadowMap.Get(), ShadowSrvDesc);
    }
    else
    {
        Device->WriteBindlessSrv(ShadowMap.SrvBindlessIndex, ShadowMap.Get(), ShadowSrvDesc);
    }

    {
        ID3D12Resource* EnvironmentCube = GetEnvironmentCubeTexture();
        EnvironmentCubeBindlessIndex = Device->CreateBindlessSrv(EnvironmentCube,
            CD3DX12_SHADER_RESOURCE_VIEW_DESC::TexCube(EnvironmentCube->GetDesc().Format, EnvironmentCube->GetDesc().MipLevels));
    }

    {
        ID3D12Resource* BrdfLut = GetBrdfLutTexture();
        BrdfLutBindlessIndex = Device->CreateBindlessSrv(BrdfLut,
            CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(BrdfLut->GetDesc().Format, BrdfLut->GetDesc().MipLevels));
    }

    {
        WriteOrCreateBindlessTextureSrv(Device, LinearDepthTexture);
    }

    {
        WriteOrCreateBindlessTextureSrv(Device, VelocityTexture);
    }

    {
        WriteOrCreateBindlessTextureSrv(Device, BlueNoiseSobolTexture);
    }

    {
        WriteOrCreateBindlessTextureSrv(Device, BlueNoiseScramblingRanking1SPPTexture);
    }

    if (!Ssr->CreatePersistentDescriptors(*this, Device))
    {
        return false;
    }

    {
        WriteOrCreateBindlessTextureSrv(Device, LightingBuffer);
    }

    const auto CreateTemporalAndHzbDescriptors = [&]()
    {
        DepthBindlessIndices.clear();
        DepthBindlessIndices.resize(GetFramesInFlight(), UINT32_MAX);
        for (uint32_t Index = 0; Index < DepthBindlessIndices.size(); ++Index)
        {
            ID3D12Resource* DepthBuffer = DepthResourcesPerFrame[Index].DepthBuffer.Get();
            DepthBindlessIndices[Index] = Device->CreateBindlessSrv(DepthBuffer,
                CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1));
        }

    };
    CreateTemporalAndHzbDescriptors();

    return true;
}
