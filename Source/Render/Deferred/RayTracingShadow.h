#pragma once

#include <d3d12.h>

#include "../RenderGraph.h"

class FDeferredRenderer;
struct FDeferredPassContext;

struct FRayTracingShadowFrameResources
{
    FRGResourceHandle ShadowMaskHandle{};
};

class FRayTracingShadow
{
public:
    void AddPass(FDeferredPassContext& Context) const;
};
