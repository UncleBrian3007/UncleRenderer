#include "DeferredResourceImporter.h"

#include "../DeferredRenderer.h"
#include "Gtao.h"
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
        &Owner.ShadowMapState,
        { 2048, 2048, DXGI_FORMAT_D32_FLOAT });

    const FRGTextureDesc DepthDesc =
    {
        static_cast<uint32>(Owner.Viewport.Width),
        static_cast<uint32>(Owner.Viewport.Height),
        DXGI_FORMAT_R24G8_TYPELESS
    };

    D3D12_RESOURCE_STATES& DepthState = Owner.GetDepthBufferState();
    OutResources.DepthHandle = Graph.ImportTexture("Depth", Owner.GetDepthBuffer(), &DepthState, DepthDesc);
    OutResources.ObjectIdHandle = Graph.ImportTexture(
        "ObjectId",
        Owner.ObjectIdTexture.Get(),
        &Owner.ObjectIdState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R32_UINT });
    OutResources.VelocityHandle = Graph.ImportTexture(
        "Velocity",
        Owner.VelocityTexture.Get(),
        &Owner.VelocityState,
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
        &Owner.LinearDepthState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16_FLOAT });

    if (Owner.Gtao)
    {
        Owner.Gtao->ImportPersistentResources(Context);
    }

    if (Owner.RestirGI)
    {
        Owner.RestirGI->ImportPersistentResources(Context);
    }

    if (Owner.RestirGIDenoiser)
    {
        Owner.RestirGIDenoiser->ImportPersistentResources(Context);
    }

    if (Owner.Ssr)
    {
        Owner.Ssr->ImportPersistentResources(Context);
    }

    OutResources.LightingHandle = Graph.ImportTexture(
        "Lighting",
        Owner.LightingBuffer.Get(),
        &Owner.LightingBufferState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::LightingBufferFormat });

    if (Owner.AutoExposure)
    {
        Owner.AutoExposure->ImportPersistentResources(Context);
    }

    if (Owner.Cas)
    {
        Owner.Cas->ImportPersistentResources(Context);
    }

    if (Owner.Tonemap)
    {
        Owner.Tonemap->ImportPersistentResources(Context);
    }

    if (Owner.Taa)
    {
        Owner.Taa->ImportPersistentResources(Context);
    }

    if (Owner.PathTracing)
    {
        Owner.PathTracing->ImportPersistentResources(Context);
    }

    OutResources.HZBHandle = Graph.ImportTexture(
        "HZB",
        Owner.HierarchicalZBuffer.Get(),
        &Owner.HZBState,
        { Owner.HZBWidth, Owner.HZBHeight, DXGI_FORMAT_R32G32_FLOAT });
}

