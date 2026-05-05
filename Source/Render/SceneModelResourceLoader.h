#pragma once

#include <string>
#include <vector>
#include <DirectXMath.h>
#include "SceneModelResource.h"
#include "../Scene/GltfAnimation.h"

class FDX12Device;

namespace SceneModelResourceLoader
{
    bool LoadModelsFromJson(
        FDX12Device* Device,
        const std::wstring& SceneFilePath,
        std::vector<FSceneModelResource>& OutModels,
        DirectX::XMFLOAT3& OutSceneCenter,
        float& OutSceneRadius,
        std::vector<FGltfScene>* OutGltfScenes = nullptr);
}
