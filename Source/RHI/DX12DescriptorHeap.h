#pragma once

#include "DX12Commons.h"

class FDX12DescriptorHeap
{
public:
    FDX12DescriptorHeap() = default;

    bool Initialize(ID3D12Device* Device, const D3D12_DESCRIPTOR_HEAP_DESC& Desc);

    ID3D12DescriptorHeap* GetHeap() const { return Heap.Get(); }
    uint32_t GetDescriptorSize() const { return DescriptorSize; }

private:
    ComPtr<ID3D12DescriptorHeap> Heap;
    uint32_t DescriptorSize = 0;
};
