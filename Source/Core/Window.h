#pragma once

#include <Windows.h>
#include <string>
#include <cstdint>

class FWindow
{
public:
    FWindow();
    ~FWindow();

    bool Create(HINSTANCE InstanceHandle, int32_t Width, int32_t Height, const wchar_t* Title);
    bool ProcessMessages();

    HWND GetHWND() const { return WindowHandle; }
    int32_t GetWidth() const { return Width; }
    int32_t GetHeight() const { return Height; }

private:
    static LRESULT CALLBACK WndProc(HWND Hwnd, UINT Message, WPARAM WParam, LPARAM LParam);

private:
    HWND    WindowHandle;
    int32_t   Width;
    int32_t   Height;
    std::wstring WindowClassName;
};

