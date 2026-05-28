#pragma once

#include <cstddef>

#include "Object.h"

struct FMeshMaterial;

// A skinned mesh object. Groups one glTF node's primitive sections plus skin state.
class FSkeletalMesh : public FObject
{
public:
    EObjectType GetType() const override { return EObjectType::SkeletalMesh; }

    int GetGltfSkinIndex() const { return GltfSkinIndex; }
    void SetGltfSkinIndex(int SkinIndex) { GltfSkinIndex = SkinIndex; }

    // Returns nullptr if SectionIndex is out of range.
    FMeshMaterial* GetMaterial(size_t SectionIndex);
    const FMeshMaterial* GetMaterial(size_t SectionIndex) const;

private:
    int GltfSkinIndex = -1;
};
