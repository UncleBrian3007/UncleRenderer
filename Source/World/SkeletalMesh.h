#pragma once

#include <cstddef>
#include <vector>

#include "Object.h"

struct FSceneModelResource;
struct FMeshMaterial;

// A skinned mesh object. Groups one glTF node's primitive sections (each a
// separate FSceneModelResource) plus the skin used to animate them. The actual
// skinning buffers/bone matrices live on the per-section FSceneModelResource.
class FSkeletalMesh : public IObject
{
public:
    EObjectType GetType() const override { return EObjectType::SkeletalMesh; }

    int GetGltfSkinIndex() const { return GltfSkinIndex; }
    void SetGltfSkinIndex(int SkinIndex) { GltfSkinIndex = SkinIndex; }

    // Material for one section, fetched from the World's render-resource list.
    // Returns nullptr if SectionIndex is out of range.
    FMeshMaterial* GetMaterial(std::vector<FSceneModelResource>& SceneModels, size_t SectionIndex) const;

private:
    int GltfSkinIndex = -1;
};
