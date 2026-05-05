#pragma once

#include "RenderGraph.h"
#include "../RHI/DX12Device.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <wrl.h>

inline bool IsValidBindlessIndex(uint32_t BindlessIndex)
{
    return BindlessIndex != UINT32_MAX;
}

template <typename... TIndices>
inline bool AreAllBindlessIndicesValid(TIndices... BindlessIndices)
{
    return (... && IsValidBindlessIndex(static_cast<uint32_t>(BindlessIndices)));
}

struct FBindlessBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    FRGBufferDesc Desc = {};
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
    uint32_t SrvBindlessIndex = UINT32_MAX;
    uint32_t UavBindlessIndex = UINT32_MAX;

    ID3D12Resource* Get() const { return Resource.Get(); }
    ID3D12Resource** GetAddressOf() { return Resource.GetAddressOf(); }
    ID3D12Resource** ReleaseAndGetAddressOf() { return Resource.ReleaseAndGetAddressOf(); }
    bool IsValid() const { return Resource != nullptr; }
    explicit operator bool() const { return IsValid(); }
    ID3D12Resource* operator->() const { return Resource.Get(); }
    bool HasSrv() const { return IsValidBindlessIndex(SrvBindlessIndex); }
    bool HasUav() const { return IsValidBindlessIndex(UavBindlessIndex); }
    bool IsFullyBound() const { return IsValid() && HasSrv() && HasUav(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return Resource ? Resource->GetGPUVirtualAddress() : 0; }
};

struct FUploadBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    uint64_t Size = 0;

    ID3D12Resource* Get() const { return Resource.Get(); }
    ID3D12Resource** GetAddressOf() { return Resource.GetAddressOf(); }
    ID3D12Resource** ReleaseAndGetAddressOf() { return Resource.ReleaseAndGetAddressOf(); }
    bool IsValid() const { return Resource != nullptr; }
    explicit operator bool() const { return IsValid(); }
    ID3D12Resource* operator->() const { return Resource.Get(); }
};

struct FMappedUploadBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    uint64_t Size = 0;
    uint8_t* MappedData = nullptr;

    ID3D12Resource* Get() const { return Resource.Get(); }
    ID3D12Resource** GetAddressOf() { return Resource.GetAddressOf(); }
    ID3D12Resource** ReleaseAndGetAddressOf() { return Resource.ReleaseAndGetAddressOf(); }
    bool IsValid() const { return Resource != nullptr && MappedData != nullptr; }
    explicit operator bool() const { return IsValid(); }
    ID3D12Resource* operator->() const { return Resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() const { return Resource ? Resource->GetGPUVirtualAddress() : 0; }
};

inline FRGBufferHandle ImportBindlessBuffer(FRenderGraph& Graph, const std::string& Name, FBindlessBuffer& Buffer)
{
    return Graph.ImportBuffer(
        Name,
        Buffer.Get(),
        &Buffer.State,
        Buffer.Desc,
        Buffer.SrvBindlessIndex,
        Buffer.UavBindlessIndex);
}

inline void InitializeBindlessBuffer(FBindlessBuffer& Buffer, const FRGBufferDesc& Desc, D3D12_RESOURCE_STATES InitialState)
{
    Buffer.Desc = Desc;
    Buffer.State = InitialState;
    Buffer.SrvBindlessIndex = UINT32_MAX;
    Buffer.UavBindlessIndex = UINT32_MAX;
}

inline void InitializeUploadBuffer(FUploadBuffer& Buffer, uint64_t Size)
{
    Buffer.Size = Size;
}

inline void InitializeMappedUploadBuffer(FMappedUploadBuffer& Buffer, uint64_t Size, uint8_t* MappedData)
{
    Buffer.Size = Size;
    Buffer.MappedData = MappedData;
}

