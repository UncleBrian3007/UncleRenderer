#pragma once

#include <cstddef>
#include <vector>

#include "Object.h"

struct FSceneModelResource;
struct FMeshMaterial;

// A static (non-skinned) mesh object. Groups one glTF node's primitive sections,
// each of which is a separate FSceneModelResource in the World's flat list.
class FStaticMesh : public IObject
{
public:
    EObjectType GetType() const override { return EObjectType::StaticMesh; }

    // Material for one section, fetched from the World's render-resource list.
    // Returns nullptr if SectionIndex is out of range.
    FMeshMaterial* GetMaterial(std::vector<FSceneModelResource>& SceneModels, size_t SectionIndex) const;
};
