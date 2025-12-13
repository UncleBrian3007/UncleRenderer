#include "DX12Device.h"
#include "../Core/Logger.h"
#include <string>
#include <sstream>
#include <cwchar>
#include <filesystem>
#include <array>

namespace
{
    std::string WStringToUtf8(const std::wstring& Str)
    {
        return std::filesystem::path(Str).u8string();
    }

    std::filesystem::path GetExecutableDirectory()
    {
        std::array<wchar_t, MAX_PATH> Buffer{};
        DWORD Length = GetModuleFileNameW(nullptr, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        return std::filesystem::path(Buffer.data(), Buffer.data() + Length).remove_filename();
    }

    std::filesystem::path ResolveSDKDirectory()
    {
        std::filesystem::path SdkPath(D3D12SDKPath);
        if (SdkPath.is_relative())
        {
            SdkPath = GetExecutableDirectory() / SdkPath;
        }
        return SdkPath.lexically_normal();
    }

    void LogLoadedModulePath(const wchar_t* ModuleName, const char* Label)
    {
        HMODULE Module = GetModuleHandleW(ModuleName);
        if (!Module) return;

        std::array<wchar_t, MAX_PATH> Buffer{};
        DWORD Length = GetModuleFileNameW(Module, Buffer.data(), static_cast<DWORD>(Buffer.size()));
        if (Length == 0) return;

        std::wstring PathW(Buffer.data(), Buffer.data() + Length);
        LogInfo(std::string(Label) + std::string(": ") + WStringToUtf8(PathW));
    }

    std::string ShaderModelToString(D3D_SHADER_MODEL ShaderModel)
    {
        switch (ShaderModel)
        {
        case D3D_SHADER_MODEL_5_1: return "5.1";
        case D3D_SHADER_MODEL_6_0: return "6.0";
        case D3D_SHADER_MODEL_6_1: return "6.1";
        case D3D_SHADER_MODEL_6_2: return "6.2";
        case D3D_SHADER_MODEL_6_3: return "6.3";
        case D3D_SHADER_MODEL_6_4: return "6.4";
        case D3D_SHADER_MODEL_6_5: return "6.5";
        case D3D_SHADER_MODEL_6_6: return "6.6";
        case D3D_SHADER_MODEL_6_7: return "6.7";
        default: return "Unknown";
        }
    }
}

FDX12Device::FDX12Device()
{
}

FDX12Device::~FDX12Device()
{
    if (GraphicsQueue)
    {
        GraphicsQueue->Flush();
    }
}

bool FDX12Device::Initialize()
{
    LogInfo("DX12 device initialization started");
    LoadAgilitySDK();
    if (!CreateFactory()) { LogError("Failed to create DXGI factory"); return false; }
    if (!PickAdapter())   { LogError("No suitable adapter found"); return false; }
    if (!CreateDevice())  { LogError("Failed to create D3D12 device"); return false; }
    if (!DetermineShaderModel()) { LogError("Failed to determine shader model"); return false; }
    if (!CreateCommandQueues()) { LogError("Failed to create command queues"); return false; }

    LogInfo("DX12 device initialization complete");
    return true;
}

bool FDX12Device::LoadAgilitySDK()
{
    const std::filesystem::path ResolvedSdkPath = ResolveSDKDirectory();
    const std::wstring ResolvedSdkPathW = ResolvedSdkPath.wstring();
    const std::string ResolvedSdkPathUtf8 = ResolvedSdkPath.u8string();

    if (D3D12SDKVersion > 0)
    {
        std::ostringstream Oss;
        Oss << "Agility SDK Version: " << D3D12SDKVersion << ", Path: " << ResolvedSdkPathUtf8;
        LogInfo(Oss.str());

        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS))
        {
            LogWarning("Failed to call SetDefaultDllDirectories");
        }
        else if (!AddDllDirectory(ResolvedSdkPathW.c_str()))
        {
            LogWarning("Failed to add Agility SDK DLL path. d3d12core.dll placement and permissions should be checked");
        }
    }
    else if (!ResolvedSdkPath.empty())
    {
        LogWarning("SDK version is set to 0. Using the default D3D12 runtime.");
    }

    return true;
}

