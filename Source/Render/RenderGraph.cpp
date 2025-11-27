#include "RenderGraph.h"
#include "../RHI/DX12CommandContext.h"

FRenderGraph::FRenderGraph()
{
}

FRGResourceHandle FRGPassBuilder::CreateTexture(const std::string&, const FRGTextureDesc&)
{
    static uint32 NextId = 0;
    return { NextId++ };
}

FRGResourceHandle FRGPassBuilder::ReadTexture(const FRGResourceHandle& Handle)
{
    return Handle;
}

FRGResourceHandle FRGPassBuilder::WriteTexture(const FRGResourceHandle& Handle)
{
    return Handle;
}

void FRenderGraph::Execute(FDX12CommandContext& CmdContext)
{
    for (PassEntry& Entry : Passes)
    {
        if (Entry.ExecuteFunc)
        {
            Entry.ExecuteFunc(Entry.DataStorage, CmdContext);
        }
    }
}

