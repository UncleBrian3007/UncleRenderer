#include "FrameResources.h"
#include "../RHI/DX12Device.h"

bool FFrameResources::Initialize(FDX12Device* Device)
{
    if (Device == nullptr)
    {
        return false;
    }

    ID3D12Device* RawDevice = Device->GetDevice();
    if (RawDevice == nullptr)
    {
        return false;
    }

    HR_CHECK(RawDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&DirectAllocator)));
    return true;
}
