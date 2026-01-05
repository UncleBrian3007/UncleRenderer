#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <memory>
#include <cstdint>
#include <string>
#include <DirectXMath.h>
#include <atomic>
#include <vector>
#include "../Scene/Camera.h"
#include "../RHI/DX12Commons.h"
#include "RendererConfig.h"

// ImGui availability is determined in ImGuiSupport.h to avoid build failures
// when the library is not present locally.
#include "ImGuiSupport.h"

#if WITH_IMGUI
struct ImGuiContext;
#endif

class FWindow;
class FDX12Device;
class FDX12SwapChain;
class FDX12CommandContext;
class FTime;
class FRenderer;
class FForwardRenderer;
class FDeferredRenderer;
class FCamera;

class FApplication
{
public:
    FApplication();
    ~FApplication();

    bool Initialize(HINSTANCE InstanceHandle);
    int32_t Run();

private:
    bool RenderFrame();
    void HandleCameraInput(float DeltaSeconds);
    void PositionCameraForScene();
    void ApplySceneCameraFromJson(const std::wstring& ScenePath);
    void UpdateSelectionFromMouseClick();
    void DrawSelectionBounds(float DisplayWidth, float DisplayHeight);
    bool ReloadScene(const std::wstring& ScenePath);
    void StartAsyncSceneReload(const std::wstring& ScenePath);
    void CompleteAsyncSceneReload();
    std::wstring OpenSceneFileDialog(const std::wstring& InitialDirectory) const;
    bool InitializeImGui(int32_t Width, int32_t Height);
    void ShutdownImGui();
    void RenderUI();
    bool EnsureImGuiFontAtlas();
    void UpdateRendererLighting() const;
    DirectX::XMVECTOR GetLightDirectionVector() const;

private:
    std::unique_ptr<FWindow>           MainWindow;
    std::unique_ptr<FDX12Device>       Device;
    std::unique_ptr<FDX12SwapChain>    SwapChain;
    std::unique_ptr<FDX12CommandContext> CommandContext;
    std::unique_ptr<FTime>             Time;
    std::unique_ptr<FForwardRenderer>  ForwardRenderer;
    std::unique_ptr<FDeferredRenderer> DeferredRenderer;
    FRenderer*                         ActiveRenderer = nullptr;
    std::unique_ptr<FCamera>           Camera;
    FRendererConfig                    RendererConfig;

    ComPtr<ID3D12DescriptorHeap>       ImGuiDescriptorHeap;
#if WITH_IMGUI
    ImGuiContext*                      ImGuiCtx = nullptr;
#endif

    bool bIsRunning;
    bool bTaskSystemEnabled = true;
    bool bDepthPrepassEnabled = false;
    bool bFrameOverlapEnabled = false;
    bool bShadowsEnabled = true;
    bool bHZBEnabled = true;
    bool bGpuTimingEnabled = false;
    bool bGpuDebugPrintEnabled = false;
    bool bTonemapEnabled = true;
    bool bIndirectDrawEnabled = true;
    bool bModelPixEventsEnabled = true;
    bool bCasEnabled = true;
    float CasSharpness = 0.2f;
    float TonemapExposure = 0.5f;
    float TonemapWhitePoint = 4.0f;
    float TonemapGamma = 1.0f;
    bool bAutoExposureEnabled = false;
    float AutoExposureKey = 0.18f;
    float AutoExposureMin = 0.1f;
    float AutoExposureMax = 5.0f;
    float AutoExposureSpeedUp = 3.0f;
    float AutoExposureSpeedDown = 1.0f;
    bool bTaaEnabled = false;
    float TaaHistoryWeight = 0.9f;
    bool bFreezeCamera = false;
    FCamera FrozenCamera;
    int32_t SelectedModelIndex = -1;
    std::string SelectedModelName;
    bool bPendingObjectIdReadback = false;
    uint32_t PendingObjectIdX = 0;
    uint32_t PendingObjectIdY = 0;
    float CameraYaw = 0.0f;
    float CameraPitch = 0.0f;
    bool bIsRotatingWithMouse = false;
    bool bWasLeftMouseDown = false;
    POINT LastMousePosition = {};
    std::wstring CurrentScenePath = L"Assets/Scenes/Scene.json";
    std::wstring PendingScenePath;
    float LightYaw = -1.19028997f;
    float LightPitch = -1.07681236f;
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    float ShadowBias = 0.0f;
    ComPtr<ID3D12QueryHeap>            FrameTimingQueryHeap;
    ComPtr<ID3D12Resource>             FrameTimingReadback;
    std::vector<uint64>                FrameTimingFenceValues;
    uint64                             FrameTimingFrequency = 0;

    // Async scene loading
    std::unique_ptr<FForwardRenderer>  AsyncForwardRenderer;
    std::unique_ptr<FDeferredRenderer> AsyncDeferredRenderer;
    FRenderer*                         AsyncActiveRenderer = nullptr;
    std::wstring                       AsyncScenePath;
    std::atomic<bool>                  bAsyncSceneLoadComplete{ false };
};
