#include "ForwardRenderer.h"
#include "FrameResources.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12CommandContext.h"

FForwardRenderer::FForwardRenderer()
    : RenderGraph(std::make_unique<FRenderGraph>())
    , FrameResources(std::make_unique<FFrameResources>())
{
}

bool FForwardRenderer::Initialize(FDX12Device* /*Device*/, uint32_t /*Width*/, uint32_t /*Height*/)
{

    return true;
}

void FForwardRenderer::RenderFrame(FDX12CommandContext& CmdContext)
{
    RenderGraph->Execute(CmdContext);
}
