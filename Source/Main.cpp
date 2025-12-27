#include <Windows.h>
#include "Core/Application.h"
#include "Core/Logger.h"

#include <filesystem>
#include <string>
#include <cwchar>

namespace
{
    std::string ToUtf8(const std::filesystem::path& Path)
    {
        const auto Utf8 = Path.u8string();
        return std::string(Utf8.begin(), Utf8.end());
    }

    bool IsBinDirectory(const std::filesystem::path& Path)
    {
        const std::wstring Name = Path.filename().wstring();
        return _wcsicmp(Name.c_str(), L"bin") == 0;
    }

    void EnsureWorkingDirectory()
    {
        namespace fs = std::filesystem;

        std::error_code Error;
        fs::path CurrentPath = fs::current_path(Error);
        if (Error)
        {
            LogError("Failed to query current working directory: " + std::to_string(Error.value()));
            return;
        }

        LogInfo("Current working directory: " + ToUtf8(CurrentPath));

        wchar_t ExecutablePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, ExecutablePath, MAX_PATH) == 0)
        {
            LogError("Failed to retrieve executable path; skipping working directory adjustment");
            return;
        }

        fs::path ExecutableDirectory(ExecutablePath);
        ExecutableDirectory = ExecutableDirectory.parent_path();
        LogInfo("Executable directory: " + ToUtf8(ExecutableDirectory));

        fs::path DesiredPath = CurrentPath;

        if (IsBinDirectory(ExecutableDirectory))
        {
            DesiredPath = ExecutableDirectory.parent_path();
        }
        else if (IsBinDirectory(CurrentPath))
        {
            DesiredPath = CurrentPath.parent_path();
        }

        if (DesiredPath == CurrentPath)
        {
            LogInfo("Working directory is already set to a non-bin location; no change needed.");
            return;
        }

        LogInfo("Attempting to change working directory to: " + ToUtf8(DesiredPath));
        fs::current_path(DesiredPath, Error);
        if (Error)
        {
            LogError("Failed to change working directory: " + std::to_string(Error.value()));
            return;
        }

        const fs::path NewPath = fs::current_path();
        LogInfo("Working directory updated to: " + ToUtf8(NewPath));
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    EnsureWorkingDirectory();

    FApplication App;
    if (!App.Initialize(hInstance))
    {
        return -1;
    }

    return App.Run();
}
