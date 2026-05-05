#include "DeferredFrameOrchestrator.h"

#include "../DeferredRenderer.h"
#include "DeferredBasePass.h"
#include "DeferredLightingPass.h"
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
    if (Owner.GpuDrivenCullingState.HasMeshletVisibilityInputs())
    {
        const uint32_t FramesInFlight = Owner.GetFramesInFlight();
        const uint32_t PrevFrameIndex = (Owner.GetFrameIndex() + FramesInFlight - 1u) % FramesInFlight;
        PrevVisibilityIndex = Owner.GpuDrivenCullingState.GetMeshletVisibilityFrameData(PrevFrameIndex).SrvBindlessIndex;
        PrevVisibilityFrameIndex = PrevFrameIndex;
        CurrentVisibilityIndex = Owner.GpuDrivenCullingState.GetMeshletVisibilityFrameData(Context.FrameIndex).SrvBindlessIndex;
    }
    const FRenderer::FVisibilityListFrameSrvIndices VisibilityListSrvs = Owner.GpuDrivenCullingState.GetVisibilityListFrameSrvIndices(Context.FrameIndex);
    PrevVisibleListSrvIndex = VisibilityListSrvs.PrevVisibleListSrv;
    PrevVisibleCountSrvIndex = VisibilityListSrvs.PrevVisibleCountSrv;
    LateListSrvIndex = VisibilityListSrvs.LateListSrv;
    LateListCountSrvIndex = VisibilityListSrvs.LateListCountSrv;

    if (FrameState.bUseHzbTwoPass)
    {
        if (Owner.IsClusterDagEnabled())
        {
            Owner.ClusterDagRuntime->AddPasses(Context);
        }
        Owner.GpuDrivenCullingState.AddVisibilityListPass(Context, PrevVisibilityIndex, PrevVisibilityFrameIndex);
        Owner.GpuDrivenCullingState.AddGpuCullingPass(
            Context,
            FRenderer::ECullingMode::All,
            UINT32_MAX,
            UINT32_MAX,
            PrevVisibleListSrvIndex,
            PrevVisibleCountSrvIndex,
            "GPUCulling Early");

        Owner.BasePass->AddBasePass(
            Context,
            true,
            true,
            "GBuffer Early",
            true);
    }
    else
    {
        if (Owner.IsClusterDagEnabled())
        {
            Owner.ClusterDagRuntime->AddPasses(Context);
        }
        Owner.GpuDrivenCullingState.AddGpuCullingPass(
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
        Owner.BasePass->AddShadowPass(Context);
    }

    if (FrameState.bDoDepthPrepass)
    {
        Owner.BasePass->AddDepthPrepass(Context);
    }

    if (!FrameState.bUseHzbTwoPass)
    {
        Owner.BasePass->AddBasePass(
            Context,
            true,
            !FrameState.bDoDepthPrepass,
            "GBuffer",
            true);
    }
    if (FrameState.bUseHzbTwoPass)
    {
        Owner.GpuDrivenCullingState.AddEarlyRejectListPass(Context, CurrentVisibilityIndex);
    }
    Owner.Hzb->AddPass(Context);
    if (FrameState.bUseHzbTwoPass)
    {
        Owner.GpuDrivenCullingState.AddLateListMergePass(Context);
        Owner.GpuDrivenCullingState.AddGpuCullingPass(
            Context,
            FRenderer::ECullingMode::LateAfterEarly,
            UINT32_MAX,
            UINT32_MAX,
            LateListSrvIndex,
            LateListCountSrvIndex,
            "GPU Culling (Late)");
        Owner.BasePass->AddBasePass(
            Context,
            false,
            false,
            "GBuffer (Late)",
            false);
    }

    Owner.BasePass->AddVelocityPass(Context);

    if (Context.bUsePathTracing)
    {
        Owner.PathTracing->AddPasses(Context);
    }
    else
    {
        Owner.RayTracingShadow->AddPass(Context);
        Owner.LightingPass->AddLinearDepthPass(Context);
        Owner.LightingPass->AddExtractHalfDepthNormalPass(Context);
        Owner.Gtao->AddPass(Context);
        Owner.RestirGI->AddPasses(Context);
        Owner.RestirGIDenoiser->AddPasses(Context);
        Owner.Ssr->AddPasses(Context);
        FRGResourceHandle DirectLightingHandle{};
        Owner.LightingPass->AddDirectLightingPass(Context, DirectLightingHandle);
        Owner.LightingPass->AddCompositeLightPass(Context, DirectLightingHandle);
    }

    Owner.SkyAtmosphere->AddPass(Context);
    Owner.ObjectId->AddPass(Context);
    Owner.Taa->AddPass(Context);
    Owner.AutoExposure->AddPass(Context);
    Owner.Tonemap->AddPasses(Context);
    Owner.Cas->AddPass(Context);
    Owner.GetGpuDebugState().AddPass(Context);
}
