#include "Window.h"
#include "ImGuiSupport.h"
#include "Logger.h"

FWindow::FWindow()
    : WindowHandle(nullptr)
    , Width(1280)
    , Height(720)
    , WindowClassName(L"UncleRendererWindow")
{
}

FWindow::~FWindow()
{
    if (WindowHandle)
    {
        DestroyWindow(WindowHandle);
        WindowHandle = nullptr;
    }
}

bool FWindow::Create(HINSTANCE InstanceHandle, int32_t InWidth, int32_t InHeight, const wchar_t* Title)
{
    Width = InWidth;
    Height = InHeight;

    WNDCLASSEXW WndClass = {};
    WndClass.cbSize = sizeof(WNDCLASSEXW);
    WndClass.style = CS_HREDRAW | CS_VREDRAW;
    WndClass.lpfnWndProc = FWindow::WndProc;
    WndClass.hInstance = InstanceHandle;
    WndClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    WndClass.lpszClassName = WindowClassName.c_str();

    if (!RegisterClassExW(&WndClass))
    {
        LogError("Failed to register window class");
        return false;
    }

    RECT WindowRect = { 0, 0, Width, Height };
    AdjustWindowRect(&WindowRect, WS_OVERLAPPEDWINDOW, FALSE);

    WindowHandle = CreateWindowExW(
        0,
        WindowClassName.c_str(),
        Title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        WindowRect.right - WindowRect.left,
        WindowRect.bottom - WindowRect.top,
        nullptr,
        nullptr,
        InstanceHandle,
        this);

    if (!WindowHandle)
    {
        LogError("Failed to create window handle");
        return false;
    }

    ShowWindow(WindowHandle, SW_SHOW);
    UpdateWindow(WindowHandle);
    return true;
}

bool FWindow::ProcessMessages()
{
    MSG Message = {};
    while (PeekMessage(&Message, nullptr, 0, 0, PM_REMOVE))
    {
        if (Message.message == WM_QUIT)
        {
            return false;
        }

        TranslateMessage(&Message);
        DispatchMessage(&Message);
    }
    return true;
}

LRESULT CALLBACK FWindow::WndProc(HWND Hwnd, UINT Message, WPARAM WParam, LPARAM LParam)
{
    if (ImGuiHandleWin32Message(Hwnd, Message, WParam, LParam))
    {
        return true;
    }

    switch (Message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(Hwnd, Message, WParam, LParam);
    }
}