bool FDX12Device::CreateFactory()
{
    UINT Flags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> DebugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&DebugController))))
        {
            DebugController->EnableDebugLayer();
            Flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    HR_CHECK(CreateDXGIFactory2(Flags, IID_PPV_ARGS(Factory.GetAddressOf())));
    CheckTearingSupport();

    std::ostringstream Oss;
    const std::filesystem::path ResolvedSdkPath = ResolveSDKDirectory();
    Oss << "D3D12SDKVersion: " << D3D12SDKVersion << ", Path: " << ResolvedSdkPath.u8string();
    LogInfo(Oss.str());
    return true;
}

bool FDX12Device::PickAdapter()
{
    ComPtr<IDXGIAdapter1> TempAdapter;
    SIZE_T MaxVRAM = 0;

    for (UINT i = 0; Factory->EnumAdapters1(i, TempAdapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 Desc;
        TempAdapter->GetDesc1(&Desc);

        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (Desc.DedicatedVideoMemory > MaxVRAM)
        {
            MaxVRAM = Desc.DedicatedVideoMemory;
            TempAdapter.As(&Adapter);
        }
    }

    if (!Adapter)
    {
        LogError("Could not find a hardware adapter");
        return false;
    }

    return Adapter != nullptr;
}

bool FDX12Device::CreateDevice()
{
    HR_CHECK(D3D12CreateDevice(
        Adapter.Get(),
        D3D_FEATURE_LEVEL_12_1,
        IID_PPV_ARGS(Device.GetAddressOf())
    ));

    LogLoadedModulePath(L"d3d12.dll", "D3D12.dll load path");
    LogLoadedModulePath(L"d3d12core.dll", "D3D12Core.dll load path");
    return true;
}

bool FDX12Device::DetermineShaderModel()
{
    const D3D_SHADER_MODEL DesiredShaderModel = D3D_SHADER_MODEL_6_7;
    static const D3D_SHADER_MODEL Candidates[] =
    {
        DesiredShaderModel,
        D3D_SHADER_MODEL_6_6,
        D3D_SHADER_MODEL_6_5,
        D3D_SHADER_MODEL_6_4,
        D3D_SHADER_MODEL_6_3,
        D3D_SHADER_MODEL_6_2,
        D3D_SHADER_MODEL_6_1,
        D3D_SHADER_MODEL_6_0,
        D3D_SHADER_MODEL_5_1,
    };

    D3D12_FEATURE_DATA_SHADER_MODEL FeatureData = {};
    ShaderModel = D3D_SHADER_MODEL_5_1;

    for (D3D_SHADER_MODEL Candidate : Candidates)
    {
        FeatureData.HighestShaderModel = Candidate;
        if (SUCCEEDED(Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &FeatureData, sizeof(FeatureData))))
        {
            ShaderModel = FeatureData.HighestShaderModel;
            break;
        }
    }

    std::ostringstream Oss;
    Oss << "Requested shader model: " << ShaderModelToString(DesiredShaderModel)
        << ", device supports up to: " << ShaderModelToString(ShaderModel);
    LogInfo(Oss.str());

    if (ShaderModel < DesiredShaderModel)
    {
        LogWarning("Falling back to lower shader model; consider updating the Agility SDK/runtime for SM 6.7 support.");
    }

    return true;
}

bool FDX12Device::CreateCommandQueues()
{
    GraphicsQueue = std::make_unique<FDX12CommandQueue>();
    return GraphicsQueue->Initialize(Device.Get(), EDX12QueueType::Direct);
}

bool FDX12Device::CheckTearingSupport()
{
    bAllowTearing = false;

    ComPtr<IDXGIFactory5> Factory5;
    if (SUCCEEDED(Factory.As(&Factory5)))
    {
        BOOL AllowTearing = FALSE;
        if (SUCCEEDED(Factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &AllowTearing,
            sizeof(AllowTearing))))
        {
            bAllowTearing = AllowTearing == TRUE;
        }
    }

    LogInfo(std::string("DXGI_PRESENT_ALLOW_TEARING : ") + (bAllowTearing ? "Enabled" : "Disabled"));
    return true;
}

bool FDX12Device::QueryLocalVideoMemory(DXGI_QUERY_VIDEO_MEMORY_INFO& OutInfo) const
{
    ComPtr<IDXGIAdapter3> Adapter3;
    if (FAILED(Adapter.As(&Adapter3)))
    {
        return false;
    }

    if (FAILED(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &OutInfo)))
    {
        return false;
    }

    return true;
}
