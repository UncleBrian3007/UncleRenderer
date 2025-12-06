#pragma once

#include <string>

enum class ELogLevel
{
    Verbose,
    Info,
    Warning,
    Error
};

void LogMessage(ELogLevel Level, const std::string& Message);
void SetLogLevel(ELogLevel Level);
ELogLevel GetLogLevel();
void LogVerbose(const std::string& Message);
void LogInfo(const std::string& Message);
void LogWarning(const std::string& Message);
void LogError(const std::string& Message);
