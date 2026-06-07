#include "Application.h"
#include "FrameGpuTimer.h"
#include "Window.h"
#include "EngineTime.h"
#include "ImGuiSupport.h"
#include "Logger.h"
#include "StringUtils.h"
#include "../RHI/DX12DeviceRemoved.h"
#include "GpuDebugMarkers.h"
#include "TaskSystem.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12SwapChain.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Commons.h"
#include "../Render/Renderer.h"
#include "../Render/DeferredRenderer.h"
#include "../Render/ForwardRenderer.h"
#include "../Render/TextureLoader.h"
#include "../World/SkeletalMesh.h"
#include "../Render/RenderGraph.h"
#include "../Render/RendererUtils.h"
#include "../Scene/Camera.h"
#include "../Scene/SceneJsonLoader.h"
#include "../../Shaders/PathTracing/PathTracingShared.h"
#include "RendererConfig.h"
#include <dxgi1_6.h>
#include <commdlg.h>
#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <DirectXMath.h>
#include <limits>
#include <string>
#include <filesystem>
#include <iterator>
#include <array>
#include <chrono>
#include <cwchar>
#include <fstream>
#include <unordered_set>
#include <vector>
#include <d3dx12.h>

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace
{
    std::filesystem::path GetRendererConfigPath()
    {
        return std::filesystem::current_path() / "bin/RendererConfig.ini";
    }

    std::string TrimConfigLine(const std::string& Input)
    {
        const char* Whitespace = " \t\r\n";
        const size_t Start = Input.find_first_not_of(Whitespace);
        if (Start == std::string::npos)
        {
            return std::string();
        }

        const size_t End = Input.find_last_not_of(Whitespace);
        return Input.substr(Start, End - Start + 1);
    }

    const char* RestirRandomModeToConfigString(ERestirGIRandomMode Mode)
    {
        return (Mode == ERestirGIRandomMode::BlueNoiseSobol) ? "BlueNoiseSobol" : "Hash";
    }

    const char* DeferredLightingVisualizationModeToConfigString(EDeferredLightingVisualizationMode Mode)
    {
        switch (Mode)
        {
        case EDeferredLightingVisualizationMode::DiffuseIndirect:
            return "DiffuseIndirect";
        case EDeferredLightingVisualizationMode::AO:
            return "AO";
        case EDeferredLightingVisualizationMode::DirectLighting:
            return "DirectLighting";
        case EDeferredLightingVisualizationMode::SpecularIndirect:
            return "SpecularIndirect";
            case EDeferredLightingVisualizationMode::ClusterDagClusters:
            return "ClusterDagClusters";
        case EDeferredLightingVisualizationMode::ClusterDagMip:
            return "ClusterDagMip";
        case EDeferredLightingVisualizationMode::IndirectIrradiance:
            return "IndirectIrradiance";
        case EDeferredLightingVisualizationMode::Off:
        default:
            return "Off";
        }
    }

    void UpsertConfigValue(const std::filesystem::path& ConfigPath, const std::string& Key, const std::string& Value)
    {
        std::vector<std::string> Lines;
        bool bUpdated = false;

        if (std::ifstream Input(ConfigPath); Input.is_open())
        {
            std::string Line;
            while (std::getline(Input, Line))
            {
                const std::string Trimmed = TrimConfigLine(Line);
                const size_t DelimiterPos = Trimmed.find('=');
                if (DelimiterPos != std::string::npos)
                {
                    const std::string ExistingKey = TrimConfigLine(Trimmed.substr(0, DelimiterPos));
                    std::string LowerExistingKey = ExistingKey;
                    std::string LowerTargetKey = Key;
                    std::transform(LowerExistingKey.begin(), LowerExistingKey.end(), LowerExistingKey.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
                    std::transform(LowerTargetKey.begin(), LowerTargetKey.end(), LowerTargetKey.begin(), [](unsigned char Ch) { return static_cast<char>(std::tolower(Ch)); });
                    if (LowerExistingKey == LowerTargetKey)
                    {
                        Lines.push_back(Key + "=" + Value);
                        bUpdated = true;
                        continue;
                    }
                }

                Lines.push_back(Line);
            }
        }

        if (!bUpdated)
        {
            Lines.push_back(Key + "=" + Value);
        }

        std::ofstream Output(ConfigPath, std::ios::trunc);
        if (!Output.is_open())
        {
            LogWarning("Failed to update renderer config file: " + ConfigPath.string());
            return;
        }

        for (size_t Index = 0; Index < Lines.size(); ++Index)
        {
            Output << Lines[Index];
            if (Index + 1u < Lines.size())
            {
                Output << "\n";
            }
        }
    }

#if WITH_IMGUI
    ImVec2 ProjectAxisToScreen(const DirectX::XMVECTOR& ViewSpaceDir, float Scale)
    {
        const float X = DirectX::XMVectorGetX(ViewSpaceDir);
        const float Y = DirectX::XMVectorGetY(ViewSpaceDir);
        const float Z = DirectX::XMVectorGetZ(ViewSpaceDir);

        const float Perspective = 1.0f / (std::max)(0.1f, Z + 1.2f);
        return ImVec2(X * Perspective * Scale, -Y * Perspective * Scale);
    }

    void DrawAxisGizmo(const FMatrix& ViewMatrix, const ImVec2& DisplaySize)
    {
        ImDrawList* DrawList = ImGui::GetForegroundDrawList();

        const float GizmoRadius = 14.0f;
        const float GizmoScale = 52.0f;
        const ImVec2 Margin(16.0f, 16.0f);
        const ImVec2 Center(Margin.x + GizmoRadius, DisplaySize.y - Margin.y - GizmoRadius);

        DrawList->AddCircleFilled(Center, GizmoRadius + 6.0f, IM_COL32(18, 22, 33, 220));
        DrawList->AddCircle(Center, GizmoRadius + 6.0f, IM_COL32(80, 90, 110, 230), 32, 2.0f);

        const DirectX::XMMATRIX RotationOnly = ViewMatrix;

        struct AxisInfo
        {
            DirectX::XMVECTOR Direction;
            ImU32 Color;
            const char* Label;
        };

        const AxisInfo Axes[] = {
            { DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), IM_COL32(230, 70, 70, 255), "X" },
            { DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), IM_COL32(70, 200, 120, 255), "Y" },
            { DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), IM_COL32(80, 160, 230, 255), "Z" }
        };

        for (const AxisInfo& Axis : Axes)
        {
            const DirectX::XMVECTOR ViewDir = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(Axis.Direction, RotationOnly));
            const ImVec2 Offset = ProjectAxisToScreen(ViewDir, GizmoScale);
            const ImVec2 End = ImVec2(Center.x + Offset.x, Center.y + Offset.y);

            DrawList->AddLine(Center, End, Axis.Color, 3.0f);
            DrawList->AddCircleFilled(End, 3.5f, Axis.Color);
            DrawList->AddText(ImVec2(End.x + 6.0f, End.y - 10.0f), IM_COL32(240, 240, 240, 255), Axis.Label);
        }
    }

    bool ProjectWorldToScreen(
        const DirectX::XMVECTOR& WorldPosition,
        const DirectX::XMMATRIX& ViewProjection,
        float DisplayWidth,
        float DisplayHeight,
        ImVec2& OutScreen)
    {
        const DirectX::XMVECTOR Clip = DirectX::XMVector4Transform(WorldPosition, ViewProjection);
        const float W = DirectX::XMVectorGetW(Clip);
        if (W <= 0.0f)
        {
            return false;
        }

        const float InvW = 1.0f / W;
        const float NdcX = DirectX::XMVectorGetX(Clip) * InvW;
        const float NdcY = DirectX::XMVectorGetY(Clip) * InvW;

        OutScreen.x = (NdcX * 0.5f + 0.5f) * DisplayWidth;
        OutScreen.y = (1.0f - (NdcY * 0.5f + 0.5f)) * DisplayHeight;
        return true;
    }
#endif
}

FApplication::FApplication()
    : bIsRunning(false)
    , CameraYaw(0.0f)
    , CameraPitch(0.0f)
    , bIsRotatingWithMouse(false)
    , LastMousePosition{}
{
}

FApplication::~FApplication()
{
    LogInfo("Application shutdown started");
    FTaskScheduler::Get().Shutdown();
    ShutdownGraphics();

    LogInfo("Application shutdown complete");
}

void FApplication::ShutdownGraphics()
{
    ShutdownImGui();

    if (Device && Device->GetGraphicsQueue())
    {
        Device->GetGraphicsQueue()->Flush();
    }

    ActiveRenderer = nullptr;
    AsyncActiveRenderer = nullptr;
    AsyncForwardRenderer.reset();
    AsyncDeferredRenderer.reset();
    DeferredRenderer.reset();
    ForwardRenderer.reset();
    Camera.reset();
    FrameTimer.Reset();
    ImGuiDescriptorHeap.Reset();
    FTextureLoader::ClearGlobalCache();
    FRenderGraph::ReleaseStaticGpuResources();

    CommandContext.reset();
    SwapChain.reset();
    if (Device && Device->GetGraphicsQueue())
    {
        Device->GetGraphicsQueue()->Flush();
    }
    Device.reset();

    FDX12Device::ReportLiveObjects();
}

bool FApplication::Initialize(HINSTANCE InstanceHandle)
{
    LogInfo("Application initialization started");

    const std::filesystem::path ConfigPath = GetRendererConfigPath();
    RendererConfig = FRendererConfigLoader::LoadOrDefault(ConfigPath);

    if (RendererConfig.bEnableTaskSystem)
    {
        // Initialize task system early
        FTaskScheduler::Get().Initialize();
    }
    else
    {
        LogInfo("Task system disabled via renderer config; running tasks on main thread");
    }

    const int32_t WindowWidth = static_cast<int32_t>(RendererConfig.WindowWidth);
    const int32_t WindowHeight = static_cast<int32_t>(RendererConfig.WindowHeight);

    MainWindow = std::make_unique<FWindow>();
    Device = std::make_unique<FDX12Device>();
    SwapChain = std::make_unique<FDX12SwapChain>();
    CommandContext = std::make_unique<FDX12CommandContext>();
    Time = std::make_unique<FTime>();
    ForwardRenderer = std::make_unique<FForwardRenderer>();
    DeferredRenderer = std::make_unique<FDeferredRenderer>();
    Camera = std::make_unique<FCamera>();
    SetSectionPixEventsEnabled(RendererConfig.bEnableSectionPixEvents);

    const std::wstring SceneFilePath = RendererConfig.SceneFile.empty() ? L"Assets/Scenes/Scene.json" : RendererConfig.SceneFile;
    CurrentScenePath = SceneFilePath;

    ApplySceneLightingFromJson(SceneFilePath);

    LogInfo("Creating window...");
    if (!MainWindow->Create(InstanceHandle, WindowWidth, WindowHeight, L"UncleRenderer"))
    {
        LogError("Failed to create window");
        return false;
    }

    LogInfo("Initializing D3D12 device...");
    if (!Device->Initialize())
    {
        LogError("Failed to initialize D3D12 device");
        return false;
    }

    if (RendererConfig.bEnableRayTracedShadows && !Device->IsRayTracingSupported())
    {
        RendererConfig.bEnableRayTracedShadows = false;
        LogWarning("Ray traced shadows requested, but DXR is not supported. Falling back to raster shadows.");
    }

    if (RendererConfig.bEnablePathTracing && !Device->IsRayTracingSupported())
    {
        RendererConfig.bEnablePathTracing = false;
        LogWarning("Path tracing requested, but DXR is not supported. Disabling path tracing.");
    }

    if (RendererConfig.bEnableIndirectDraw && !Device->IsShaderModelForIndirectDrawSupported())
    {
        RendererConfig.bEnableIndirectDraw = false;
        RendererConfig.bEnableSkinningIndirectDraw = false;
        LogWarning("Indirect draw disabled because the device does not support Shader Model 6.8.");
    }

    const uint32 SwapChainBufferCount = (std::max)(2u, RendererConfig.FramesInFlight);

    LogInfo("Initializing swap chain...");
    if (!SwapChain->Initialize(Device.get(), MainWindow->GetHWND(), WindowWidth, WindowHeight, SwapChainBufferCount))
    {
        LogError("Failed to initialize swap chain");
        return false;
    }

    LogInfo("Initializing command context...");
    if (!CommandContext->Initialize(Device.get(), Device->GetGraphicsQueue(), SwapChain->GetBackBufferCount()))
    {
        LogError("Failed to initialize command context");
        return false;
    }

    RendererConfig.FramesInFlight = SwapChain->GetBackBufferCount();

    Camera->SetPerspective(DirectX::XM_PIDIV4, static_cast<float>(WindowWidth) / static_cast<float>(WindowHeight), 0.1f, 1000.0f);

    auto TryInitializeRenderer = [&](ERendererType Type) -> bool
    {
        if (Type == ERendererType::Deferred)
        {
            LogInfo("Attempting to initialize deferred renderer...");
            if (DeferredRenderer->Initialize(Device.get(), WindowWidth, WindowHeight, SwapChain->GetFormat(), RendererConfig))
            {
                LogInfo("Deferred renderer activated");
                ActiveRenderer = DeferredRenderer.get();
                return true;
            }

            LogWarning("Deferred renderer initialization failed");
            return false;
        }

        LogInfo("Attempting to initialize forward renderer...");
        if (ForwardRenderer->Initialize(Device.get(), WindowWidth, WindowHeight, SwapChain->GetFormat(), RendererConfig))
        {
            LogInfo("Forward renderer activated");
            ActiveRenderer = ForwardRenderer.get();
            return true;
        }

        LogWarning("Forward renderer initialization failed");
        return false;
    };

    const bool bPreferDeferred = RendererConfig.RendererType == ERendererType::Deferred;
    const bool bRendererReady = bPreferDeferred ?
        (TryInitializeRenderer(ERendererType::Deferred) || TryInitializeRenderer(ERendererType::Forward)) :
        (TryInitializeRenderer(ERendererType::Forward) || TryInitializeRenderer(ERendererType::Deferred));

    if (!bRendererReady)
    {
        LogError("Failed to initialize renderer: both deferred and forward renderers failed to initialize");
        return false;
    }

    SyncRendererDepthPrepassConfig();
    SyncDeferredHzbConfig();
    UpdateRendererLighting();
    ApplySceneCameraFromJson(RendererConfig.SceneFile);
    SyncDeferredRestirGITransientState();

    if (!InitializeImGui(WindowWidth, WindowHeight))
    {
        LogError("Failed to initialize ImGui");
        return false;
    }

    bIsRunning = true;
    LogInfo("Application initialization complete");
    return true;
}

