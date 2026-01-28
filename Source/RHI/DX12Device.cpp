#include "DX12Device.h"
#include "RayTracing.h"
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
        case D3D_SHADER_MODEL_6_8: return "6.8";
        default: return "Unknown";
        }
    }

    std::string RaytracingTierToString(D3D12_RAYTRACING_TIER Tier)
    {
        switch (Tier)
        {
        case D3D12_RAYTRACING_TIER_NOT_SUPPORTED: return "Not Supported";
        case D3D12_RAYTRACING_TIER_1_0: return "1.0";
        case D3D12_RAYTRACING_TIER_1_1: return "1.1";
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
    if (!CreateFactory()) { LogError("Failed to create DXGI factory"); return false; }
    if (!PickAdapter())   { LogError("No suitable adapter found"); return false; }
    if (!CreateDevice())  { LogError("Failed to create D3D12 device"); return false; }
    if (!QueryRayTracingSupport()) { LogError("Failed to query DXR support"); return false; }
    if (!CreateBindlessDescriptorHeap()) { LogError("Failed to create bindless descriptor heap"); return false; }
    if (!CreateSamplerDescriptorHeap()) { LogError("Failed to create sampler descriptor heap"); return false; }
    if (!DetermineShaderModel()) { LogError("Failed to determine shader model"); return false; }
    if (!CreateCommandQueues()) { LogError("Failed to create command queues"); return false; }

    LogInfo("DX12 device initialization complete");
    return true;
}

bool FDX12Device::QueryRayTracingSupport()
{
    bSupportsRayTracing = false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 Options5 = {};
    if (FAILED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &Options5, sizeof(Options5))))
    {
        LogWarning("Failed to query D3D12 options5; DXR support disabled.");
        return true;
    }

    bSupportsRayTracing = Options5.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;

    std::ostringstream Oss;
    Oss << "DXR support: " << (bSupportsRayTracing ? "Enabled" : "Disabled")
        << " (Tier " << RaytracingTierToString(Options5.RaytracingTier) << ")";
    LogInfo(Oss.str());
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

bool FDX12Device::CreateBindlessDescriptorHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = 65536;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HeapDesc.NodeMask = 0;

    HR_CHECK(Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(BindlessDescriptorHeap.ReleaseAndGetAddressOf())));
    if (BindlessDescriptorHeap)
    {
        BindlessDescriptorHeap->SetName(L"BindlessDescriptorHeap");
    }

    BindlessDescriptorCount = HeapDesc.NumDescriptors;
    BindlessDescriptorStride = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    BindlessDescriptorNextIndex.store(0);

    return true;
}

bool FDX12Device::CreateSamplerDescriptorHeap()
{
    // Create sampler descriptor heap with enough space for multiple samplers
    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    HeapDesc.NumDescriptors = 16;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HeapDesc.NodeMask = 0;

    HR_CHECK(Device->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(SamplerDescriptorHeap.ReleaseAndGetAddressOf())));
    if (SamplerDescriptorHeap)
    {
        SamplerDescriptorHeap->SetName(L"SamplerDescriptorHeap");
    }

    const uint32_t SamplerDescriptorSize = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    CD3DX12_CPU_DESCRIPTOR_HANDLE SamplerHandle(SamplerDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // Sampler 0: Linear filter with Clamp address mode
    {
        D3D12_SAMPLER_DESC SamplerDesc = {};
        SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        SamplerDesc.MipLODBias = 0.0f;
        SamplerDesc.MaxAnisotropy = 1;
        SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        SamplerDesc.BorderColor[0] = 0.0f;
        SamplerDesc.BorderColor[1] = 0.0f;
        SamplerDesc.BorderColor[2] = 0.0f;
        SamplerDesc.BorderColor[3] = 0.0f;
        SamplerDesc.MinLOD = 0.0f;
        SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        
        Device->CreateSampler(&SamplerDesc, SamplerHandle);
        LinearClampSamplerIndex = 0;
        SamplerHandle.Offset(1, SamplerDescriptorSize);
    }

    // Sampler 1: Linear filter with Wrap address mode
    {
        D3D12_SAMPLER_DESC SamplerDesc = {};
        SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        SamplerDesc.MipLODBias = 0.0f;
        SamplerDesc.MaxAnisotropy = 1;
        SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        SamplerDesc.BorderColor[0] = 0.0f;
        SamplerDesc.BorderColor[1] = 0.0f;
        SamplerDesc.BorderColor[2] = 0.0f;
        SamplerDesc.BorderColor[3] = 0.0f;
        SamplerDesc.MinLOD = 0.0f;
        SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        
        Device->CreateSampler(&SamplerDesc, SamplerHandle);
        LinearWrapSamplerIndex = 1;
        SamplerHandle.Offset(1, SamplerDescriptorSize);
    }

    // Sampler 2: Anisotropic filter with Clamp address mode
    {
        D3D12_SAMPLER_DESC SamplerDesc = {};
        SamplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
        SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        SamplerDesc.MipLODBias = 0.0f;
        SamplerDesc.MaxAnisotropy = 4;
        SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        SamplerDesc.BorderColor[0] = 0.0f;
        SamplerDesc.BorderColor[1] = 0.0f;
        SamplerDesc.BorderColor[2] = 0.0f;
        SamplerDesc.BorderColor[3] = 0.0f;
        SamplerDesc.MinLOD = 0.0f;
        SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        
        Device->CreateSampler(&SamplerDesc, SamplerHandle);
        AnisotropicClampSamplerIndex = 2;
        SamplerHandle.Offset(1, SamplerDescriptorSize);
    }

    // Sampler 3: Anisotropic filter with Wrap address mode
    {
        D3D12_SAMPLER_DESC SamplerDesc = {};
        SamplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
        SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        SamplerDesc.MipLODBias = 0.0f;
        SamplerDesc.MaxAnisotropy = 4;
        SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        SamplerDesc.BorderColor[0] = 0.0f;
        SamplerDesc.BorderColor[1] = 0.0f;
        SamplerDesc.BorderColor[2] = 0.0f;
        SamplerDesc.BorderColor[3] = 0.0f;
        SamplerDesc.MinLOD = 0.0f;
        SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
        
        Device->CreateSampler(&SamplerDesc, SamplerHandle);
        AnisotropicWrapSamplerIndex = 3;
    }

    LogInfo("Sampler descriptor heap created with 4 samplers: LinearClamp(0), LinearWrap(1), AnisotropicClamp(2), AnisotropicWrap(3)");
    return true;
}

