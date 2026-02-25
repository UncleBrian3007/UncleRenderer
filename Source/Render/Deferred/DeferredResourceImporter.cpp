#include "DeferredResourceImporter.h"

#include "../DeferredRenderer.h"
#include <algorithm>
#include <string>

namespace
{
    constexpr DXGI_FORMAT GBufferFormats[4] =
    {
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
    };

    constexpr DXGI_FORMAT LightingBufferFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT PathTracingBufferFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
}

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
        Graph.ImportTexture("GBufferA", Owner.GBufferA.Get(), &Owner.GBufferStates[0], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), GBufferFormats[0] }),
        Graph.ImportTexture("GBufferB", Owner.GBufferB.Get(), &Owner.GBufferStates[1], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), GBufferFormats[1] }),
        Graph.ImportTexture("GBufferC", Owner.GBufferC.Get(), &Owner.GBufferStates[2], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), GBufferFormats[2] }),
        Graph.ImportTexture("GBufferD", Owner.GBufferD.Get(), &Owner.GBufferStates[3], { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), GBufferFormats[3] }),
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
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), LightingBufferFormat });

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
            { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), LightingBufferFormat }));
    }

    OutResources.PathTracingTempHandle = Graph.ImportTexture(
        "PathTracingTemp",
        Owner.PathTracingTempTexture.Get(),
        &Owner.PathTracingTempState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), PathTracingBufferFormat });

    OutResources.PathTracingAccumulationHandles.reserve(Owner.PathTracingAccumulationTextures.size());
    for (size_t Index = 0; Index < Owner.PathTracingAccumulationTextures.size(); ++Index)
    {
        const std::string HandleName = "PathTracingAccumulation_" + std::to_string(Index);
        OutResources.PathTracingAccumulationHandles.push_back(Graph.ImportTexture(
            HandleName,
            Owner.PathTracingAccumulationTextures[Index].Get(),
            &Owner.PathTracingAccumulationStates[Index],
            { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), PathTracingBufferFormat }));
    }

    OutResources.HZBHandle = Graph.ImportTexture(
        "HZB",
        Owner.HierarchicalZBuffer.Get(),
        &Owner.HZBState,
        { Owner.HZBWidth, Owner.HZBHeight, DXGI_FORMAT_R32G32_FLOAT });
}