int32_t FApplication::Run()
{
    LogInfo("Main loop started");

    while (bIsRunning)
    {
        if (!MainWindow->ProcessMessages())
        {
            LogInfo("Detected window message loop exit");
            bIsRunning = false;
            break;
        }

        bIsRunning = RenderFrame();
    }

    LogInfo("Main loop ended");
    return 0;
}

bool FApplication::RenderFrame()
{
    static uint64 FrameIndex = 0;
    ++FrameIndex;
    LogVerbose("Frame start: " + std::to_string(FrameIndex));

    // Check if async scene load is complete (atomic read)
    if (bAsyncSceneLoadComplete.load(std::memory_order_acquire))
    {
        CompleteAsyncSceneReload();
    }

    if (!bAsyncSceneLoadInProgress.load(std::memory_order_acquire) && !PendingScenePath.empty())
    {
        const std::wstring SceneToLoad = std::move(PendingScenePath);
        PendingScenePath.clear();

        // Start async scene reload
        StartAsyncSceneReload(SceneToLoad);
    }

    Time->Tick();
    const float DeltaSeconds = static_cast<float>(Time->GetDeltaTimeSeconds());

    HandleCameraInput(DeltaSeconds);

    const uint32 BackBufferIndex = SwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* BackBuffer = SwapChain->GetBackBuffer(BackBufferIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = SwapChain->GetRTV(BackBufferIndex);

    const D3D12_RESOURCE_STATES PreviousState = SwapChain->GetBackBufferState(BackBufferIndex);

    if (RendererConfig.bEnableGpuTiming && Device && Device->GetGraphicsQueue())
    {
        FrameTimer.BeginFrame(
            Device->GetDevice(),
            Device->GetGraphicsQueue()->GetD3DQueue(),
            Device->GetGraphicsQueue()->GetCompletedFenceValue(),
            BackBufferIndex,
            SwapChain->GetBackBufferCount());
    }

    CommandContext->BeginFrame(BackBufferIndex);

    {
        wchar_t FrameLabel[64] = {};
        swprintf_s(FrameLabel, L"Frame %llu", static_cast<unsigned long long>(FrameIndex));
        FScopedPixEvent FrameEvent(CommandContext->GetCommandList(), FrameLabel);

        if (RendererConfig.bEnableGpuTiming)
        {
            FrameTimer.RecordBeginTimestamp(CommandContext->GetCommandList(), BackBufferIndex);
        }

        CommandContext->TransitionResource(
            BackBuffer,
            PreviousState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        if (ActiveRenderer)
        {
            ActiveRenderer->SetFrameIndex(BackBufferIndex);
            ActiveRenderer->SetFrameNumber(FrameIndex);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE* DsvHandle = ActiveRenderer ? &ActiveRenderer->GetDSVHandle() : nullptr;

        CommandContext->SetRenderTarget(RtvHandle, DsvHandle);

        const float ClearColor[4] = { 0.05f, 0.10f, 0.20f, 1.0f };
        CommandContext->ClearRenderTarget(RtvHandle, ClearColor);

          if (ActiveRenderer && Camera)
          {
              UpdateDebugPrimitives();

              if (bPendingObjectIdReadback)
              {
                  ActiveRenderer->RequestObjectIdReadback(PendingObjectIdX, PendingObjectIdY);
            }

            if (bFreezeCamera)
            {
                ActiveRenderer->SetCullingCameraOverride(&FrozenCamera);
            }
            else
            {
                ActiveRenderer->SetCullingCameraOverride(nullptr);
            }

            ActiveRenderer->RenderFrame(*CommandContext, RtvHandle, *Camera, DeltaSeconds);
        }

        RenderUI();

        CommandContext->TransitionResource(
            BackBuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);

        FScopedPixEvent PresentEvent(CommandContext->GetCommandList(), L"Present");

        if (RendererConfig.bEnableGpuTiming)
        {
            FrameTimer.RecordEndTimestamp(CommandContext->GetCommandList(), BackBufferIndex);
        }
    }
    CommandContext->CloseAndExecute();

    ProcessObjectIdReadback();

    LogVerbose("Preparing frame end: " + std::to_string(FrameIndex));

    SwapChain->SetBackBufferState(BackBufferIndex, D3D12_RESOURCE_STATE_PRESENT);

    const UINT SyncInterval = RendererConfig.bEnableVSync ? 1u : 0u;
    const UINT PresentFlags = (!RendererConfig.bEnableVSync && SwapChain->AllowsTearing()) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    LogVerbose("Present called (SyncInterval: " + std::to_string(SyncInterval) + ", Flags: " + std::to_string(PresentFlags) + ")");
    const HRESULT PresentHr = SwapChain->GetSwapChain()->Present(SyncInterval, PresentFlags);
    if (FAILED(PresentHr))
    {
        ReportDxFailure(Device ? Device->GetDevice() : nullptr, PresentHr, L"IDXGISwapChain::Present");
        return false;
    }

    const uint64 FenceValue = Device->GetGraphicsQueue()->Signal();
    FRenderGraph::FinalizeReleasedTransientFences(Device.get(), FenceValue);
    if (!RendererConfig.bEnableFrameOverlap)
    {
        Device->GetGraphicsQueue()->Wait(FenceValue);
    }
    CommandContext->SetFrameFenceValue(BackBufferIndex, FenceValue);
    if (ActiveRenderer)
    {
        ActiveRenderer->OnFrameFenceSignaled(BackBufferIndex, FenceValue);
    }
    FrameTimer.OnFenceSignaled(BackBufferIndex, FenceValue);
#if WITH_BINDLESS_DESCRIPTOR_STATS
    UpdateBindlessDescriptorStatsSnapshot();
#endif

    DrainD3D12DebugMessages(Device ? Device->GetDevice() : nullptr);

    LogVerbose("Frame completed: " + std::to_string(FrameIndex));

    return true;
}

#if WITH_BINDLESS_DESCRIPTOR_STATS
void FApplication::UpdateBindlessDescriptorStatsSnapshot()
{
    if (!Device)
    {
        bCachedBindlessDescriptorStatsValid = false;
        CachedBindlessDescriptorStats = {};
        return;
    }

    Device->PumpTransientBindlessDescriptorReclaim();
    CachedBindlessDescriptorStats = Device->GetBindlessDescriptorStats();
    Device->ResetBindlessDescriptorFrameStats();
    bCachedBindlessDescriptorStatsValid = true;
}
#endif

void FApplication::HandleCameraInput(float DeltaSeconds)
{
    if (!Camera)
    {
        return;
    }

#if WITH_IMGUI
    if (ImGuiCtx)
    {
        ImGui::SetCurrentContext(ImGuiCtx);
        const ImGuiIO& Io = ImGui::GetIO();

        if (Io.WantCaptureMouse || Io.WantCaptureKeyboard)
        {
            bIsRotatingWithMouse = false;
            return;
        }
    }
#endif

    auto IsKeyDown = [](int32 VirtualKey) -> bool
    {
        return (GetAsyncKeyState(VirtualKey) & 0x8000) != 0;
    };

	const HWND WindowHandle = MainWindow ? MainWindow->GetHWND() : nullptr;
	const bool bWindowInForeground = WindowHandle && GetForegroundWindow() == WindowHandle;
	if (!bWindowInForeground)
	{
		bIsRotatingWithMouse = false;
        bWasLeftMouseDown = false;
		return;
	}

    using namespace DirectX;

    const float SceneRadius = ActiveRenderer ? ActiveRenderer->GetSceneRadius() : 1.0f;
    const float MoveSpeed = (std::max)(5.0f, SceneRadius * 0.5f);
    const float FovSpeed = XMConvertToRadians(45.0f);
    const float MinFov = XMConvertToRadians(20.0f);
    const float MaxFov = XMConvertToRadians(120.0f);
    const float RotationSpeed = 0.005f;

    const bool LeftButtonDown = IsKeyDown(VK_LBUTTON);
    if (LeftButtonDown && !bWasLeftMouseDown)
    {
        UpdateSelectionFromMouseClick();
    }
    bWasLeftMouseDown = LeftButtonDown;

    const bool RightButtonDown = IsKeyDown(VK_RBUTTON);
    if (RightButtonDown)
    {
        POINT CursorPos{};
        if (GetCursorPos(&CursorPos))
        {
            if (!bIsRotatingWithMouse)
            {
                bIsRotatingWithMouse = true;
                {
                    const XMVECTOR ForwardVec = XMVector3Normalize(XMLoadFloat3(&Camera->GetForward()));
                    const float ForwardY = std::clamp(XMVectorGetY(ForwardVec), -1.0f, 1.0f);
                    CameraPitch = -asinf(ForwardY);
                    CameraYaw = atan2f(XMVectorGetX(ForwardVec), XMVectorGetZ(ForwardVec));
                }
                LastMousePosition = CursorPos;
            }
            else
            {
                const LONG DeltaX = CursorPos.x - LastMousePosition.x;
                const LONG DeltaY = CursorPos.y - LastMousePosition.y;

                CameraYaw += static_cast<float>(DeltaX) * RotationSpeed;
                CameraPitch += static_cast<float>(DeltaY) * RotationSpeed;

                const float PitchLimit = DirectX::XM_PIDIV2 - 0.01f;
                CameraPitch = std::clamp(CameraPitch, -PitchLimit, PitchLimit);

                const XMVECTOR DefaultForward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
                const XMVECTOR DefaultUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                const XMMATRIX Rotation = XMMatrixRotationRollPitchYaw(CameraPitch, CameraYaw, 0.0f);

                XMVECTOR NewForwardVec = XMVector3Normalize(XMVector3TransformNormal(DefaultForward, Rotation));
                XMVECTOR NewUpVec = XMVector3Normalize(XMVector3TransformNormal(DefaultUp, Rotation));

                FFloat3 NewForward;
                FFloat3 NewUp;
                XMStoreFloat3(&NewForward, NewForwardVec);
                XMStoreFloat3(&NewUp, NewUpVec);

                Camera->SetForward(NewForward);
                Camera->SetUp(NewUp);
            }

            LastMousePosition = CursorPos;
        }
    }
    else
    {
        bIsRotatingWithMouse = false;
    }

    XMVECTOR Forward = XMVector3Normalize(XMLoadFloat3(&Camera->GetForward()));
    XMVECTOR Up = XMVector3Normalize(XMLoadFloat3(&Camera->GetUp()));
    XMVECTOR Right = XMVector3Normalize(XMVector3Cross(Up, Forward));

    XMVECTOR MoveDirection = XMVectorZero();
    if (IsKeyDown('W')) MoveDirection += Forward;
    if (IsKeyDown('S')) MoveDirection -= Forward;
    if (IsKeyDown('A')) MoveDirection -= Right;
    if (IsKeyDown('D')) MoveDirection += Right;

    if (!XMVector3Equal(MoveDirection, XMVectorZero()))
    {
        MoveDirection = XMVector3Normalize(MoveDirection);
        XMVECTOR Position = XMLoadFloat3(&Camera->GetPosition());
        Position += MoveDirection * MoveSpeed * DeltaSeconds;
        FFloat3 NewPosition;
        XMStoreFloat3(&NewPosition, Position);
        Camera->SetPosition(NewPosition);
    }

    float FovY = Camera->GetFovY();
    if (IsKeyDown(VK_OEM_PLUS) || IsKeyDown(VK_ADD))
    {
        FovY -= FovSpeed * DeltaSeconds;
    }
    if (IsKeyDown(VK_OEM_MINUS) || IsKeyDown(VK_SUBTRACT))
    {
        FovY += FovSpeed * DeltaSeconds;
    }

    FovY = std::clamp(FovY, MinFov, MaxFov);
    Camera->SetFovY(FovY);
}

void FApplication::ProcessObjectIdReadback()
{
    if (!bPendingObjectIdReadback || !ActiveRenderer || !Device || !Device->GetGraphicsQueue())
    {
        return;
    }

    Device->GetGraphicsQueue()->Flush();

    uint32_t ObjectId = 0;
    if (ActiveRenderer->ConsumeObjectIdReadback(ObjectId))
    {
        auto Sections = ActiveRenderer->GetWorld().BuildSectionList();
        if (ObjectId > 0 && ObjectId <= Sections.size())
        {
            SelectedSectionIndex = static_cast<int32_t>(ObjectId - 1);
            SelectedSectionName = Sections[SelectedSectionIndex].Name;
            if (SelectedSectionName.empty())
            {
                SelectedSectionName = "Unnamed";
            }
        }
        else
        {
            SelectedSectionIndex = -1;
            SelectedSectionName.clear();
        }
    }
    bPendingObjectIdReadback = false;
}

void FApplication::UpdateSelectionFromMouseClick()
{
    if (!ActiveRenderer || !MainWindow)
    {
        return;
    }

    POINT CursorPos{};
    if (!GetCursorPos(&CursorPos))
    {
        return;
    }

    if (!ScreenToClient(MainWindow->GetHWND(), &CursorPos))
    {
        return;
    }

    if (CursorPos.x < 0 || CursorPos.y < 0)
    {
        return;
    }

    PendingObjectIdX = static_cast<uint32_t>(CursorPos.x);
    PendingObjectIdY = static_cast<uint32_t>(CursorPos.y);
    bPendingObjectIdReadback = true;

    RestirGIDebugPixelX = (CursorPos.x > 0) ? static_cast<int>(CursorPos.x / 2) : 0;
    RestirGIDebugPixelY = (CursorPos.y > 0) ? static_cast<int>(CursorPos.y / 2) : 0;
        SyncDeferredRestirGITransientState();
}

void FApplication::SyncDeferredPostProcessConfig()
{
    if (!DeferredRenderer || ActiveRenderer != DeferredRenderer.get())
    {
        return;
    }

    DeferredRenderer->ApplyPostProcessConfig(RendererConfig);
}

void FApplication::SyncRendererDepthPrepassConfig()
{
    if (ForwardRenderer)
    {
        ForwardRenderer->SetDepthPrepassEnabled(RendererConfig.bUseDepthPrepass);
    }

    if (DeferredRenderer)
    {
        DeferredRenderer->SetDepthPrepassEnabled(RendererConfig.bUseDepthPrepass);
    }
}

void FApplication::SyncDeferredLightingPassConfig()
{
    if (!DeferredRenderer)
    {
        return;
    }

    DeferredRenderer->ApplyLightingPassConfig(RendererConfig);
}

void FApplication::SyncDeferredHzbConfig()
{
    if (DeferredRenderer)
    {
        DeferredRenderer->SetHZBEnabled(RendererConfig.bEnableHZB);
        DeferredRenderer->SetHzbTwoPassEnabled(RendererConfig.bEnableHzbTwoPass);
    }
}

void FApplication::SyncRendererGtaoConfig()
{
    if (DeferredRenderer)
    {
        DeferredRenderer->ApplyGtaoConfig(RendererConfig);
    }

    if (ForwardRenderer)
    {
        ForwardRenderer->ApplyGtaoConfig(RendererConfig);
    }
}

void FApplication::SyncRendererPathTracingConfig()
{
    if (DeferredRenderer)
    {
        DeferredRenderer->ApplyPathTracingConfig(RendererConfig);
    }

    if (ForwardRenderer)
    {
        ForwardRenderer->ApplyPathTracingConfig(RendererConfig);
    }
}

void FApplication::SyncDeferredClusterDagConfig()
{
    if (!DeferredRenderer)
    {
        return;
    }

    DeferredRenderer->ApplyClusterDAGConfig(RendererConfig);
}

void FApplication::SyncDeferredSsrConfig()
{
    if (!DeferredRenderer || ActiveRenderer != DeferredRenderer.get())
    {
        return;
    }

    DeferredRenderer->ApplySsrConfig(RendererConfig);
}

void FApplication::SyncDeferredRestirGIConfig()
{
    if (!DeferredRenderer || ActiveRenderer != DeferredRenderer.get())
    {
        return;
    }

    DeferredRenderer->ApplyRestirGIConfig(RendererConfig);
    SyncDeferredRestirGITransientState();
}

void FApplication::SyncDeferredSparseSdfGIConfig()
{
    if (!DeferredRenderer || ActiveRenderer != DeferredRenderer.get())
    {
        return;
    }

    DeferredRenderer->ApplySparseSdfGIConfig(RendererConfig);
}

void FApplication::SyncDeferredRestirGITransientState()
{
    if (!DeferredRenderer || ActiveRenderer != DeferredRenderer.get())
    {
        return;
    }

    RestirGIDebugPixelX = (std::max)(0, RestirGIDebugPixelX);
    RestirGIDebugPixelY = (std::max)(0, RestirGIDebugPixelY);

    FDeferredRenderer::FRestirGITransientState TransientState;
    TransientState.bFreezeFrame = bRestirGIFreezeFrame;
    TransientState.bDebugRayEnabled = bRestirGIDebugRayEnabled;
    TransientState.DebugPixelX = static_cast<uint32_t>(RestirGIDebugPixelX);
    TransientState.DebugPixelY = static_cast<uint32_t>(RestirGIDebugPixelY);
    DeferredRenderer->ApplyRestirGITransientState(TransientState);
}

void FApplication::UpdateDebugPrimitives()
{
    if (!ActiveRenderer)
    {
        return;
    }

    std::vector<FRenderer::FGpuDebugLineEntry> DebugLines;
    std::vector<FRenderer::FGpuDebugBoxEntry> DebugBoxes;

    auto Sections = ActiveRenderer->GetWorld().BuildSectionList();
    const bool bHasDrawSections = !Sections.empty();
    const bool bHasSelectedSection =
        bHasDrawSections
        && SelectedSectionIndex >= 0
        && SelectedSectionIndex < static_cast<int32_t>(Sections.size());

    if (!bHasSelectedSection)
    {
        ActiveRenderer->GetGpuDebugState().SetCpuDebugLines(DebugLines);
        ActiveRenderer->GetGpuDebugState().SetCpuDebugBoxes(DebugBoxes);
        return;
    }

    const size_t MaxDebugLines = FRenderer::GpuDebugLineMaxEntries;
    const size_t MaxDebugBoxes = FRenderer::GpuDebugBoxMaxEntries;
    DebugLines.reserve(MaxDebugLines);
    DebugBoxes.reserve(MaxDebugBoxes);
    const auto PackColor = [](uint8_t Alpha, uint8_t Red, uint8_t Green, uint8_t Blue) -> uint32_t
    {
        return (static_cast<uint32_t>(Alpha) << 24)
            | (static_cast<uint32_t>(Red) << 16)
            | (static_cast<uint32_t>(Green) << 8)
            | static_cast<uint32_t>(Blue);
    };

    const auto AppendLine = [&](const DirectX::XMFLOAT3& P0, const DirectX::XMFLOAT3& P1, uint32_t PackedColor) -> bool
    {
        if (DebugLines.size() >= MaxDebugLines)
        {
            return false;
        }

        FRenderer::FGpuDebugLineEntry Entry;
        Entry.P0 = P0;
        Entry.P1 = P1;
        Entry.PackedColor = PackedColor;
        DebugLines.push_back(Entry);
        return true;
    };

    const auto AppendSelectionBounds = [&](const FMeshSection& Section)
    {
        if (DebugLines.size() + 12u > MaxDebugLines)
        {
            return;
        }

        const DirectX::XMFLOAT3& Min = Section.BoundsMin;
        const DirectX::XMFLOAT3& Max = Section.BoundsMax;
        const std::array<DirectX::XMFLOAT3, 8> Corners =
        {
            DirectX::XMFLOAT3(Min.x, Min.y, Min.z),
            DirectX::XMFLOAT3(Max.x, Min.y, Min.z),
            DirectX::XMFLOAT3(Min.x, Max.y, Min.z),
            DirectX::XMFLOAT3(Max.x, Max.y, Min.z),
            DirectX::XMFLOAT3(Min.x, Min.y, Max.z),
            DirectX::XMFLOAT3(Max.x, Min.y, Max.z),
            DirectX::XMFLOAT3(Min.x, Max.y, Max.z),
            DirectX::XMFLOAT3(Max.x, Max.y, Max.z)
        };
        const uint32_t SelectionBoundsColor = PackColor(220u, 255u, 200u, 64u);

        const auto DrawEdge = [&](size_t A, size_t B)
        {
            AppendLine(Corners[A], Corners[B], SelectionBoundsColor);
        };

        DrawEdge(0u, 1u);
        DrawEdge(1u, 3u);
        DrawEdge(3u, 2u);
        DrawEdge(2u, 0u);
        DrawEdge(4u, 5u);
        DrawEdge(5u, 7u);
        DrawEdge(7u, 6u);
        DrawEdge(6u, 4u);
        DrawEdge(0u, 4u);
        DrawEdge(1u, 5u);
        DrawEdge(2u, 6u);
        DrawEdge(3u, 7u);
    };

    const FMeshSection& SelectedSection = Sections[SelectedSectionIndex];
    AppendSelectionBounds(SelectedSection);

    ActiveRenderer->GetGpuDebugState().SetCpuDebugLines(DebugLines);
    ActiveRenderer->GetGpuDebugState().SetCpuDebugBoxes(DebugBoxes);
}

void FApplication::PositionCameraForScene()
{
    if (!Camera)
    {
        return;
    }

    const DirectX::XMFLOAT3 SceneCenter = ActiveRenderer ? ActiveRenderer->GetSceneCenter() : DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f };
    const float SceneRadius = ActiveRenderer ? ActiveRenderer->GetSceneRadius() : 1.0f;

    const float AngularHalfHeight = Camera->GetFovY() * 0.5f;
    const float Distance = SceneRadius / std::tan(AngularHalfHeight);

    const float NearClip = 0.1f;
    Camera->SetPerspective(Camera->GetFovY(), Camera->GetAspectRatio(), NearClip, std::numeric_limits<float>::infinity());

    FFloat3 Position =
    {
        SceneCenter.x,
        SceneCenter.y,
        SceneCenter.z - Distance
    };
    Camera->SetPosition(Position);

    const DirectX::XMVECTOR Eye = DirectX::XMLoadFloat3(&Camera->GetPosition());
    const DirectX::XMVECTOR Target = DirectX::XMLoadFloat3(&SceneCenter);
    const DirectX::XMVECTOR ForwardVec = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(Target, Eye));
    const DirectX::XMVECTOR UpVec = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    FFloat3 Forward;
    FFloat3 Up;
    DirectX::XMStoreFloat3(&Forward, ForwardVec);
    DirectX::XMStoreFloat3(&Up, UpVec);
    Camera->SetForward(Forward);
    Camera->SetUp(Up);

    CameraPitch = -asinf(DirectX::XMVectorGetY(ForwardVec));
    CameraYaw = atan2f(DirectX::XMVectorGetX(ForwardVec), DirectX::XMVectorGetZ(ForwardVec));

    const DirectX::XMVECTOR DefaultUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const DirectX::XMMATRIX Rotation = DirectX::XMMatrixRotationRollPitchYaw(CameraPitch, CameraYaw, 0.0f);
    const DirectX::XMVECTOR RecomputedUp = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DefaultUp, Rotation));
    DirectX::XMStoreFloat3(&Up, RecomputedUp);
    Camera->SetUp(Up);
}

