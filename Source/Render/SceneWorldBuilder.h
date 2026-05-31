#pragma once

#include <string>
#include <DirectXMath.h>

class FDX12Device;
class FWorld;

namespace SceneWorldBuilder
{
    bool LoadWorldFromSceneFile(
        FDX12Device* Device,
        const std::wstring& SceneFilePath,
        FWorld& OutWorld,
        DirectX::XMFLOAT3& OutSceneCenter,
        float& OutSceneRadius);
}
