#include "Application.h"
#include "Window.h"
#include "EngineTime.h"
#include "ImGuiSupport.h"
#include "Logger.h"
#include "GpuDebugMarkers.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12SwapChain.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Commons.h"
#include "../Render/Renderer.h"
#include "../Render/DeferredRenderer.h"
#include "../Render/ForwardRenderer.h"
#include "../Scene/Camera.h"
#include "../Scene/SceneJsonLoader.h"
#include "RendererConfig.h"
#include <dxgi1_6.h>
#include <commdlg.h>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <DirectXMath.h>
#include <limits>
#include <string>
#include <filesystem>
#include <iterator>

namespace
{
    std::string PathToUtf8String(const std::wstring& Path)
    {
        const auto Utf8 = std::filesystem::path(Path).u8string();
        return std::string(Utf8.begin(), Utf8.end());
    }

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
    ShutdownImGui();

    if (Device)
    {
        Device->GetGraphicsQueue()->Flush();
    }

    LogInfo("Application shutdown complete");
}

bool FApplication::Initialize(HINSTANCE InstanceHandle, int32_t Width, int32_t Height)
{
    LogInfo("Application initialization started");

    MainWindow = std::make_unique<FWindow>();
    Device = std::make_unique<FDX12Device>();
    SwapChain = std::make_unique<FDX12SwapChain>();
    CommandContext = std::make_unique<FDX12CommandContext>();
    Time = std::make_unique<FTime>();
    ForwardRenderer = std::make_unique<FForwardRenderer>();
    DeferredRenderer = std::make_unique<FDeferredRenderer>();
    Camera = std::make_unique<FCamera>();

    const std::filesystem::path ConfigPath = std::filesystem::current_path() / "bin/RendererConfig.ini";
    RendererConfig = FRendererConfigLoader::LoadOrDefault(ConfigPath);
    bFrameOverlapEnabled = RendererConfig.bEnableFrameOverlap;
    bDepthPrepassEnabled = RendererConfig.bUseDepthPrepass;

    FRendererOptions RendererOptions{};
    RendererOptions.SceneFilePath = RendererConfig.SceneFile;
    RendererOptions.bUseDepthPrepass = RendererConfig.bUseDepthPrepass;
    RendererOptions.bEnableShadows = bShadowsEnabled;
    RendererOptions.ShadowBias = ShadowBias;

    const std::wstring SceneFilePath = RendererOptions.SceneFilePath.empty() ? L"Assets/Scenes/Scene.json" : RendererOptions.SceneFilePath;
    RendererOptions.SceneFilePath = SceneFilePath;
    CurrentScenePath = SceneFilePath;

    FSceneLightDesc SceneLight;
    if (FSceneJsonLoader::LoadSceneLighting(SceneFilePath, SceneLight))
    {
        LightIntensity = SceneLight.Intensity;
        LightColor = DirectX::XMFLOAT3(SceneLight.Color.x, SceneLight.Color.y, SceneLight.Color.z);

        DirectX::XMFLOAT3 Direction(SceneLight.Direction.x, SceneLight.Direction.y, SceneLight.Direction.z);
        const DirectX::XMVECTOR DirectionVec = DirectX::XMVector3Normalize(DirectX::XMLoadFloat3(&Direction));
        const float LengthSq = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(DirectionVec));
        if (LengthSq > 0.0f)
        {
            LightPitch = asinf(DirectX::XMVectorGetY(DirectionVec));
            LightYaw = atan2f(DirectX::XMVectorGetX(DirectionVec), DirectX::XMVectorGetZ(DirectionVec));
        }
    }

    LogInfo("Creating window...");
    if (!MainWindow->Create(InstanceHandle, Width, Height, L"UncleRenderer"))
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

    LogInfo("Initializing swap chain...");
    if (!SwapChain->Initialize(Device.get(), MainWindow->GetHWND(), Width, Height, 3))
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

    Camera->SetPerspective(DirectX::XM_PIDIV4, static_cast<float>(Width) / static_cast<float>(Height), 0.1f, 1000.0f);

    auto TryInitializeRenderer = [&](ERendererType Type) -> bool
    {
        if (Type == ERendererType::Deferred)
        {
            LogInfo("Attempting to initialize deferred renderer...");
            if (DeferredRenderer->Initialize(Device.get(), Width, Height, SwapChain->GetFormat(), RendererOptions))
            {
                LogInfo("Deferred renderer activated");
                ActiveRenderer = DeferredRenderer.get();
                return true;
            }

            LogWarning("Deferred renderer initialization failed");
            return false;
        }

        LogInfo("Attempting to initialize forward renderer...");
        if (ForwardRenderer->Initialize(Device.get(), Width, Height, SwapChain->GetFormat(), RendererOptions))
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

    UpdateRendererLighting();
    PositionCameraForScene();

    if (!InitializeImGui(Width, Height))
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

    if (!PendingScenePath.empty())
    {
        if (Device && Device->GetGraphicsQueue())
        {
            Device->GetGraphicsQueue()->Flush();
        }

        const std::wstring SceneToLoad = std::move(PendingScenePath);
        PendingScenePath.clear();

        if (!ReloadScene(SceneToLoad))
        {
            LogError("Failed to reload scene: " + PathToUtf8String(SceneToLoad));
        }
    }

    Time->Tick();
    const float DeltaSeconds = static_cast<float>(Time->GetDeltaTimeSeconds());

    HandleCameraInput(DeltaSeconds);

    const uint32 BackBufferIndex = SwapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* BackBuffer = SwapChain->GetBackBuffer(BackBufferIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = SwapChain->GetRTV(BackBufferIndex);

    const D3D12_RESOURCE_STATES PreviousState = SwapChain->GetBackBufferState(BackBufferIndex);

    CommandContext->BeginFrame(BackBufferIndex);

    {
        FScopedPixEvent FrameEvent(CommandContext->GetCommandList(), L"Frame");

        CommandContext->TransitionResource(
            BackBuffer,
            PreviousState,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        const D3D12_CPU_DESCRIPTOR_HANDLE* DsvHandle = ActiveRenderer ? &ActiveRenderer->GetDSVHandle() : nullptr;

        CommandContext->SetRenderTarget(RtvHandle, DsvHandle);

        const float ClearColor[4] = { 0.05f, 0.10f, 0.20f, 1.0f };
        CommandContext->ClearRenderTarget(RtvHandle, ClearColor);

        if (ActiveRenderer && Camera)
        {
            ActiveRenderer->RenderFrame(*CommandContext, RtvHandle, *Camera, DeltaSeconds);
        }

        RenderUI();

        CommandContext->TransitionResource(
            BackBuffer,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);

        PixSetMarker(CommandContext->GetCommandList(), L"Present");
    }
    CommandContext->CloseAndExecute();

    LogVerbose("Preparing frame end: " + std::to_string(FrameIndex));

    SwapChain->SetBackBufferState(BackBufferIndex, D3D12_RESOURCE_STATE_PRESENT);

    const UINT PresentFlags = SwapChain->AllowsTearing() ? DXGI_PRESENT_ALLOW_TEARING : 0;
    LogVerbose("Present called (Flags: " + std::to_string(PresentFlags) + ")");
    HR_CHECK(SwapChain->GetSwapChain()->Present(0, PresentFlags));

    const uint64 FenceValue = Device->GetGraphicsQueue()->Signal();
    if (!bFrameOverlapEnabled)
    {
        Device->GetGraphicsQueue()->Wait(FenceValue);
    }
    CommandContext->SetFrameFenceValue(BackBufferIndex, FenceValue);

    LogVerbose("Frame completed: " + std::to_string(FrameIndex));

    return true;
}

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
    if (LeftButtonDown)
    {
        POINT CursorPos{};
        if (GetCursorPos(&CursorPos))
        {
            if (!bIsRotatingWithMouse)
            {
                bIsRotatingWithMouse = true;
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

    CameraPitch = asinf(DirectX::XMVectorGetY(ForwardVec));
    CameraYaw = atan2f(DirectX::XMVectorGetX(ForwardVec), DirectX::XMVectorGetZ(ForwardVec));
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

    FRendererOptions RendererOptions{};
    RendererOptions.SceneFilePath = ScenePath;
    RendererOptions.bUseDepthPrepass = bDepthPrepassEnabled;
    RendererOptions.bEnableShadows = bShadowsEnabled;
    RendererOptions.ShadowBias = ShadowBias;

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
            if (NewDeferredRenderer->Initialize(Device.get(), Width, Height, BackBufferFormat, RendererOptions))
            {
                NewActiveRenderer = NewDeferredRenderer.get();
                return true;
            }
            return false;
        }

        if (NewForwardRenderer->Initialize(Device.get(), Width, Height, BackBufferFormat, RendererOptions))
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

    CurrentScenePath = ScenePath;
    RendererConfig.SceneFile = ScenePath;

    UpdateRendererLighting();
    PositionCameraForScene();

    LogInfo("Scene reloaded from: " + PathToUtf8String(ScenePath));
    return true;
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
        LogWarning("Failed to resolve absolute scene directory: " + PathToUtf8String(InitialPath));
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
        LogWarning("Failed to restore working directory after file dialog: " + PathToUtf8String(OriginalWorkingDir));
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
    const DirectX::XMMATRIX Rotation = DirectX::XMMatrixRotationRollPitchYaw(LightPitch, LightYaw, 0.0f);
    return DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(Forward, Rotation));
}

void FApplication::UpdateRendererLighting() const
{
    DirectX::XMFLOAT3 Direction{};
    DirectX::XMStoreFloat3(&Direction, GetLightDirectionVector());

    if (ForwardRenderer)
    {
        ForwardRenderer->SetLightDirection(Direction);
        ForwardRenderer->SetLightIntensity(LightIntensity);
        ForwardRenderer->SetLightColor(LightColor);
    }

    if (DeferredRenderer)
    {
        DeferredRenderer->SetLightDirection(Direction);
        DeferredRenderer->SetLightIntensity(LightIntensity);
        DeferredRenderer->SetLightColor(LightColor);
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
    HeapDesc.NumDescriptors = 1;
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

    ImGui::Separator();
    ImGui::Text("Scene");
    const std::string ScenePathUtf8 = PathToUtf8String(CurrentScenePath);
    ImGui::TextWrapped("File: %s", ScenePathUtf8.c_str());
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

    DXGI_QUERY_VIDEO_MEMORY_INFO LocalMemoryInfo = {};
    if (Device && Device->QueryLocalVideoMemory(LocalMemoryInfo))
    {
        const double UsageMB = static_cast<double>(LocalMemoryInfo.CurrentUsage) / (1024.0 * 1024.0);
        const double BudgetMB = static_cast<double>(LocalMemoryInfo.Budget) / (1024.0 * 1024.0);
        const double AvailableMB = static_cast<double>(LocalMemoryInfo.AvailableForReservation) / (1024.0 * 1024.0);
		const double ReservedMB = static_cast<double>(LocalMemoryInfo.CurrentReservation) / (1024.0 * 1024.0);

        ImGui::Separator();
        ImGui::Text("GPU Memory (Local)");
        ImGui::Text("Usage: %.1f MB", UsageMB);
        ImGui::Text("Budget: %.1f MB", BudgetMB);
        ImGui::Text("Available: %.1f MB", AvailableMB);
		ImGui::Text("Reserved: %.1f MB", ReservedMB);
    }

    bool bFrameOverlap = bFrameOverlapEnabled;
    if (ImGui::Checkbox("Frame Overlap", &bFrameOverlap))
    {
        bFrameOverlapEnabled = bFrameOverlap;
    }

    if (Device && Device->GetGraphicsQueue())
    {
        const uint64 CompletedFence = Device->GetGraphicsQueue()->GetCompletedFenceValue();
        const uint64 LastSignaledFence = Device->GetGraphicsQueue()->GetLastSignaledFenceValue();
        const uint64 InFlightFrames = (LastSignaledFence > CompletedFence) ? (LastSignaledFence - CompletedFence) : 0;

        ImGui::Text("In-flight frames: %llu", static_cast<unsigned long long>(InFlightFrames));
//        ImGui::Text("GPU fences: completed %llu / last signaled %llu", static_cast<unsigned long long>(CompletedFence), static_cast<unsigned long long>(LastSignaledFence));
    }

    bool bDepthPrepass = bDepthPrepassEnabled;
    if (ImGui::Checkbox("Depth Prepass", &bDepthPrepass))
    {
        bDepthPrepassEnabled = bDepthPrepass;

        if (ForwardRenderer)
        {
            ForwardRenderer->SetDepthPrepassEnabled(bDepthPrepassEnabled);
        }

        if (DeferredRenderer)
        {
            DeferredRenderer->SetDepthPrepassEnabled(bDepthPrepassEnabled);
        }
    }

    bool bShadows = bShadowsEnabled;
    if (ImGui::Checkbox("Shadows", &bShadows))
    {
        bShadowsEnabled = bShadows;

        if (DeferredRenderer)
        {
            DeferredRenderer->SetShadowsEnabled(bShadowsEnabled);
        }

        if (ForwardRenderer)
        {
            ForwardRenderer->SetShadowsEnabled(bShadowsEnabled);
        }
    }

    float ShadowBiasValue = ShadowBias;
    if (ImGui::SliderFloat("Shadow Bias", &ShadowBiasValue, 0.0f, 0.01f, "%.5f"))
    {
        ShadowBias = ShadowBiasValue;

        if (DeferredRenderer)
        {
            DeferredRenderer->SetShadowBias(ShadowBias);
        }

        if (ForwardRenderer)
        {
            ForwardRenderer->SetShadowBias(ShadowBias);
        }
    }

    ImGui::Separator();
    bool bLightingChanged = false;

    float YawDegrees = DirectX::XMConvertToDegrees(LightYaw);
    if (ImGui::SliderFloat("Light Yaw", &YawDegrees, -180.0f, 180.0f, "%.1f deg"))
    {
        LightYaw = DirectX::XMConvertToRadians(YawDegrees);
        bLightingChanged = true;
    }

    float PitchDegrees = DirectX::XMConvertToDegrees(LightPitch);
    if (ImGui::SliderFloat("Light Pitch", &PitchDegrees, -89.0f, 89.0f, "%.1f deg"))
    {
        LightPitch = DirectX::XMConvertToRadians(PitchDegrees);
        bLightingChanged = true;
    }

    if (ImGui::SliderFloat("Light Intensity", &LightIntensity, 0.0f, 5.0f, "%.2f"))
    {
        bLightingChanged = true;
    }

    if (bLightingChanged)
    {
        UpdateRendererLighting();
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