void FApplication::ApplySceneCameraFromJson(const std::wstring& ScenePath)
{
    if (!Camera)
    {
        return;
    }

    FSceneCameraDesc SceneCamera;
    if (!FSceneJsonLoader::LoadSceneCamera(ScenePath, SceneCamera))
    {
        PositionCameraForScene();
        return;
    }

    const float FovRadians = DirectX::XMConvertToRadians(SceneCamera.FovYDegrees);
    Camera->SetPerspective(FovRadians, Camera->GetAspectRatio(), Camera->GetNearClip(), Camera->GetFarClip());
    Camera->SetPosition(SceneCamera.Position);

    const DirectX::XMVECTOR UpVec = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    DirectX::XMVECTOR ForwardVec = DirectX::XMLoadFloat3(&Camera->GetForward());

    if (SceneCamera.bHasLookAt)
    {
        const DirectX::XMVECTOR Eye = DirectX::XMLoadFloat3(&SceneCamera.Position);
        const DirectX::XMVECTOR Target = DirectX::XMLoadFloat3(&SceneCamera.LookAt);
        ForwardVec = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(Target, Eye));
    }
    else if (SceneCamera.bHasRotation)
    {
        const float Pitch = DirectX::XMConvertToRadians(SceneCamera.RotationEuler.x);
        const float Yaw = DirectX::XMConvertToRadians(SceneCamera.RotationEuler.y);
        const float Roll = DirectX::XMConvertToRadians(SceneCamera.RotationEuler.z);
        const DirectX::XMMATRIX Rotation = DirectX::XMMatrixRotationRollPitchYaw(Pitch, Yaw, Roll);
        ForwardVec = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), Rotation));
    }

    FFloat3 Forward;
    FFloat3 Up;
    DirectX::XMStoreFloat3(&Forward, ForwardVec);
    DirectX::XMStoreFloat3(&Up, UpVec);
    Camera->SetForward(Forward);
    Camera->SetUp(Up);

    CameraPitch = -asinf(DirectX::XMVectorGetY(ForwardVec));
    CameraYaw = atan2f(DirectX::XMVectorGetX(ForwardVec), DirectX::XMVectorGetZ(ForwardVec));

    const DirectX::XMVECTOR DefaultUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const DirectX::XMMATRIX Rotation = DirectX::XMMatrixRotationRollPitchYaw(CameraPitch, CameraYaw, 0.0f);
    const DirectX::XMVECTOR RecomputedUp = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DefaultUp, Rotation));
    DirectX::XMStoreFloat3(&Up, RecomputedUp);
    Camera->SetUp(Up);
}

void FApplication::ApplySceneLightingFromJson(const std::wstring& ScenePath)
{
    FSceneLightDesc SceneLight;
    if (!FSceneJsonLoader::LoadSceneLighting(ScenePath, SceneLight))
    {
        return;
    }

    RendererConfig.LightIntensity = SceneLight.Intensity;
    RendererConfig.LightColor = DirectX::XMFLOAT3(SceneLight.Color.x, SceneLight.Color.y, SceneLight.Color.z);

    DirectX::XMFLOAT3 Direction(SceneLight.Direction.x, SceneLight.Direction.y, SceneLight.Direction.z);
    const DirectX::XMVECTOR DirectionVec = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&Direction));
    const float LengthSq = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(DirectionVec));
    if (LengthSq > 0.0f)
    {
        RendererConfig.LightPitch = asinf(DirectX::XMVectorGetY(DirectionVec));
        RendererConfig.LightYaw = atan2f(DirectX::XMVectorGetX(DirectionVec), DirectX::XMVectorGetZ(DirectionVec));
    }
}

