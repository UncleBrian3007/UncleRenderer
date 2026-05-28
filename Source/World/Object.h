#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

// Kind of world object. Used instead of RTTI to branch on concrete type.
enum class EObjectType : uint32_t
{
    StaticMesh,
    SkeletalMesh
};

// Base class for everything the World owns. UncleRenderer renders data-driven
// over the flat FSceneModelResource list, so IObject is intentionally a light
// grouping/ownership node rather than a polymorphic render dispatcher: it links
// a logical object (a glTF node) to the render-resource sections it produced.
class IObject
{
public:
    virtual ~IObject() = default;

    virtual EObjectType GetType() const = 0;

    const std::string& GetName() const { return Name; }
    void SetName(const std::string& InName) { Name = InName; }

    uint32_t GetObjectId() const { return ObjectId; }
    void SetObjectId(uint32_t InObjectId) { ObjectId = InObjectId; }

    const DirectX::XMFLOAT4X4& GetWorldMatrix() const { return WorldMatrix; }
    void SetWorldMatrix(const DirectX::XMFLOAT4X4& InMatrix) { WorldMatrix = InMatrix; }

    // Indices into FWorld's flat SceneModels list for the sections this object owns.
    const std::vector<uint32_t>& GetSectionModelIndices() const { return SectionModelIndices; }
    std::vector<uint32_t>& GetSectionModelIndices() { return SectionModelIndices; }

protected:
    std::string Name;
    uint32_t ObjectId = 0;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    std::vector<uint32_t> SectionModelIndices;

    // Link back to the source glTF data (shared with FSceneModelResource).
    int GltfSceneIndex = -1;
    int GltfNodeIndex = -1;
    int GltfMeshIndex = -1;

public:
    int GetGltfSceneIndex() const { return GltfSceneIndex; }
    int GetGltfNodeIndex() const { return GltfNodeIndex; }
    int GetGltfMeshIndex() const { return GltfMeshIndex; }
    void SetGltfIndices(int SceneIndex, int NodeIndex, int MeshIndex)
    {
        GltfSceneIndex = SceneIndex;
        GltfNodeIndex = NodeIndex;
        GltfMeshIndex = MeshIndex;
    }
};
