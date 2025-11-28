#pragma once

#include <memory>
#include "../RHI/DX12Commons.h"

class FDX12Device;

class FFrameResources
{
public:
    FFrameResources() = default;

    bool Initialize(FDX12Device* Device);

private:
    ComPtr<ID3D12CommandAllocator> DirectAllocator;
};