bool FApplication::ReloadScene(const std::wstring& ScenePath)
{
    if (ScenePath.empty())
    {
        LogWarning("Cannot reload scene: path is empty");
        return false;
    }

    if (!Device || !SwapChain || !MainWindow)
    {
        LogError("Cannot reload scene: renderer prerequisites are missing");
        return false;
    }

    if (Device->GetGraphicsQueue())
    {
        Device->GetGraphicsQueue()->Flush();
    }

    // Create a temporary config for scene reloading
    FRendererConfig ReloadConfig = RendererConfig;
    ReloadConfig.SceneFile = ScenePath;
    ReloadConfig.ShadowBias = ActiveRenderer ? ActiveRenderer->GetShadowBias() : RendererConfig.ShadowBias;
    ReloadConfig.FramesInFlight = SwapChain ? SwapChain->GetBackBufferCount() : 2u;

    const uint32_t Width = static_cast<uint32_t>(MainWindow->GetWidth());
    const uint32_t Height = static_cast<uint32_t>(MainWindow->GetHeight());
    const DXGI_FORMAT BackBufferFormat = SwapChain->GetFormat();

    auto NewForwardRenderer = std::make_unique<FForwardRenderer>();
    auto NewDeferredRenderer = std::make_unique<FDeferredRenderer>();

    FRenderer* NewActiveRenderer = nullptr;

    auto TryInitializeRenderer = [&](ERendererType Type) -> bool
    {
        if (Type == ERendererType::Deferred)
        {
            if (NewDeferredRenderer->Initialize(Device.get(), Width, Height, BackBufferFormat, ReloadConfig))
            {
                NewActiveRenderer = NewDeferredRenderer.get();
                return true;
            }
            return false;
        }

        if (NewForwardRenderer->Initialize(Device.get(), Width, Height, BackBufferFormat, ReloadConfig))
        {
            NewActiveRenderer = NewForwardRenderer.get();
            return true;
        }
        return false;
    };

    const bool bPreferDeferred = ActiveRenderer == DeferredRenderer.get() || RendererConfig.RendererType == ERendererType::Deferred;
    const bool bInitialized = bPreferDeferred ?
        (TryInitializeRenderer(ERendererType::Deferred) || TryInitializeRenderer(ERendererType::Forward)) :
        (TryInitializeRenderer(ERendererType::Forward) || TryInitializeRenderer(ERendererType::Deferred));

    if (!bInitialized || NewActiveRenderer == nullptr)
    {
        LogError("Failed to reload scene: renderer initialization failed for new scene");
        return false;
    }

    ForwardRenderer = std::move(NewForwardRenderer);
    DeferredRenderer = std::move(NewDeferredRenderer);
    ActiveRenderer = NewActiveRenderer;
    SelectedSectionIndex = -1;
    SelectedSectionName.clear();
    bPendingObjectIdReadback = false;

    CurrentScenePath = ScenePath;
    RendererConfig.SceneFile = ScenePath;

    SyncRendererDepthPrepassConfig();
    SyncDeferredHzbConfig();
    SyncDeferredRestirGITransientState();

    ApplySceneLightingFromJson(ScenePath);
    UpdateRendererLighting();
    ApplySceneCameraFromJson(ScenePath);

    LogInfo("Scene reloaded from: " + StringUtils::WideToUtf8(ScenePath));
    return true;
}

void FApplication::StartAsyncSceneReload(const std::wstring& ScenePath)
{
    if (ScenePath.empty())
    {
        LogWarning("Cannot reload scene: path is empty");
        return;
    }

    if (!Device || !SwapChain || !MainWindow)
    {
        LogError("Cannot reload scene: renderer prerequisites are missing");
        return;
    }

    if (!FTaskScheduler::Get().IsRunning())
    {
        // Fallback to synchronous loading if task system is not available
        LogWarning("Task system not available, using synchronous scene reload");
        if (Device->GetGraphicsQueue())
        {
            Device->GetGraphicsQueue()->Flush();
        }
        if (!ReloadScene(ScenePath))
        {
            LogError("Failed to reload scene: " + StringUtils::WideToUtf8(ScenePath));
        }
        return;
    }

    bool bExpected = false;
    if (!bAsyncSceneLoadInProgress.compare_exchange_strong(bExpected, true, std::memory_order_acq_rel))
    {
        LogWarning("Async scene reload already in progress, keeping pending request for: " + StringUtils::WideToUtf8(ScenePath));
        PendingScenePath = ScenePath;
        return;
    }

    // Flush GPU before starting async load
    if (Device->GetGraphicsQueue())
    {
        Device->GetGraphicsQueue()->Flush();
    }

    LogInfo("Starting async scene reload: " + StringUtils::WideToUtf8(ScenePath));
    const auto StartTime = std::chrono::high_resolution_clock::now();

    AsyncScenePath = ScenePath;
    bAsyncSceneLoadComplete.store(false, std::memory_order_release);

    // Capture all required data for async loading
    const uint32_t Width = static_cast<uint32_t>(MainWindow->GetWidth());
    const uint32_t Height = static_cast<uint32_t>(MainWindow->GetHeight());
    const DXGI_FORMAT BackBufferFormat = SwapChain->GetFormat();
    
    // Create a temporary config for async scene loading
    FRendererConfig AsyncConfig = RendererConfig;
    AsyncConfig.SceneFile = ScenePath;
    AsyncConfig.ShadowBias = ActiveRenderer ? ActiveRenderer->GetShadowBias() : RendererConfig.ShadowBias;
    AsyncConfig.FramesInFlight = SwapChain ? SwapChain->GetBackBufferCount() : 2u;

    const bool bPreferDeferred = ActiveRenderer == DeferredRenderer.get() || RendererConfig.RendererType == ERendererType::Deferred;

    // Schedule async task
    FTaskScheduler::Get().ScheduleTask([this, ScenePath, Width, Height, BackBufferFormat, AsyncConfig, bPreferDeferred, StartTime]()
    {
        AsyncForwardRenderer = std::make_unique<FForwardRenderer>();
        AsyncDeferredRenderer = std::make_unique<FDeferredRenderer>();
        AsyncActiveRenderer = nullptr;

        auto TryInitializeRenderer = [&](ERendererType Type) -> bool
        {
            if (Type == ERendererType::Deferred)
            {
                if (AsyncDeferredRenderer->Initialize(Device.get(), Width, Height, BackBufferFormat, AsyncConfig))
                {
                    AsyncActiveRenderer = AsyncDeferredRenderer.get();
                    return true;
                }
                return false;
            }

            if (AsyncForwardRenderer->Initialize(Device.get(), Width, Height, BackBufferFormat, AsyncConfig))
            {
                AsyncActiveRenderer = AsyncForwardRenderer.get();
                return true;
            }
            return false;
        };

        const bool bInitialized = bPreferDeferred ?
            (TryInitializeRenderer(ERendererType::Deferred) || TryInitializeRenderer(ERendererType::Forward)) :
            (TryInitializeRenderer(ERendererType::Forward) || TryInitializeRenderer(ERendererType::Deferred));

        if (!bInitialized || AsyncActiveRenderer == nullptr)
        {
            LogError("Failed to reload scene asynchronously: renderer initialization failed for new scene");
            AsyncForwardRenderer.reset();
            AsyncDeferredRenderer.reset();
            AsyncActiveRenderer = nullptr;
            bAsyncSceneLoadComplete.store(true, std::memory_order_release);
            return;
        }

        const auto EndTime = std::chrono::high_resolution_clock::now();
        const auto Duration = std::chrono::duration_cast<std::chrono::milliseconds>(EndTime - StartTime);
        LogInfo("Async scene reload completed in " + std::to_string(Duration.count()) + " ms");

        // Signal completion with atomic store (this flag will be checked on the main thread)
        bAsyncSceneLoadComplete.store(true, std::memory_order_release);
    });
}

void FApplication::CompleteAsyncSceneReload()
{
    // Atomic load to check completion
    if (!bAsyncSceneLoadComplete.load(std::memory_order_acquire))
    {
        return;
    }

    bAsyncSceneLoadComplete.store(false, std::memory_order_release);
    bAsyncSceneLoadInProgress.store(false, std::memory_order_release);

    if (!AsyncActiveRenderer || (!AsyncForwardRenderer && !AsyncDeferredRenderer))
    {
        LogError("Async scene reload failed: no valid renderer was created");
        AsyncForwardRenderer.reset();
        AsyncDeferredRenderer.reset();
        AsyncActiveRenderer = nullptr;
        AsyncScenePath.clear();
        return;
    }

    if (Device && Device->GetGraphicsQueue())
    {
        Device->GetGraphicsQueue()->Flush();
    }

    // Swap renderers on the main thread
    ForwardRenderer = std::move(AsyncForwardRenderer);
    DeferredRenderer = std::move(AsyncDeferredRenderer);
    ActiveRenderer = AsyncActiveRenderer;
    SelectedSectionIndex = -1;
    SelectedSectionName.clear();
    bPendingObjectIdReadback = false;
    
    CurrentScenePath = AsyncScenePath;
    RendererConfig.SceneFile = AsyncScenePath;

    SyncRendererDepthPrepassConfig();
    SyncDeferredHzbConfig();
    SyncDeferredRestirGITransientState();

    ApplySceneLightingFromJson(AsyncScenePath);
    UpdateRendererLighting();
    ApplySceneCameraFromJson(AsyncScenePath);

    LogInfo("Scene swapped to: " + StringUtils::WideToUtf8(AsyncScenePath));
    
    // Clean up
    AsyncForwardRenderer.reset();
    AsyncDeferredRenderer.reset();
    AsyncActiveRenderer = nullptr;
    AsyncScenePath.clear();
}

std::wstring FApplication::OpenSceneFileDialog(const std::wstring& InitialDirectory) const
{
    OPENFILENAMEW OpenFileName{};
    wchar_t FilePath[MAX_PATH] = {};

    OpenFileName.lStructSize = sizeof(OpenFileName);
    OpenFileName.hwndOwner = MainWindow ? MainWindow->GetHWND() : nullptr;
    OpenFileName.lpstrFilter = L"Scene JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    OpenFileName.lpstrFile = FilePath;
    OpenFileName.nMaxFile = static_cast<DWORD>(std::size(FilePath));
    OpenFileName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    const std::filesystem::path OriginalWorkingDir = std::filesystem::current_path();

    std::filesystem::path InitialPath = InitialDirectory;
    if (InitialPath.empty())
    {
        InitialPath = std::filesystem::current_path() / L"Assets/Scenes";
    }

    std::error_code ErrorCode;
    InitialPath = std::filesystem::absolute(InitialPath, ErrorCode);
    if (ErrorCode)
    {
        LogWarning("Failed to resolve absolute scene directory: " + StringUtils::WideToUtf8(InitialPath));
    }

    std::wstring InitialPathWStr = InitialPath.wstring();
    if (!InitialPathWStr.empty())
    {
        OpenFileName.lpstrInitialDir = InitialPathWStr.c_str();
    }

    const BOOL bDialogAccepted = GetOpenFileNameW(&OpenFileName);

    std::error_code RestoreError;
    std::filesystem::current_path(OriginalWorkingDir, RestoreError);
    if (RestoreError)
    {
        LogWarning("Failed to restore working directory after file dialog: " + StringUtils::WideToUtf8(OriginalWorkingDir));
    }

    if (bDialogAccepted == TRUE)
    {
        return std::wstring(FilePath);
    }

    return std::wstring();
}

DirectX::XMVECTOR FApplication::GetLightDirectionVector() const
{
    const DirectX::XMVECTOR Forward = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
    const DirectX::XMMATRIX Rotation = DirectX::XMMatrixRotationRollPitchYaw(RendererConfig.LightPitch, RendererConfig.LightYaw, 0.0f);
    return DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(Forward, Rotation));
}

void FApplication::UpdateRendererLighting() const
{
    DirectX::XMFLOAT3 Direction{};
    DirectX::XMStoreFloat3(&Direction, GetLightDirectionVector());

    if (ForwardRenderer)
    {
        ForwardRenderer->SetLightDirection(Direction);
        ForwardRenderer->SetLightIntensity(RendererConfig.LightIntensity);
        ForwardRenderer->SetLightColor(RendererConfig.LightColor);
    }

    if (DeferredRenderer)
    {
        DeferredRenderer->SetLightDirection(Direction);
        DeferredRenderer->SetLightIntensity(RendererConfig.LightIntensity);
        DeferredRenderer->SetLightColor(RendererConfig.LightColor);
    }
}

bool FApplication::EnsureImGuiFontAtlas()
{
#if !WITH_IMGUI
    return false;
#else
    if (!ImGuiCtx)
    {
        LogError("ImGui context is missing");
        return false;
    }

    ImGuiIO& Io = ImGui::GetIO();
    ImFontAtlas* Atlas = Io.Fonts;

    if (!Atlas)
    {
        LogError("ImGui font atlas object is missing");
        return false;
    }

    if (Atlas->IsBuilt())
    {
        return true;
    }

	// Ensure there is at least one font in the atlas; otherwise, building will fail.
    if (Atlas->Fonts.empty())
    {
        Atlas->AddFontDefault();
    }

    if (!Atlas->Build())
    {
        LogError("Failed to build ImGui font atlas");
        return false;
    }

    // Ensure the GPU is idle before invalidating ImGui resources because the DX12 backend releases its pipeline state object during invalidation.
    if (Device && Device->GetGraphicsQueue())
    {
        Device->GetGraphicsQueue()->Flush();
    }

	// Recreate device objects to rebuild the font atlas texture.
    ImGui_ImplDX12_InvalidateDeviceObjects();
    if (!ImGui_ImplDX12_CreateDeviceObjects())
    {
        LogError("Failed to recreate ImGui device objects");
        return false;
    }

    return Atlas->IsBuilt();
#endif
}

bool FApplication::InitializeImGui(int32_t Width, int32_t Height)
{
#if !WITH_IMGUI
    // ImGui is not available; allow the application to continue without UI rendering.
    return true;
#else
    LogInfo("ImGui initialization started");

    IMGUI_CHECKVERSION();
    ImGuiCtx = ImGui::CreateContext();

    ImGuiIO& Io = ImGui::GetIO();
    Io.DisplaySize = ImVec2(static_cast<float>(Width), static_cast<float>(Height));
    ImGui::StyleColorsDark();

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = 8;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HeapDesc.NodeMask = 0;

    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(ImGuiDescriptorHeap.GetAddressOf())));

    ImGui_ImplWin32_Init(MainWindow->GetHWND());

	ImGui_ImplDX12_InitInfo InitInfo = {};
	InitInfo.Device = Device->GetDevice();
	InitInfo.CommandQueue = Device->GetGraphicsQueue()->GetD3DQueue();
	InitInfo.NumFramesInFlight = SwapChain->GetBackBufferCount();
	InitInfo.RTVFormat = SwapChain->GetFormat();
	InitInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
	InitInfo.SrvDescriptorHeap = ImGuiDescriptorHeap.Get();
#ifndef IMGUI_DISABLE_OBSOLETE_FUNCTIONS
	InitInfo.LegacySingleSrvCpuDescriptor = ImGuiDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	InitInfo.LegacySingleSrvGpuDescriptor = ImGuiDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
#endif
	ImGui_ImplDX12_Init(&InitInfo);


    if (!ImGui_ImplDX12_CreateDeviceObjects())
    {
        LogError("Failed to create ImGui device objects");
        return false;
    }

    LogInfo("ImGui initialization complete");
    return true;
#endif
}

void FApplication::ShutdownImGui()
{
#if WITH_IMGUI
    if (ImGuiCtx)
    {
        LogInfo("ImGui shutdown");
        if (Device && Device->GetGraphicsQueue())
        {
            Device->GetGraphicsQueue()->Flush();
        }
        ImGui_ImplDX12_InvalidateDeviceObjects();
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(ImGuiCtx);
        ImGuiCtx = nullptr;
    }
#endif
}

