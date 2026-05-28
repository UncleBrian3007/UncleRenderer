#include "StaticMesh.h"

FMeshMaterial* FStaticMesh::GetMaterial(size_t SectionIndex)
{
    if (SectionIndex >= Sections.size())
    {
        return nullptr;
    }

    return &Sections[SectionIndex].Material;
}

const FMeshMaterial* FStaticMesh::GetMaterial(size_t SectionIndex) const
{
    if (SectionIndex >= Sections.size())
    {
        return nullptr;
    }

    return &Sections[SectionIndex].Material;
}
