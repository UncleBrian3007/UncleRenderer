#include <Windows.h>
#include "Core/Application.h"

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    FApplication App;
    if (!App.Initialize(hInstance, 1280, 720))
    {
        return -1;
    }

    return App.Run();
}

