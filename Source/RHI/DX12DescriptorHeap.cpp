#include "DX12DescriptorHeap.h"

bool FDX12DescriptorHeap::Initialize(ID3D12Device* Device, const D3D12_DESCRIPTOR_HEAP_DESC& Desc)
{
    if (Device == nullptr)
    {
        return false;
    }

    HR_CHECK(Device->CreateDescriptorHeap(&Desc, IID_PPV_ARGS(&Heap)));
    DescriptorSize = Device->GetDescriptorHandleIncrementSize(Desc.Type);
    return true;
}
