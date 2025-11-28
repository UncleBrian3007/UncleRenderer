#pragma once

#include "DX12Commons.h"

class FDX12Resource
{
public:
    FDX12Resource() = default;

    bool InitializeBuffer(ID3D12Device* Device, uint64_t Size, D3D12_RESOURCE_FLAGS Flags, D3D12_RESOURCE_STATES InitialState);
    bool InitializeTexture2D(ID3D12Device* Device, const D3D12_RESOURCE_DESC& Desc, D3D12_RESOURCE_STATES InitialState);

    ID3D12Resource* GetResource() const { return Resource.Get(); }

private:
    ComPtr<ID3D12Resource> Resource;
};
