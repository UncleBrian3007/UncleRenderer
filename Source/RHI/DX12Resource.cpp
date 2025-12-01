#include "DX12Resource.h"

bool FDX12Resource::InitializeBuffer(ID3D12Device* Device, uint64_t Size, D3D12_HEAP_TYPE HeapType, D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES InitialState)
{
    if (Device == nullptr)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = HeapType;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC BufferDesc = {};
    BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufferDesc.Alignment = 0;
    BufferDesc.Width = Size;
    BufferDesc.Height = 1;
    BufferDesc.DepthOrArraySize = 1;
    BufferDesc.MipLevels = 1;
    BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    BufferDesc.SampleDesc.Count = 1;
    BufferDesc.SampleDesc.Quality = 0;
    BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    BufferDesc.Flags = Flags;

    HR_CHECK(Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &BufferDesc, InitialState, nullptr, IID_PPV_ARGS(&Resource)));
    return true;
}

bool FDX12Resource::InitializeTexture2D(ID3D12Device* Device, const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES InitialState)
{
    if (Device == nullptr)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    HR_CHECK(Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &Desc, InitialState, nullptr, IID_PPV_ARGS(&Resource)));
    return true;
}