bool FDeferredRenderer::CreateDescriptorHeap(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

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

    const auto CreateSceneTextureDescriptors = [&]() -> bool
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
        return true;
    };
    if (!CreateSceneTextureDescriptors())
    {
        return false;
    }

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
    ShadowMapBindlessIndex = Device->CreateBindlessSrv(ShadowMap.Get(), ShadowSrvDesc);

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC EnvSrvDesc = {};
        EnvSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        EnvSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        EnvSrvDesc.Format = EnvironmentCubeTexture->GetDesc().Format;
        EnvSrvDesc.TextureCube.MipLevels = EnvironmentCubeTexture->GetDesc().MipLevels;
        EnvSrvDesc.TextureCube.MostDetailedMip = 0;
        EnvSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        EnvironmentCubeBindlessIndex = Device->CreateBindlessSrv(EnvironmentCubeTexture.Get(), EnvSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC BrdfSrvDesc = {};
        BrdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        BrdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        BrdfSrvDesc.Format = BrdfLutTexture->GetDesc().Format;
        BrdfSrvDesc.Texture2D.MipLevels = BrdfLutTexture->GetDesc().MipLevels;
        BrdfSrvDesc.Texture2D.MostDetailedMip = 0;
        BrdfSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        BrdfLutBindlessIndex = Device->CreateBindlessSrv(BrdfLutTexture.Get(), BrdfSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LinearDepthSrvDesc = {};
        LinearDepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LinearDepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LinearDepthSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        LinearDepthSrvDesc.Texture2D.MipLevels = 1;
        LinearDepthBindlessIndex = Device->CreateBindlessSrv(LinearDepthTexture.Get(), LinearDepthSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC VelocitySrvDesc = {};
        VelocitySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        VelocitySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        VelocitySrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        VelocitySrvDesc.Texture2D.MipLevels = 1;
        VelocityBindlessIndex = Device->CreateBindlessSrv(VelocityTexture.Get(), VelocitySrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC SobolSrvDesc = {};
        SobolSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SobolSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SobolSrvDesc.Format = BlueNoiseSobolTexture->GetDesc().Format;
        SobolSrvDesc.Texture2D.MipLevels = 1;
        SobolSrvDesc.Texture2D.MostDetailedMip = 0;
        SobolSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        BlueNoiseSobolSrvBindlessIndex = Device->CreateBindlessSrv(BlueNoiseSobolTexture.Get(), SobolSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC ScramblingSrvDesc = {};
        ScramblingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        ScramblingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ScramblingSrvDesc.Format = BlueNoiseScramblingRanking1SPPTexture->GetDesc().Format;
        ScramblingSrvDesc.Texture2D.MipLevels = 1;
        ScramblingSrvDesc.Texture2D.MostDetailedMip = 0;
        ScramblingSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        BlueNoiseScramblingRanking1SPPSrvBindlessIndex = Device->CreateBindlessSrv(BlueNoiseScramblingRanking1SPPTexture.Get(), ScramblingSrvDesc);
    }

    if (Ssr && !Ssr->CreatePersistentDescriptors(*this, Device))
    {
        return false;
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LightingSrvDesc = {};
        LightingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LightingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LightingSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        LightingSrvDesc.Texture2D.MipLevels = 1;
        LightingBufferBindlessIndex = Device->CreateBindlessSrv(LightingBuffer.Get(), LightingSrvDesc);
    }

    const auto CreateTemporalAndHzbDescriptors = [&]() -> bool
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
            ID3D12Resource* DepthBuffer = DepthResourcesPerFrame.empty() ? nullptr : DepthResourcesPerFrame[Index].DepthBuffer.Get();
            DepthBindlessIndices[Index] = Device->CreateBindlessSrv(DepthBuffer, DepthSrvDesc);
        }

        {
            D3D12_SHADER_RESOURCE_VIEW_DESC HZBSrvDesc = {};
            HZBSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            HZBSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            HZBSrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
            HZBSrvDesc.Texture2D.MipLevels = HZBMipCount;
            HZBSrvDesc.Texture2D.MostDetailedMip = 0;
            HZBSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            HZBSrvBindlessIndex = Device->CreateBindlessSrv(HierarchicalZBuffer.Get(), HZBSrvDesc);
        }

        HZBSrvMipBindlessIndices.clear();
        HZBSrvMipBindlessIndices.reserve(HZBMipCount);
        for (uint32_t Mip = 0; Mip < HZBMipCount; ++Mip)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC HZBMipSrvDesc = {};
            HZBMipSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            HZBMipSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            HZBMipSrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
            HZBMipSrvDesc.Texture2D.MipLevels = 1;
            HZBMipSrvDesc.Texture2D.MostDetailedMip = Mip;
            HZBMipSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

            HZBSrvMipBindlessIndices.push_back(Device->CreateBindlessSrv(HierarchicalZBuffer.Get(), HZBMipSrvDesc));
        }

        HZBUavBindlessIndices.clear();
        HZBUavBindlessIndices.reserve(HZBMipCount);
        for (uint32_t Mip = 0; Mip < HZBMipCount; ++Mip)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
            UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            UavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
            UavDesc.Texture2D.MipSlice = Mip;
            UavDesc.Texture2D.PlaneSlice = 0;

            HZBUavBindlessIndices.push_back(Device->CreateBindlessUav(HierarchicalZBuffer.Get(), nullptr, UavDesc));
        }

        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC NullUavDesc = {};
            NullUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            NullUavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
            NullUavDesc.Texture2D.MipSlice = 0;
            NullUavDesc.Texture2D.PlaneSlice = 0;

            HZBNullUavBindlessIndex = Device->CreateBindlessUav(HZBNullUavResource.Get(), nullptr, NullUavDesc);
        }

        return true;
    };
    if (!CreateTemporalAndHzbDescriptors())
    {
        return false;
    }

    return true;
}
