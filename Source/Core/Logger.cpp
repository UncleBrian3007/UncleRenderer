#include "Logger.h"
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>

namespace
{
    std::once_flag GLogInitFlag;
    std::ofstream GLogFile;
    std::atomic<ELogLevel> GCurrentLogLevel = ELogLevel::Info;

    void InitializeLogFile()
    {
        std::call_once(GLogInitFlag, []
            {
                char Path[MAX_PATH] = {};
                if (GetModuleFileNameA(nullptr, Path, MAX_PATH) == 0)
                {
                    return;
                }

                std::filesystem::path LogPath(Path);
                LogPath = LogPath.parent_path() / "UncleRenderer.log";

                GLogFile.open(LogPath, std::ios::out | std::ios::app);
                if (GLogFile.is_open())
                {
                    SYSTEMTIME SystemTime;
                    GetLocalTime(&SystemTime);
                    GLogFile << "\n----- Log Start "
                             << SystemTime.wYear << "-" << SystemTime.wMonth << "-" << SystemTime.wDay << " "
                             << SystemTime.wHour << ":" << SystemTime.wMinute << ":" << SystemTime.wSecond
                             << " -----\n";
                }
            });
    }

    const char* GetPrefix(ELogLevel Level)
    {
        switch (Level)
        {
        case ELogLevel::Verbose: return "[VERBOSE] ";
        case ELogLevel::Info:    return "[INFO] ";
        case ELogLevel::Warning: return "[WARN] ";
        case ELogLevel::Error:   return "[ERROR] ";
        default:                 return "[LOG] ";
        }
    }

    bool ShouldLog(ELogLevel Level)
    {
        return static_cast<int>(Level) >= static_cast<int>(GCurrentLogLevel.load());
    }
}

void LogMessage(ELogLevel Level, const std::string& Message)
{
    if (!ShouldLog(Level))
    {
        return;
    }

    const std::string Output = std::string("[UncleRenderer] ") + GetPrefix(Level) + Message + "\n";

    InitializeLogFile();
    if (GLogFile.is_open())
    {
        GLogFile << Output;
        GLogFile.flush();
    }

    OutputDebugStringA(Output.c_str());
}

void SetLogLevel(ELogLevel Level)
{
    GCurrentLogLevel.store(Level);
}

ELogLevel GetLogLevel()
{
    return GCurrentLogLevel.load();
}

void LogVerbose(const std::string& Message)
{
    LogMessage(ELogLevel::Verbose, Message);
}

void LogInfo(const std::string& Message)
{
    LogMessage(ELogLevel::Info, Message);
}

void LogWarning(const std::string& Message)
{
    LogMessage(ELogLevel::Warning, Message);
}

void LogError(const std::string& Message)
{
    LogMessage(ELogLevel::Error, Message);
}
