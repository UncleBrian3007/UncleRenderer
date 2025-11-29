#include "DX12SwapChain.h"
#include "DX12Device.h"
#include "DX12CommandQueue.h"
#include "../Core/Logger.h"
#include <string>

FDX12SwapChain::FDX12SwapChain()
    : RTVDescriptorSize(0)
    , BackBufferFormat(DXGI_FORMAT_R8G8B8A8_UNORM)
    , BufferCount(3)
    , bAllowTearing(false)
{
}

FDX12SwapChain::~FDX12SwapChain()
{
    ReleaseBuffers();
}

bool FDX12SwapChain::Initialize(FDX12Device* InDevice, HWND WindowHandle, uint32 Width, uint32 Height, uint32 InBufferCount)
{
    BufferCount = InBufferCount;
    BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bAllowTearing = InDevice->IsTearingSupported();

    if (!CreateSwapChain(InDevice, WindowHandle, Width, Height))
    {
        return false;
    }

    return CreateRTVs(InDevice);
}

bool FDX12SwapChain::Resize(FDX12Device* InDevice, uint32 Width, uint32 Height)
{
    ReleaseBuffers();

    if (SwapChain)
    {
        const UINT SwapChainFlags = bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        HR_CHECK(SwapChain->ResizeBuffers(BufferCount, Width, Height, BackBufferFormat, SwapChainFlags));
    }

    LogInfo(std::string("SwapChain Resize - ") + std::to_string(Width) + "x" + std::to_string(Height));

    return CreateRTVs(InDevice);
}

bool FDX12SwapChain::CreateSwapChain(FDX12Device* InDevice, HWND WindowHandle, uint32 Width, uint32 Height)
{
    DXGI_SWAP_CHAIN_DESC1 SwapChainDesc = {};
    SwapChainDesc.BufferCount = BufferCount;
    SwapChainDesc.Width = Width;
    SwapChainDesc.Height = Height;
    SwapChainDesc.Format = BackBufferFormat;
    SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    SwapChainDesc.SampleDesc.Count = 1;
    SwapChainDesc.Flags = bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> TempSwapChain;
    HR_CHECK(InDevice->GetFactory()->CreateSwapChainForHwnd(
        InDevice->GetGraphicsQueue()->GetD3DQueue(),
        WindowHandle,
        &SwapChainDesc,
        nullptr,
        nullptr,
        TempSwapChain.GetAddressOf()));

    HR_CHECK(InDevice->GetFactory()->MakeWindowAssociation(WindowHandle, DXGI_MWA_NO_ALT_ENTER));
    HR_CHECK(TempSwapChain.As(&SwapChain));

    LogInfo(std::string("SwapChain - BufferCount: ") + std::to_string(BufferCount)
        + ", Format: DXGI_FORMAT_R8G8B8A8_UNORM"
        + (bAllowTearing ? ", DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING On" : ", TEARING Off"));

    return true;
}

bool FDX12SwapChain::CreateRTVs(FDX12Device* InDevice)
{
    BackBuffers.resize(BufferCount);
    RTVHandles.resize(BufferCount);
    BackBufferStates.assign(BufferCount, D3D12_RESOURCE_STATE_PRESENT);

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = BufferCount;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(InDevice->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(RTVHeap.GetAddressOf())));

    RTVDescriptorSize = InDevice->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = RTVHeap->GetCPUDescriptorHandleForHeapStart();

    for (uint32 i = 0; i < BufferCount; ++i)
    {
        HR_CHECK(SwapChain->GetBuffer(i, IID_PPV_ARGS(BackBuffers[i].GetAddressOf())));
        InDevice->GetDevice()->CreateRenderTargetView(BackBuffers[i].Get(), nullptr, RtvHandle);
        RTVHandles[i] = RtvHandle;
        RtvHandle.ptr += RTVDescriptorSize;
    }

    return true;
}

void FDX12SwapChain::ReleaseBuffers()
{
    BackBuffers.clear();
    RTVHandles.clear();
    BackBufferStates.clear();
    RTVHeap.Reset();
}