void FApplication::RenderUI()
{
#if !WITH_IMGUI
    return;
#else
    if (!ImGuiCtx)
    {
        return;
    }

    if (!EnsureImGuiFontAtlas())
    {
        return;
    }

    ImGuiIO& Io = ImGui::GetIO();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    Io.DisplaySize = ImVec2(static_cast<float>(MainWindow->GetWidth()), static_cast<float>(MainWindow->GetHeight()));
    const ImVec2 WindowPos = ImVec2(Io.DisplaySize.x - 10.0f, 10.0f);
    const ImVec2 WindowPivot = ImVec2(1.0f, 0.0f);

    ImGui::SetNextWindowPos(WindowPos, ImGuiCond_Always, WindowPivot);
    ImGui::SetNextWindowBgAlpha(0.35f);

    ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    ImGui::Begin("Performance", nullptr, Flags);
    ImGui::Text("FPS: %.1f", Time->GetFPS());

	ImGui::SameLine();
	const double CpuFrameMs = Time->GetDeltaTimeSeconds() * 1000.0;
	double GpuFrameMs = -1.0;
	for (const FRenderGraph::FGpuPassTimingStats& Stats : FRenderGraph::GetGpuTimingStats())
	{
		if (Stats.Name == "Frame")
		{
			GpuFrameMs = Stats.AvgMs;
			break;
		}
	}
    if (GpuFrameMs >= 0.0)
    {
        ImGui::Text("CPU/GPU: %.3f / %.3f", CpuFrameMs, GpuFrameMs);
    }
    else
    {
        ImGui::Text("CPU/GPU: %.3f / N/A", CpuFrameMs);
    }

    size_t TotalSections = 0;
    size_t CulledSections = 0;
    size_t TotalMeshlets = 0;
    if (ActiveRenderer)
    {
        auto Sections = ActiveRenderer->GetWorld().BuildSectionList();
        for (const FMeshSection& Section : Sections)
        {
            TotalMeshlets += Section.Meshlets.size();
        }
    }

    const bool bHasSectionStats = ActiveRenderer && ActiveRenderer->GetSceneSectionStats(TotalSections, CulledSections);
    if (bHasSectionStats)
    {
        ImGui::Text("Sections (Total/Culled): %zu / %zu | Meshlets: %zu", TotalSections, CulledSections, TotalMeshlets);
    }
    else
    {
        ImGui::Text("Sections (Total/Culled): N/A | Meshlets: %zu", TotalMeshlets);
    }

    if (ImGui::CollapsingHeader("Details", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Separator();
        ImGui::Text("GPU Timing (avg/min/max ms)");

        //float TimingWindowSeconds = static_cast<float>(FRenderGraph::GetGpuTimingWindowSeconds());
        //if (ImGui::SliderFloat("GPU Timing Window (s)", &TimingWindowSeconds, 0.1f, 5.0f, "%.1f"))
        //{
        //    FRenderGraph::SetGpuTimingWindowSeconds(TimingWindowSeconds);
        //}

        int TimingDisplayCount = static_cast<int>(FRenderGraph::GetGpuTimingDisplayCount());
        if (ImGui::SliderInt("Display Count", &TimingDisplayCount, 1, 20))
        {
            FRenderGraph::SetGpuTimingDisplayCount(static_cast<uint32>(TimingDisplayCount));
        }

        const std::vector<FRenderGraph::FGpuPassTimingStats>& TimingStats = FRenderGraph::GetGpuTimingStats();
        const uint32 MaxDisplay = FRenderGraph::GetGpuTimingDisplayCount();
        const uint32 DisplayCount = (std::min)(MaxDisplay, static_cast<uint32>(TimingStats.size()));
        constexpr int GpuTimingNameDisplayWidth = 24;
        const auto FormatGpuTimingName = [GpuTimingNameDisplayWidth](const std::string& Name)
        {
            if (static_cast<int>(Name.size()) <= GpuTimingNameDisplayWidth)
            {
                return Name;
            }

            return Name.substr(0, GpuTimingNameDisplayWidth - 3) + "...";
        };

        for (uint32 Index = 1; Index < DisplayCount + 1; ++Index)
        {
            if (TimingStats.size() <= Index)
            {
                break;
			}
            const FRenderGraph::FGpuPassTimingStats& Stats = TimingStats[Index];
            const std::string DisplayName = FormatGpuTimingName(Stats.Name);
            ImGui::Text("%-*s: %.3f / %.3f / %.3f (n=%u)",
                GpuTimingNameDisplayWidth,
                DisplayName.c_str(),
                Stats.AvgMs,
                Stats.MinMs,
                Stats.MaxMs,
                Stats.SampleCount);
        }

        ImGui::Separator();
        if (ImGui::Button("Load Scene"))
        {
            const std::filesystem::path ScenePath(CurrentScenePath);
            const std::filesystem::path InitialDir = ScenePath.has_parent_path() ? ScenePath.parent_path() : std::filesystem::path();
            const std::wstring SelectedScene = OpenSceneFileDialog(InitialDir.wstring());
            if (!SelectedScene.empty())
            {
                PendingScenePath = SelectedScene;
            }
        }
        const std::string ScenePathUtf8 = StringUtils::WideToUtf8(CurrentScenePath);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", ScenePathUtf8.c_str());

        const char* SelectedName = SelectedSectionIndex >= 0 ? SelectedSectionName.c_str() : "None";
        ImGui::Text("Selected: %s [%d]", SelectedName, SelectedSectionIndex);
        DXGI_QUERY_VIDEO_MEMORY_INFO LocalMemoryInfo = {};
        if (Device && Device->QueryLocalVideoMemory(LocalMemoryInfo))
        {
            const double UsageMB = static_cast<double>(LocalMemoryInfo.CurrentUsage) / (1024.0 * 1024.0);
            const double BudgetMB = static_cast<double>(LocalMemoryInfo.Budget) / (1024.0 * 1024.0);
            const double AvailableMB = static_cast<double>(LocalMemoryInfo.AvailableForReservation) / (1024.0 * 1024.0);
		    const double ReservedMB = static_cast<double>(LocalMemoryInfo.CurrentReservation) / (1024.0 * 1024.0);

            ImGui::Separator();
            ImGui::Text("GPU Memory (Local)");
            ImGui::Text("Usage/Budget: %.1f / %.1f MB", UsageMB, BudgetMB);
            ImGui::Text("Available/Reserved: %.1f / %.1f MB", AvailableMB, ReservedMB);
        } 
#if WITH_BINDLESS_DESCRIPTOR_STATS
        if (Device)
        {
            const FDX12Device::FBindlessDescriptorStats BindlessStats = bCachedBindlessDescriptorStatsValid
                ? CachedBindlessDescriptorStats
                : Device->GetBindlessDescriptorStats();
            const uint32_t UsedDescriptors = (std::min)(BindlessStats.NextIndex, BindlessStats.DescriptorCount);
            const float UsedPercent = (BindlessStats.DescriptorCount > 0u)
                ? (100.0f * static_cast<float>(UsedDescriptors) / static_cast<float>(BindlessStats.DescriptorCount))
                : 0.0f;

            ImGui::Separator();
            if (ImGui::TreeNode("Bindless Descriptor Stats"))
            {
                ImGui::TextUnformatted(bCachedBindlessDescriptorStatsValid ? "Snapshot: post Signal/Wait from previous frame" : "Snapshot: live (pre-frame)");
                bool bTrackOwners = bTrackLiveTransientBindlessOwners;
                if (ImGui::Checkbox("Track Live Transient Owners", &bTrackOwners))
                {
                    bTrackLiveTransientBindlessOwners = bTrackOwners;
                    Device->SetLiveTransientBindlessOwnerTrackingEnabled(bTrackLiveTransientBindlessOwners);
                    bCachedBindlessDescriptorStatsValid = false;
                }
                const uint64_t InFlightFenceLag = (BindlessStats.LastSignaledFenceValue > BindlessStats.CompletedFenceValue)
                    ? (BindlessStats.LastSignaledFenceValue - BindlessStats.CompletedFenceValue)
                    : 0u;
                ImGui::Text("Allocated Slots: %u / %u (%.1f%%)", UsedDescriptors, BindlessStats.DescriptorCount, UsedPercent);
                ImGui::Text("Permanent Allocations: %llu", static_cast<unsigned long long>(BindlessStats.PermanentAllocationCount));
                ImGui::Text("Transient Heap High Watermark: %llu", static_cast<unsigned long long>(BindlessStats.TransientHeapAllocationCount));
                ImGui::Text("Transient Heap Allocs This Frame: %llu", static_cast<unsigned long long>(BindlessStats.TransientHeapAllocsThisFrame));
                ImGui::Text("Transient Reuse Count: %llu", static_cast<unsigned long long>(BindlessStats.TransientReuseCount));
                ImGui::Text("Transient Retired Count: %llu", static_cast<unsigned long long>(BindlessStats.TransientRetireCount));
                ImGui::Text("Transient Reclaimed Count: %llu", static_cast<unsigned long long>(BindlessStats.TransientReclaimCount));
                ImGui::Text("Reusable Transient Slots: %u", BindlessStats.FreeTransientCount);
                ImGui::Text("Min Reusable Slots This Frame: %u", BindlessStats.MinFreeTransientThisFrame);
                ImGui::Text("Peak Live Transient Slots This Frame: %u", BindlessStats.PeakTransientLiveThisFrame);
                ImGui::Text("Live Transient Descriptor Owners: %u", BindlessStats.LiveTransientDescriptorCount);
                ImGui::Text("Retired Transient: %u", BindlessStats.RetiredTransientCount);
                ImGui::Text("Reclaimable Now: %u", BindlessStats.ReclaimableTransientCount);
                ImGui::Text("Fence: completed=%llu signaled=%llu",
                    static_cast<unsigned long long>(BindlessStats.CompletedFenceValue),
                    static_cast<unsigned long long>(BindlessStats.LastSignaledFenceValue));
                ImGui::Text("In-flight Fence Lag: %llu", static_cast<unsigned long long>(InFlightFenceLag));
                if (BindlessStats.RetiredTransientCount > 0u)
                {
                    ImGui::Text("Retired Fence Range: %llu -> %llu",
                        static_cast<unsigned long long>(BindlessStats.OldestRetiredFenceValue),
                        static_cast<unsigned long long>(BindlessStats.NewestRetiredFenceValue));
                }

                if (BindlessStats.ReclaimableTransientCount > 0u && BindlessStats.FreeTransientCount == 0u)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Reclaimable descriptors exist but are not in the free list yet.");
                }
                else if (BindlessStats.RetiredTransientCount > 0u && BindlessStats.ReclaimableTransientCount == 0u)
                {
                    ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "Retired descriptors are waiting on GPU fence completion.");
                }
                else if (BindlessStats.FreeTransientCount == 0u && BindlessStats.RetiredTransientCount == 0u)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "No transient descriptors are currently reusable.");
                }

                if (!bTrackLiveTransientBindlessOwners)
                {
                    ImGui::TextDisabled("Owner sampling is disabled.");
                }
                else if (!BindlessStats.LiveTransientOwnerSamples.empty() && ImGui::TreeNode("Live Transient Owner Samples"))
                {
                    if (BindlessStats.LiveTransientDescriptorCount > BindlessStats.LiveTransientOwnerSamples.size())
                    {
                        ImGui::Text("Showing %zu of %u live transient descriptors.",
                            BindlessStats.LiveTransientOwnerSamples.size(),
                            BindlessStats.LiveTransientDescriptorCount);
                    }

                    for (const std::string& OwnerSample : BindlessStats.LiveTransientOwnerSamples)
                    {
                        ImGui::BulletText("%s", OwnerSample.c_str());
                    }

                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
        }
#endif

	    ImGui::Separator();

        if (ImGui::Checkbox("Frame Overlap", &RendererConfig.bEnableFrameOverlap))
        {
        }

        ImGui::SameLine();
        if (ImGui::Checkbox("VSync", &RendererConfig.bEnableVSync))
        {
        }

        if (Device && Device->GetGraphicsQueue())
        {
            const uint64 CompletedFence = Device->GetGraphicsQueue()->GetCompletedFenceValue();
            const uint64 LastSignaledFence = Device->GetGraphicsQueue()->GetLastSignaledFenceValue();
            const uint64 InFlightFrames = (LastSignaledFence > CompletedFence) ? (LastSignaledFence - CompletedFence) : 0;

			ImGui::SameLine();
            ImGui::Text("In-flight frames: %llu", static_cast<unsigned long long>(InFlightFrames));
    //        ImGui::Text("GPU fences: completed %llu / last signaled %llu", static_cast<unsigned long long>(CompletedFence), static_cast<unsigned long long>(LastSignaledFence));
        }

        const auto ApplyDepthPrepassConfig = [this]()
        {
            SyncRendererDepthPrepassConfig();
        };

        if (ImGui::Checkbox("Depth Prepass", &RendererConfig.bUseDepthPrepass))
        {
            ApplyDepthPrepassConfig();
        }

        ImGui::SameLine();
        bool bFreezeCameraValue = bFreezeCamera;
        if (ImGui::Checkbox("Freeze Camera", &bFreezeCameraValue))
        {
            bFreezeCamera = bFreezeCameraValue;
            bIsRotatingWithMouse = false;
            if (bFreezeCamera && Camera)
            {
                FrozenCamera = *Camera;
            }
        }

        ImGui::Separator();
        const auto ApplyDeferredHzbConfig = [this]()
        {
            SyncDeferredHzbConfig();
        };

        if (ImGui::Checkbox("Build HZB", &RendererConfig.bEnableHZB))
        {
            ApplyDeferredHzbConfig();
        }

        ImGui::SameLine();
        if (ImGui::Checkbox("HZB Two Pass", &RendererConfig.bEnableHzbTwoPass))
        {
            ApplyDeferredHzbConfig();
        }

        ImGui::Separator();
        static const char* LightingDebugViewItems[] =
        {
            "Off",
            "DiffuseIndirect",
            "AO",
            "DirectLighting",
            "SpecularIndirect",
            "ClusterDagClusters",
            "ClusterDagMip",
            "IndirectIrradiance"
        };
        int LightingDebugViewIndex = static_cast<int>(RendererConfig.DeferredLightingVisualizationMode);
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::Combo("Lighting Debug View", &LightingDebugViewIndex, LightingDebugViewItems, IM_ARRAYSIZE(LightingDebugViewItems)))
        {
            RendererConfig.DeferredLightingVisualizationMode = static_cast<EDeferredLightingVisualizationMode>(
                std::clamp(LightingDebugViewIndex, 0, static_cast<int>(EDeferredLightingVisualizationMode::IndirectIrradiance)));
            UpsertConfigValue(GetRendererConfigPath(), "DeferredLightingVisualizationMode", DeferredLightingVisualizationModeToConfigString(RendererConfig.DeferredLightingVisualizationMode));
            SyncDeferredLightingPassConfig();
        }

        ImGui::Separator();
        if (ImGui::Checkbox("Diffuse GI Denoiser", &RendererConfig.bEnableDiffuseGIDenoiser))
        {
            UpsertConfigValue(GetRendererConfigPath(), "EnableDiffuseGIDenoiser", RendererConfig.bEnableDiffuseGIDenoiser ? "true" : "false");
            SyncDeferredRestirGIConfig();
        }

        static const char* DiffuseGISourceItems[] = { "Off", "Sparse SDF GI", "ReSTIR GI" };
        int DiffuseGISourceIndex = static_cast<int>(RendererConfig.DiffuseGISource);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Diffuse GI Source", &DiffuseGISourceIndex, DiffuseGISourceItems, IM_ARRAYSIZE(DiffuseGISourceItems)))
        {
            RendererConfig.DiffuseGISource = static_cast<EDiffuseGISource>(std::clamp(DiffuseGISourceIndex, 0, 2));
            const char* DiffuseGISourceValue =
                RendererConfig.DiffuseGISource == EDiffuseGISource::RestirGI ? "RestirGI" :
                RendererConfig.DiffuseGISource == EDiffuseGISource::SparseSdfGI ? "SparseSdfGI" : "Off";
            UpsertConfigValue(GetRendererConfigPath(), "DiffuseGISource", DiffuseGISourceValue);
            SyncDeferredRestirGIConfig();
            SyncDeferredSparseSdfGIConfig();
        }

        if (RendererConfig.DiffuseGISource == EDiffuseGISource::RestirGI)
        {
            if (ImGui::Checkbox("Use Visibility", &RendererConfig.bRestirGIUseVisibility))
            {
                SyncDeferredRestirGIConfig();
            }

		    ImGui::SameLine();
            if (ImGui::Checkbox("Use BRDF", &RendererConfig.bRestirGIUseBrdf))
            {
                SyncDeferredRestirGIConfig();
            }

            if (ImGui::Checkbox("Use History Indirect", &RendererConfig.bRestirGIUseHistoryIndirect))
            {
                SyncDeferredRestirGIConfig();
            }

            int RestirRandomModeIndex = (RendererConfig.RestirGIRandomMode == ERestirGIRandomMode::BlueNoiseSobol) ? 1 : 0;
            static const char* RestirRandomModeItems[] = { "Hash", "BlueNoiseSobol" };
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("Random Mode", &RestirRandomModeIndex, RestirRandomModeItems, IM_ARRAYSIZE(RestirRandomModeItems)))
            {
                RendererConfig.RestirGIRandomMode = (RestirRandomModeIndex == 1) ? ERestirGIRandomMode::BlueNoiseSobol : ERestirGIRandomMode::Hash;
                UpsertConfigValue(GetRendererConfigPath(), "RestirGIRandomMode", RestirRandomModeToConfigString(RendererConfig.RestirGIRandomMode));
                SyncDeferredRestirGIConfig();
            }

            bool bRestirGIFreezeFrameUI = bRestirGIFreezeFrame;
            if (ImGui::Checkbox("Freeze ReSTIR GI", &bRestirGIFreezeFrameUI))
            {
                bRestirGIFreezeFrame = bRestirGIFreezeFrameUI;
		        SyncDeferredRestirGITransientState();
            }

            ImGui::SameLine();
            if (ImGui::Button("Step ReSTIR GI"))
            {
                if (DeferredRenderer)
                {
                    DeferredRenderer->GetRestirGI()->StepFreezeFrame();
                }
            }

            ImGui::SameLine();
            ImGui::Text("%u", DeferredRenderer ? DeferredRenderer->GetRestirGI()->GetFreezeStartFrameNumber() : 0u);

            bool bRestirGIDebugRay = bRestirGIDebugRayEnabled;
            if (ImGui::Checkbox("Debug ReSTIR GI Ray", &bRestirGIDebugRay))
            {
                bRestirGIDebugRayEnabled = bRestirGIDebugRay;
		        SyncDeferredRestirGITransientState();
            }

            int RestirDebugPixel[2] = { RestirGIDebugPixelX, RestirGIDebugPixelY };
            if (ImGui::InputInt2("Debug Pixel", RestirDebugPixel))
            {
                RestirGIDebugPixelX = (std::max)(0, RestirDebugPixel[0]);
                RestirGIDebugPixelY = (std::max)(0, RestirDebugPixel[1]);
                SyncDeferredRestirGITransientState();
            }
            if (ImGui::Checkbox("Temporal Reuse", &RendererConfig.bEnableRestirGITemporalReuse))
            {
                SyncDeferredRestirGIConfig();
            }

		    ImGui::SameLine();
            if (ImGui::Checkbox("Spatial Reuse", &RendererConfig.bEnableRestirGISpatialReuse))
            {
                SyncDeferredRestirGIConfig();
            }

            if (ImGui::SliderFloat("Temporal Add Scale", &RendererConfig.RestirGITemporalAdditionalScale, 0.0f, 1.0f, "%.2f"))
            {
                SyncDeferredRestirGIConfig();
            }

            if (ImGui::SliderFloat("Spatial Add Scale", &RendererConfig.RestirGISpatialAdditionalScale, 0.0f, 1.0f, "%.2f"))
            {
                SyncDeferredRestirGIConfig();
            }
        }

        if (RendererConfig.DiffuseGISource == EDiffuseGISource::SparseSdfGI)
        {
            static const char* SparseSdfGIDebugModeItems[] = { "Off", "Ray Trace", "Cascade Slice", "Gradient", "Step Count", "Shared Sample Mismatch", "Brick Local Gradient", "Hit UVW", "Brick ID", "Brick Local Gradient (FFX Rounded)" };
            int SparseSdfGIDebugModeIndex = static_cast<int>(std::clamp(RendererConfig.SparseSdfGIDebugMode, 0u, 9u));
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("SDF GI Debug Mode", &SparseSdfGIDebugModeIndex, SparseSdfGIDebugModeItems, IM_ARRAYSIZE(SparseSdfGIDebugModeItems)))
            {
                RendererConfig.SparseSdfGIDebugMode = static_cast<uint32_t>(SparseSdfGIDebugModeIndex);
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIDebugMode", std::to_string(RendererConfig.SparseSdfGIDebugMode));
                SyncDeferredSparseSdfGIConfig();
            }

            static const char* SparseSdfGIBuildModeItems[] = { "Legacy Eikonal", "Exact Shared Border" };
            int SparseSdfGIBuildModeIndex = static_cast<int>(std::clamp(RendererConfig.SparseSdfGISdfBuildMode, 0u, 1u));
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::Combo("SDF Build Mode", &SparseSdfGIBuildModeIndex, SparseSdfGIBuildModeItems, IM_ARRAYSIZE(SparseSdfGIBuildModeItems)))
            {
                RendererConfig.SparseSdfGISdfBuildMode = static_cast<uint32_t>(SparseSdfGIBuildModeIndex);
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGISdfBuildMode", std::to_string(RendererConfig.SparseSdfGISdfBuildMode));
                SyncDeferredSparseSdfGIConfig();
            }

            if (ImGui::Checkbox("SDF Hierarchical Trace", &RendererConfig.bSparseSdfGIUseHierarchicalTrace))
            {
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIUseHierarchicalTrace", RendererConfig.bSparseSdfGIUseHierarchicalTrace ? "true" : "false");
                SyncDeferredSparseSdfGIConfig();
            }

            // SparseSdfGIBaseVoxelSize <= 0 auto-fits from scene radius; a positive value is a manual override.
            float SparseSdfGIBaseVoxelSize = RendererConfig.SparseSdfGIBaseVoxelSize;
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::DragFloat("Base Voxel Size (0=Auto)", &SparseSdfGIBaseVoxelSize, 0.005f, 0.0f, 4.0f, "%.3f"))
            {
                RendererConfig.SparseSdfGIBaseVoxelSize = (std::max)(SparseSdfGIBaseVoxelSize, 0.0f);
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIBaseVoxelSize", std::to_string(RendererConfig.SparseSdfGIBaseVoxelSize));
                SyncDeferredSparseSdfGIConfig();
            }
            if (RendererConfig.SparseSdfGIBaseVoxelSize <= 0.0f && DeferredRenderer && DeferredRenderer->GetSparseSdfGI())
            {
                ImGui::Text("  Auto Voxel Size: %.4f", DeferredRenderer->GetSparseSdfGI()->GetEffectiveVoxelSize());
            }
            if (DeferredRenderer && DeferredRenderer->GetSparseSdfGI())
            {
                ImGui::Text("  Cascade Extent: %.3f", DeferredRenderer->GetSparseSdfGI()->GetEffectiveCascadeExtent());
            }

            int SparseSdfGIMaxBrickRefsM = static_cast<int>(std::clamp(
                RendererConfig.SparseSdfGIMaxBrickTriangleReferences / (1024u * 1024u),
                1u,
                32u));
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Max Brick Triangle Refs (M)", &SparseSdfGIMaxBrickRefsM, 1, 32))
            {
                RendererConfig.SparseSdfGIMaxBrickTriangleReferences = static_cast<uint32_t>(std::clamp(SparseSdfGIMaxBrickRefsM, 1, 32)) * 1024u * 1024u;
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIMaxBrickTriangleReferences", std::to_string(RendererConfig.SparseSdfGIMaxBrickTriangleReferences));
                SyncDeferredSparseSdfGIConfig();
            }

            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderFloat("SDF GI Bounce Strength", &RendererConfig.SparseSdfGIBounceStrength, 0.0f, 4.0f, "%.2f"))
            {
                RendererConfig.SparseSdfGIBounceStrength = (std::max)(RendererConfig.SparseSdfGIBounceStrength, 0.0f);
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIBounceStrength", std::to_string(RendererConfig.SparseSdfGIBounceStrength));
                SyncDeferredSparseSdfGIConfig();
            }

            if (ImGui::Checkbox("SDF GI Temporal Reuse", &RendererConfig.bSparseSdfGIEnableRadianceTemporalReuse))
            {
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIRadianceTemporalReuse", RendererConfig.bSparseSdfGIEnableRadianceTemporalReuse ? "true" : "false");
                SyncDeferredSparseSdfGIConfig();
            }

            if (ImGui::Checkbox("SDF GI Multi-Bounce", &RendererConfig.bSparseSdfGIMultiBounce))
            {
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIMultiBounce", RendererConfig.bSparseSdfGIMultiBounce ? "true" : "false");
                SyncDeferredSparseSdfGIConfig();
            }
            if (RendererConfig.bSparseSdfGIMultiBounce)
            {
                if (ImGui::SliderFloat("Multi-Bounce Strength", &RendererConfig.SparseSdfGIMultiBounceStrength, 0.0f, 2.0f, "%.2f"))
                {
                    RendererConfig.SparseSdfGIMultiBounceStrength = (std::max)(RendererConfig.SparseSdfGIMultiBounceStrength, 0.0f);
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIMultiBounceStrength", std::to_string(RendererConfig.SparseSdfGIMultiBounceStrength));
                    SyncDeferredSparseSdfGIConfig();
                }
            }
            if (ImGui::SliderFloat("SDF Hit Threshold", &RendererConfig.SparseSdfGISurfaceHitThresholdVoxels, 0.05f, 0.75f, "%.3f"))
            {
                RendererConfig.SparseSdfGISurfaceHitThresholdVoxels = std::clamp(RendererConfig.SparseSdfGISurfaceHitThresholdVoxels, 0.05f, 0.75f);
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGISurfaceHitThresholdVoxels", std::to_string(RendererConfig.SparseSdfGISurfaceHitThresholdVoxels));
                SyncDeferredSparseSdfGIConfig();
            }

            if (ImGui::Checkbox("Screen Probes", &RendererConfig.bSparseSdfGIUseScreenProbes))
            {
                UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIUseScreenProbes", RendererConfig.bSparseSdfGIUseScreenProbes ? "true" : "false");
                SyncDeferredSparseSdfGIConfig();
            }

            if (RendererConfig.bSparseSdfGIUseScreenProbes)
            {
                int ProbeTileSize = static_cast<int>(std::clamp(RendererConfig.SparseSdfGIProbeTileSize, 4u, 16u));
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderInt("Probe Tile Size", &ProbeTileSize, 4, 16))
                {
                    RendererConfig.SparseSdfGIProbeTileSize = static_cast<uint32_t>(std::clamp(ProbeTileSize, 4, 16));
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIProbeTileSize", std::to_string(RendererConfig.SparseSdfGIProbeTileSize));
                    SyncDeferredSparseSdfGIConfig();
                }

                int ProbeRaysPerProbe = static_cast<int>(std::clamp(RendererConfig.SparseSdfGIProbeRaysPerProbe, 4u, 64u));
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderInt("Probe Rays", &ProbeRaysPerProbe, 4, 64))
                {
                    RendererConfig.SparseSdfGIProbeRaysPerProbe = static_cast<uint32_t>(std::clamp(ProbeRaysPerProbe, 4, 64));
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIProbeRaysPerProbe", std::to_string(RendererConfig.SparseSdfGIProbeRaysPerProbe));
                    SyncDeferredSparseSdfGIConfig();
                }

                static const char* ProbeDebugItems[] = { "Off", "Placement", "Validity", "Hit Ratio", "Variance", "Confidence", "Irradiance" };
                int ProbeDebugMode = static_cast<int>(std::clamp(RendererConfig.SparseSdfGIProbeDebugMode, 0u, 6u));
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::Combo("Probe Debug", &ProbeDebugMode, ProbeDebugItems, IM_ARRAYSIZE(ProbeDebugItems)))
                {
                    RendererConfig.SparseSdfGIProbeDebugMode = static_cast<uint32_t>(ProbeDebugMode);
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIProbeDebugMode", std::to_string(RendererConfig.SparseSdfGIProbeDebugMode));
                    SyncDeferredSparseSdfGIConfig();
                }

                if (ImGui::Checkbox("Probe Temporal Reuse", &RendererConfig.bSparseSdfGIProbeTemporalReuse))
                {
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIProbeTemporalReuse", RendererConfig.bSparseSdfGIProbeTemporalReuse ? "true" : "false");
                    SyncDeferredSparseSdfGIConfig();
                }

                if (ImGui::Checkbox("Probe Directional SH", &RendererConfig.bSparseSdfGIProbeDirectionalSH))
                {
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIProbeDirectionalSH", RendererConfig.bSparseSdfGIProbeDirectionalSH ? "true" : "false");
                    SyncDeferredSparseSdfGIConfig();
                }

                if (ImGui::Checkbox("Probe Spawn Jitter", &RendererConfig.bSparseSdfGIProbeSpawnJitter))
                {
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIProbeSpawnJitter", RendererConfig.bSparseSdfGIProbeSpawnJitter ? "true" : "false");
                    SyncDeferredSparseSdfGIConfig();
                }

                if (ImGui::Checkbox("Probe Motion Reproject", &RendererConfig.bSparseSdfGIProbeMotionReproject))
                {
                    UpsertConfigValue(GetRendererConfigPath(), "SparseSdfGIProbeMotionReproject", RendererConfig.bSparseSdfGIProbeMotionReproject ? "true" : "false");
                    SyncDeferredSparseSdfGIConfig();
                }
            }

            if (ImGui::Button("Force Rebuild SDF"))
            {
                if (DeferredRenderer && DeferredRenderer->GetSparseSdfGI())
                {
                    DeferredRenderer->GetSparseSdfGI()->ForceInvalidateCache();
                }
            }
        }

        const bool bRayTracingSupported = Device && Device->IsRayTracingSupported();
        bool bPathTracing = RendererConfig.bEnablePathTracing;
        ImGui::Separator();
        if (ImGui::Checkbox("Path Tracing", &bPathTracing))
        {
            if (bPathTracing && !bRayTracingSupported)
            {
                RendererConfig.bEnablePathTracing = false;
                LogWarning("Path tracing requested, but DXR is not supported. Disabling path tracing.");
            }
            else
            {
                RendererConfig.bEnablePathTracing = bPathTracing;
            }

            SyncRendererPathTracingConfig();
        }

        if (RendererConfig.bEnablePathTracing)
        {
            ImGui::SameLine();
            if (ImGui::Checkbox("Accumulation", &RendererConfig.bEnablePathTracingAccumulation))
            {
                SyncRendererPathTracingConfig();
            }

		    ImGui::SameLine();
		    if (ImGui::Checkbox("PT GGX VNDF", &RendererConfig.bEnablePathTracingVndf))
		    {
			    SyncRendererPathTracingConfig();
		    }

            int PathTracingMaxBounces = static_cast<int>(RendererConfig.PathTracingMaxBounces);
            if (ImGui::SliderInt("Max Bounces", &PathTracingMaxBounces, 0, 16))
            {
                RendererConfig.PathTracingMaxBounces = static_cast<uint32_t>(PathTracingMaxBounces);
                SyncRendererPathTracingConfig();
            }

            // Path Tracing Debug Mode
            const char* DebugModeNames[] = {
                "Normal PT",
                "GBuffer Albedo",
                "First Hit Albedo",
                "Texture Index Hash",
                "Direct Light",
                "Diffuse Probability",
                "Hit/Miss Mask",
                "Throughput Over Pdf",
                "Firefly Metric",
                "First Hit Distance",
                "Sky Miss Contribution",
                "First Hit NdotV",
                "Bounce1 NdotV",
                "Indirect Irradiance"
            };
            static_assert(IM_ARRAYSIZE(DebugModeNames) == PATH_TRACING_DEBUG_MAX + 1);
            int CurrentDebugMode = 0;
            if (DeferredRenderer)
            {
                CurrentDebugMode = DeferredRenderer->GetPathTracing()->GetDebugMode();
            }
            if (ImGui::Combo("PT Debug Mode", &CurrentDebugMode, DebugModeNames, IM_ARRAYSIZE(DebugModeNames)))
            {
                if (DeferredRenderer)
                {
                    DeferredRenderer->GetPathTracing()->SetDebugMode(CurrentDebugMode);
                }
            }
        }

		ImGui::Separator();

        if (ImGui::Checkbox("SW SSR", &RendererConfig.bEnableSsrSw))
        {
            SyncDeferredSsrConfig();
        }

        ImGui::SameLine();

        if (ImGui::Checkbox("HW SSR", &RendererConfig.bEnableSsrHw))
        {
            SyncDeferredSsrConfig();
        }

        const bool bSsrEnabled = RendererConfig.bEnableSsrSw || RendererConfig.bEnableSsrHw;
        if (bSsrEnabled)
        {
            ImGui::SameLine();

            if (ImGui::Checkbox("HZB", &RendererConfig.bEnableSsrHzb))
            {
                SyncDeferredSsrConfig();
            }

            if (ImGui::Checkbox("Full Res Depth", &RendererConfig.bEnableSsrHzbFullResDepth))
            {
                SyncDeferredSsrConfig();
            }

            ImGui::SameLine();

            if (ImGui::Checkbox("Refine", &RendererConfig.bEnableSsrRefine))
            {
                SyncDeferredSsrConfig();
            }

            ImGui::SameLine();

            if (ImGui::Checkbox("Denoise", &RendererConfig.bEnableSsrDenoise))
            {
                SyncDeferredSsrConfig();
            }

            const char* SsrModeItems[] = { "PS", "CS" };
            int SsrModeIndex = (RendererConfig.SsrMode == ESSRMode::CS) ? 1 : 0;
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("SSR Mode", &SsrModeIndex, SsrModeItems, IM_ARRAYSIZE(SsrModeItems)))
            {
                RendererConfig.SsrMode = (SsrModeIndex == 1) ? ESSRMode::CS : ESSRMode::PS;
                SyncDeferredSsrConfig();
            }

            ImGui::SameLine();

            const int SampleOptions[] = { 4, 2, 1 };
            int SampleIndex = 2;
            for (int OptionIndex = 0; OptionIndex < 3; ++OptionIndex)
            {
                if (RendererConfig.SsrSamplesPerQuad == static_cast<uint32_t>(SampleOptions[OptionIndex]))
                {
                    SampleIndex = OptionIndex;
                    break;
                }
            }
            const char* SampleLabels[] = { "4", "2", "1" };
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("Samples Per Quad", &SampleIndex, SampleLabels, IM_ARRAYSIZE(SampleLabels)))
            {
                RendererConfig.SsrSamplesPerQuad = static_cast<uint32_t>(SampleOptions[SampleIndex]);
                SyncDeferredSsrConfig();
            }

            int SsrMaxStepsValue = static_cast<int>(RendererConfig.SsrMaxSteps);
            if (ImGui::SliderInt("SSR Max Steps", &SsrMaxStepsValue, 8, 256))
            {
                RendererConfig.SsrMaxSteps = static_cast<uint32_t>(SsrMaxStepsValue);
                SyncDeferredSsrConfig();
            }

            if (ImGui::SliderFloat("SSR Max Distance", &RendererConfig.SsrMaxDistance, 1.0f, 200.0f, "%.1f"))
            {
                SyncDeferredSsrConfig();
            }

            if (ImGui::SliderFloat("SSR Thickness", &RendererConfig.SsrThickness, 0.01f, 1.0f, "%.2f"))
            {
                SyncDeferredSsrConfig();
            }

            if (ImGui::SliderFloat("SSR Stride", &RendererConfig.SsrStride, 0.05f, 2.0f, "%.2f"))
            {
                SyncDeferredSsrConfig();
            }

            if (ImGui::SliderFloat("SSR Roughness Cutoff", &RendererConfig.SsrRoughnessCutoff, 0.0f, 1.0f, "%.2f"))
            {
                SyncDeferredSsrConfig();
            }

            if (ImGui::SliderFloat("SSR Intensity", &RendererConfig.SsrIntensity, 0.0f, 2.0f, "%.2f"))
            {
                SyncDeferredSsrConfig();
            }
        }

        const bool bIndirectDrawSupported = Device && Device->IsShaderModelForIndirectDrawSupported();
        bool bIndirectDraw = RendererConfig.bEnableIndirectDraw;
		ImGui::Separator();
        if (ImGui::Checkbox("Indirect Draw", &bIndirectDraw))
        {
            RendererConfig.bEnableIndirectDraw = bIndirectDrawSupported && bIndirectDraw;

            if (DeferredRenderer)
            {
                DeferredRenderer->SetIndirectDrawEnabled(RendererConfig.bEnableIndirectDraw);
            }

            if (ForwardRenderer)
            {
                ForwardRenderer->SetIndirectDrawEnabled(RendererConfig.bEnableIndirectDraw);
            }

            if (bIndirectDraw && !bIndirectDrawSupported)
            {
                LogWarning("Indirect draw requires Shader Model 6.8 and remains disabled.");
            }
        }

		ImGui::SameLine();
        bool bSkinningIndirectDraw = RendererConfig.bEnableSkinningIndirectDraw;
        if (ImGui::Checkbox("Skinning", &bSkinningIndirectDraw))
        {
            RendererConfig.bEnableSkinningIndirectDraw = bSkinningIndirectDraw;

            if (DeferredRenderer)
            {
                DeferredRenderer->SetSkinningIndirectDrawEnabled(RendererConfig.bEnableSkinningIndirectDraw);
            }

            if (ForwardRenderer)
            {
                ForwardRenderer->SetSkinningIndirectDrawEnabled(RendererConfig.bEnableSkinningIndirectDraw);
            }
        }
        /*
        ImGui::SameLine();
        bool bPbrResearch = RendererConfig.bEnablePbrResearch;
        if (ImGui::Checkbox("PBR 2", &bPbrResearch))
        {
            RendererConfig.bEnablePbrResearch = bPbrResearch;
            SyncDeferredLightingPassConfig();
        }
        */
        if (DeferredRenderer)
        {
            const bool bClusterDagRuntimeEnabled = RendererConfig.bEnableClusterDAGRuntime;
            if (bClusterDagRuntimeEnabled)
            {
				ImGui::Separator();
                bool bClusterDagForceSoftwareRaster = RendererConfig.bEnableClusterDAGForceSoftwareRaster;
                if (ImGui::Checkbox("Cluster DAG Force SW Raster", &bClusterDagForceSoftwareRaster))
                {
                    RendererConfig.bEnableClusterDAGForceSoftwareRaster = bClusterDagForceSoftwareRaster;
                    UpsertConfigValue(
                        GetRendererConfigPath(),
                        "ClusterDAGForceSoftwareRaster",
                        RendererConfig.bEnableClusterDAGForceSoftwareRaster ? "true" : "false");
                    SyncDeferredClusterDagConfig();
                }

                ImGui::SetNextItemWidth(160.0f);
                float ClusterDagSwRasterThresholdPixels = RendererConfig.ClusterDAGSwRasterThresholdPixels;
                if (ImGui::SliderFloat("Cluster DAG SW Edge Pixels", &ClusterDagSwRasterThresholdPixels, 0.0f, 64.0f, "%.1f"))
                {
                    RendererConfig.ClusterDAGSwRasterThresholdPixels = (std::max)(0.0f, ClusterDagSwRasterThresholdPixels);
                    SyncDeferredClusterDagConfig();
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    UpsertConfigValue(
                        GetRendererConfigPath(),
                        "ClusterDAGSwRasterThresholdPixels",
                        std::to_string(RendererConfig.ClusterDAGSwRasterThresholdPixels));
                }

                bool bClusterDagSwRasterHzbReject = RendererConfig.bEnableClusterDAGSwRasterHzbReject;
                if (ImGui::Checkbox("Cluster DAG SW HZB Reject", &bClusterDagSwRasterHzbReject))
                {
                    RendererConfig.bEnableClusterDAGSwRasterHzbReject = bClusterDagSwRasterHzbReject;
                    UpsertConfigValue(
                        GetRendererConfigPath(),
                        "ClusterDAGSwRasterHzbReject",
                        RendererConfig.bEnableClusterDAGSwRasterHzbReject ? "true" : "false");
                    SyncDeferredClusterDagConfig();
                }

                bool bClusterDagStreaming = RendererConfig.bEnableClusterDAGStreaming;
                if (ImGui::Checkbox("Cluster DAG Streaming", &bClusterDagStreaming))
                {
                    RendererConfig.bEnableClusterDAGStreaming = bClusterDagStreaming;
                    UpsertConfigValue(
                        GetRendererConfigPath(),
                        "ClusterDAGStreaming",
                        RendererConfig.bEnableClusterDAGStreaming ? "true" : "false");
                    SyncDeferredClusterDagConfig();
                }

                if (bClusterDagStreaming)
                {
                    ImGui::SetNextItemWidth(160.0f);
                    int StreamingPoolMB = static_cast<int>(RendererConfig.ClusterDAGStreamingPoolMB);
                    if (ImGui::InputInt("Cluster DAG Stream Pool MB", &StreamingPoolMB))
                    {
                        RendererConfig.ClusterDAGStreamingPoolMB = static_cast<uint32_t>(std::clamp(StreamingPoolMB, 1, 4096));
                        SyncDeferredClusterDagConfig();
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        UpsertConfigValue(
                            GetRendererConfigPath(),
                            "ClusterDAGStreamingPoolMB",
                            std::to_string(RendererConfig.ClusterDAGStreamingPoolMB));
                    }

                    ImGui::SetNextItemWidth(160.0f);
                    int InstallsPerFrame = static_cast<int>(RendererConfig.ClusterDAGStreamingMaxPageInstallsPerFrame);
                    if (ImGui::InputInt("Cluster DAG Installs/Frame", &InstallsPerFrame))
                    {
                        RendererConfig.ClusterDAGStreamingMaxPageInstallsPerFrame = static_cast<uint32_t>(std::clamp(InstallsPerFrame, 1, 1024));
                        SyncDeferredClusterDagConfig();
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        UpsertConfigValue(
                            GetRendererConfigPath(),
                            "ClusterDAGStreamingMaxPageInstallsPerFrame",
                            std::to_string(RendererConfig.ClusterDAGStreamingMaxPageInstallsPerFrame));
                    }

                    ImGui::SetNextItemWidth(160.0f);
                    int MaxIoInFlight = static_cast<int>(RendererConfig.ClusterDAGStreamingMaxIoInFlight);
                    if (ImGui::InputInt("Cluster DAG Max IO In Flight", &MaxIoInFlight))
                    {
                        RendererConfig.ClusterDAGStreamingMaxIoInFlight = static_cast<uint32_t>(std::clamp(MaxIoInFlight, 1, 1024));
                        SyncDeferredClusterDagConfig();
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        UpsertConfigValue(
                            GetRendererConfigPath(),
                            "ClusterDAGStreamingMaxIoInFlight",
                            std::to_string(RendererConfig.ClusterDAGStreamingMaxIoInFlight));
                    }

                    ImGui::SetNextItemWidth(160.0f);
                    int MaxUploadMB = static_cast<int>((RendererConfig.ClusterDAGStreamingMaxPageUploadBytesPerFrame + 1024u * 1024u - 1u) / (1024u * 1024u));
                    if (ImGui::InputInt("Cluster DAG Max Upload MB/Frame", &MaxUploadMB))
                    {
                        MaxUploadMB = std::clamp(MaxUploadMB, 1, 1024);
                        RendererConfig.ClusterDAGStreamingMaxPageUploadBytesPerFrame = static_cast<uint32_t>(MaxUploadMB) * 1024u * 1024u;
                        SyncDeferredClusterDagConfig();
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        UpsertConfigValue(
                            GetRendererConfigPath(),
                            "ClusterDAGStreamingMaxPageUploadBytesPerFrame",
                            std::to_string(RendererConfig.ClusterDAGStreamingMaxPageUploadBytesPerFrame));
                    }

                    if (DeferredRenderer && ActiveRenderer == DeferredRenderer.get())
                    {
                        if (const FClusterDagStreamingManager* StreamingManager = DeferredRenderer->GetClusterDagStreamingManager())
                        {
                            ImGui::Text(
                                "Stream Pages R/P/I: %u / %u / %u",
                                StreamingManager->GetResidentPageCount(),
                                StreamingManager->GetPendingPageCount(),
                                StreamingManager->GetLastFrameInstallCount());
                            ImGui::Text(
                                "Stream Req/Drop/Repl: %u / %u / %u",
                                StreamingManager->GetLastFrameRequestCount(),
                                StreamingManager->GetDroppedRequestCount(),
                                StreamingManager->GetReplacedRequestCount());
                            ImGui::Text(
                                "Stream IO F/Iss/Done/Fail: %u / %u / %u / %u",
                                StreamingManager->GetIoInFlightCount(),
                                StreamingManager->GetLastFrameIoIssueCount(),
                                StreamingManager->GetLastFrameIoCompleteCount(),
                                StreamingManager->GetLastFrameIoFailCount());
                            ImGui::Text(
                                "Stream Upload F/Iss/Done/MB: %u / %u / %u / %u",
                                StreamingManager->GetUploadInFlightCount(),
                                StreamingManager->GetLastFrameUploadIssueCount(),
                                StreamingManager->GetLastFrameUploadCompleteCount(),
                                (StreamingManager->GetLastFrameUploadBytes() + 1024u * 1024u - 1u) / (1024u * 1024u));
                            ImGui::Text(
                                "Stream Slots U/T/F: %u / %u / %u",
                                StreamingManager->GetUsedPageSlotCount(),
                                StreamingManager->GetPageSlotCount(),
                                StreamingManager->GetFreePageSlotCount());
                            ImGui::Text(
                                "Stream Slot KB/Full Drops: %u / %u",
                                (StreamingManager->GetPageSlotBytes() + 1023u) / 1024u,
                                StreamingManager->GetSlotPoolFullDropCount());
                        }
                    }
                }

				bool bClusterDagForceMip = RendererConfig.bEnableClusterDAGForceMip;
				if (ImGui::Checkbox("Cluster DAG Force Mip", &bClusterDagForceMip))
				{
					RendererConfig.bEnableClusterDAGForceMip = bClusterDagForceMip;
					SyncDeferredClusterDagConfig();
				}

                if (bClusterDagForceMip)
                {
					ImGui::SetNextItemWidth(160.0f);
					int ClusterDagForceMipLevelValue = static_cast<int>(RendererConfig.ClusterDAGForceMipLevel);
					if (ImGui::InputInt("Cluster DAG Force Mip Level", &ClusterDagForceMipLevelValue))
					{
						RendererConfig.ClusterDAGForceMipLevel = static_cast<uint32_t>((std::max)(0, ClusterDagForceMipLevelValue));
						SyncDeferredClusterDagConfig();
					}
                }
            }
        }

        ImGui::Separator();
        bool bShadows = RendererConfig.bEnableShadows;
        if (ImGui::Checkbox("Shadows", &bShadows))
        {
            RendererConfig.bEnableShadows = bShadows;

            if (DeferredRenderer)
            {
                DeferredRenderer->SetShadowsEnabled(RendererConfig.bEnableShadows);
            }

            if (ForwardRenderer)
            {
                ForwardRenderer->SetShadowsEnabled(RendererConfig.bEnableShadows);
            }
        }

		ImGui::SameLine();
        bool bRayTracedShadows = RendererConfig.bEnableRayTracedShadows;
        if (ImGui::Checkbox("Ray Traced Shadows", &bRayTracedShadows))
        {
            if (bRayTracedShadows && !bRayTracingSupported)
            {
                RendererConfig.bEnableRayTracedShadows = false;
                LogWarning("Ray traced shadows requested, but DXR is not supported. Falling back to raster shadows.");
            }
            else
            {
                RendererConfig.bEnableRayTracedShadows = bRayTracedShadows;
            }

            if (DeferredRenderer)
            {
                DeferredRenderer->SetRayTracedShadowsEnabled(RendererConfig.bEnableRayTracedShadows);
            }

            if (ForwardRenderer)
            {
                ForwardRenderer->SetRayTracedShadowsEnabled(RendererConfig.bEnableRayTracedShadows);
            }
        }

		//float ShadowBiasValue = ActiveRenderer ? ActiveRenderer->GetShadowBias() : 0.0f;
        //if (ImGui::SliderFloat("Shadow Bias", &ShadowBiasValue, 0.0f, 0.01f, "%.5f"))
        //{
        //    if (DeferredRenderer)
        //    {
        //        DeferredRenderer->SetShadowBias(ShadowBiasValue);
        //    }

        //    if (ForwardRenderer)
        //    {
        //        ForwardRenderer->SetShadowBias(ShadowBiasValue);
        //    }
        //}

		ImGui::Separator();
        if (ImGui::Checkbox("GTAO", &RendererConfig.bEnableGtao))
        {
			SyncRendererGtaoConfig();
        }

        if (RendererConfig.bEnableGtao)
        {
			ImGui::SameLine();
			if (ImGui::Checkbox("GTAO Jitter", &RendererConfig.bEnableGtaoJitter))
			{
				SyncRendererGtaoConfig();
			}

			if (ImGui::SliderFloat("GTAO Radius", &RendererConfig.GtaoRadius, 0.05f, 3.0f, "%.2f"))
			{
				SyncRendererGtaoConfig();
			}

			if (ImGui::SliderFloat("GTAO Thickness", &RendererConfig.GtaoThickness, 0.0f, 1.0f, "%.2f"))
			{
				SyncRendererGtaoConfig();
			}
        }
        
		//if (ImGui::Checkbox("Section Pix Events", &RendererConfig.bEnableSectionPixEvents))
		//{
		//	SetSectionPixEventsEnabled(RendererConfig.bEnableSectionPixEvents);
		//}

        ImGui::Separator();
        if (ImGui::Checkbox("Tonemap", &RendererConfig.bEnableTonemap))
        {
            SyncDeferredPostProcessConfig();
        }
		ImGui::SameLine();
		if (ImGui::Checkbox("Auto Exposure", &RendererConfig.bEnableAutoExposure))
		{
			SyncDeferredPostProcessConfig();
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("CAS", &RendererConfig.bEnableCas))
		{
			SyncDeferredPostProcessConfig();
		}
		ImGui::SameLine();
		if (ImGui::Checkbox("TAA", &RendererConfig.bEnableTAA))
		{
			SyncDeferredPostProcessConfig();
		}

        if (RendererConfig.bEnableTonemap)
        {
			if (ImGui::SliderFloat("Tonemap Exposure", &RendererConfig.TonemapExposure, 0.1f, 5.0f, "%.2f"))
			{
				SyncDeferredPostProcessConfig();
			}

			if (ImGui::SliderFloat("Tonemap Gamma", &RendererConfig.TonemapGamma, 1.0f, 3.0f, "%.2f"))
			{
				SyncDeferredPostProcessConfig();
			}
        }

        if (RendererConfig.bEnableAutoExposure)
        {
			if (ImGui::SliderFloat("Exposure Key", &RendererConfig.AutoExposureKey, 0.05f, 1.0f, "%.2f"))
			{
				SyncDeferredPostProcessConfig();
			}

			if (ImGui::SliderFloat("Exposure Min", &RendererConfig.AutoExposureMin, 0.01f, 2.0f, "%.2f"))
			{
				RendererConfig.AutoExposureMax = (std::max)(RendererConfig.AutoExposureMax, RendererConfig.AutoExposureMin + 0.01f);
				SyncDeferredPostProcessConfig();
			}

			if (ImGui::SliderFloat("Exposure Max", &RendererConfig.AutoExposureMax, 0.1f, 10.0f, "%.2f"))
			{
				RendererConfig.AutoExposureMax = (std::max)(RendererConfig.AutoExposureMax, RendererConfig.AutoExposureMin + 0.01f);
				SyncDeferredPostProcessConfig();
			}

			if (ImGui::SliderFloat("Exposure Speed Up", &RendererConfig.AutoExposureSpeedUp, 0.1f, 10.0f, "%.2f"))
			{
				SyncDeferredPostProcessConfig();
			}

			if (ImGui::SliderFloat("Exposure Speed Down", &RendererConfig.AutoExposureSpeedDown, 0.1f, 10.0f, "%.2f"))
			{
				SyncDeferredPostProcessConfig();
			}
        }

        if (RendererConfig.bEnableCas)
        {
			if (ImGui::SliderFloat("CAS Sharpness", &RendererConfig.CasSharpness, 0.0f, 1.0f, "%.2f"))
			{
				SyncDeferredPostProcessConfig();
			}
        }

        if (RendererConfig.bEnableTAA)
        {
			if (ImGui::SliderFloat("TAA History Weight", &RendererConfig.TaaHistoryWeight, 0.0f, 0.98f, "%.2f"))
			{
				SyncDeferredPostProcessConfig();
			}
        }

        ImGui::Separator();
        bool bLightingChanged = false;

        float YawDegrees = DirectX::XMConvertToDegrees(RendererConfig.LightYaw);
        if (ImGui::SliderFloat("Light Yaw", &YawDegrees, -180.0f, 180.0f, "%.1f deg"))
        {
            RendererConfig.LightYaw = DirectX::XMConvertToRadians(YawDegrees);
            bLightingChanged = true;
        }

        float PitchDegrees = DirectX::XMConvertToDegrees(RendererConfig.LightPitch);
        if (ImGui::SliderFloat("Light Pitch", &PitchDegrees, -89.0f, 89.0f, "%.1f deg"))
        {
            RendererConfig.LightPitch = DirectX::XMConvertToRadians(PitchDegrees);
            bLightingChanged = true;
        }

        if (ImGui::SliderFloat("Light Intensity", &RendererConfig.LightIntensity, 0.0f, 5.0f, "%.2f"))
        {
            bLightingChanged = true;
        }

        if (bLightingChanged)
        {
            UpdateRendererLighting();
        }
    }

    if (ActiveRenderer && ImGui::CollapsingHeader("World Objects"))
    {
        const FWorld& SceneWorld = ActiveRenderer->GetWorld();
        const std::vector<std::unique_ptr<FObject>>& Objects = SceneWorld.GetObjects();
        ImGui::Text("Objects: %zu", Objects.size());
        ImGui::Separator();

        for (size_t ObjectIndex = 0; ObjectIndex < Objects.size(); ++ObjectIndex)
        {
            const FObject* Object = Objects[ObjectIndex].get();
            if (!Object)
            {
                continue;
            }

            const bool bSkeletal = Object->GetType() == EObjectType::SkeletalMesh;
            const char* TypeLabel = bSkeletal ? "Skeletal" : "Static";
            const std::vector<FMeshSection>& Sections = Object->GetSections();

            const std::string Label = std::string(Object->GetName().empty() ? "(unnamed)" : Object->GetName())
                + "  [" + TypeLabel + ", " + std::to_string(Sections.size()) + " sec]##obj" + std::to_string(ObjectIndex);

            if (ImGui::TreeNode(Label.c_str()))
            {
                ImGui::Text("ObjectId: %u", Object->GetObjectId());
                if (bSkeletal)
                {
                    ImGui::Text("Skin: %d", static_cast<const FSkeletalMesh*>(Object)->GetGltfSkinIndex());
                }

                const DirectX::XMFLOAT4X4& M = Object->GetWorldMatrix();
                ImGui::Text("Position: %.2f, %.2f, %.2f", M._41, M._42, M._43);

                std::string SectionList;
                for (size_t s = 0; s < Sections.size(); ++s)
                {
                    SectionList += Sections[s].Name.empty() ? std::to_string(s) : Sections[s].Name;
                    if (s + 1 < Sections.size()) { SectionList += ", "; }
                }
                ImGui::TextWrapped("Sections: %s", SectionList.c_str());
                ImGui::TreePop();
            }
        }
    }

    ImGui::End();

    if (Camera)
    {
        DrawAxisGizmo(Camera->GetViewMatrix(), Io.DisplaySize);
    }

    ImGui::Render();

    ID3D12DescriptorHeap* Heaps[] = { ImGuiDescriptorHeap.Get() };
    CommandContext->GetCommandList()->SetDescriptorHeaps(_countof(Heaps), Heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), CommandContext->GetCommandList());
#endif
}
