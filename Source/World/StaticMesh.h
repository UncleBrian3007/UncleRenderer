#pragma once

#include <cstddef>

#include "Object.h"

struct FMeshMaterial;

// A static (non-skinned) mesh object. Groups one glTF node's primitive sections.
class FStaticMesh : public FObject
{
public:
    EObjectType GetType() const override { return EObjectType::StaticMesh; }

    // Returns nullptr if SectionIndex is out of range.
    FMeshMaterial* GetMaterial(size_t SectionIndex);
    const FMeshMaterial* GetMaterial(size_t SectionIndex) const;
};
