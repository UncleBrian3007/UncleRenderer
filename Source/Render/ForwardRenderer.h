#pragma once

#include <memory>
#include "RenderGraph.h"

class FDX12Device;
class FDX12CommandContext;
class FFrameResources;

class FForwardRenderer
{
public:
    FForwardRenderer();

    bool Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height);
    void RenderFrame(FDX12CommandContext& CmdContext);

private:
    std::unique_ptr<FRenderGraph> RenderGraph;
    std::unique_ptr<FFrameResources> FrameResources;
};
