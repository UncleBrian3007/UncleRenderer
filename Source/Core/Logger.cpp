#include "Logger.h"
#include <Windows.h>

namespace
{
    const char* GetPrefix(ELogLevel Level)
    {
        switch (Level)
        {
        case ELogLevel::Info:    return "[INFO] ";
        case ELogLevel::Warning: return "[WARN] ";
        case ELogLevel::Error:   return "[ERROR] ";
        default:                 return "[LOG] ";
        }
    }
}

void LogMessage(ELogLevel Level, const std::string& Message)
{
    const std::string Output = std::string("[UncleRenderer] ") + GetPrefix(Level) + Message + "\n";
    OutputDebugStringA(Output.c_str());
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
