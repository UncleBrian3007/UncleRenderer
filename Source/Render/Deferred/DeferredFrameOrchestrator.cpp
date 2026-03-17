#include "DeferredFrameOrchestrator.h"

#include "../DeferredRenderer.h"
#include "DeferredVisibilityPasses.h"
#include "DeferredGeometryPasses.h"
#include "DeferredLightingPasses.h"
#include "DeferredPostProcessPasses.h"
#include "Gtao.h"
#include "RayTracingShadow.h"
#include "Ssr.h"
#include "RestirGI.h"
#include "PathTracing.h"
#include "AutoExposure.h"
#include "Cas.h"
#include "Taa.h"
#include "Tonemap.h"

void FDeferredFrameOrchestrator::BuildFrameGraph(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    FDeferredRenderer::FDeferredFrameResources& Resources = Context.Resources;
    const FCamera& Camera = Context.Camera;

    uint32_t PrevVisibilityIndex = UINT32_MAX;
    uint32_t PrevVisibilityFrameIndex = UINT32_MAX;
    uint32_t CurrentVisibilityIndex = UINT32_MAX;
    uint32_t PrevVisibleListSrvIndex = UINT32_MAX;
    uint32_t PrevVisibleCountSrvIndex = UINT32_MAX;
    uint32_t LateListSrvIndex = UINT32_MAX;
    uint32_t LateListCountSrvIndex = UINT32_MAX;
    if (!Owner.MeshletVisibilitySrvBindlessIndices.empty())
    {
        const uint32_t FramesInFlight = Owner.GetFramesInFlight();
        const uint32_t PrevFrameIndex = (Owner.GetFrameIndex() + FramesInFlight - 1u) % FramesInFlight;
        PrevVisibilityIndex = Owner.MeshletVisibilitySrvBindlessIndices[PrevFrameIndex];
        PrevVisibilityFrameIndex = PrevFrameIndex;
        CurrentVisibilityIndex = Owner.MeshletVisibilitySrvBindlessIndices[Context.FrameIndex];
    }
    if (Context.FrameIndex < Owner.PrevVisibleListSrvBindlessIndices.size())
    {
        PrevVisibleListSrvIndex = Owner.PrevVisibleListSrvBindlessIndices[Context.FrameIndex];
        PrevVisibleCountSrvIndex = Owner.PrevVisibleCountSrvBindlessIndices[Context.FrameIndex];
    }
    if (Context.FrameIndex < Owner.LateListSrvBindlessIndices.size())
    {
        LateListSrvIndex = Owner.LateListSrvBindlessIndices[Context.FrameIndex];
        LateListCountSrvIndex = Owner.LateListCountSrvBindlessIndices[Context.FrameIndex];
    }

    if (FrameState.bUseHzbTwoPass)
    {
        Owner.VisibilityPasses->AddVisibilityListPass(Context, PrevVisibilityIndex, PrevVisibilityFrameIndex);
        Owner.VisibilityPasses->AddGpuCullingPass(
            Context,
            FRenderer::ECullingMode::All,
            UINT32_MAX,
            UINT32_MAX,
            PrevVisibleListSrvIndex,
            PrevVisibleCountSrvIndex,
            "GPUCulling Early");

        Owner.GeometryPasses->AddBasePass(
            Context,
            true,
            true,
            "GBuffer Early",
            true);
    }
    else
    {
        Owner.VisibilityPasses->AddGpuCullingPass(
            Context,
            FRenderer::ECullingMode::All,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            "GPUCulling");
    }

    if (!Owner.bRayTracedShadowsEnabled && !Context.bUsePathTracing)
    {
        Owner.GeometryPasses->AddShadowPass(Context);
    }

    if (FrameState.bDoDepthPrepass)
    {
        Owner.GeometryPasses->AddDepthPrepass(Context);
    }

    if (!FrameState.bUseHzbTwoPass)
    {
        Owner.GeometryPasses->AddBasePass(
            Context,
            true,
            !FrameState.bDoDepthPrepass,
            "GBuffer",
            true);
    }
    if (FrameState.bUseHzbTwoPass)
    {
        Owner.VisibilityPasses->AddEarlyRejectListPass(Context, CurrentVisibilityIndex);
    }
    Owner.VisibilityPasses->AddHZBPass(Context);
    if (FrameState.bUseHzbTwoPass)
    {
        Owner.VisibilityPasses->AddLateListMergePass(Context);
        Owner.VisibilityPasses->AddGpuCullingPass(
            Context,
            FRenderer::ECullingMode::LateAfterEarly,
            UINT32_MAX,
            UINT32_MAX,
            LateListSrvIndex,
            LateListCountSrvIndex,
            "GPU Culling (Late)");
        Owner.GeometryPasses->AddBasePass(
            Context,
            false,
            false,
            "GBuffer (Late)",
            false);
    }

    Owner.GeometryPasses->AddVelocityPass(Context);

    if (Context.bUsePathTracing)
    {
        if (Owner.PathTracing)
        {
            Owner.PathTracing->AddPasses(Context);
        }
    }
    else
    {
        if (Owner.RayTracingShadow)
        {
            Owner.RayTracingShadow->AddPass(Context);
        }
        Owner.LightingPasses->AddLinearDepthPass(Context);
        Owner.LightingPasses->AddExtractHalfDepthNormalPass(Context);
        if (Owner.Gtao)
        {
            Owner.Gtao->AddPass(Context);
        }
        Owner.RestirGI->AddPasses(Context);
        if (Owner.RestirGIDenoiser)
        {
            Owner.RestirGIDenoiser->AddPasses(Context);
        }
        if (Owner.Ssr)
        {
            Owner.Ssr->AddPasses(Context);
        }
        FRGResourceHandle DirectLightingHandle{};
        Owner.LightingPasses->AddDirectLightingPass(Context, DirectLightingHandle);
        Owner.LightingPasses->AddCompositeLightPass(Context, DirectLightingHandle);
    }

    Owner.LightingPasses->AddSkyPass(Context);
    Owner.GeometryPasses->AddObjectIdPass(Context);
    if (Owner.Taa)
    {
        Owner.Taa->AddPass(Context);
    }
    if (Owner.AutoExposure)
    {
        Owner.AutoExposure->AddPass(Context);
    }
    if (Owner.Tonemap)
    {
        Owner.Tonemap->AddPasses(Context);
    }
    if (Owner.Cas)
    {
        Owner.Cas->AddPass(Context);
    }
    Owner.PostProcessPasses->AddDebugPrintPass(Context);
}
