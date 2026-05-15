#include "DebugPrintFont.h"
#include "GpuResource.h"

#include "../RHI/DX12Device.h"
#include "../RHI/DX12Commons.h"
#include "../Core/Logger.h"

#include <vector>
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../ThirdParty/stb/stb_truetype.h"

using Microsoft::WRL::ComPtr;

namespace
{
    bool LoadFileBytes(const std::wstring& Path, std::vector<uint8_t>& OutData)
    {
        std::ifstream File(Path, std::ios::binary | std::ios::ate);
        if (!File)
        {
            return false;
        }

        const std::streamsize Size = File.tellg();
        File.seekg(0, std::ios::beg);
        OutData.resize(static_cast<size_t>(Size));
        if (!File.read(reinterpret_cast<char*>(OutData.data()), Size))
        {
            OutData.clear();
            return false;
        }
        return true;
    }
}

bool CreateDebugPrintFontResources(
    FDX12Device* Device,
    const std::wstring& FontPath,
    float FontSize,
    uint32_t AtlasWidth,
    uint32_t AtlasHeight,
    FDebugPrintFontResources& OutResources)
{
    if (!Device || AtlasWidth == 0 || AtlasHeight == 0)
    {
        return false;
    }

    std::vector<uint8_t> FontData;
    if (!LoadFileBytes(FontPath, FontData))
    {
        LogError("Failed to load debug font file.");
        return false;
    }

    const uint32_t FirstChar = 32;
    const uint32_t CharCount = 96;

    std::vector<unsigned char> Bitmap(AtlasWidth * AtlasHeight, 0);
    std::vector<stbtt_bakedchar> Baked(CharCount);

    const int Result = stbtt_BakeFontBitmap(
        FontData.data(),
        0,
        FontSize,
        Bitmap.data(),
        static_cast<int>(AtlasWidth),
        static_cast<int>(AtlasHeight),
        static_cast<int>(FirstChar),
        static_cast<int>(CharCount),
        Baked.data());
    if (Result <= 0)
    {
        LogError("Failed to bake debug font bitmap.");
        return false;
    }

    std::vector<FDebugPrintGlyph> Glyphs(128);
    for (uint32_t Index = 0; Index < CharCount; ++Index)
    {
        const stbtt_bakedchar& Src = Baked[Index];
        FDebugPrintGlyph& Glyph = Glyphs[Index + FirstChar];
        Glyph.UvMin[0] = Src.x0 / static_cast<float>(AtlasWidth);
        Glyph.UvMin[1] = Src.y0 / static_cast<float>(AtlasHeight);
        Glyph.UvMax[0] = Src.x1 / static_cast<float>(AtlasWidth);
        Glyph.UvMax[1] = Src.y1 / static_cast<float>(AtlasHeight);
        Glyph.Size[0] = static_cast<float>(Src.x1 - Src.x0);
        Glyph.Size[1] = static_cast<float>(Src.y1 - Src.y0);
        Glyph.Offset[0] = Src.xoff;
        Glyph.Offset[1] = Src.yoff;
        Glyph.Advance = Src.xadvance;
    }

    // Font texture
    const FRGTextureDesc AtlasDesc = { AtlasWidth, AtlasHeight, DXGI_FORMAT_R8_UNORM };
    FBindlessTexture FontTexture;
    CreateBindlessTexture(Device, L"DebugPrintFontAtlas", AtlasDesc, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COMMON, FontTexture, false, false);

    // Atlas upload buffer - texture row pitch requires manual row copy
    const D3D12_RESOURCE_DESC AtlasD3DDesc = CreateTextureResourceDesc(AtlasDesc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&AtlasD3DDesc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    FUploadBuffer AtlasUpload;
    CreateUploadBuffer(Device, L"DebugPrintFontAtlasUpload", UploadBufferSize, AtlasUpload, nullptr);
    {
        uint8_t* Mapped = nullptr;
        const D3D12_RANGE EmptyRange = { 0, 0 };
        HR_CHECK(AtlasUpload->Map(0, &EmptyRange, reinterpret_cast<void**>(&Mapped)));
        for (UINT Row = 0; Row < NumRows; ++Row)
        {
            memcpy(Mapped + Layout.Offset + Row * Layout.Footprint.RowPitch,
                Bitmap.data() + Row * AtlasWidth, AtlasWidth);
        }
        AtlasUpload->Unmap(0, nullptr);
    }

    // Glyph buffer + upload
    FBindlessBuffer GlyphBuffer;
    FUploadBuffer GlyphUpload;
    CreateBindlessBufferWithUpload(
        Device,
        L"DebugPrintGlyphBuffer",
        CreateStructuredBufferDesc<FDebugPrintGlyph>(Glyphs.size()),
        D3D12_RESOURCE_STATE_COMMON,
        GlyphBuffer,
        GlyphUpload,
        Glyphs.data(),
        false,
        false);

    // GPU upload
    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"DebugPrintFont_Upload_CL");

    const D3D12_RESOURCE_BARRIER PreCopyBarriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(FontTexture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
        CD3DX12_RESOURCE_BARRIER::Transition(GlyphBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    UploadList->ResourceBarrier(_countof(PreCopyBarriers), PreCopyBarriers);

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = FontTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = AtlasUpload.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);
    UploadList->CopyBufferRegion(GlyphBuffer.Get(), 0, GlyphUpload.Get(), 0, GlyphBuffer.Desc.Size);

    const D3D12_RESOURCE_BARRIER PostCopyBarriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(FontTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(GlyphBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    UploadList->ResourceBarrier(_countof(PostCopyBarriers), PostCopyBarriers);

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    CreateBindlessTextureSrv(Device, FontTexture);
    CreateBindlessBufferSrv(Device, GlyphBuffer);

    OutResources.FontTexture = FontTexture;
    OutResources.GlyphBuffer = GlyphBuffer;
    OutResources.AtlasWidth = AtlasWidth;
    OutResources.AtlasHeight = AtlasHeight;
    OutResources.FirstChar = FirstChar;
    OutResources.CharCount = CharCount;
    OutResources.FontSize = FontSize;

    return true;
}
