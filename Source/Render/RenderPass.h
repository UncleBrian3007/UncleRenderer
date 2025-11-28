#pragma once

#include <string>
#include "../RHI/DX12Commons.h"

class FDX12CommandContext;

class FRenderPass
{
public:
    virtual ~FRenderPass() = default;

    virtual std::string GetName() const = 0;
    virtual void Execute(FDX12CommandContext& CmdContext) = 0;
};
