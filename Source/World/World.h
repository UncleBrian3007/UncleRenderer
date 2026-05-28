#pragma once

#include <memory>
#include <vector>

#include "Object.h"

struct FSceneModelResource;

// Owns the logical scene objects (StaticMesh / SkeletalMesh) produced by the
// glTF loader. The renderer keeps consuming the flat FSceneModelResource list;
// each object here references its sections in that list by index, so this is an
// additive grouping/ownership layer rather than a replacement.
class FWorld
{
public:
    // Temporary bridge during SceneModels ownership migration.
    void LinkSceneModels(std::vector<FSceneModelResource>* InSceneModels)
    {
        SceneModels = InSceneModels;
    }

    const std::vector<FSceneModelResource>* GetSceneModels() const { return SceneModels; }
    std::vector<FSceneModelResource>* GetSceneModelsMutable() { return SceneModels; }

    // Takes ownership of an object and returns a non-owning pointer to it.
    IObject* AddObject(std::unique_ptr<IObject> Object)
    {
        IObject* Ptr = Object.get();
        Objects.push_back(std::move(Object));
        return Ptr;
    }

    const std::vector<std::unique_ptr<IObject>>& GetObjects() const { return Objects; }
    std::vector<std::unique_ptr<IObject>>& GetObjects() { return Objects; }

    size_t GetObjectCount() const { return Objects.size(); }

    void Clear() { Objects.clear(); }

private:
    std::vector<std::unique_ptr<IObject>> Objects;
    std::vector<FSceneModelResource>* SceneModels = nullptr;
};