inline D3D12_HEAP_PROPERTIES CreateHeapProperties(D3D12_HEAP_TYPE HeapType)
{
    D3D12_HEAP_PROPERTIES HeapProperties = {};
    HeapProperties.Type = HeapType;
    HeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProperties.CreationNodeMask = 1;
    HeapProperties.VisibleNodeMask = 1;
    return HeapProperties;
}

inline D3D12_RESOURCE_DESC CreateBufferResourceDesc(uint64_t Size, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC ResourceDesc = {};
    ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ResourceDesc.Width = Size;
    ResourceDesc.Height = 1;
    ResourceDesc.DepthOrArraySize = 1;
    ResourceDesc.MipLevels = 1;
    ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    ResourceDesc.SampleDesc.Count = 1;
    ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ResourceDesc.Flags = Flags;
    return ResourceDesc;
}

inline FRGBufferDesc CreateStructuredBufferDesc(uint64_t Size, uint32_t NumElements, uint32_t StructureByteStride, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    FRGBufferDesc Desc = {};
    Desc.Size = Size;
    Desc.Flags = Flags;
    Desc.ViewFormat = ViewFormat;
    Desc.NumElements = NumElements;
    Desc.StructureByteStride = StructureByteStride;
    Desc.SrvFlags = D3D12_BUFFER_SRV_FLAG_NONE;
    Desc.UavFlags = D3D12_BUFFER_UAV_FLAG_NONE;
    return Desc;
}

inline FRGBufferDesc CreateStructuredBufferDesc(size_t NumElements, uint32_t StructureByteStride, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    return CreateStructuredBufferDesc(
        static_cast<uint64_t>(NumElements) * StructureByteStride,
        static_cast<uint32_t>(NumElements),
        StructureByteStride,
        Flags,
        ViewFormat);
}

template <typename T>
inline FRGBufferDesc CreateStructuredBufferDesc(size_t NumElements, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    return CreateStructuredBufferDesc(NumElements, static_cast<uint32_t>(sizeof(T)), Flags, ViewFormat);
}

template <typename T>
inline FRGBufferDesc CreateStructuredBufferDesc(const std::vector<T>& Elements, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    return CreateStructuredBufferDesc<T>(Elements.size(), Flags, ViewFormat);
}

inline FRGBufferDesc CreateRWStructuredBufferDesc(uint64_t Size, uint32_t NumElements, uint32_t StructureByteStride, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    return CreateStructuredBufferDesc(Size, NumElements, StructureByteStride, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, ViewFormat);
}

inline FRGBufferDesc CreateRWStructuredBufferDesc(size_t NumElements, uint32_t StructureByteStride, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    return CreateRWStructuredBufferDesc(
        static_cast<uint64_t>(NumElements) * StructureByteStride,
        static_cast<uint32_t>(NumElements),
        StructureByteStride,
        ViewFormat);
}

template <typename T>
inline FRGBufferDesc CreateRWStructuredBufferDesc(size_t NumElements, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    return CreateRWStructuredBufferDesc(NumElements, static_cast<uint32_t>(sizeof(T)), ViewFormat);
}

template <typename T>
inline FRGBufferDesc CreateRWStructuredBufferDesc(const std::vector<T>& Elements, DXGI_FORMAT ViewFormat = DXGI_FORMAT_UNKNOWN)
{
    return CreateRWStructuredBufferDesc<T>(Elements.size(), ViewFormat);
}

inline FRGBufferDesc CreateRawBufferDesc(uint64_t Size, uint32_t NumElements, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE, DXGI_FORMAT ViewFormat = DXGI_FORMAT_R32_TYPELESS)
{
    FRGBufferDesc Desc = {};
    Desc.Size = Size;
    Desc.Flags = Flags;
    Desc.ViewFormat = ViewFormat;
    Desc.NumElements = NumElements;
    Desc.StructureByteStride = 0;
    Desc.SrvFlags = D3D12_BUFFER_SRV_FLAG_RAW;
    Desc.UavFlags = D3D12_BUFFER_UAV_FLAG_RAW;
    return Desc;
}

