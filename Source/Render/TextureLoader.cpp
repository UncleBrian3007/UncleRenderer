#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TextureLoader.h"

#include "RendererUtils.h"
#include "../RHI/DX12Commons.h"
#include "../RHI/DX12Device.h"
#include "../Core/TaskSystem.h"
#include "../Core/Logger.h"
#include <array>
#include <vector>
#include <filesystem>
#include <mutex>
#include <chrono>
#include <fstream>
#include <cctype>
#include <cwctype>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include "../../ThirdParty/stb/stb_image.h"
#include "../../ThirdParty/ddspp/ddspp.h"

using Microsoft::WRL::ComPtr;

namespace
{
    const std::wstring GDefaultGridCacheKey = L"__default_grid_texture__";

    std::wstring BuildCacheKey(const std::wstring& BaseKey, bool bUseSRGB)
    {
        if (!bUseSRGB)
        {
            return BaseKey;
        }

        return BaseKey + L"|srgb";
    }

    DXGI_FORMAT MakeSRGBFormat(DXGI_FORMAT Format)
    {
        switch (Format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        case DXGI_FORMAT_BC1_UNORM:
            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_UNORM:
            return DXGI_FORMAT_BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_UNORM:
            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC7_UNORM:
            return DXGI_FORMAT_BC7_UNORM_SRGB;
        default:
            return Format;
        }
    }
}

std::unordered_map<std::wstring, ComPtr<ID3D12Resource>> FTextureLoader::GlobalTextureCache;
static std::mutex GTextureCacheMutex;

FTextureLoader::FTextureLoader(FDX12Device* InDevice)
    : Device(InDevice)
{
}

bool FTextureLoader::LoadOrDefault(const std::wstring& TexturePath, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload, bool bUseSRGB)
{
    const std::wstring CacheKey = BuildCacheKey(TexturePath, bUseSRGB);
    if (TryGetCachedTexture(CacheKey, OutTexture))
    {
        return true;
    }

    if (!TexturePath.empty() && LoadTextureInternal(TexturePath, OutTexture, RecordedUpload, bUseSRGB))
    {
        std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
        GlobalTextureCache[CacheKey] = OutTexture;
        return true;
    }

    const std::wstring DefaultCacheKey = BuildCacheKey(GDefaultGridCacheKey, bUseSRGB);
    if (TryGetCachedTexture(DefaultCacheKey, OutTexture))
    {
        return true;
    }

    if (CreateDefaultGridTexture(OutTexture, RecordedUpload, bUseSRGB))
    {
        std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
        GlobalTextureCache[DefaultCacheKey] = OutTexture;
        return true;
    }

    return false;
}

bool FTextureLoader::LoadOrSolidColor(const std::wstring& TexturePath, uint32_t Color, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload, bool bUseSRGB)
{
    const std::wstring CacheKey = BuildCacheKey(TexturePath, bUseSRGB);
    if (TryGetCachedTexture(CacheKey, OutTexture))
    {
        return true;
    }

    if (!TexturePath.empty() && LoadTextureInternal(TexturePath, OutTexture, RecordedUpload, bUseSRGB))
    {
        std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
        GlobalTextureCache[CacheKey] = OutTexture;
        return true;
    }

    const std::wstring SolidColorKey = BuildCacheKey(L"__solid_color_" + std::to_wstring(static_cast<uint64_t>(Color)), bUseSRGB);
    if (TryGetCachedTexture(SolidColorKey, OutTexture))
    {
        return true;
    }

    if (!CreateSolidColorTexture(Color, OutTexture, RecordedUpload, bUseSRGB))
    {
        return false;
    }

    std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
    GlobalTextureCache[SolidColorKey] = OutTexture;
    return true;
}



