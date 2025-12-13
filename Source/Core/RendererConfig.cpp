#include "RendererConfig.h"

#include "Logger.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

namespace
{
    std::wstring ToWide(const std::string& Input)
    {
        return std::wstring(Input.begin(), Input.end());
    }

    std::string ToLowerCopy(const std::string& Input)
    {
        std::string Result(Input);
        std::transform(Result.begin(), Result.end(), Result.begin(), [](unsigned char Ch)
        {
            return static_cast<char>(std::tolower(Ch));
        });
        return Result;
    }
}

FRendererConfig FRendererConfigLoader::LoadOrDefault(const std::filesystem::path& ConfigPath)
{
    FRendererConfig Config = {};

    if (!std::filesystem::exists(ConfigPath))
    {
        LogWarning("Renderer config not found; using default values.");
        return Config;
    }

    std::ifstream File(ConfigPath);
    if (!File.is_open())
    {
        LogWarning("Failed to open renderer config; using default values.");
        return Config;
    }

    std::string Line;
    while (std::getline(File, Line))
    {
        const std::string Trimmed = TrimCopy(Line);
        if (Trimmed.empty() || Trimmed[0] == ';' || Trimmed[0] == '#')
        {
            continue;
        }

        const size_t DelimiterPos = Trimmed.find('=');
        if (DelimiterPos == std::string::npos)
        {
            continue;
        }

        std::string Key = TrimCopy(Trimmed.substr(0, DelimiterPos));
        std::string Value = TrimCopy(Trimmed.substr(DelimiterPos + 1));

        if (Key.empty())
        {
            continue;
        }

        ApplyKeyValue(Key, Value, Config);
    }

    return Config;
}

void FRendererConfigLoader::ApplyKeyValue(const std::string& Key, const std::string& Value, FRendererConfig& OutConfig)
{
    const std::string LowerKey = ToLowerCopy(Key);
    const std::string LowerValue = ToLowerCopy(Value);

    if (LowerKey == "type" || LowerKey == "renderer")
    {
        if (LowerValue == "forward")
        {
            OutConfig.RendererType = ERendererType::Forward;
        }
        else
        {
            OutConfig.RendererType = ERendererType::Deferred;
        }
        return;
    }

    if (LowerKey == "scene" || LowerKey == "scenefile")
    {
        OutConfig.SceneFile = ToWide(Value);
        return;
    }

    if (LowerKey == "usedepthprepass" || LowerKey == "depthprepass")
    {
        OutConfig.bUseDepthPrepass = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "frameoverlap" || LowerKey == "useframeoverlap")
    {
        OutConfig.bEnableFrameOverlap = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
}

std::string FRendererConfigLoader::TrimCopy(const std::string& Input)
{
    constexpr std::string_view Whitespace = " \t\r\n";

    const size_t Start = Input.find_first_not_of(Whitespace);
    if (Start == std::string::npos)
    {
        return std::string();
    }

    const size_t End = Input.find_last_not_of(Whitespace);
    return Input.substr(Start, End - Start + 1);
}