inline FRGBufferDesc CreateRawBufferDesc(uint64_t Size, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE, DXGI_FORMAT ViewFormat = DXGI_FORMAT_R32_TYPELESS)
{
    const uint32_t NumElements = static_cast<uint32_t>((Size + sizeof(uint32_t) - 1ull) / sizeof(uint32_t));
    return CreateRawBufferDesc(Size, NumElements, Flags, ViewFormat);
}

inline D3D12_SHADER_RESOURCE_VIEW_DESC CreateBufferSrvDesc(const FRGBufferDesc& Desc)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
    SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Format = Desc.ViewFormat;
    SrvDesc.Buffer.FirstElement = 0;
    SrvDesc.Buffer.NumElements = Desc.NumElements;
    SrvDesc.Buffer.StructureByteStride = Desc.StructureByteStride;
    SrvDesc.Buffer.Flags = Desc.SrvFlags;
    return SrvDesc;
}

inline D3D12_UNORDERED_ACCESS_VIEW_DESC CreateBufferUavDesc(const FRGBufferDesc& Desc)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
    UavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    UavDesc.Format = Desc.ViewFormat;
    UavDesc.Buffer.FirstElement = 0;
    UavDesc.Buffer.NumElements = Desc.NumElements;
    UavDesc.Buffer.StructureByteStride = Desc.StructureByteStride;
    UavDesc.Buffer.CounterOffsetInBytes = 0;
    UavDesc.Buffer.Flags = Desc.UavFlags;
    return UavDesc;
}

inline void CreateBindlessBufferSrv(FDX12Device* Device, FBindlessBuffer& Buffer)
{
    Buffer.SrvBindlessIndex = Device->CreateBindlessSrv(Buffer.Get(), CreateBufferSrvDesc(Buffer.Desc));
}

inline void CreateBindlessBufferUav(FDX12Device* Device, FBindlessBuffer& Buffer)
{
    Buffer.UavBindlessIndex = Device->CreateBindlessUav(Buffer.Get(), nullptr, CreateBufferUavDesc(Buffer.Desc));
}

inline void CreateBindlessBufferViews(FDX12Device* Device, FBindlessBuffer& Buffer, bool bCreateSrv, bool bCreateUav)
{
    if (bCreateSrv)
    {
        CreateBindlessBufferSrv(Device, Buffer);
    }

    if (bCreateUav)
    {
        CreateBindlessBufferUav(Device, Buffer);
    }
}

inline void CreateUploadBuffer(FDX12Device* Device, const std::wstring& Name, uint64_t Size, FUploadBuffer& Buffer, const void* SrcData)
{
    const D3D12_HEAP_PROPERTIES UploadHeap = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC UploadDesc = CreateBufferResourceDesc(Size);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(Buffer.ReleaseAndGetAddressOf())));

    InitializeUploadBuffer(Buffer, Size);
    if (Buffer)
    {
        if (!Name.empty())
        {
            Buffer->SetName(Name.c_str());
        }

        if (SrcData)
        {
            void* UploadData = nullptr;
            const D3D12_RANGE EmptyRange = { 0, 0 };
            HR_CHECK(Buffer->Map(0, &EmptyRange, &UploadData));
            std::memcpy(UploadData, SrcData, static_cast<size_t>(Size));
            Buffer->Unmap(0, nullptr);
        }
    }
}

inline bool CreateMappedUploadBuffer(FDX12Device* Device, const std::wstring& Name, uint64_t Size, FMappedUploadBuffer& Buffer)
{
    const D3D12_HEAP_PROPERTIES UploadHeap = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC UploadDesc = CreateBufferResourceDesc(Size);
    const HRESULT Hr = Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(Buffer.ReleaseAndGetAddressOf()));
    if (FAILED(Hr) || !Buffer.Get())
    {
        Buffer = {};
        return false;
    }

    if (!Name.empty())
    {
        Buffer->SetName(Name.c_str());
    }

    void* UploadData = nullptr;
    const D3D12_RANGE EmptyRange = { 0, 0 };
    if (FAILED(Buffer->Map(0, &EmptyRange, &UploadData)) || !UploadData)
    {
        Buffer = {};
        return false;
    }

    InitializeMappedUploadBuffer(Buffer, Size, static_cast<uint8_t*>(UploadData));
    return true;
}