bool FTextureLoader::LoadHdrTexture(const std::wstring& TexturePath, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload)
{
    if (Device == nullptr || TexturePath.empty())
    {
        return false;
    }

    int Width = 0;
    int Height = 0;
    int Channels = 0;
    const std::string NarrowPath = std::filesystem::path(TexturePath).string();
    float* Pixels = stbi_loadf(NarrowPath.c_str(), &Width, &Height, &Channels, 3);
    if (!Pixels || Width <= 0 || Height <= 0)
    {
        if (Pixels)
        {
            stbi_image_free(Pixels);
        }
        return false;
    }

    const size_t PixelCount = static_cast<size_t>(Width) * static_cast<size_t>(Height);
    std::vector<float> TextureData(PixelCount * 4u, 1.0f);
    for (size_t Index = 0; Index < PixelCount; ++Index)
    {
        TextureData[Index * 4u + 0u] = Pixels[Index * 3u + 0u];
        TextureData[Index * 4u + 1u] = Pixels[Index * 3u + 1u];
        TextureData[Index * 4u + 2u] = Pixels[Index * 3u + 2u];
    }
    stbi_image_free(Pixels);

    const FRGTextureDesc TextureRGDesc = { static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), DXGI_FORMAT_R32G32B32A32_FLOAT };
    FBindlessTexture TextureWrapper;
    CreateBindlessTexture(Device, std::filesystem::path(TexturePath).filename().wstring(),
        TextureRGDesc, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, TextureWrapper, false, false);
    OutTexture = TextureWrapper.Resource;

    const D3D12_RESOURCE_DESC TextureDesc = CreateTextureResourceDesc(TextureRGDesc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    FUploadBuffer UploadBuffer;
    CreateUploadBuffer(Device, L"", UploadBufferSize, UploadBuffer, nullptr);

    uint8_t* MappedData = nullptr;
    const D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadBuffer->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    const size_t SrcRowPitch = static_cast<size_t>(Width) * sizeof(float) * 4u;
    for (UINT Row = 0; Row < NumRows; ++Row)
    {
        const uint8_t* SrcRow = reinterpret_cast<const uint8_t*>(TextureData.data()) + static_cast<size_t>(Row) * SrcRowPitch;
        memcpy(MappedData + Layout.Offset + static_cast<size_t>(Row) * Layout.Footprint.RowPitch, SrcRow, SrcRowPitch);
    }

    UploadBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"TextureLoader_HDRUpload_CL");

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadBuffer.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(OutTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    if (RecordedUpload)
    {
        RecordedUpload->UploadResource = UploadBuffer.Resource;
        RecordedUpload->CommandAllocator = UploadAllocator;
        RecordedUpload->CommandList = UploadList;
    }
    else
    {
        ID3D12CommandList* Lists[] = { UploadList.Get() };
        Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
        Device->GetGraphicsQueue()->Flush();
    }

    return true;
}
void FTextureLoader::ClearCache()
{
    std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
    GlobalTextureCache.clear();
}

bool FTextureLoader::TryGetCachedTexture(const std::wstring& TexturePath, ComPtr<ID3D12Resource>& OutTexture) const
{
    if (TexturePath.empty())
    {
        return false;
    }

    std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
    const auto It = GlobalTextureCache.find(TexturePath);
    if (It != GlobalTextureCache.end() && It->second)
    {
        OutTexture = It->second;
        return true;
    }

    return false;
}

