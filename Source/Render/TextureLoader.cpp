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
}

std::unordered_map<std::wstring, ComPtr<ID3D12Resource>> FTextureLoader::GlobalTextureCache;
static std::mutex GTextureCacheMutex;

FTextureLoader::FTextureLoader(FDX12Device* InDevice)
    : Device(InDevice)
{
}

bool FTextureLoader::LoadOrDefault(const std::wstring& TexturePath, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload)
{
    if (TryGetCachedTexture(TexturePath, OutTexture))
    {
        return true;
    }

    if (!TexturePath.empty() && LoadTextureInternal(TexturePath, OutTexture, RecordedUpload))
    {
        std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
        GlobalTextureCache[TexturePath] = OutTexture;
        return true;
    }

    if (TryGetCachedTexture(GDefaultGridCacheKey, OutTexture))
    {
        return true;
    }

    if (CreateDefaultGridTexture(OutTexture, RecordedUpload))
    {
        std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
        GlobalTextureCache[GDefaultGridCacheKey] = OutTexture;
        return true;
    }

    return false;
}

bool FTextureLoader::LoadOrSolidColor(const std::wstring& TexturePath, uint32_t Color, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload)
{
    if (TryGetCachedTexture(TexturePath, OutTexture))
    {
        return true;
    }

    if (!TexturePath.empty() && LoadTextureInternal(TexturePath, OutTexture, RecordedUpload))
    {
        std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
        GlobalTextureCache[TexturePath] = OutTexture;
        return true;
    }

    const std::wstring CacheKey = L"__solid_color_" + std::to_wstring(static_cast<uint64_t>(Color));
    if (TryGetCachedTexture(CacheKey, OutTexture))
    {
        return true;
    }

    if (!CreateSolidColorTexture(Color, OutTexture, RecordedUpload))
    {
        return false;
    }

    std::lock_guard<std::mutex> Lock(GTextureCacheMutex);
    GlobalTextureCache[CacheKey] = OutTexture;
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

bool FTextureLoader::LoadTextureInternal(const std::wstring& FilePath, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload)
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

        const uint32_t ArraySize = Descriptor.type == ddspp::Texture3D ? 1u : std::max(1u, Descriptor.arraySize);
        const uint32_t Depth = Descriptor.type == ddspp::Texture3D ? std::max(1u, Descriptor.depth) : 1u;
        const UINT SubresourceCount = Descriptor.numMips * ArraySize;
        const DXGI_FORMAT Format = static_cast<DXGI_FORMAT>(Descriptor.format);

        D3D12_RESOURCE_DESC TextureDesc = {};
        TextureDesc.Dimension = Descriptor.type == ddspp::Texture3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        TextureDesc.Width = Descriptor.width;
        TextureDesc.Height = Descriptor.height;
        TextureDesc.DepthOrArraySize = Descriptor.type == ddspp::Texture3D ? Depth : ArraySize;
        TextureDesc.MipLevels = static_cast<UINT16>(Descriptor.numMips);
        TextureDesc.Format = Format;
        TextureDesc.SampleDesc.Count = 1;
        TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES DefaultHeap = {};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        DefaultHeap.CreationNodeMask = 1;
        DefaultHeap.VisibleNodeMask = 1;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &TextureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(OutTexture.GetAddressOf())));

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Layouts(SubresourceCount);
        std::vector<UINT> NumRows(SubresourceCount);
        UINT64 UploadBufferSize = 0;
        Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, SubresourceCount, 0, Layouts.data(), NumRows.data(), nullptr, &UploadBufferSize);

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

        uint8_t* MappedData = nullptr;
        D3D12_RANGE EmptyRange = { 0, 0 };
        HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

        size_t DataOffset = Descriptor.headerSize;
        for (uint32_t ArrayIndex = 0; ArrayIndex < ArraySize; ++ArrayIndex)
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
                    UploadResource->Unmap(0, nullptr);
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

        UploadResource->Unmap(0, nullptr);

        ComPtr<ID3D12CommandAllocator> UploadAllocator;
        ComPtr<ID3D12GraphicsCommandList> UploadList;
        HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
        HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

        for (UINT Subresource = 0; Subresource < SubresourceCount; ++Subresource)
        {
            D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
            DstLocation.pResource = OutTexture.Get();
            DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            DstLocation.SubresourceIndex = Subresource;

            D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
            SrcLocation.pResource = UploadResource.Get();
            SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            SrcLocation.PlacedFootprint = Layouts[Subresource];

            UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);
        }

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = OutTexture.Get();
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        UploadList->ResourceBarrier(1, &Barrier);

        HR_CHECK(UploadList->Close());

        if (RecordedUpload)
        {
            RecordedUpload->UploadResource = UploadResource;
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

    D3D12_RESOURCE_DESC TextureDesc = {};
    TextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TextureDesc.Width = static_cast<UINT>(Width);
    TextureDesc.Height = static_cast<UINT>(Height);
    TextureDesc.DepthOrArraySize = 1;
    TextureDesc.MipLevels = 1;
    TextureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TextureDesc.SampleDesc.Count = 1;
    TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &TextureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(OutTexture.GetAddressOf())));

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

    uint8_t* MappedData = nullptr;
    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    for (UINT Row = 0; Row < NumRows; ++Row)
    {
        const uint8_t* SrcRow = TextureData.data() + Row * static_cast<UINT64>(Width) * 4ULL;
        memcpy(MappedData + Layout.Offset + Row * Layout.Footprint.RowPitch, SrcRow, Width * 4ULL);
    }

    UploadResource->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadResource.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = OutTexture.Get();
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    if (RecordedUpload)
    {
        RecordedUpload->UploadResource = UploadResource;
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

bool FTextureLoader::CreateDefaultGridTexture(ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload)
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

    D3D12_RESOURCE_DESC TextureDesc = {};
    TextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TextureDesc.Width = Width;
    TextureDesc.Height = Height;
    TextureDesc.DepthOrArraySize = 1;
    TextureDesc.MipLevels = 1;
    TextureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TextureDesc.SampleDesc.Count = 1;
    TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &TextureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(OutTexture.GetAddressOf())));

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

    uint8_t* MappedData = nullptr;
    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    for (UINT Row = 0; Row < NumRows; ++Row)
    {
        const uint8_t* SrcRow = reinterpret_cast<const uint8_t*>(&TextureData[Row * Width]);
        memcpy(MappedData + Layout.Offset + Row * Layout.Footprint.RowPitch, SrcRow, Width * sizeof(uint32_t));
    }

    UploadResource->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadResource.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = OutTexture.Get();
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    if (RecordedUpload)
    {
        RecordedUpload->UploadResource = UploadResource;
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

bool FTextureLoader::CreateSolidColorTexture(uint32_t Color, ComPtr<ID3D12Resource>& OutTexture, FTextureUploadWork* RecordedUpload)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t Width = 1;
    const uint32_t Height = 1;
    const std::array<uint32_t, 1> TextureData = { Color };

    D3D12_RESOURCE_DESC TextureDesc = {};
    TextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TextureDesc.Width = Width;
    TextureDesc.Height = Height;
    TextureDesc.DepthOrArraySize = 1;
    TextureDesc.MipLevels = 1;
    TextureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TextureDesc.SampleDesc.Count = 1;
    TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &TextureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(OutTexture.GetAddressOf())));

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

    uint8_t* MappedData = nullptr;
    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    const uint8_t* SrcRow = reinterpret_cast<const uint8_t*>(TextureData.data());
    memcpy(MappedData + Layout.Offset, SrcRow, sizeof(uint32_t));
    UploadResource->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadResource.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = OutTexture.Get();
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    if (RecordedUpload)
    {
        RecordedUpload->UploadResource = UploadResource;
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
                Request.bSuccess = LoadOrSolidColor(Request.Path, Request.SolidColor, *Request.OutTexture);
            }
            else
            {
                Request.bSuccess = LoadOrDefault(Request.Path, *Request.OutTexture);
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
                    Request.bSuccess = LoadOrSolidColor(Request.Path, Request.SolidColor, *Request.OutTexture, Work);
                }
                else
                {
                    Request.bSuccess = LoadOrDefault(Request.Path, *Request.OutTexture, Work);
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