inline bool CreateMappedConstantBuffer(FDX12Device* Device, uint64_t BufferSize, FMappedUploadBuffer& OutConstantBuffer)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint64_t ConstantBufferSize = (BufferSize + 255ULL) & ~255ULL;
    return CreateMappedUploadBuffer(Device, L"MappedConstantBuffer", ConstantBufferSize, OutConstantBuffer);
}

inline bool CreateMappedBindlessBuffer(
    FDX12Device* Device,
    const std::wstring& Name,
    const FRGBufferDesc& Desc,
    FBindlessBuffer& OutBuffer,
    void*& OutMappedData)
{
    const D3D12_HEAP_PROPERTIES UploadHeap = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC ResourceDesc = CreateBufferResourceDesc(Desc.Size, Desc.Flags);
    const HRESULT Hr = Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ResourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(OutBuffer.ReleaseAndGetAddressOf()));
    if (FAILED(Hr) || !OutBuffer)
    {
        return false;
    }

    if (!Name.empty())
    {
        OutBuffer->SetName(Name.c_str());
    }

    const D3D12_RANGE EmptyRange = { 0, 0 };
    if (FAILED(OutBuffer->Map(0, &EmptyRange, &OutMappedData)) || !OutMappedData)
    {
        OutBuffer = {};
        OutMappedData = nullptr;
        return false;
    }

    InitializeBindlessBuffer(OutBuffer, Desc, D3D12_RESOURCE_STATE_GENERIC_READ);
    return true;
}

inline void CreateBindlessBuffer(
    FDX12Device* Device,
    const std::wstring& Name,
    const FRGBufferDesc& Desc,
    D3D12_RESOURCE_STATES InitialState,
    FBindlessBuffer& OutBuffer,
    bool bCreateSrv,
    bool bCreateUav)
{
    const D3D12_HEAP_PROPERTIES DefaultHeap = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC ResourceDesc = CreateBufferResourceDesc(Desc.Size, Desc.Flags);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &ResourceDesc,
        InitialState,
        nullptr,
        IID_PPV_ARGS(OutBuffer.ReleaseAndGetAddressOf())));

    if (OutBuffer)
    {
        if (!Name.empty())
        {
            OutBuffer->SetName(Name.c_str());
        }
    }

    InitializeBindlessBuffer(OutBuffer, Desc, InitialState);
    CreateBindlessBufferViews(Device, OutBuffer, bCreateSrv, bCreateUav);
}

inline void CreateBindlessBufferWithUpload(
    FDX12Device* Device,
    const std::wstring& Name,
    const FRGBufferDesc& Desc,
    D3D12_RESOURCE_STATES InitialState,
    FBindlessBuffer& OutBuffer,
    FUploadBuffer& OutUpload,
    const void* SrcData,
    bool bCreateSrv,
    bool bCreateUav)
{
    CreateBindlessBuffer(Device, Name, Desc, InitialState, OutBuffer, bCreateSrv, bCreateUav);
    CreateUploadBuffer(Device, Name + L"Upload", Desc.Size, OutUpload, SrcData);
}

