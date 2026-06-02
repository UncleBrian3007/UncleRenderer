#include "DeferredResourceImporter.h"

#include "../DeferredRenderer.h"
#include "Gtao.h"
#include "Hzb.h"
#include "Ssr.h"
#include "RestirGI.h"
#include "SparseSdfGI.h"
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

    OutResources.ShadowHandle = ImportBindlessTexture(Graph, "ShadowMap", Owner.ShadowMap);

    const FRGTextureDesc DepthDesc =
    {
        static_cast<uint32>(Owner.Viewport.Width),
        static_cast<uint32>(Owner.Viewport.Height),
        Owner.GetDepthTypelessFormat()
    };

    D3D12_RESOURCE_STATES& DepthState = Owner.GetDepthBufferState();
    OutResources.DepthHandle = Graph.ImportTexture("Depth", Owner.GetDepthBuffer(), &DepthState, DepthDesc);
    OutResources.ObjectIdHandle = Owner.ObjectId->ImportResource(
        Graph,
        static_cast<uint32_t>(Owner.Viewport.Width),
        static_cast<uint32_t>(Owner.Viewport.Height));
    OutResources.VelocityHandle = ImportBindlessTexture(Graph, "Velocity", Owner.VelocityTexture);
    OutResources.GBufferHandles =
    {
        ImportBindlessTexture(Graph, "GBufferA", Owner.GBufferA),
        ImportBindlessTexture(Graph, "GBufferB", Owner.GBufferB),
        ImportBindlessTexture(Graph, "GBufferC", Owner.GBufferC),
        ImportBindlessTexture(Graph, "GBufferD", Owner.GBufferD),
    };
    Owner.ClusterDagVisibilityPass->ImportPersistentResources(Context);

    OutResources.LinearDepthHandle = ImportBindlessTexture(Graph, "LinearDepth", Owner.LinearDepthTexture);

    Owner.Gtao->ImportPersistentResources(Context);
    Owner.SparseSdfGI->ImportPersistentResources(Context);
    Owner.RestirGI->ImportPersistentResources(Context);
    Owner.DiffuseGIDenoiser->ImportPersistentResources(Context);
    Owner.Ssr->ImportPersistentResources(Context);

    OutResources.LightingHandle = ImportBindlessTexture(Graph, "Lighting", Owner.LightingBuffer);

    Owner.AutoExposure->ImportPersistentResources(Context);
    Owner.Cas->ImportPersistentResources(Context);
    Owner.Tonemap->ImportPersistentResources(Context);
    Owner.Taa->ImportPersistentResources(Context);
    Owner.PathTracing->ImportPersistentResources(Context);

    Owner.Hzb->ImportPersistentResources(Context);
}

