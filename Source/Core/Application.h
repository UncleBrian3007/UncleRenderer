#pragma once

#include <Windows.h>
#include <memory>
#include <cstdint>

class FWindow;
class FDX12Device;
class FDX12SwapChain;
class FDX12CommandContext;

class FApplication
{
public:
    FApplication();
    ~FApplication();

    bool Initialize(HINSTANCE InstanceHandle, int32_t Width, int32_t Height);
    int32_t Run();

private:
    bool RenderFrame();

private:
    std::unique_ptr<FWindow>           MainWindow;
    std::unique_ptr<FDX12Device>       Device;
    std::unique_ptr<FDX12SwapChain>    SwapChain;
    std::unique_ptr<FDX12CommandContext> CommandContext;

    bool bIsRunning;
};

