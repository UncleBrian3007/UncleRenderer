#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "MeshSection.h"

// Kind of world object. Used instead of RTTI to branch on concrete type.
enum class EObjectType : uint32_t
{
    StaticMesh,
    SkeletalMesh
};

struct FObjectBounds
{
    DirectX::XMFLOAT3 Min{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Max{ 0.0f, 0.0f, 0.0f };
};

// Base class for everything the World owns. Renderable primitive sections live
// directly on the object, so render passes can consume object-owned data.
class FObject
{
public:
    virtual ~FObject() = default;

    virtual EObjectType GetType() const = 0;
    virtual void Tick(float DeltaTime) { (void)DeltaTime; }

    const std::string& GetName() const { return Name; }
    void SetName(const std::string& InName) { Name = InName; }

    uint32_t GetObjectId() const { return ObjectId; }
    void SetObjectId(uint32_t InObjectId) { ObjectId = InObjectId; }

    const DirectX::XMFLOAT4X4& GetWorldMatrix() const { return WorldMatrix; }
    void SetWorldMatrix(const DirectX::XMFLOAT4X4& InMatrix) { WorldMatrix = InMatrix; }
    const DirectX::XMFLOAT4X4& GetPreviousWorldMatrix() const { return PreviousWorldMatrix; }
    void SetPreviousWorldMatrix(const DirectX::XMFLOAT4X4& InMatrix)
    {
        PreviousWorldMatrix = InMatrix;
        bHasPreviousWorldMatrix = true;
    }
    bool HasPreviousWorldMatrix() const { return bHasPreviousWorldMatrix; }
    void ResetPreviousWorldMatrix() { bHasPreviousWorldMatrix = false; }

    const FObjectBounds& GetBounds() const { return Bounds; }
    void SetBounds(const DirectX::XMFLOAT3& InMin, const DirectX::XMFLOAT3& InMax)
    {
        Bounds.Min = InMin;
        Bounds.Max = InMax;
    }

    const std::vector<FMeshSection>& GetSections() const { return Sections; }
    std::vector<FMeshSection>& GetSections() { return Sections; }

protected:
    std::string Name;
    uint32_t ObjectId = 0;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    DirectX::XMFLOAT4X4 PreviousWorldMatrix{};
    bool bHasPreviousWorldMatrix = false;
    FObjectBounds Bounds{};
    std::vector<FMeshSection> Sections;

    // Link back to the source glTF data.
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
