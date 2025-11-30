#pragma once

#include <Windows.h>

// Helper macro to detect ImGui availability at build time.
#if defined(__has_include)
#  if __has_include(<imgui.h>) && __has_include(<backends/imgui_impl_dx12.h>) && __has_include(<backends/imgui_impl_win32.h>)
#    define WITH_IMGUI 1
#  else
#    define WITH_IMGUI 0
#  endif
#else
#  define WITH_IMGUI 0
#endif

#if WITH_IMGUI
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#endif

inline bool ImGuiHandleWin32Message(HWND Hwnd, UINT Message, WPARAM WParam, LPARAM LParam)
{
#if WITH_IMGUI
    if (ImGui::GetCurrentContext())
    {
        return ImGui_ImplWin32_WndProcHandler(Hwnd, Message, WParam, LParam);
    }
#endif

    (void)Hwnd;
    (void)Message;
    (void)WParam;
    (void)LParam;
    return false;
}

