#include "MeshMaterial.h"

#include <string>

#include <d3dx12.h>

#include "../Render/TextureLoader.h"
#include "../RHI/DX12Device.h"

namespace
{
    // Describes one of the material's texture slots: where the source path and
    // GPU handle live (as member pointers), whether it is sRGB, and a debug name.
    struct FTextureSlot
    {
        std::wstring FMeshMaterial::* Path;
        FBindlessTexture FMeshMaterial::* Texture;
        bool bSRGB;
        const wchar_t* DebugName;
    };

    const FTextureSlot kTextureSlots[] =
    {
        { &FMeshMaterial::BaseColorTexturePath,         &FMeshMaterial::BaseColor,         true,  L"BaseColorTexture" },
        { &FMeshMaterial::MetallicRoughnessTexturePath, &FMeshMaterial::MetallicRoughness, false, L"MetallicRoughnessTexture" },
        { &FMeshMaterial::NormalTexturePath,            &FMeshMaterial::Normal,            false, L"NormalTexture" },
        { &FMeshMaterial::EmissiveTexturePath,          &FMeshMaterial::Emissive,          true,  L"EmissiveTexture" },
        { &FMeshMaterial::SheenColorTexturePath,        &FMeshMaterial::SheenColor,        true,  L"SheenColorTexture" },
        { &FMeshMaterial::SheenRoughnessTexturePath,    &FMeshMaterial::SheenRoughness,    false, L"SheenRoughnessTexture" },
        { &FMeshMaterial::ClearcoatTexturePath,         &FMeshMaterial::Clearcoat,         false, L"ClearcoatTexture" },
        { &FMeshMaterial::ClearcoatRoughnessTexturePath,&FMeshMaterial::ClearcoatRoughness,false, L"ClearcoatRoughnessTexture" },
        { &FMeshMaterial::ClearcoatNormalTexturePath,   &FMeshMaterial::ClearcoatNormal,   false, L"ClearcoatNormalTexture" },
        { &FMeshMaterial::AnisotropyTexturePath,        &FMeshMaterial::Anisotropy,        false, L"AnisotropyTexture" },
    };
}

void FMeshMaterial::AppendTextureLoadRequests(std::vector<FTextureLoadRequest>& OutRequests)
{
    for (const FTextureSlot& Slot : kTextureSlots)
    {
        const std::wstring& Path = this->*Slot.Path;
        if (Path.empty())
        {
            continue;
        }

        FTextureLoadRequest Request;
        Request.Path = Path;
        Request.bUseSolidColor = false;
        Request.bUseSRGB = Slot.bSRGB;
        Request.OutTexture = &((this->*Slot.Texture).Resource);
        OutRequests.push_back(Request);
    }
}

void FMeshMaterial::CreateTextureSrvs(FDX12Device* Device, int DebugIndex)
{
    for (const FTextureSlot& Slot : kTextureSlots)
    {
        FBindlessTexture& Texture = this->*Slot.Texture;
        if (!Texture.Get())
        {
            continue;
        }

        if (DebugIndex >= 0)
        {
            const std::wstring Name = std::wstring(Slot.DebugName) + L"_" + std::to_wstring(DebugIndex);
            Texture->SetName(Name.c_str());
        }

        const D3D12_RESOURCE_DESC Desc = Texture->GetDesc();
        Texture.SrvBindlessIndex = Device->CreateBindlessSrv(
            Texture.Get(),
            CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(Desc.Format, Desc.MipLevels));
    }
}
