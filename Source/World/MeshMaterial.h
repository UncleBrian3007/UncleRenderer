#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "../Render/GpuResource.h"

class FDX12Device;
struct FTextureLoadRequest;

// Alpha blending mode for a material. Mirrors the glTF alphaMode semantics.
enum class EAlphaMode : uint32_t
{
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

// Packed UV transform (KHR_texture_transform) ready for shader consumption.
struct FTextureTransform
{
    DirectX::XMFLOAT4 OffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT2 Rotation{ 1.0f, 0.0f };
};

// Material parameters and texture bindings for a single mesh section.
// Extracted from FMeshSection so meshes can own/share materials
struct FMeshMaterial
{
    // PBR / shading factors.
    DirectX::XMFLOAT3 BaseColorFactor{ 1.0f, 1.0f, 1.0f };
    float BaseColorAlpha = 1.0f;
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    DirectX::XMFLOAT3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 SheenColorFactor{ 0.0f, 0.0f, 0.0f };
    float SheenRoughnessFactor = 0.0f;
    float ClearcoatFactor = 0.0f;
    float ClearcoatRoughnessFactor = 0.0f;
    float AnisotropyStrength = 0.0f;
    float AnisotropyRotation = 0.0f;
    float AlphaCutoff = 0.5f;
    uint32_t AlphaMode = 0;
    bool bDoubleSided = false;
    uint32_t ShadingModelId = 0;
    bool bHasNormalMap = true;

    // Source texture file paths.
    std::wstring BaseColorTexturePath;
    std::wstring MetallicRoughnessTexturePath;
    std::wstring NormalTexturePath;
    std::wstring EmissiveTexturePath;
    std::wstring SheenColorTexturePath;
    std::wstring SheenRoughnessTexturePath;
    std::wstring ClearcoatTexturePath;
    std::wstring ClearcoatRoughnessTexturePath;
    std::wstring ClearcoatNormalTexturePath;
    std::wstring AnisotropyTexturePath;

    // Loaded bindless GPU textures.
    FBindlessTexture BaseColor;
    FBindlessTexture MetallicRoughness;
    FBindlessTexture Normal;
    FBindlessTexture Emissive;
    FBindlessTexture SheenColor;
    FBindlessTexture SheenRoughness;
    FBindlessTexture Clearcoat;
    FBindlessTexture ClearcoatRoughness;
    FBindlessTexture ClearcoatNormal;
    FBindlessTexture Anisotropy;

    // Per-texture UV transforms.
    FTextureTransform BaseColorTransform;
    FTextureTransform MetallicRoughnessTransform;
    FTextureTransform NormalTransform;
    FTextureTransform EmissiveTransform;
    FTextureTransform SheenColorTransform;
    FTextureTransform SheenRoughnessTransform;
    FTextureTransform ClearcoatTransform;
    FTextureTransform ClearcoatRoughnessTransform;
    FTextureTransform ClearcoatNormalTransform;
    FTextureTransform AnisotropyTransform;

    // Append a parallel-load request for each non-empty texture path, with the
    // correct sRGB flag, targeting this material's own texture handles.
    void AppendTextureLoadRequests(std::vector<FTextureLoadRequest>& OutRequests);

    // Create bindless SRVs for every loaded texture. If DebugIndex >= 0, each
    // texture is also given a debug name suffixed with that index.
    void CreateTextureSrvs(FDX12Device* Device, int DebugIndex = -1);
};
