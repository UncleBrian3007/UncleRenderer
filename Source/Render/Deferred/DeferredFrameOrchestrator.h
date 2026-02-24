#pragma once

#include "DeferredPassContext.h"

class FDeferredFrameOrchestrator
{
public:
    void BuildFrameGraph(FDeferredPassContext& Context) const;
};
