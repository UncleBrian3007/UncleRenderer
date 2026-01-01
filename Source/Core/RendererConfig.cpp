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

    if (LowerKey == "framesinflight" || LowerKey == "inflightframes" || LowerKey == "swapchainbuffercount")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            const int32_t ClampedValue = std::clamp(ParsedValue, 1, 8);
            OutConfig.FramesInFlight = static_cast<uint32_t>(ClampedValue);
        }
        catch (...)
        {
            LogWarning("Invalid frames in flight value in renderer config: " + Value);
        }
    }

    if (LowerKey == "enableshadows" || LowerKey == "shadows")
    {
        OutConfig.bEnableShadows = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "shadowbias")
    {
        try
        {
            OutConfig.ShadowBias = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid shadow bias value in renderer config: " + Value);
        }
    }

    if (LowerKey == "enabletonemap" || LowerKey == "tonemap")
    {
        OutConfig.bEnableTonemap = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "tonemapexposure")
    {
        try
        {
            OutConfig.TonemapExposure = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid tonemap exposure value in renderer config: " + Value);
        }
    }

    if (LowerKey == "tonemapwhitepoint")
    {
        try
        {
            OutConfig.TonemapWhitePoint = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid tonemap white point value in renderer config: " + Value);
        }
    }

    if (LowerKey == "tonemapgamma")
    {
        try
        {
            OutConfig.TonemapGamma = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid tonemap gamma value in renderer config: " + Value);
        }
    }

    if (LowerKey == "enableautoexposure" || LowerKey == "autoexposure")
    {
        OutConfig.bEnableAutoExposure = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "autoexposurekey")
    {
        try
        {
            OutConfig.AutoExposureKey = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid auto exposure key value in renderer config: " + Value);
        }
    }

    if (LowerKey == "autoexposuremin")
    {
        try
        {
            OutConfig.AutoExposureMin = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid auto exposure min value in renderer config: " + Value);
        }
    }

    if (LowerKey == "autoexposuremax")
    {
        try
        {
            OutConfig.AutoExposureMax = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid auto exposure max value in renderer config: " + Value);
        }
    }

    if (LowerKey == "autoexposurespeedup")
    {
        try
        {
            OutConfig.AutoExposureSpeedUp = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid auto exposure speed up value in renderer config: " + Value);
        }
    }

    if (LowerKey == "autoexposurespeeddown")
    {
        try
        {
            OutConfig.AutoExposureSpeedDown = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid auto exposure speed down value in renderer config: " + Value);
        }
    }

    if (LowerKey == "usetasksystem" || LowerKey == "enabletasksystem" || LowerKey == "tasksystem")
    {
        OutConfig.bEnableTaskSystem = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "logresourcebarriers" || LowerKey == "logbarriers" || LowerKey == "barrierlogging")
    {
        OutConfig.bLogResourceBarriers = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "graphdump" || LowerKey == "enablegraphdump" || LowerKey == "dumpgraph")
    {
        OutConfig.bEnableGraphDump = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "gputiming" || LowerKey == "enablegputiming" || LowerKey == "recordgputiming")
    {
        OutConfig.bEnableGpuTiming = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "indirectdraw" || LowerKey == "enableindirectdraw")
    {
        OutConfig.bEnableIndirectDraw = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "width" || LowerKey == "windowwidth")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            OutConfig.WindowWidth = static_cast<uint32_t>((std::max)(1, ParsedValue));
        }
        catch (...)
        {
            LogWarning("Invalid window width value in renderer config: " + Value);
        }
    }

    if (LowerKey == "height" || LowerKey == "windowheight")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            OutConfig.WindowHeight = static_cast<uint32_t>((std::max)(1, ParsedValue));
        }
        catch (...)
        {
            LogWarning("Invalid window height value in renderer config: " + Value);
        }
    }

    if (LowerKey == "resolution")
    {
        const size_t Separator = Value.find_first_of("xX");
        if (Separator != std::string::npos)
        {
            try
            {
                const int32_t ParsedWidth = std::stoi(Value.substr(0, Separator));
                const int32_t ParsedHeight = std::stoi(Value.substr(Separator + 1));
                OutConfig.WindowWidth = static_cast<uint32_t>((std::max)(1, ParsedWidth));
                OutConfig.WindowHeight = static_cast<uint32_t>((std::max)(1, ParsedHeight));
            }
            catch (...)
            {
                LogWarning("Invalid resolution value in renderer config: " + Value);
            }
        }
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
