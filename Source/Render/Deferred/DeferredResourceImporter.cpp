#include "DeferredResourceImporter.h"

#include "../DeferredRenderer.h"
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
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16_FLOAT });
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

    OutResources.GtaoHandle = Graph.ImportTexture(
        "GTAO",
        Owner.GtaoTexture.Get(),
        &Owner.GtaoState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R8_UNORM });

    OutResources.RestirGIHandle = Graph.ImportTexture(
        "ReSTIR GI",
        Owner.RestirGITexture.Get(),
        &Owner.RestirGIState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIHistoryHandle = Graph.ImportTexture(
        "ReSTIR GI History",
        Owner.RestirGIHistoryTexture.Get(),
        &Owner.RestirGIHistoryState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    FRGBufferDesc RestirReservoirDesc = {};
    RestirReservoirDesc.Size = static_cast<size_t>(Owner.Viewport.Width) * static_cast<size_t>(Owner.Viewport.Height) * sizeof(float) * 8u;
    RestirReservoirDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    OutResources.RestirGITemporalReservoirHandle = Graph.ImportBuffer(
        "ReSTIR GI Temporal Reservoir",
        Owner.RestirGITemporalReservoirBuffer.Get(),
        &Owner.RestirGITemporalReservoirState,
        RestirReservoirDesc);

    OutResources.RestirGISpatialReservoirHandle = Graph.ImportBuffer(
        "ReSTIR GI Spatial Reservoir",
        Owner.RestirGISpatialReservoirBuffer.Get(),
        &Owner.RestirGISpatialReservoirState,
        RestirReservoirDesc);

    OutResources.RestirGIReservoirHistoryHandle = Graph.ImportBuffer(
        "ReSTIR GI Reservoir History",
        Owner.RestirGIReservoirHistoryBuffer.Get(),
        &Owner.RestirGIReservoirHistoryState,
        RestirReservoirDesc);

    const uint32_t HalfWidth = (static_cast<uint32>(Owner.Viewport.Width) + 1u) / 2u;
    const uint32_t HalfHeight = (static_cast<uint32>(Owner.Viewport.Height) + 1u) / 2u;

    OutResources.RestirGIInitialRadianceHandle = Graph.ImportTexture(
        "ReSTIR GI Initial Radiance",
        Owner.RestirGIInitialRadianceTexture.Get(),
        &Owner.RestirGIInitialRadianceState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIInitialRayDirectionHandle = Graph.ImportTexture(
        "ReSTIR GI Initial RayDir",
        Owner.RestirGIInitialRayDirectionTexture.Get(),
        &Owner.RestirGIInitialRayDirectionState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    OutResources.RestirGIReservoirDepthNormalAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir DepthNormal A",
        Owner.RestirGIReservoirDepthNormalATexture.Get(),
        &Owner.RestirGIReservoirDepthNormalAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT });

    OutResources.RestirGIReservoirDepthNormalBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir DepthNormal B",
        Owner.RestirGIReservoirDepthNormalBTexture.Get(),
        &Owner.RestirGIReservoirDepthNormalBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT });

    OutResources.RestirGIReservoirSampleRadianceAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir SampleRadiance A",
        Owner.RestirGIReservoirSampleRadianceATexture.Get(),
        &Owner.RestirGIReservoirSampleRadianceAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIReservoirSampleRadianceBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir SampleRadiance B",
        Owner.RestirGIReservoirSampleRadianceBTexture.Get(),
        &Owner.RestirGIReservoirSampleRadianceBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.RestirGIReservoirRayDirectionAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir RayDirection A",
        Owner.RestirGIReservoirRayDirectionATexture.Get(),
        &Owner.RestirGIReservoirRayDirectionAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    OutResources.RestirGIReservoirRayDirectionBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir RayDirection B",
        Owner.RestirGIReservoirRayDirectionBTexture.Get(),
        &Owner.RestirGIReservoirRayDirectionBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    OutResources.RestirGIReservoirMWAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir MW A",
        Owner.RestirGIReservoirMWATexture.Get(),
        &Owner.RestirGIReservoirMWAState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_FLOAT });

    OutResources.RestirGIReservoirMWBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir MW B",
        Owner.RestirGIReservoirMWBTexture.Get(),
        &Owner.RestirGIReservoirMWBState,
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_FLOAT });

    OutResources.RestirGiInputSHHandle = Graph.ImportTexture(
        "ReSTIR GI Input SH",
        Owner.RestirGiInputSHTexture.Get(),
        &Owner.RestirGiInputSHState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R32G32B32A32_UINT });

    OutResources.RestirGiVarianceHandle = Graph.ImportTexture(
        "ReSTIR GI Variance",
        Owner.RestirGiVarianceTexture.Get(),
        &Owner.RestirGiVarianceState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R8_UNORM });

    OutResources.RestirGiHistoryIrradianceHandle = Graph.ImportTexture(
        "ReSTIR GI Denoised Irradiance",
        Owner.RestirGiHistoryIrradianceTexture.Get(),
        &Owner.RestirGiHistoryIrradianceState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R11G11B10_FLOAT });

    OutResources.RestirGiTemporalSHHandle = Graph.ImportTexture(
        "ReSTIR GI Temporal SH",
        Owner.RestirGiTemporalSHTexture.Get(),
        &Owner.RestirGiTemporalSHState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R32G32B32A32_UINT });

    OutResources.RestirGiHistorySHHandle = Graph.ImportTexture(
        "ReSTIR GI History SH",
        Owner.RestirGiHistorySHTexture.Get(),
        &Owner.RestirGiHistorySHState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R32G32B32A32_UINT });

    OutResources.RestirGiHistoryCountAHandle = Graph.ImportTexture(
        "ReSTIR GI History Count A",
        Owner.RestirGiHistoryCountATexture.Get(),
        &Owner.RestirGiHistoryCountAState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R8_UINT });

    OutResources.RestirGiHistoryCountBHandle = Graph.ImportTexture(
        "ReSTIR GI History Count B",
        Owner.RestirGiHistoryCountBTexture.Get(),
        &Owner.RestirGiHistoryCountBState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R8_UINT });

    OutResources.RestirGiPrevLinearDepthHandle = Graph.ImportTexture(
        "ReSTIR GI Prev LinearDepth",
        Owner.RestirGiPrevLinearDepthTexture.Get(),
        &Owner.RestirGiPrevLinearDepthState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16_FLOAT });

    OutResources.RestirGiPrevNormalHandle = Graph.ImportTexture(
        "ReSTIR GI Prev Normal",
        Owner.RestirGiPrevNormalTexture.Get(),
        &Owner.RestirGiPrevNormalState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    uint32_t MipWidth = (static_cast<uint32>(Owner.Viewport.Width) + 1u) / 2u;
    uint32_t MipHeight = (static_cast<uint32>(Owner.Viewport.Height) + 1u) / 2u;
    for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
    {
        const std::string ShName = "ReSTIR GI SH Mip " + std::to_string(MipIndex);
        OutResources.RestirGiShMipHandles[MipIndex] = Graph.ImportTexture(
            ShName,
            Owner.RestirGiShMipTextures[MipIndex].Get(),
            &Owner.RestirGiShMipStates[MipIndex],
            { MipWidth, MipHeight, DXGI_FORMAT_R32G32B32A32_UINT });

        const std::string DepthName = "ReSTIR GI LinearDepth Mip " + std::to_string(MipIndex);
        OutResources.RestirGiLinearDepthMipHandles[MipIndex] = Graph.ImportTexture(
            DepthName,
            Owner.RestirGiLinearDepthMipTextures[MipIndex].Get(),
            &Owner.RestirGiLinearDepthMipStates[MipIndex],
            { MipWidth, MipHeight, DXGI_FORMAT_R16_FLOAT });

        MipWidth = (std::max)(1u, (MipWidth + 1u) / 2u);
        MipHeight = (std::max)(1u, (MipHeight + 1u) / 2u);
    }

    OutResources.SsrHandle = Graph.ImportTexture(
        "SSR",
        Owner.SsrTexture.Get(),
        &Owner.SsrState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrDenoiseHandle = Graph.ImportTexture(
        "SSR Denoise",
        Owner.SsrDenoiseTexture.Get(),
        &Owner.SsrDenoiseState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrFallbackHandle = Graph.ImportTexture(
        "SSR Fallback",
        Owner.SsrFallbackTexture.Get(),
        &Owner.SsrFallbackState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrResolveHandle = Graph.ImportTexture(
        "SSR Resolve",
        Owner.SsrResolveTexture.Get(),
        &Owner.SsrResolveState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.LightingHandle = Graph.ImportTexture(
        "Lighting",
        Owner.LightingBuffer.Get(),
        &Owner.LightingBufferState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::LightingBufferFormat });

    OutResources.TonemapOutputResource = Graph.ImportTexture(
        "TonemapOutput",
        Owner.TonemapOutput.Get(),
        &Owner.TonemapOutputState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), Owner.BackBufferFormat });

    OutResources.LuminanceHandles =
    {
        Graph.ImportTexture(
            "LuminanceA",
            Owner.LuminanceTextures[0].Get(),
            &Owner.LuminanceStates[0],
            { 1u, 1u, DXGI_FORMAT_R32_FLOAT }),
        Graph.ImportTexture(
            "LuminanceB",
            Owner.LuminanceTextures[1].Get(),
            &Owner.LuminanceStates[1],
            { 1u, 1u, DXGI_FORMAT_R32_FLOAT })
    };

    OutResources.TaaHandles.reserve(Owner.TaaHistoryTextures.size());
    for (size_t Index = 0; Index < Owner.TaaHistoryTextures.size(); ++Index)
    {
        const std::string HandleName = "TaaHistory_" + std::to_string(Index);
        OutResources.TaaHandles.push_back(Graph.ImportTexture(
            HandleName,
            Owner.TaaHistoryTextures[Index].Get(),
            &Owner.TaaStates[Index],
            { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::LightingBufferFormat }));
    }

    OutResources.PathTracingTempHandle = Graph.ImportTexture(
        "PathTracingTemp",
        Owner.PathTracingTempTexture.Get(),
        &Owner.PathTracingTempState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::PathTracingBufferFormat });

    OutResources.PathTracingAccumulationHandles.reserve(Owner.PathTracingAccumulationTextures.size());
    for (size_t Index = 0; Index < Owner.PathTracingAccumulationTextures.size(); ++Index)
    {
        const std::string HandleName = "PathTracingAccumulation_" + std::to_string(Index);
        OutResources.PathTracingAccumulationHandles.push_back(Graph.ImportTexture(
            HandleName,
            Owner.PathTracingAccumulationTextures[Index].Get(),
            &Owner.PathTracingAccumulationStates[Index],
            { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::PathTracingBufferFormat }));
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
        VelocitySrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        VelocitySrvDesc.Texture2D.MipLevels = 1;
        VelocityBindlessIndex = Device->CreateBindlessSrv(VelocityTexture.Get(), VelocitySrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC HilbertSrvDesc = {};
        HilbertSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        HilbertSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        HilbertSrvDesc.Format = DXGI_FORMAT_R16_UINT;
        HilbertSrvDesc.Texture2D.MipLevels = 1;
        HilbertLutBindlessIndex = Device->CreateBindlessSrv(HilbertLutTexture.Get(), HilbertSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC GtaoSrvDesc = {};
        GtaoSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GtaoSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GtaoSrvDesc.Format = DXGI_FORMAT_R8_UNORM;
        GtaoSrvDesc.Texture2D.MipLevels = 1;
        GtaoBindlessIndex = Device->CreateBindlessSrv(GtaoTexture.Get(), GtaoSrvDesc);
    }

    const auto CreateRestirDescriptors = [&]() -> bool
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC RestirSrvDesc = {};
        RestirSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        RestirSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        RestirSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirSrvDesc.Texture2D.MipLevels = 1;
        RestirGIBindlessIndex = Device->CreateBindlessSrv(RestirGITexture.Get(), RestirSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC RestirUavDesc = {};
        RestirUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        RestirUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirUavDesc.Texture2D.MipSlice = 0;
        RestirGIUavBindlessIndex = Device->CreateBindlessUav(RestirGITexture.Get(), nullptr, RestirUavDesc);

        RestirGIHistorySrvBindlessIndex = Device->CreateBindlessSrv(RestirGIHistoryTexture.Get(), RestirSrvDesc);
        RestirGIHistoryUavBindlessIndex = Device->CreateBindlessUav(RestirGIHistoryTexture.Get(), nullptr, RestirUavDesc);

        D3D12_SHADER_RESOURCE_VIEW_DESC ReservoirSrvDesc = {};
        ReservoirSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        ReservoirSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ReservoirSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        ReservoirSrvDesc.Buffer.FirstElement = 0;
        ReservoirSrvDesc.Buffer.NumElements = static_cast<uint32_t>(Viewport.Width * Viewport.Height);
        ReservoirSrvDesc.Buffer.StructureByteStride = sizeof(float) * 8u;
        ReservoirSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        D3D12_UNORDERED_ACCESS_VIEW_DESC ReservoirUavDesc = {};
        ReservoirUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ReservoirUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        ReservoirUavDesc.Buffer.FirstElement = 0;
        ReservoirUavDesc.Buffer.NumElements = static_cast<uint32_t>(Viewport.Width * Viewport.Height);
        ReservoirUavDesc.Buffer.StructureByteStride = sizeof(float) * 8u;
        ReservoirUavDesc.Buffer.CounterOffsetInBytes = 0;
        ReservoirUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        RestirGITemporalReservoirSrvBindlessIndex = Device->CreateBindlessSrv(RestirGITemporalReservoirBuffer.Get(), ReservoirSrvDesc);
        RestirGITemporalReservoirUavBindlessIndex = Device->CreateBindlessUav(RestirGITemporalReservoirBuffer.Get(), nullptr, ReservoirUavDesc);
        RestirGISpatialReservoirSrvBindlessIndex = Device->CreateBindlessSrv(RestirGISpatialReservoirBuffer.Get(), ReservoirSrvDesc);
        RestirGISpatialReservoirUavBindlessIndex = Device->CreateBindlessUav(RestirGISpatialReservoirBuffer.Get(), nullptr, ReservoirUavDesc);
        RestirGIReservoirHistorySrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirHistoryBuffer.Get(), ReservoirSrvDesc);
        RestirGIReservoirHistoryUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirHistoryBuffer.Get(), nullptr, ReservoirUavDesc);

        D3D12_SHADER_RESOURCE_VIEW_DESC RestirGISrvDesc = {};
        RestirGISrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        RestirGISrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        RestirGISrvDesc.Texture2D.MipLevels = 1;

        D3D12_UNORDERED_ACCESS_VIEW_DESC RestirGIUavDesc = {};
        RestirGIUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        RestirGIUavDesc.Texture2D.MipSlice = 0;

        RestirGISrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGIInitialRadianceSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIInitialRadianceTexture.Get(), RestirGISrvDesc);
        RestirGIInitialRadianceUavBindlessIndex = Device->CreateBindlessUav(RestirGIInitialRadianceTexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirSampleRadianceASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirSampleRadianceATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirSampleRadianceAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirSampleRadianceATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirSampleRadianceBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirSampleRadianceBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirSampleRadianceBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirSampleRadianceBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32_UINT;
        RestirGIInitialRayDirectionSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIInitialRayDirectionTexture.Get(), RestirGISrvDesc);
        RestirGIInitialRayDirectionUavBindlessIndex = Device->CreateBindlessUav(RestirGIInitialRayDirectionTexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirRayDirectionASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirRayDirectionATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirRayDirectionAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirRayDirectionATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirRayDirectionBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirRayDirectionBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirRayDirectionBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirRayDirectionBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32_UINT;
        RestirGIReservoirDepthNormalASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirDepthNormalATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirDepthNormalAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirDepthNormalATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirDepthNormalBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirDepthNormalBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirDepthNormalBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirDepthNormalBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        RestirGIReservoirMWASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirMWATexture.Get(), RestirGISrvDesc);
        RestirGIReservoirMWAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirMWATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGIReservoirMWBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirMWBTexture.Get(), RestirGISrvDesc);
        RestirGIReservoirMWBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirMWBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGiInputSHSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiInputSHTexture.Get(), RestirGISrvDesc);
        RestirGiInputSHUavBindlessIndex = Device->CreateBindlessUav(RestirGiInputSHTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R8_UNORM;
        RestirGIUavDesc.Format = DXGI_FORMAT_R8_UNORM;
        RestirGiVarianceSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiVarianceTexture.Get(), RestirGISrvDesc);
        RestirGiVarianceUavBindlessIndex = Device->CreateBindlessUav(RestirGiVarianceTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        RestirGiHistoryIrradianceSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistoryIrradianceTexture.Get(), RestirGISrvDesc);
        RestirGiHistoryIrradianceUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistoryIrradianceTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGiTemporalSHSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiTemporalSHTexture.Get(), RestirGISrvDesc);
        RestirGiTemporalSHUavBindlessIndex = Device->CreateBindlessUav(RestirGiTemporalSHTexture.Get(), nullptr, RestirGIUavDesc);
        RestirGiHistorySHSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistorySHTexture.Get(), RestirGISrvDesc);
        RestirGiHistorySHUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistorySHTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R8_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R8_UINT;
        RestirGiHistoryCountASrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistoryCountATexture.Get(), RestirGISrvDesc);
        RestirGiHistoryCountAUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistoryCountATexture.Get(), nullptr, RestirGIUavDesc);
        RestirGiHistoryCountBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiHistoryCountBTexture.Get(), RestirGISrvDesc);
        RestirGiHistoryCountBUavBindlessIndex = Device->CreateBindlessUav(RestirGiHistoryCountBTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        RestirGiPrevLinearDepthSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiPrevLinearDepthTexture.Get(), RestirGISrvDesc);
        RestirGiPrevLinearDepthUavBindlessIndex = Device->CreateBindlessUav(RestirGiPrevLinearDepthTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        RestirGiPrevNormalSrvBindlessIndex = Device->CreateBindlessSrv(RestirGiPrevNormalTexture.Get(), RestirGISrvDesc);
        RestirGiPrevNormalUavBindlessIndex = Device->CreateBindlessUav(RestirGiPrevNormalTexture.Get(), nullptr, RestirGIUavDesc);

        RestirGISrvDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
        for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
        {
            RestirGiShMipSrvBindlessIndices[MipIndex] = Device->CreateBindlessSrv(RestirGiShMipTextures[MipIndex].Get(), RestirGISrvDesc);
            RestirGiShMipUavBindlessIndices[MipIndex] = Device->CreateBindlessUav(RestirGiShMipTextures[MipIndex].Get(), nullptr, RestirGIUavDesc);
        }

        RestirGISrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
        RestirGIUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        for (uint32_t MipIndex = 0; MipIndex < 4u; ++MipIndex)
        {
            RestirGiLinearDepthMipSrvBindlessIndices[MipIndex] = Device->CreateBindlessSrv(RestirGiLinearDepthMipTextures[MipIndex].Get(), RestirGISrvDesc);
            RestirGiLinearDepthMipUavBindlessIndices[MipIndex] = Device->CreateBindlessUav(RestirGiLinearDepthMipTextures[MipIndex].Get(), nullptr, RestirGIUavDesc);
        }
        return true;
    };
    if (!CreateRestirDescriptors())
    {
        return false;
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC SsrSrvDesc = {};
        SsrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SsrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SsrSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrSrvDesc.Texture2D.MipLevels = 1;
        SsrBindlessIndex = Device->CreateBindlessSrv(SsrTexture.Get(), SsrSrvDesc);
        SsrDenoiseBindlessIndex = Device->CreateBindlessSrv(SsrDenoiseTexture.Get(), SsrSrvDesc);
        SsrFallbackBindlessIndex = Device->CreateBindlessSrv(SsrFallbackTexture.Get(), SsrSrvDesc);
        SsrResolveBindlessIndex = Device->CreateBindlessSrv(SsrResolveTexture.Get(), SsrSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC SsrUavDesc = {};
        SsrUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        SsrUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrUavDesc.Texture2D.MipSlice = 0;
        SsrUavBindlessIndex = Device->CreateBindlessUav(SsrTexture.Get(), nullptr, SsrUavDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC SsrResolveUavDesc = {};
        SsrResolveUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        SsrResolveUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrResolveUavDesc.Texture2D.MipSlice = 0;
        SsrResolveUavBindlessIndex = Device->CreateBindlessUav(SsrResolveTexture.Get(), nullptr, SsrResolveUavDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC SsrFallbackUavDesc = {};
        SsrFallbackUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        SsrFallbackUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        SsrFallbackUavDesc.Texture2D.MipSlice = 0;
        SsrFallbackUavBindlessIndex = Device->CreateBindlessUav(SsrFallbackTexture.Get(), nullptr, SsrFallbackUavDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC LightingSrvDesc = {};
        LightingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        LightingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        LightingSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        LightingSrvDesc.Texture2D.MipLevels = 1;
        LightingBufferBindlessIndex = Device->CreateBindlessSrv(LightingBuffer.Get(), LightingSrvDesc);
    }

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC TonemapSrvDesc = {};
        TonemapSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        TonemapSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        TonemapSrvDesc.Format = BackBufferFormat;
        TonemapSrvDesc.Texture2D.MipLevels = 1;
        TonemapOutputBindlessIndex = Device->CreateBindlessSrv(TonemapOutput.Get(), TonemapSrvDesc);
    }

    const auto CreateTemporalAndHzbDescriptors = [&]() -> bool
    {
        for (uint32_t Index = 0; Index < LuminanceTextures.size(); ++Index)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC LuminanceSrvDesc = {};
            LuminanceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            LuminanceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            LuminanceSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            LuminanceSrvDesc.Texture2D.MipLevels = 1;
            LuminanceSrvBindlessIndices[Index] = Device->CreateBindlessSrv(LuminanceTextures[Index].Get(), LuminanceSrvDesc);

            D3D12_UNORDERED_ACCESS_VIEW_DESC LuminanceUavDesc = {};
            LuminanceUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            LuminanceUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
            LuminanceUavDesc.Texture2D.MipSlice = 0;
            LuminanceUavDesc.Texture2D.PlaneSlice = 0;
            LuminanceUavBindlessIndices[Index] = Device->CreateBindlessUav(LuminanceTextures[Index].Get(), nullptr, LuminanceUavDesc);
        }

        TaaSrvBindlessIndices.clear();
        TaaUavBindlessIndices.clear();
        TaaSrvBindlessIndices.resize(TaaHistoryTextures.size(), UINT32_MAX);
        TaaUavBindlessIndices.resize(TaaHistoryTextures.size(), UINT32_MAX);

        for (uint32_t Index = 0; Index < TaaHistoryTextures.size(); ++Index)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC TaaSrvDesc = {};
            TaaSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            TaaSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            TaaSrvDesc.Format = FDeferredRenderer::LightingBufferFormat;
            TaaSrvDesc.Texture2D.MipLevels = 1;
            TaaSrvBindlessIndices[Index] = Device->CreateBindlessSrv(TaaHistoryTextures[Index].Get(), TaaSrvDesc);

            D3D12_UNORDERED_ACCESS_VIEW_DESC TaaUavDesc = {};
            TaaUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            TaaUavDesc.Format = FDeferredRenderer::LightingBufferFormat;
            TaaUavDesc.Texture2D.MipSlice = 0;
            TaaUavDesc.Texture2D.PlaneSlice = 0;
            TaaUavBindlessIndices[Index] = Device->CreateBindlessUav(TaaHistoryTextures[Index].Get(), nullptr, TaaUavDesc);
        }

        if (PathTracingTempTexture)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC PathTracingTempUavDesc = {};
            PathTracingTempUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            PathTracingTempUavDesc.Format = FDeferredRenderer::PathTracingBufferFormat;
            PathTracingTempUavDesc.Texture2D.MipSlice = 0;
            PathTracingTempBindlessIndex = Device->CreateBindlessUav(PathTracingTempTexture.Get(), nullptr, PathTracingTempUavDesc);
        }

        PathTracingAccumulationSrvBindlessIndices.clear();
        PathTracingAccumulationUavBindlessIndices.clear();
        PathTracingAccumulationSrvBindlessIndices.resize(PathTracingAccumulationTextures.size(), UINT32_MAX);
        PathTracingAccumulationUavBindlessIndices.resize(PathTracingAccumulationTextures.size(), UINT32_MAX);

        for (uint32_t Index = 0; Index < PathTracingAccumulationTextures.size(); ++Index)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC AccumSrvDesc = {};
            AccumSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            AccumSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            AccumSrvDesc.Format = FDeferredRenderer::PathTracingBufferFormat;
            AccumSrvDesc.Texture2D.MipLevels = 1;
            PathTracingAccumulationSrvBindlessIndices[Index] = Device->CreateBindlessSrv(PathTracingAccumulationTextures[Index].Get(), AccumSrvDesc);

            D3D12_UNORDERED_ACCESS_VIEW_DESC AccumUavDesc = {};
            AccumUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            AccumUavDesc.Format = FDeferredRenderer::PathTracingBufferFormat;
            AccumUavDesc.Texture2D.MipSlice = 0;
            AccumUavDesc.Texture2D.PlaneSlice = 0;
            PathTracingAccumulationUavBindlessIndices[Index] = Device->CreateBindlessUav(PathTracingAccumulationTextures[Index].Get(), nullptr, AccumUavDesc);
        }

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
