#include "DeferredResourceImporter.h"

#include "../DeferredRenderer.h"

void FDeferredResourceImporter::ImportFrameResources(FDeferredPassContext& Context) const
{
    Context.Owner.ImportFrameResources(Context.Graph, Context.Resources);
}