struct FBindlessTexture
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    FRGTextureDesc Desc = {};
    D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;
    uint32_t SrvBindlessIndex = UINT32_MAX;
    uint32_t UavBindlessIndex = UINT32_MAX;

    ID3D12Resource* Get() const { return Resource.Get(); }
    ID3D12Resource** GetAddressOf() { return Resource.GetAddressOf(); }
    ID3D12Resource** ReleaseAndGetAddressOf() { return Resource.ReleaseAndGetAddressOf(); }
    bool IsValid() const { return Resource != nullptr; }
    explicit operator bool() const { return IsValid(); }
    ID3D12Resource* operator->() const { return Resource.Get(); }
    bool HasSrv() const { return IsValidBindlessIndex(SrvBindlessIndex); }
    bool HasUav() const { return IsValidBindlessIndex(UavBindlessIndex); }
    bool IsFullyBound() const { return IsValid() && HasSrv() && HasUav(); }
};

inline void InitializeBindlessTexture(FBindlessTexture& Texture, const FRGTextureDesc& Desc, D3D12_RESOURCE_STATES InitialState)
{
    Texture.Desc = Desc;
    Texture.State = InitialState;
    Texture.SrvBindlessIndex = UINT32_MAX;
    Texture.UavBindlessIndex = UINT32_MAX;
}

inline D3D12_RESOURCE_DESC CreateTexture2DResourceDesc(const FRGTextureDesc& Desc, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC ResourceDesc = {};
    ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    ResourceDesc.Width = Desc.Width;
    ResourceDesc.Height = Desc.Height;
    ResourceDesc.DepthOrArraySize = 1;
    ResourceDesc.MipLevels = Desc.MipLevels;
    ResourceDesc.Format = Desc.Format;
    ResourceDesc.SampleDesc.Count = 1;
    ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ResourceDesc.Flags = Flags;
    return ResourceDesc;
}

inline D3D12_SHADER_RESOURCE_VIEW_DESC CreateTexture2DSrvDesc(const FRGTextureDesc& Desc, uint32_t MostDetailedMip = 0, uint32_t MipLevels = UINT32_MAX)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
    SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Format = Desc.Format;
    SrvDesc.Texture2D.MostDetailedMip = MostDetailedMip;
    SrvDesc.Texture2D.MipLevels = MipLevels == UINT32_MAX ? Desc.MipLevels : static_cast<UINT>(MipLevels);
    SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    return SrvDesc;
}

inline D3D12_UNORDERED_ACCESS_VIEW_DESC CreateTexture2DUavDesc(const FRGTextureDesc& Desc, uint32_t MipSlice = 0, uint32_t PlaneSlice = 0)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
    UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    UavDesc.Format = Desc.Format;
    UavDesc.Texture2D.MipSlice = MipSlice;
    UavDesc.Texture2D.PlaneSlice = PlaneSlice;
    return UavDesc;
}

inline void CreateBindlessTextureSrv(FDX12Device* Device, FBindlessTexture& Texture, uint32_t MostDetailedMip = 0, uint32_t MipLevels = UINT32_MAX)
{
    Texture.SrvBindlessIndex = Device->CreateBindlessSrv(Texture.Get(), CreateTexture2DSrvDesc(Texture.Desc, MostDetailedMip, MipLevels));
}

inline void CreateBindlessTextureUav(FDX12Device* Device, FBindlessTexture& Texture, uint32_t MipSlice = 0, uint32_t PlaneSlice = 0)
{
    Texture.UavBindlessIndex = Device->CreateBindlessUav(Texture.Get(), nullptr, CreateTexture2DUavDesc(Texture.Desc, MipSlice, PlaneSlice));
}

inline void CreateBindlessTextureViews(FDX12Device* Device, FBindlessTexture& Texture, bool bCreateSrv, bool bCreateUav)
{
    if (bCreateSrv)
    {
        CreateBindlessTextureSrv(Device, Texture);
    }

    if (bCreateUav)
    {
        CreateBindlessTextureUav(Device, Texture);
    }
}

