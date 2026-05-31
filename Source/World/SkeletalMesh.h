#pragma once

#include <cstddef>

#include "Object.h"

struct FMeshMaterial;

// A skinned mesh object. Groups one glTF node's primitive sections plus skin state.
class FSkeletalMesh : public FObject
{
public:
    EObjectType GetType() const override { return EObjectType::SkeletalMesh; }

    int GetGltfSceneIndex() const { return GltfSceneIndex; }
    int GetGltfNodeIndex() const { return GltfNodeIndex; }
    int GetGltfMeshIndex() const { return GltfMeshIndex; }
    void SetGltfIndices(int SceneIndex, int NodeIndex, int MeshIndex)
    {
        GltfSceneIndex = SceneIndex;
        GltfNodeIndex = NodeIndex;
        GltfMeshIndex = MeshIndex;
    }

    int GetGltfSkinIndex() const { return GltfSkinIndex; }
    void SetGltfSkinIndex(int SkinIndex) { GltfSkinIndex = SkinIndex; }

    void Tick(float DeltaTime, FWorldTickContext& Ctx) override;

    // Returns nullptr if SectionIndex is out of range.
    FMeshMaterial* GetMaterial(size_t SectionIndex);
    const FMeshMaterial* GetMaterial(size_t SectionIndex) const;

private:
    int GltfSceneIndex = -1;
    int GltfNodeIndex = -1;
    int GltfMeshIndex = -1;
    int GltfSkinIndex = -1;
};
