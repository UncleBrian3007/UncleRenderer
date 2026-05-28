#include "SkeletalMesh.h"

#include "../Render/SceneModelResource.h"

FMeshMaterial* FSkeletalMesh::GetMaterial(std::vector<FSceneModelResource>& SceneModels, size_t SectionIndex) const
{
    if (SectionIndex >= SectionModelIndices.size())
    {
        return nullptr;
    }

    const uint32_t ModelIndex = SectionModelIndices[SectionIndex];
    if (ModelIndex >= SceneModels.size())
    {
        return nullptr;
    }

    return &SceneModels[ModelIndex].Material;
}