bool FDX12Device::CreateRayTracingDevice(FRayTracingDevice& OutDevice) const
{
    if (!bSupportsRayTracing)
    {
        LogWarning("Ray tracing device creation skipped: DXR is not supported.");
        return false;
    }

    return OutDevice.Initialize(Device.Get());
}

uint32_t FDX12Device::AllocateBindlessDescriptorIndex()
{
    const uint32_t Index = BindlessDescriptorNextIndex.fetch_add(1);
    if (Index >= BindlessDescriptorCount)
    {
        LogError("Bindless descriptor heap overflow.");
        return UINT32_MAX;
    }
    return Index;
}

uint32_t FDX12Device::CreateBindlessSrv(ID3D12Resource* Resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc)
{
    if (!BindlessDescriptorHeap || !Device)
    {
        return UINT32_MAX;
    }

    const uint32_t Index = AllocateBindlessDescriptorIndex();
    if (Index == UINT32_MAX)
    {
        return UINT32_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = BindlessDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    CpuHandle.ptr += static_cast<SIZE_T>(Index) * BindlessDescriptorStride;
    Device->CreateShaderResourceView(Resource, &Desc, CpuHandle);
    return Index;
}

uint32_t FDX12Device::CreateBindlessUav(ID3D12Resource* Resource, ID3D12Resource* Counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc)
{
    if (!BindlessDescriptorHeap || !Device)
    {
        return UINT32_MAX;
    }

    const uint32_t Index = AllocateBindlessDescriptorIndex();
    if (Index == UINT32_MAX)
    {
        return UINT32_MAX;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = BindlessDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    CpuHandle.ptr += static_cast<SIZE_T>(Index) * BindlessDescriptorStride;
    Device->CreateUnorderedAccessView(Resource, Counter, &Desc, CpuHandle);
    return Index;
}

bool FDX12Device::DetermineShaderModel()
{
    const D3D_SHADER_MODEL MinimumShaderModel = D3D_SHADER_MODEL_6_6;
    const D3D_SHADER_MODEL PreferredShaderModel = D3D_SHADER_MODEL_6_8;
    static const D3D_SHADER_MODEL Candidates[] =
    {
        PreferredShaderModel,
        D3D_SHADER_MODEL_6_7,
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
    Oss << "Preferred shader model: " << ShaderModelToString(PreferredShaderModel)
        << ", device supports up to: " << ShaderModelToString(ShaderModel);
    LogInfo(Oss.str());

    if (ShaderModel < MinimumShaderModel)
    {
        LogError("Shader Model 6.6 is required for bindless descriptor heap usage. Device only supports lower versions. Exiting.");
        return false;
    }

    bIndirectDrawSupported = ShaderModel >= PreferredShaderModel;
    if (!bIndirectDrawSupported)
    {
        LogWarning("Shader Model 6.8 is required for indirect draw; indirect draw will be disabled.");
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