bool FTextureLoader::LoadTextureInternal(const std::wstring& FilePath, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload, bool bUseSRGB)
{
    if (Device == nullptr || FilePath.empty())
    {
        return false;
    }

    const auto HasDDSExtension = [](const std::wstring& Path)
    {
        const std::filesystem::path Extension = std::filesystem::path(Path).extension();
        std::wstring LowerExt = Extension.wstring();
        for (wchar_t& Char : LowerExt)
        {
            Char = static_cast<wchar_t>(std::towlower(Char));
        }
        return LowerExt == L".dds";
    };

    if (HasDDSExtension(FilePath))
    {
        std::ifstream FileStream(FilePath, std::ios::binary | std::ios::ate);
        if (!FileStream)
        {
            return false;
        }

        const std::streamsize FileSize = FileStream.tellg();
        FileStream.seekg(0, std::ios::beg);

        std::vector<uint8_t> FileData(static_cast<size_t>(FileSize));
        if (!FileStream.read(reinterpret_cast<char*>(FileData.data()), FileSize))
        {
            return false;
        }

        ddspp::Descriptor Descriptor = {};
        if (FileData.size() < sizeof(uint32_t) || ddspp::decode_header(FileData.data(), Descriptor) != ddspp::Result::Success)
        {
            return false;
        }

        if (Descriptor.format == ddspp::DXGIFormat::UNKNOWN)
        {
            return false;
        }

        const bool bIsCubemap = Descriptor.type == ddspp::Cubemap;
        const uint32_t ArraySize = Descriptor.type == ddspp::Texture3D ? 1u : std::max(1u, Descriptor.arraySize);
        const uint32_t SliceCount = bIsCubemap ? ArraySize * 6u : ArraySize;
        const uint32_t Depth = Descriptor.type == ddspp::Texture3D ? std::max(1u, Descriptor.depth) : 1u;
        const UINT SubresourceCount = Descriptor.numMips * SliceCount;
        const DXGI_FORMAT BaseFormat = static_cast<DXGI_FORMAT>(Descriptor.format);
        const DXGI_FORMAT Format = bUseSRGB ? MakeSRGBFormat(BaseFormat) : BaseFormat;

        const FRGTextureDesc TextureRGDesc = {
            Descriptor.width,
            Descriptor.height,
            Format,
            static_cast<uint16>(Descriptor.numMips),
            static_cast<uint16>(Descriptor.type == ddspp::Texture3D ? Depth : SliceCount),
            Descriptor.type == ddspp::Texture3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D
        };
        FBindlessTexture TextureWrapper;
        CreateBindlessTexture(Device, std::filesystem::path(FilePath).filename().wstring(),
            TextureRGDesc, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, TextureWrapper, false, false);
        OutTexture = TextureWrapper.Resource;

        const D3D12_RESOURCE_DESC TextureDesc = CreateTextureResourceDesc(TextureRGDesc);
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Layouts(SubresourceCount);
        std::vector<UINT> NumRows(SubresourceCount);
        UINT64 UploadBufferSize = 0;
        Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, SubresourceCount, 0, Layouts.data(), NumRows.data(), nullptr, &UploadBufferSize);

        FUploadBuffer UploadBuffer;
        CreateUploadBuffer(Device, L"", UploadBufferSize, UploadBuffer, nullptr);

        uint8_t* MappedData = nullptr;
        const D3D12_RANGE EmptyRange = { 0, 0 };
        HR_CHECK(UploadBuffer->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

        size_t DataOffset = Descriptor.headerSize;
        for (uint32_t ArrayIndex = 0; ArrayIndex < SliceCount; ++ArrayIndex)
        {
            for (uint32_t Mip = 0; Mip < Descriptor.numMips; ++Mip)
            {
                const uint32_t SubresourceIndex = ArrayIndex * Descriptor.numMips + Mip;
                const uint32_t MipWidth = std::max(1u, Descriptor.width >> Mip);
                const uint32_t MipHeight = std::max(1u, Descriptor.height >> Mip);
                const uint32_t MipDepth = Descriptor.type == ddspp::Texture3D ? std::max(1u, Descriptor.depth >> Mip) : 1u;
                const uint32_t BlockWidth = std::max(1u, Descriptor.blockWidth);
                const uint32_t BlockHeight = std::max(1u, Descriptor.blockHeight);
                const uint32_t BlocksWide = Descriptor.compressed ? (MipWidth + BlockWidth - 1) / BlockWidth : MipWidth;
                const uint32_t BlocksHigh = Descriptor.compressed ? (MipHeight + BlockHeight - 1) / BlockHeight : MipHeight;
                const uint64_t SrcRowPitch = BlocksWide * Descriptor.bitsPerPixelOrBlock / 8;
                const size_t SliceSize = static_cast<size_t>(SrcRowPitch) * BlocksHigh;
                const size_t SubresourceSize = SliceSize * MipDepth;

                if (DataOffset + SubresourceSize > FileData.size())
                {
                    UploadBuffer->Unmap(0, nullptr);
                    return false;
                }

                uint8_t* DstSubresource = MappedData + Layouts[SubresourceIndex].Offset;
                const uint8_t* SrcSubresource = FileData.data() + DataOffset;

                for (uint32_t Z = 0; Z < MipDepth; ++Z)
                {
                    const uint8_t* SrcSlice = SrcSubresource + SliceSize * Z;
                    uint8_t* DstSlice = DstSubresource + Layouts[SubresourceIndex].Footprint.RowPitch * NumRows[SubresourceIndex] * Z;

                    for (uint32_t Row = 0; Row < BlocksHigh; ++Row)
                    {
                        memcpy(DstSlice + static_cast<size_t>(Row) * Layouts[SubresourceIndex].Footprint.RowPitch, SrcSlice + static_cast<size_t>(Row) * SrcRowPitch, SrcRowPitch);
                    }
                }

                DataOffset += SubresourceSize;
            }
        }

        UploadBuffer->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> UploadAllocator;
        ComPtr<ID3D12GraphicsCommandList> UploadList;
        HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
        HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
        UploadList->SetName(L"TextureLoader_DDSUpload_CL");

        for (UINT Subresource = 0; Subresource < SubresourceCount; ++Subresource)
        {
            D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
            DstLocation.pResource = OutTexture.Get();
            DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            DstLocation.SubresourceIndex = Subresource;

            D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
            SrcLocation.pResource = UploadBuffer.Get();
            SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            SrcLocation.PlacedFootprint = Layouts[Subresource];

            UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);
        }

        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(OutTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        UploadList->ResourceBarrier(1, &Barrier);

        HR_CHECK(UploadList->Close());

        if (RecordedUpload)
        {
            RecordedUpload->UploadResource = UploadBuffer.Resource;
            RecordedUpload->CommandAllocator = UploadAllocator;
            RecordedUpload->CommandList = UploadList;
        }
        else
        {
            ID3D12CommandList* Lists[] = { UploadList.Get() };
            Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
            Device->GetGraphicsQueue()->Flush();
        }

        return true;
    }

    int Width = 0;
    int Height = 0;
    int Channels = 0;
    const std::string NarrowPath = std::filesystem::path(FilePath).string();
    stbi_uc* Pixels = stbi_load(NarrowPath.c_str(), &Width, &Height, &Channels, STBI_rgb_alpha);
    if (!Pixels || Width <= 0 || Height <= 0)
    {
        if (Pixels)
        {
            stbi_image_free(Pixels);
        }
        return false;
    }

    const UINT64 PixelDataSize = static_cast<UINT64>(Width) * static_cast<UINT64>(Height) * 4ULL;
    std::vector<uint8_t> TextureData(Pixels, Pixels + static_cast<size_t>(PixelDataSize));
    stbi_image_free(Pixels);

    const DXGI_FORMAT TexFormat = bUseSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    const FRGTextureDesc TextureRGDesc = { static_cast<uint32_t>(Width), static_cast<uint32_t>(Height), TexFormat };
    FBindlessTexture TextureWrapper;
    CreateBindlessTexture(Device, std::filesystem::path(FilePath).filename().wstring(),
        TextureRGDesc, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, TextureWrapper, false, false);
    OutTexture = TextureWrapper.Resource;

    const D3D12_RESOURCE_DESC TextureDesc = CreateTextureResourceDesc(TextureRGDesc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    FUploadBuffer UploadBuffer;
    CreateUploadBuffer(Device, L"", UploadBufferSize, UploadBuffer, nullptr);

    uint8_t* MappedData = nullptr;
    const D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadBuffer->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    for (UINT Row = 0; Row < NumRows; ++Row)
    {
        const uint8_t* SrcRow = TextureData.data() + Row * static_cast<UINT64>(Width) * 4ULL;
        memcpy(MappedData + Layout.Offset + Row * Layout.Footprint.RowPitch, SrcRow, Width * 4ULL);
    }

    UploadBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"TextureLoader_WICUpload_CL");

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadBuffer.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(OutTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    if (RecordedUpload)
    {
        RecordedUpload->UploadResource = UploadBuffer.Resource;
        RecordedUpload->CommandAllocator = UploadAllocator;
        RecordedUpload->CommandList = UploadList;
    }
    else
    {
        ID3D12CommandList* Lists[] = { UploadList.Get() };
        Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
        Device->GetGraphicsQueue()->Flush();
    }

    return true;
}

bool FTextureLoader::CreateDefaultGridTexture(ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload, bool bUseSRGB)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t Width = 256;
    const uint32_t Height = 256;
    const uint32_t CellSize = 32;

    const uint32_t LightColor = 0xffb5b5b5;
    const uint32_t DarkColor = 0xff5f5f5f;
    std::vector<uint32_t> TextureData(Width * Height, LightColor);

    for (uint32_t y = 0; y < Height; ++y)
    {
        const uint32_t CellY = y / CellSize;
        for (uint32_t x = 0; x < Width; ++x)
        {
            const uint32_t CellX = x / CellSize;
            const bool UseDark = ((CellX + CellY) % 2) == 0;
            TextureData[y * Width + x] = UseDark ? DarkColor : LightColor;
        }
    }

    const DXGI_FORMAT TexFormat = bUseSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    const FRGTextureDesc TextureRGDesc = { Width, Height, TexFormat };
    FBindlessTexture TextureWrapper;
    CreateBindlessTexture(Device, L"DefaultGridTexture",
        TextureRGDesc, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, TextureWrapper, false, false);
    OutTexture = TextureWrapper.Resource;

    const D3D12_RESOURCE_DESC TextureDesc = CreateTextureResourceDesc(TextureRGDesc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    FUploadBuffer UploadBuffer;
    CreateUploadBuffer(Device, L"", UploadBufferSize, UploadBuffer, nullptr);

    uint8_t* MappedData = nullptr;
    const D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadBuffer->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    for (UINT Row = 0; Row < NumRows; ++Row)
    {
        const uint8_t* SrcRow = reinterpret_cast<const uint8_t*>(&TextureData[Row * Width]);
        memcpy(MappedData + Layout.Offset + Row * Layout.Footprint.RowPitch, SrcRow, Width * sizeof(uint32_t));
    }

    UploadBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"TextureLoader_DataUpload_CL");

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadBuffer.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(OutTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    if (RecordedUpload)
    {
        RecordedUpload->UploadResource = UploadBuffer.Resource;
        RecordedUpload->CommandAllocator = UploadAllocator;
        RecordedUpload->CommandList = UploadList;
    }
    else
    {
        ID3D12CommandList* Lists[] = { UploadList.Get() };
        Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
        Device->GetGraphicsQueue()->Flush();
    }

    return true;
}

bool FTextureLoader::CreateSolidColorTexture(uint32_t Color, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload, bool bUseSRGB)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t Width = 1;
    const uint32_t Height = 1;
    const std::array<uint32_t, 1> TextureData = { Color };

    const DXGI_FORMAT TexFormat = bUseSRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    const FRGTextureDesc TextureRGDesc = { Width, Height, TexFormat };
    FBindlessTexture TextureWrapper;
    CreateBindlessTexture(Device, L"SolidColorTexture",
        TextureRGDesc, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, TextureWrapper, false, false);
    OutTexture = TextureWrapper.Resource;

    const D3D12_RESOURCE_DESC TextureDesc = CreateTextureResourceDesc(TextureRGDesc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    FUploadBuffer UploadBuffer;
    CreateUploadBuffer(Device, L"", UploadBufferSize, UploadBuffer, nullptr);

    uint8_t* MappedData = nullptr;
    const D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadBuffer->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    const uint8_t* SrcRow = reinterpret_cast<const uint8_t*>(TextureData.data());
    memcpy(MappedData + Layout.Offset, SrcRow, sizeof(uint32_t));
    UploadBuffer->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"TextureLoader_SolidColorUpload_CL");

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadBuffer.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(OutTexture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    if (RecordedUpload)
    {
        RecordedUpload->UploadResource = UploadBuffer.Resource;
        RecordedUpload->CommandAllocator = UploadAllocator;
        RecordedUpload->CommandList = UploadList;
    }
    else
    {
        ID3D12CommandList* CommandLists[] = { UploadList.Get() };
        Device->GetGraphicsQueue()->ExecuteCommandLists(1, CommandLists);
        Device->GetGraphicsQueue()->Flush();
    }

    return true;
}

bool FTextureLoader::LoadTexturesParallel(std::vector<FTextureLoadRequest>& Requests)
{
    if (Requests.empty())
    {
        return true;
    }

    const auto StartTime = std::chrono::high_resolution_clock::now();

    if (!FTaskScheduler::Get().IsRunning())
    {
        // Fallback to serial loading if task system is not initialized
        LogWarning("Task system not initialized, falling back to serial texture loading");
        for (FTextureLoadRequest& Request : Requests)
        {
            if (Request.bUseSolidColor)
            {
                Request.bSuccess = LoadOrSolidColor(Request.Path, Request.SolidColor, *Request.OutTexture, nullptr, Request.bUseSRGB);
            }
            else
            {
                Request.bSuccess = LoadOrDefault(Request.Path, *Request.OutTexture, nullptr, Request.bUseSRGB);
            }
        }

        const auto EndTime = std::chrono::high_resolution_clock::now();
        const auto Duration = std::chrono::duration_cast<std::chrono::milliseconds>(EndTime - StartTime);
        LogInfo("Loaded " + std::to_string(Requests.size()) + " textures serially in " + std::to_string(Duration.count()) + " ms");
    }
    else
    {
        // Load textures in parallel
        std::vector<FTextureUploadWork> UploadWork(Requests.size());
        std::vector<FTask::FTaskFunction> Tasks;
        Tasks.reserve(Requests.size());

        for (size_t Index = 0; Index < Requests.size(); ++Index)
        {
            FTextureLoadRequest& Request = Requests[Index];
            FTextureUploadWork* Work = &UploadWork[Index];

            Tasks.push_back([this, &Request, Work]()
            {
                if (Request.bUseSolidColor)
                {
                    Request.bSuccess = LoadOrSolidColor(Request.Path, Request.SolidColor, *Request.OutTexture, Work, Request.bUseSRGB);
                }
                else
                {
                    Request.bSuccess = LoadOrDefault(Request.Path, *Request.OutTexture, Work, Request.bUseSRGB);
                }
            });
        }

        std::vector<FTaskRef> ScheduledTasks = FTaskScheduler::Get().ScheduleTaskBatch(Tasks);
        
        // Wait for all texture loading tasks to complete
        for (const FTaskRef& Task : ScheduledTasks)
        {
            FTaskScheduler::Get().WaitForTask(Task);
        }

        std::vector<ID3D12CommandList*> RecordedLists;
        RecordedLists.reserve(UploadWork.size());

        for (size_t Index = 0; Index < UploadWork.size(); ++Index)
        {
            if (Requests[Index].bSuccess && UploadWork[Index].CommandList)
            {
                RecordedLists.push_back(UploadWork[Index].CommandList.Get());
            }
        }

        if (!RecordedLists.empty())
        {
            Device->GetGraphicsQueue()->ExecuteCommandLists(static_cast<UINT>(RecordedLists.size()), RecordedLists.data());
            Device->GetGraphicsQueue()->Flush();
        }

        const auto EndTime = std::chrono::high_resolution_clock::now();
        const auto Duration = std::chrono::duration_cast<std::chrono::milliseconds>(EndTime - StartTime);
        LogInfo("Loaded " + std::to_string(Requests.size()) + " textures in parallel in " + std::to_string(Duration.count()) + " ms");
    }

    // Check if all textures loaded successfully
    bool bAllSuccess = true;
    for (const FTextureLoadRequest& Request : Requests)
    {
        if (!Request.bSuccess)
        {
            bAllSuccess = false;
            break;
        }
    }

    return bAllSuccess;
}
