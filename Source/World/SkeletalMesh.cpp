#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SkeletalMesh.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <DirectXMath.h>

#include "../Scene/GltfLoader.h"

FMeshMaterial* FSkeletalMesh::GetMaterial(size_t SectionIndex)
{
    if (SectionIndex >= Sections.size())
    {
        return nullptr;
    }

    return &Sections[SectionIndex].Material;
}

const FMeshMaterial* FSkeletalMesh::GetMaterial(size_t SectionIndex) const
{
    if (SectionIndex >= Sections.size())
    {
        return nullptr;
    }

    return &Sections[SectionIndex].Material;
}

void FSkeletalMesh::Tick(float DeltaTime, FWorldTickContext& Ctx)
{
    (void)DeltaTime;
    if (Ctx.AnimationRuntimes == nullptr)
    {
        return;
    }
    if (GltfSceneIndex < 0 || GltfNodeIndex < 0 || GltfSkinIndex < 0)
    {
        return;
    }

    const std::vector<FGltfAnimationRuntime>& Runtimes = *Ctx.AnimationRuntimes;
    const size_t SceneIndex = static_cast<size_t>(GltfSceneIndex);
    if (SceneIndex >= Runtimes.size())
    {
        return;
    }

    const FGltfAnimationRuntime& Runtime = Runtimes[SceneIndex];
    const size_t NodeIndex = static_cast<size_t>(GltfNodeIndex);
    if (NodeIndex >= Runtime.Pose.WorldMatrices.size())
    {
        return;
    }

    const size_t SkinIndex = static_cast<size_t>(GltfSkinIndex);
    if (SkinIndex >= Runtime.Skins.size() || SkinIndex >= Runtime.Pose.SkinMatrices.size())
    {
        return;
    }

    const std::vector<DirectX::XMFLOAT4X4>& SkinMatrices = Runtime.Pose.SkinMatrices[SkinIndex];

    using namespace DirectX;
    const XMMATRIX NodeWorld = XMLoadFloat4x4(&Runtime.Pose.WorldMatrices[NodeIndex]);
    const XMMATRIX NodeWorldInv = XMMatrixInverse(nullptr, NodeWorld);

    bool bAnyUpdated = false;
    for (FMeshSection& Section : Sections)
    {
        if (Section.BoneMatrixBufferMapped == nullptr)
        {
            continue;
        }

        const size_t MatrixCount = std::min(SkinMatrices.size(), static_cast<size_t>(Section.BoneMatrixCount));

        std::vector<DirectX::XMFLOAT4X4> FinalMatrices(MatrixCount);
        for (size_t JointIndex = 0; JointIndex < MatrixCount; ++JointIndex)
        {
            const XMMATRIX SkinMatrix = XMLoadFloat4x4(&SkinMatrices[JointIndex]);
            const XMMATRIX FinalMatrix = XMMatrixMultiply(SkinMatrix, NodeWorldInv); // into the node's local space
            XMStoreFloat4x4(&FinalMatrices[JointIndex], FinalMatrix);
        }

        const size_t CopyBytes = MatrixCount * sizeof(DirectX::XMFLOAT4X4);
        std::memcpy(Section.BoneMatrixBufferMapped, FinalMatrices.data(), CopyBytes);

        if (Ctx.bAnimationTimeAdvanced && !Runtime.Animations.empty() && MatrixCount > 0)
        {
            Section.bSkinningUpdatedThisFrame = true;
            bAnyUpdated = true;
        }
    }

    if (bAnyUpdated)
    {
        Ctx.bAnySkinningUpdated = true;
    }
}
