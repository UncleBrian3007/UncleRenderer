#include "DX12Device.h"
#include "../Core/Logger.h"
#include <string>

FDX12Device::FDX12Device()
{
}

FDX12Device::~FDX12Device()
{
    if (GraphicsQueue)
    {
        GraphicsQueue->Flush();
    }
}

bool FDX12Device::Initialize()
{
    if (!CreateFactory()) return false;
    if (!PickAdapter())   return false;
    if (!CreateDevice())  return false;
    if (!CreateCommandQueues()) return false;

    return true;
}

bool FDX12Device::CreateFactory()
{
    UINT Flags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> DebugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugController))))
        {
            DebugController->EnableDebugLayer();
            Flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    HR_CHECK(CreateDXGIFactory2(Flags, IID_PPV_ARGS(Factory.GetAddressOf())));
    CheckTearingSupport();
    return true;
}

bool FDX12Device::PickAdapter()
{
    ComPtr<IDXGIAdapter1> TempAdapter;
    SIZE_T MaxVRAM = 0;

    for (UINT i = 0; Factory->EnumAdapters1(i, TempAdapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 Desc;
        TempAdapter->GetDesc1(&Desc);

        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (Desc.DedicatedVideoMemory > MaxVRAM)
        {
            MaxVRAM = Desc.DedicatedVideoMemory;
            TempAdapter.As(&Adapter);
        }
    }

    return Adapter != nullptr;
}

bool FDX12Device::CreateDevice()
{
    HR_CHECK(D3D12CreateDevice(
        Adapter.Get(),
        D3D_FEATURE_LEVEL_12_1,
        IID_PPV_ARGS(Device.GetAddressOf())
    ));
    return true;
}

bool FDX12Device::CreateCommandQueues()
{
    GraphicsQueue = std::make_unique<FDX12CommandQueue>();
    return GraphicsQueue->Initialize(Device.Get(), EDX12QueueType::Direct);
}

bool FDX12Device::CheckTearingSupport()
{
    bAllowTearing = false;

    ComPtr<IDXGIFactory5> Factory5;
    if (SUCCEEDED(Factory.As(&Factory5)))
    {
        BOOL AllowTearing = FALSE;
        if (SUCCEEDED(Factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &AllowTearing,
            sizeof(AllowTearing))))
        {
            bAllowTearing = AllowTearing == TRUE;
        }
    }

    LogInfo(std::string("DXGI_PRESENT_ALLOW_TEARING : ") + (bAllowTearing ? "Enabled" : "Disabled"));
    return true;
}
