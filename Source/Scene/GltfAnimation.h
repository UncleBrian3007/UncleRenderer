#pragma once

#include <vector>
#include <DirectXMath.h>

#include "GltfLoader.h"

struct FGltfAnimationPose
{
    std::vector<DirectX::XMFLOAT4X4> LocalMatrices;
    std::vector<DirectX::XMFLOAT4X4> WorldMatrices;
    std::vector<std::vector<DirectX::XMFLOAT4X4>> SkinMatrices;
};

void InitializeGltfAnimationPose(const FGltfScene& Scene, FGltfAnimationPose& OutPose);
void UpdateGltfAnimationPose(const FGltfScene& Scene, float TimeSeconds, FGltfAnimationPose& InOutPose);