inline void WriteOrCreateBindlessTextureSrv(FDX12Device* Device, FBindlessTexture& Texture, uint32_t MostDetailedMip = 0, uint32_t MipLevels = UINT32_MAX)
{
    const D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = CreateTexture2DSrvDesc(Texture.Desc, MostDetailedMip, MipLevels);
    if (Texture.SrvBindlessIndex == UINT32_MAX)
    {
        Texture.SrvBindlessIndex = Device->CreateBindlessSrv(Texture.Get(), SrvDesc);
    }
    else
    {
        Device->WriteBindlessSrv(Texture.SrvBindlessIndex, Texture.Get(), SrvDesc);
    }
}

inline void WriteOrCreateBindlessTextureUav(FDX12Device* Device, FBindlessTexture& Texture, uint32_t MipSlice = 0, uint32_t PlaneSlice = 0)
{
    const D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = CreateTexture2DUavDesc(Texture.Desc, MipSlice, PlaneSlice);
    if (Texture.UavBindlessIndex == UINT32_MAX)
    {
        Texture.UavBindlessIndex = Device->CreateBindlessUav(Texture.Get(), nullptr, UavDesc);
    }
    else
    {
        Device->WriteBindlessUav(Texture.UavBindlessIndex, Texture.Get(), nullptr, UavDesc);
    }
}

inline void WriteOrCreateBindlessTextureViews(FDX12Device* Device, FBindlessTexture& Texture, bool bWriteOrCreateSrv, bool bWriteOrCreateUav)
{
    if (bWriteOrCreateSrv)
    {
        WriteOrCreateBindlessTextureSrv(Device, Texture);
    }

    if (bWriteOrCreateUav)
    {
        WriteOrCreateBindlessTextureUav(Device, Texture);
    }
}

inline FRGTextureHandle ImportBindlessTexture(FRenderGraph& Graph, const std::string& Name, FBindlessTexture& Texture)
{
    return Graph.ImportTexture(
        Name,
        Texture.Get(),
        &Texture.State,
        Texture.Desc,
        Texture.SrvBindlessIndex,
        Texture.UavBindlessIndex);
}

inline bool MatchesTextureSize(ID3D12Resource* Resource, uint32_t ExpectedWidth, uint32_t ExpectedHeight)
{
    if (!Resource)
    {
        return false;
    }
    const D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
    return Desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
        && Desc.Width == ExpectedWidth
        && Desc.Height == ExpectedHeight;
}

inline bool MatchesTextureSize(const FBindlessTexture& Texture, uint32_t ExpectedWidth, uint32_t ExpectedHeight)
{
    return MatchesTextureSize(Texture.Get(), ExpectedWidth, ExpectedHeight);
}

inline bool MatchesBufferSize(ID3D12Resource* Resource, uint64_t ExpectedSize)
{
    if (!Resource)
    {
        return false;
    }
    const D3D12_RESOURCE_DESC Desc = Resource->GetDesc();
    return Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && Desc.Width == ExpectedSize;
}

inline bool MatchesBufferSize(const FBindlessBuffer& Buffer, uint64_t ExpectedSize)
{
    return MatchesBufferSize(Buffer.Get(), ExpectedSize);
}

inline void CreateBindlessTexture(
    FDX12Device* Device,
    const std::wstring& Name,
    const FRGTextureDesc& Desc,
    D3D12_RESOURCE_FLAGS Flags,
    D3D12_RESOURCE_STATES InitialState,
    FBindlessTexture& OutTexture,
    bool bCreateSrv,
    bool bCreateUav,
    const D3D12_CLEAR_VALUE* OptimizedClearValue = nullptr)
{
    const D3D12_HEAP_PROPERTIES DefaultHeap = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC ResourceDesc = CreateTexture2DResourceDesc(Desc, Flags);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &ResourceDesc,
        InitialState,
        OptimizedClearValue,
        IID_PPV_ARGS(OutTexture.ReleaseAndGetAddressOf())));

    if (OutTexture && !Name.empty())
    {
        OutTexture->SetName(Name.c_str());
    }

    InitializeBindlessTexture(OutTexture, Desc, InitialState);
    CreateBindlessTextureViews(Device, OutTexture, bCreateSrv, bCreateUav);
}
