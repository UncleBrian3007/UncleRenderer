#include "DebugPrintFont.h"

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

    D3D12_RESOURCE_DESC TextureDesc = {};
    TextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TextureDesc.Width = AtlasWidth;
    TextureDesc.Height = AtlasHeight;
    TextureDesc.DepthOrArraySize = 1;
    TextureDesc.MipLevels = 1;
    TextureDesc.Format = DXGI_FORMAT_R8_UNORM;
    TextureDesc.SampleDesc.Count = 1;
    TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    ComPtr<ID3D12Resource> FontTexture;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &TextureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(FontTexture.GetAddressOf())));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    UploadHeap.CreationNodeMask = 1;
    UploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC UploadDesc = {};
    UploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    UploadDesc.Width = UploadBufferSize;
    UploadDesc.Height = 1;
    UploadDesc.DepthOrArraySize = 1;
    UploadDesc.MipLevels = 1;
    UploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    UploadDesc.SampleDesc.Count = 1;
    UploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> UploadResource;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(UploadResource.GetAddressOf())));

    uint8_t* Mapped = nullptr;
    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&Mapped)));
    for (UINT Row = 0; Row < NumRows; ++Row)
    {
        const uint8_t* SrcRow = Bitmap.data() + Row * AtlasWidth;
        memcpy(Mapped + Layout.Offset + Row * Layout.Footprint.RowPitch, SrcRow, AtlasWidth);
    }
    UploadResource->Unmap(0, nullptr);

    const uint64_t GlyphBufferSize = sizeof(FDebugPrintGlyph) * Glyphs.size();
    D3D12_RESOURCE_DESC GlyphDesc = {};
    GlyphDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    GlyphDesc.Width = GlyphBufferSize;
    GlyphDesc.Height = 1;
    GlyphDesc.DepthOrArraySize = 1;
    GlyphDesc.MipLevels = 1;
    GlyphDesc.Format = DXGI_FORMAT_UNKNOWN;
    GlyphDesc.SampleDesc.Count = 1;
    GlyphDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> GlyphBuffer;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &GlyphDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(GlyphBuffer.GetAddressOf())));

    D3D12_RESOURCE_DESC GlyphUploadDesc = GlyphDesc;
    ComPtr<ID3D12Resource> GlyphUpload;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &GlyphUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(GlyphUpload.GetAddressOf())));

    uint8_t* GlyphMapped = nullptr;
    HR_CHECK(GlyphUpload->Map(0, &EmptyRange, reinterpret_cast<void**>(&GlyphMapped)));
    memcpy(GlyphMapped, Glyphs.data(), GlyphBufferSize);
    GlyphUpload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = FontTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadResource.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);
    UploadList->CopyBufferRegion(GlyphBuffer.Get(), 0, GlyphUpload.Get(), 0, GlyphBufferSize);

    D3D12_RESOURCE_BARRIER Barriers[2] = {};
    Barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barriers[0].Transition.pResource = FontTexture.Get();
    Barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barriers[1].Transition.pResource = GlyphBuffer.Get();
    Barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(2, Barriers);

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    if (FontTexture)
    {
        FontTexture->SetName(L"DebugPrintFontAtlas");
    }
    if (GlyphBuffer)
    {
        GlyphBuffer->SetName(L"DebugPrintGlyphBuffer");
    }

    OutResources.FontTexture = FontTexture;
    OutResources.GlyphBuffer = GlyphBuffer;
    OutResources.AtlasWidth = AtlasWidth;
    OutResources.AtlasHeight = AtlasHeight;
    OutResources.FirstChar = FirstChar;
    OutResources.CharCount = CharCount;
    OutResources.FontSize = FontSize;

    return true;
}
