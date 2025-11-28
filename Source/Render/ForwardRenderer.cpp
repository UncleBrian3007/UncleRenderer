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
    // 실제 초기화 로직은 이후 구현합니다.
    return true;
}

void FForwardRenderer::RenderFrame(FDX12CommandContext& CmdContext)
{
    // 기본 프레임 렌더링 순서: RenderGraph 실행
    RenderGraph->Execute(CmdContext);
}
