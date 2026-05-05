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
#include "../../RHI/DX12Device.h"
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

        D3D12_SHADER_RESOURCE_VIEW_DESC SceneSrvDesc = {};
        SceneSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SceneSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SceneSrvDesc.Format = TextureDesc.Format;
        SceneSrvDesc.Texture2D.MipLevels = TextureDesc.MipLevels;
        SceneSrvDesc.Texture2D.MostDetailedMip = 0;
        SceneSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        return Device->CreateBindlessSrv(Texture, SceneSrvDesc);
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
        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
        SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Format = FDeferredRenderer::GBufferFormats[i];
        SrvDesc.Texture2D.MipLevels = 1;
        GBufferBindlessIndices[i] = Device->CreateBindlessSrv(Buffers[i], SrvDesc);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC ShadowSrvDesc = {};
    ShadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ShadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ShadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ShadowSrvDesc.Texture2D.MipLevels = 1;
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
        D3D12_SHADER_RESOURCE_VIEW_DESC EnvSrvDesc = {};
        EnvSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        EnvSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        EnvSrvDesc.Format = EnvironmentCube->GetDesc().Format;
        EnvSrvDesc.TextureCube.MipLevels = EnvironmentCube->GetDesc().MipLevels;
        EnvSrvDesc.TextureCube.MostDetailedMip = 0;
        EnvSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        EnvironmentCubeBindlessIndex = Device->CreateBindlessSrv(EnvironmentCube, EnvSrvDesc);
    }

    {
        ID3D12Resource* BrdfLut = GetBrdfLutTexture();
        D3D12_SHADER_RESOURCE_VIEW_DESC BrdfSrvDesc = {};
        BrdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        BrdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        BrdfSrvDesc.Format = BrdfLut->GetDesc().Format;
        BrdfSrvDesc.Texture2D.MipLevels = BrdfLut->GetDesc().MipLevels;
        BrdfSrvDesc.Texture2D.MostDetailedMip = 0;
        BrdfSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        BrdfLutBindlessIndex = Device->CreateBindlessSrv(BrdfLut, BrdfSrvDesc);
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
            D3D12_SHADER_RESOURCE_VIEW_DESC DepthSrvDesc = {};
            DepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            DepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            DepthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            DepthSrvDesc.Texture2D.MipLevels = 1;
            DepthSrvDesc.Texture2D.MostDetailedMip = 0;
            DepthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            ID3D12Resource* DepthBuffer = DepthResourcesPerFrame[Index].DepthBuffer.Get();
            DepthBindlessIndices[Index] = Device->CreateBindlessSrv(DepthBuffer, DepthSrvDesc);
        }

    };
    CreateTemporalAndHzbDescriptors();

    return true;
}
