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
#include "../RHI/DX12Device.h"
#include "RendererConfig.h"
#include "FrameGpuTimer.h"

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
    // Frame loop
    bool RenderFrame();

    // Camera
    void HandleCameraInput(float DeltaSeconds);
    void PositionCameraForScene();
    void ApplySceneCameraFromJson(const std::wstring& ScenePath);

    // Scene
    bool ReloadScene(const std::wstring& ScenePath);
    void StartAsyncSceneReload(const std::wstring& ScenePath);
    void CompleteAsyncSceneReload();
    std::wstring OpenSceneFileDialog(const std::wstring& InitialDirectory) const;

    // Lighting
    void ApplySceneLightingFromJson(const std::wstring& ScenePath);
    void UpdateRendererLighting() const;
    DirectX::XMVECTOR GetLightDirectionVector() const;

    // Selection
    void UpdateSelectionFromMouseClick();
    void ProcessObjectIdReadback();

    // Debug primitives
    void UpdateDebugPrimitives();

    // ImGui
    bool InitializeImGui(int32_t Width, int32_t Height);
    void ShutdownImGui();
    void ShutdownGraphics();
    void RenderUI();
    bool EnsureImGuiFontAtlas();

    // Renderer config sync
    void SyncRendererDepthPrepassConfig();
    void SyncDeferredLightingPassConfig();
    void SyncDeferredHzbConfig();
    void SyncRendererGtaoConfig();
    void SyncRendererPathTracingConfig();
    void SyncDeferredClusterDagConfig();
    void SyncDeferredPostProcessConfig();
    void SyncDeferredSsrConfig();
    void SyncDeferredRestirGIConfig();
    void SyncDeferredRestirGITransientState();
    void SyncDeferredSparseSdfGIConfig();
#if WITH_BINDLESS_DESCRIPTOR_STATS
    void UpdateBindlessDescriptorStatsSnapshot();
#endif

private:
    // Core subsystems
    std::unique_ptr<FWindow>             MainWindow;
    std::unique_ptr<FDX12Device>         Device;
    std::unique_ptr<FDX12SwapChain>      SwapChain;
    std::unique_ptr<FDX12CommandContext> CommandContext;
    std::unique_ptr<FTime>               Time;

    // Renderers
    std::unique_ptr<FForwardRenderer>    ForwardRenderer;
    std::unique_ptr<FDeferredRenderer>   DeferredRenderer;
    FRenderer*                           ActiveRenderer = nullptr;
    std::unique_ptr<FCamera>             Camera;
    FRendererConfig                      RendererConfig;

    // ImGui
    ComPtr<ID3D12DescriptorHeap>         ImGuiDescriptorHeap;
#if WITH_IMGUI
    ImGuiContext*                        ImGuiCtx = nullptr;
#endif
#if WITH_BINDLESS_DESCRIPTOR_STATS
    FDX12Device::FBindlessDescriptorStats CachedBindlessDescriptorStats;
    bool                                 bCachedBindlessDescriptorStatsValid = false;
    bool                                 bTrackLiveTransientBindlessOwners = false;
#endif

    // App state
    bool bIsRunning;

    // Camera
    bool   bFreezeCamera = false;
    FCamera FrozenCamera;
    float  CameraYaw = 0.0f;
    float  CameraPitch = 0.0f;
    bool   bIsRotatingWithMouse = false;
    bool   bWasLeftMouseDown = false;
    POINT  LastMousePosition = {};

    // Scene
    std::wstring CurrentScenePath = L"Assets/Scenes/Scene.json";
    std::wstring PendingScenePath;

    // Selection
    int32_t      SelectedSectionIndex = -1;
    std::string  SelectedSectionName;
    bool         bPendingObjectIdReadback = false;
    uint32_t     PendingObjectIdX = 0;
    uint32_t     PendingObjectIdY = 0;

    // GPU frame timing
    FFrameGpuTimer FrameTimer;

    // ReSTIR GI transient debug state
    bool bRestirGIFreezeFrame = false;
    bool bRestirGIDebugRayEnabled = false;
    int  RestirGIDebugPixelX = 0;
    int  RestirGIDebugPixelY = 0;

    // Screen Probe Gather transient debug state

    // Async scene loading
    std::unique_ptr<FForwardRenderer>  AsyncForwardRenderer;
    std::unique_ptr<FDeferredRenderer> AsyncDeferredRenderer;
    FRenderer*                         AsyncActiveRenderer = nullptr;
    std::wstring                       AsyncScenePath;
    std::atomic<bool>                  bAsyncSceneLoadInProgress{ false };
    std::atomic<bool>                  bAsyncSceneLoadComplete{ false };
};
