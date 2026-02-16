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

    if (LowerKey == "enableraytracedshadows" || LowerKey == "raytracedshadows" || LowerKey == "raytraceshadows")
    {
        OutConfig.bEnableRayTracedShadows = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "enablepathtracing" || LowerKey == "pathtracing")
    {
        OutConfig.bEnablePathTracing = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "enablepathtracingaccumulation" || LowerKey == "pathtracingaccumulation")
    {
        OutConfig.bEnablePathTracingAccumulation = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "enablepathtracingvndf" || LowerKey == "pathtracingvndf" || LowerKey == "pathtracingusevndf")
    {
        OutConfig.bEnablePathTracingVndf = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
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

    if (LowerKey == "enablecas" || LowerKey == "cas")
    {
        OutConfig.bEnableCas = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "cassharpness")
    {
        try
        {
            OutConfig.CasSharpness = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid CAS sharpness value in renderer config: " + Value);
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

    if (LowerKey == "enabletaa" || LowerKey == "taa" || LowerKey == "temporalaa")
    {
        OutConfig.bEnableTAA = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "taahistoryweight" || LowerKey == "taaweight")
    {
        try
        {
            OutConfig.TaaHistoryWeight = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid TAA history weight value in renderer config: " + Value);
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

    if (LowerKey == "hzb" || LowerKey == "enablehzb")
    {
        OutConfig.bEnableHZB = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "gtao" || LowerKey == "enablegtao")
    {
        OutConfig.bEnableGtao = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "ssr" || LowerKey == "enablessr" || LowerKey == "enablescreenspacereflections")
    {
        const bool bEnable = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
        OutConfig.bEnableSsrSw = bEnable;
        OutConfig.bEnableSsrHw = bEnable;
    }
    if (LowerKey == "ssrsw" || LowerKey == "enablessrsw" || LowerKey == "ssr_sw")
    {
        OutConfig.bEnableSsrSw = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "ssrhw" || LowerKey == "enablessrhw" || LowerKey == "ssr_hw")
    {
        OutConfig.bEnableSsrHw = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "ssrhzb" || LowerKey == "enablessrhzb" || LowerKey == "ssr_hzb")
    {
        OutConfig.bEnableSsrHzb = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "ssrrefine" || LowerKey == "enablessrrefine" || LowerKey == "ssr_refine")
    {
        OutConfig.bEnableSsrRefine = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "ssrdenoise" || LowerKey == "enablessrdenoise" || LowerKey == "ssr_denoise")
    {
        OutConfig.bEnableSsrDenoise = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "restirgi" || LowerKey == "enablerestirgi")
    {
        OutConfig.bEnableRestirGI = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "restirgisamplesperpixel")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            const int32_t ClampedValue = std::clamp(ParsedValue, 1, 32);
            OutConfig.RestirGISamplesPerPixel = static_cast<uint32_t>(ClampedValue);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI samples per pixel in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgiintensity")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGIIntensity = (std::max)(0.0f, ParsedValue);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI intensity in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgiraylength")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGIRayLength = (std::max)(0.1f, ParsedValue);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI ray length in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgiclamp")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGIClamp = (std::max)(0.1f, ParsedValue);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI clamp in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgitemporalreuse" || LowerKey == "enablerestirgitemporalreuse")
    {
        OutConfig.bEnableRestirGITemporalReuse = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "restirgispatialreuse" || LowerKey == "enablerestirgispatialreuse")
    {
        OutConfig.bEnableRestirGISpatialReuse = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "restirgitemporaladditionalscale")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGITemporalAdditionalScale = std::clamp(ParsedValue, 0.0f, 1.0f);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI temporal additional scale in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgispatialadditionalscale")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGISpatialAdditionalScale = std::clamp(ParsedValue, 0.0f, 1.0f);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI spatial additional scale in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgiresolvemindenominator")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGIResolveMinDenominator = (std::max)(ParsedValue, 1e-6f);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI resolve min denominator in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgiresolvemaxnormalization")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGIResolveMaxNormalization = (std::max)(ParsedValue, 1.0f);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI resolve max normalization in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgiresolvelowsampleboostguard")
    {
        try
        {
            const float ParsedValue = std::stof(Value);
            OutConfig.RestirGIResolveLowSampleBoostGuard = std::clamp(ParsedValue, 0.0f, 1.0f);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI resolve low-sample boost guard in renderer config: " + Value);
        }
    }
    if (LowerKey == "restirgiresolveuseconfidence")
    {
        OutConfig.bRestirGIResolveUseConfidence = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }
    if (LowerKey == "restirgimaxhistoryframes")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            const int32_t ClampedValue = std::clamp(ParsedValue, 1, 16);
            OutConfig.RestirGIMaxHistoryFrames = static_cast<uint32_t>(ClampedValue);
        }
        catch (...)
        {
            LogWarning("Invalid ReSTIR GI max history frames in renderer config: " + Value);
        }
    }
    if (LowerKey == "ssrmode")
    {
        if (LowerValue == "cs")
        {
            OutConfig.SsrMode = ESSRMode::CS;
        }
        else
        {
            OutConfig.SsrMode = ESSRMode::PS;
        }
    }
    if (LowerKey == "ssrsamplesperquad")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            const int32_t ClampedValue = std::clamp(ParsedValue, 1, 4);
            OutConfig.SsrSamplesPerQuad = static_cast<uint32_t>(ClampedValue);
        }
        catch (...)
        {
            LogWarning("Invalid SSR samples per quad value in renderer config: " + Value);
        }
    }
    if (LowerKey == "gtaojitter" || LowerKey == "enablegtaojitter")
    {
        OutConfig.bEnableGtaoJitter = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "gtaoradius")
    {
        try
        {
            OutConfig.GtaoRadius = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid GTAO radius value in renderer config: " + Value);
        }
    }

    if (LowerKey == "gtaointensity")
    {
        try
        {
            OutConfig.GtaoIntensity = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid GTAO intensity value in renderer config: " + Value);
        }
    }

    if (LowerKey == "gtaopower")
    {
        try
        {
            OutConfig.GtaoPower = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid GTAO power value in renderer config: " + Value);
        }
    }

    if (LowerKey == "gtaothickness")
    {
        try
        {
            OutConfig.GtaoThickness = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid GTAO thickness value in renderer config: " + Value);
        }
    }

    if (LowerKey == "ssrmaxsteps")
    {
        try
        {
            OutConfig.SsrMaxSteps = static_cast<uint32_t>(std::stoul(Value));
        }
        catch (...)
        {
            LogWarning("Invalid SSR max steps value in renderer config: " + Value);
        }
    }

    if (LowerKey == "ssrmaxdistance")
    {
        try
        {
            OutConfig.SsrMaxDistance = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid SSR max distance value in renderer config: " + Value);
        }
    }

    if (LowerKey == "ssrthickness")
    {
        try
        {
            OutConfig.SsrThickness = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid SSR thickness value in renderer config: " + Value);
        }
    }

    if (LowerKey == "ssrstride")
    {
        try
        {
            OutConfig.SsrStride = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid SSR stride value in renderer config: " + Value);
        }
    }

    if (LowerKey == "ssrroughnesscutoff")
    {
        try
        {
            OutConfig.SsrRoughnessCutoff = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid SSR roughness cutoff value in renderer config: " + Value);
        }
    }

    if (LowerKey == "ssrintensity")
    {
        try
        {
            OutConfig.SsrIntensity = std::stof(Value);
        }
        catch (...)
        {
            LogWarning("Invalid SSR intensity value in renderer config: " + Value);
        }
    }

    if (LowerKey == "gtaodirectioncount")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            const int32_t ClampedValue = std::clamp(ParsedValue, 1, 16);
            OutConfig.GtaoDirectionCount = static_cast<uint32_t>(ClampedValue);
        }
        catch (...)
        {
            LogWarning("Invalid GTAO direction count value in renderer config: " + Value);
        }
    }

    if (LowerKey == "gtaostepcount")
    {
        try
        {
            const int32_t ParsedValue = std::stoi(Value);
            const int32_t ClampedValue = std::clamp(ParsedValue, 1, 8);
            OutConfig.GtaoStepCount = static_cast<uint32_t>(ClampedValue);
        }
        catch (...)
        {
            LogWarning("Invalid GTAO step count value in renderer config: " + Value);
        }
    }

    if (LowerKey == "gpudebugprint" || LowerKey == "enablegpudebugprint")
    {
        OutConfig.bEnableGpuDebugPrint = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "indirectdraw" || LowerKey == "enableindirectdraw")
    {
        OutConfig.bEnableIndirectDraw = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "skinningindirectdraw" || LowerKey == "enableskinningindirectdraw")
    {
        OutConfig.bEnableSkinningIndirectDraw = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
    }

    if (LowerKey == "pbrresearch" || LowerKey == "enablepbrresearch")
    {
        OutConfig.bEnablePbrResearch = (LowerValue == "1" || LowerValue == "true" || LowerValue == "yes");
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
