#pragma once

#include <string>

enum class ELogLevel
{
    Info,
    Warning,
    Error
};

void LogMessage(ELogLevel Level, const std::string& Message);
void LogInfo(const std::string& Message);
void LogWarning(const std::string& Message);
void LogError(const std::string& Message);
