#include "Application.h"
#include "Window.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12SwapChain.h"
#include "../RHI/DX12CommandContext.h"
#include <dxgi1_6.h>
#include <cstdint>

FApplication::FApplication()
    : bIsRunning(false)
{
}

FApplication::~FApplication()
{
    if (Device)
    {
        Device->GetGraphicsQueue()->Flush();
    }
}

bool FApplication::Initialize(HINSTANCE InstanceHandle, int32_t Width, int32_t Height)
{
    MainWindow = std::make_unique<FWindow>();
    Device = std::make_unique<FDX12Device>();
    SwapChain = std::make_unique<FDX12SwapChain>();
    CommandContext = std::make_unique<FDX12CommandContext>();

    if (!MainWindow->Create(InstanceHandle, Width, Height, L"UncleRenderer"))
    {
        return false;
    }

    if (!Device->Initialize())
    {
        return false;
    }

    if (!SwapChain->Initialize(Device.get(), MainWindow->GetHWND(), Width, Height, 3))
    {
        return false;
    }

    if (!CommandContext->Initialize(Device.get(), Device->GetGraphicsQueue()))
    {
        return false;
    }

    bIsRunning = true;
    return true;
}

int32_t FApplication::Run()
{
    while (bIsRunning)
    {
        if (!MainWindow->ProcessMessages())
        {
            bIsRunning = false;
            break;
        }

        bIsRunning = RenderFrame();
    }

    return 0;
}

bool FApplication::RenderFrame()
{
    const uint32 BackBufferIndex = SwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* BackBuffer = SwapChain->GetBackBuffer(BackBufferIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = SwapChain->GetRTV(BackBufferIndex);

    const D3D12_RESOURCE_STATES PreviousState = SwapChain->GetBackBufferState(BackBufferIndex);

    CommandContext->BeginFrame();

    CommandContext->TransitionResource(
        BackBuffer,
        PreviousState,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    CommandContext->SetRenderTarget(RtvHandle);

    const float ClearColor[4] = { 0.05f, 0.10f, 0.20f, 1.0f };
    CommandContext->ClearRenderTarget(RtvHandle, ClearColor);

    CommandContext->TransitionResource(
        BackBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);

    CommandContext->CloseAndExecute();

    SwapChain->SetBackBufferState(BackBufferIndex, D3D12_RESOURCE_STATE_PRESENT);

    const UINT PresentFlags = SwapChain->AllowsTearing() ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HR_CHECK(SwapChain->GetSwapChain()->Present(0, PresentFlags));

    const uint64 FenceValue = Device->GetGraphicsQueue()->Signal();
    Device->GetGraphicsQueue()->Wait(FenceValue);

    return true;
}

