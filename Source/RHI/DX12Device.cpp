#include "DX12Device.h"
#include "RayTracing.h"
#include "DX12DeviceRemoved.h"
#include "../Core/Logger.h"
#include <string>
#include <sstream>
#include <cwchar>
#include <filesystem>
#include <array>
#include <dxgidebug.h>

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

void FDX12Device::ReportLiveObjects()
{
    ComPtr<IDXGIDebug1> DxgiDebug;
    const HRESULT Hr = DXGIGetDebugInterface1(0, IID_PPV_ARGS(DxgiDebug.GetAddressOf()));
    if (FAILED(Hr))
    {
        std::ostringstream Stream;
        Stream << "DXGI live object report unavailable, hr=0x" << std::hex << static_cast<unsigned long>(Hr);
        LogInfo(Stream.str());
        return;
    }

    LogInfo("DXGI live object report begin");
    DxgiDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
    LogInfo("DXGI live object report complete");
}

FDX12Device::FBindlessDescriptorStats FDX12Device::GetBindlessDescriptorStats() const
{
    FBindlessDescriptorStats Stats;
#if WITH_BINDLESS_DESCRIPTOR_STATS
    Stats.DescriptorCount = BindlessDescriptorCount;
    Stats.NextIndex = BindlessDescriptorNextIndex.load();
    Stats.PermanentAllocationCount = PermanentBindlessDescriptorAllocationCount.load();
    Stats.TransientHeapAllocationCount = TransientBindlessDescriptorHeapAllocationCount.load();
    Stats.TransientReuseCount = TransientBindlessDescriptorReuseCount.load();
    Stats.TransientRetireCount = TransientBindlessDescriptorRetireCount.load();
    Stats.TransientReclaimCount = TransientBindlessDescriptorReclaimCount.load();

    const FDX12CommandQueue* Queue = GraphicsQueue.get();
    Stats.CompletedFenceValue = Queue ? Queue->GetCompletedFenceValue() : 0u;
    Stats.LastSignaledFenceValue = Queue ? Queue->GetLastSignaledFenceValue() : 0u;

    {
        std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
        Stats.FreeTransientCount = static_cast<uint32_t>(FreeTransientBindlessDescriptorIndices.size());
        Stats.RetiredTransientCount = static_cast<uint32_t>(RetiredTransientBindlessDescriptorIndices.size());
        Stats.MinFreeTransientThisFrame = MinFreeTransientThisFrame;
        Stats.PeakTransientLiveThisFrame = PeakTransientLiveThisFrame;
        Stats.TransientHeapAllocsThisFrame = TransientHeapAllocsThisFrame;
        if (bTrackLiveTransientBindlessOwners)
        {
            Stats.LiveTransientDescriptorCount = static_cast<uint32_t>(LiveTransientBindlessDescriptorOwners.size());

            if (!LiveTransientBindlessDescriptorOwners.empty())
            {
                std::vector<std::pair<uint32_t, std::string>> SortedOwners;
                SortedOwners.reserve(LiveTransientBindlessDescriptorOwners.size());
                for (const auto& Entry : LiveTransientBindlessDescriptorOwners)
                {
                    SortedOwners.push_back(Entry);
                }

                std::sort(SortedOwners.begin(), SortedOwners.end(),
                    [](const auto& A, const auto& B)
                    {
                        return A.first < B.first;
                    });

                constexpr size_t MaxOwnerSamples = 12;
                const size_t SampleCount = (std::min)(SortedOwners.size(), MaxOwnerSamples);
                Stats.LiveTransientOwnerSamples.reserve(SampleCount);
                for (size_t SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
                {
                    std::ostringstream Stream;
                    Stream << "#" << SortedOwners[SampleIndex].first << " " << SortedOwners[SampleIndex].second;
                    Stats.LiveTransientOwnerSamples.push_back(Stream.str());
                }
            }
        }

        if (!RetiredTransientBindlessDescriptorIndices.empty())
        {
            Stats.OldestRetiredFenceValue = RetiredTransientBindlessDescriptorIndices.front().FenceValue;
            Stats.NewestRetiredFenceValue = RetiredTransientBindlessDescriptorIndices.back().FenceValue;

            for (const FRetiredBindlessDescriptor& Retired : RetiredTransientBindlessDescriptorIndices)
            {
                if (Retired.FenceValue <= Stats.CompletedFenceValue)
                {
                    Stats.ReclaimableTransientCount++;
                }
                else
                {
                    break;
                }
            }
        }
    }
#endif

    return Stats;
}

bool FDX12Device::Initialize()
{
    LogInfo("DX12 device initialization started");
    if (!CreateFactory()) { LogError("Failed to create DXGI factory"); return false; }
    if (!PickAdapter())   { LogError("No suitable adapter found"); return false; }
    if (!CreateDevice())  { LogError("Failed to create D3D12 device"); return false; }
    RtvDescriptorStride = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!QueryRayTracingSupport()) { LogError("Failed to query DXR support"); return false; }
    if (!QueryAtomicInt64Support()) { LogError("Failed to query atomic int64 support"); return false; }
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

bool FDX12Device::QueryAtomicInt64Support()
{
    bSupportsAtomicInt64OnTypedResource = false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS9 Options9 = {};
    if (FAILED(Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &Options9, sizeof(Options9))))
    {
        LogWarning("D3D12 OPTIONS9 query failed; typed resource int64 atomics disabled.");
        return true;
    }

    bSupportsAtomicInt64OnTypedResource = Options9.AtomicInt64OnTypedResourceSupported == TRUE;
    LogInfo(std::string("Atomic int64 typed resource support: ") + (bSupportsAtomicInt64OnTypedResource ? "Enabled" : "Disabled"));
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

            ComPtr<ID3D12Debug1> DebugController1;
            if (SUCCEEDED(DebugController.As(&DebugController1)))
            {
                DebugController1->SetEnableGPUBasedValidation(TRUE);
                LogInfo("D3D12 GPU-based validation enabled");
            }

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

    for (UINT i = 0; Factory->EnumAdapters1(i, TempAdapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
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
    ConfigureDredSettingsBeforeDeviceCreation();

    HR_CHECK(D3D12CreateDevice(
        Adapter.Get(),
        D3D_FEATURE_LEVEL_12_1,
        IID_PPV_ARGS(Device.GetAddressOf())
    ));

#if defined(_DEBUG)
    {
        ComPtr<ID3D12InfoQueue> InfoQueue;
        if (SUCCEEDED(Device.As(&InfoQueue)))
        {
            InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            InfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            InfoQueue->SetMessageCountLimit(8192);
            LogInfo("D3D12 InfoQueue break-on-error disabled to allow DRED logging");
        }
    }
#endif

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
    BindlessDescriptorHeap->SetName(L"BindlessDescriptorHeap");

    D3D12_DESCRIPTOR_HEAP_DESC CpuHeapDesc = HeapDesc;
    CpuHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->CreateDescriptorHeap(&CpuHeapDesc, IID_PPV_ARGS(BindlessCpuDescriptorHeap.ReleaseAndGetAddressOf())));
    BindlessCpuDescriptorHeap->SetName(L"BindlessCpuDescriptorHeap");

    BindlessDescriptorCount = HeapDesc.NumDescriptors;
    BindlessDescriptorStride = Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    BindlessDescriptorNextIndex.store(0);
#if WITH_BINDLESS_DESCRIPTOR_STATS
    PermanentBindlessDescriptorAllocationCount.store(0);
    TransientBindlessDescriptorHeapAllocationCount.store(0);
    TransientBindlessDescriptorReuseCount.store(0);
    TransientBindlessDescriptorRetireCount.store(0);
    TransientBindlessDescriptorReclaimCount.store(0);
    BindlessPressureLogLevel.store(0);
    {
        std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
        FreeTransientBindlessDescriptorIndices.clear();
        RetiredTransientBindlessDescriptorIndices.clear();
        MinFreeTransientThisFrame = 0u;
        PeakTransientLiveThisFrame = 0u;
        TransientHeapAllocsThisFrame = 0u;
    }
#endif

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
    SamplerDescriptorHeap->SetName(L"SamplerDescriptorHeap");

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
        SamplerHandle.Offset(1, SamplerDescriptorSize);
    }

    // Sampler 4: Point filter with Clamp address mode
    {
        D3D12_SAMPLER_DESC SamplerDesc = {};
        SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
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
        PointClampSamplerIndex = 4;
    }

    LogInfo("Sampler descriptor heap created with 5 samplers: LinearClamp(0), LinearWrap(1), AnisotropicClamp(2), AnisotropicWrap(3), PointClamp(4)");
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
#if WITH_BINDLESS_DESCRIPTOR_STATS
        LogBindlessDescriptorStats("Bindless descriptor heap overflow");
#endif
        return UINT32_MAX;
    }

#if WITH_BINDLESS_DESCRIPTOR_STATS
    MaybeLogBindlessDescriptorPressure("bindless-descriptor-pressure", Index + 1u);
#endif
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

    WriteBindlessSrv(Index, Resource, Desc);
#if WITH_BINDLESS_DESCRIPTOR_STATS
    PermanentBindlessDescriptorAllocationCount.fetch_add(1);
#endif
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

    WriteBindlessUav(Index, Resource, Counter, Desc);
#if WITH_BINDLESS_DESCRIPTOR_STATS
    PermanentBindlessDescriptorAllocationCount.fetch_add(1);
#endif
    return Index;
}

uint32_t FDX12Device::AllocateTransientBindlessDescriptorIndex()
{
    if (!BindlessDescriptorHeap || !Device)
    {
        return UINT32_MAX;
    }

    const FDX12CommandQueue* Queue = GetGraphicsQueue();
    const uint64 CompletedFenceValue = Queue ? Queue->GetCompletedFenceValue() : 0;

    {
        std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
        ReclaimTransientBindlessDescriptorIndicesLocked(CompletedFenceValue);

        if (!FreeTransientBindlessDescriptorIndices.empty())
        {
            const uint32_t Index = FreeTransientBindlessDescriptorIndices.back();
            FreeTransientBindlessDescriptorIndices.pop_back();
#if WITH_BINDLESS_DESCRIPTOR_STATS
            TransientBindlessDescriptorReuseCount.fetch_add(1);
            UpdateTransientBindlessFrameStatsLocked();
#endif
            return Index;
        }
    }

    const uint32_t Index = AllocateBindlessDescriptorIndex();
    if (Index != UINT32_MAX)
    {
#if WITH_BINDLESS_DESCRIPTOR_STATS
        TransientBindlessDescriptorHeapAllocationCount.fetch_add(1);
        std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
        TransientHeapAllocsThisFrame++;
        UpdateTransientBindlessFrameStatsLocked();
#endif
    }
    else
    {
#if WITH_BINDLESS_DESCRIPTOR_STATS
        LogBindlessDescriptorStats("AllocateTransientBindlessDescriptorIndex failed");
#endif
    }

    return Index;
}

void FDX12Device::RetireTransientBindlessDescriptorIndex(uint32_t Index, uint64_t FenceValue)
{
    if (Index == UINT32_MAX)
    {
        return;
    }

    std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
#if WITH_BINDLESS_DESCRIPTOR_STATS
    LiveTransientBindlessDescriptorOwners.erase(Index);
#endif
    if (FenceValue == 0)
    {
        FreeTransientBindlessDescriptorIndices.push_back(Index);
#if WITH_BINDLESS_DESCRIPTOR_STATS
        TransientBindlessDescriptorRetireCount.fetch_add(1);
        UpdateTransientBindlessFrameStatsLocked();
#endif
        return;
    }

    RetiredTransientBindlessDescriptorIndices.push_back({ Index, FenceValue });
#if WITH_BINDLESS_DESCRIPTOR_STATS
    TransientBindlessDescriptorRetireCount.fetch_add(1);
    UpdateTransientBindlessFrameStatsLocked();
#endif
}

void FDX12Device::TrackTransientBindlessDescriptorOwner(uint32_t Index, const std::string& OwnerLabel)
{
#if WITH_BINDLESS_DESCRIPTOR_STATS
    if (Index == UINT32_MAX)
    {
        return;
    }

    std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
    if (!bTrackLiveTransientBindlessOwners)
    {
        return;
    }
    LiveTransientBindlessDescriptorOwners[Index] = OwnerLabel;
#else
    (void)Index;
    (void)OwnerLabel;
#endif
}

void FDX12Device::SetLiveTransientBindlessOwnerTrackingEnabled(bool bEnabled)
{
#if WITH_BINDLESS_DESCRIPTOR_STATS
    std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
    bTrackLiveTransientBindlessOwners = bEnabled;
    if (!bTrackLiveTransientBindlessOwners)
    {
        LiveTransientBindlessDescriptorOwners.clear();
    }
#else
    (void)bEnabled;
#endif
}

void FDX12Device::PumpTransientBindlessDescriptorReclaim()
{
    if (!BindlessDescriptorHeap || !Device)
    {
        return;
    }

    const FDX12CommandQueue* Queue = GetGraphicsQueue();
    const uint64 CompletedFenceValue = Queue ? Queue->GetCompletedFenceValue() : 0;

    std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
    ReclaimTransientBindlessDescriptorIndicesLocked(CompletedFenceValue);
}

void FDX12Device::ResetBindlessDescriptorFrameStats()
{
#if WITH_BINDLESS_DESCRIPTOR_STATS
    std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
    const uint32_t FreeCount = static_cast<uint32_t>(FreeTransientBindlessDescriptorIndices.size());
    const uint32_t RetiredCount = static_cast<uint32_t>(RetiredTransientBindlessDescriptorIndices.size());
    const uint32_t HeapAllocatedCount = static_cast<uint32_t>(TransientBindlessDescriptorHeapAllocationCount.load());
    MinFreeTransientThisFrame = FreeCount;
    PeakTransientLiveThisFrame = (HeapAllocatedCount >= (FreeCount + RetiredCount))
        ? (HeapAllocatedCount - FreeCount - RetiredCount)
        : 0u;
    TransientHeapAllocsThisFrame = 0u;
#endif
}

void FDX12Device::WriteBindlessSrv(uint32_t Index, ID3D12Resource* Resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& Desc) const
{
    if (!BindlessDescriptorHeap || !Device || Index == UINT32_MAX)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = BindlessDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    CpuHandle.ptr += static_cast<SIZE_T>(Index) * BindlessDescriptorStride;
    Device->CreateShaderResourceView(Resource, &Desc, CpuHandle);
    if (BindlessCpuDescriptorHeap)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE CpuOnlyHandle = BindlessCpuDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        CpuOnlyHandle.ptr += static_cast<SIZE_T>(Index) * BindlessDescriptorStride;
        Device->CreateShaderResourceView(Resource, &Desc, CpuOnlyHandle);
    }
}

void FDX12Device::WriteBindlessUav(uint32_t Index, ID3D12Resource* Resource, ID3D12Resource* Counter, const D3D12_UNORDERED_ACCESS_VIEW_DESC& Desc) const
{
    if (!BindlessDescriptorHeap || !Device || Index == UINT32_MAX)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = BindlessDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    CpuHandle.ptr += static_cast<SIZE_T>(Index) * BindlessDescriptorStride;
    Device->CreateUnorderedAccessView(Resource, Counter, &Desc, CpuHandle);
    if (BindlessCpuDescriptorHeap)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE CpuOnlyHandle = BindlessCpuDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        CpuOnlyHandle.ptr += static_cast<SIZE_T>(Index) * BindlessDescriptorStride;
        Device->CreateUnorderedAccessView(Resource, Counter, &Desc, CpuOnlyHandle);
    }
}

void FDX12Device::ReclaimTransientBindlessDescriptorIndicesLocked(uint64_t CompletedFenceValue)
{
    uint64_t ReclaimedCount = 0;
    while (!RetiredTransientBindlessDescriptorIndices.empty())
    {
        const FRetiredBindlessDescriptor& Retired = RetiredTransientBindlessDescriptorIndices.front();
        if (Retired.FenceValue > CompletedFenceValue)
        {
            break;
        }

        FreeTransientBindlessDescriptorIndices.push_back(Retired.Index);
        RetiredTransientBindlessDescriptorIndices.pop_front();
        ++ReclaimedCount;
    }

    if (ReclaimedCount > 0)
    {
#if WITH_BINDLESS_DESCRIPTOR_STATS
        TransientBindlessDescriptorReclaimCount.fetch_add(ReclaimedCount);
        UpdateTransientBindlessFrameStatsLocked();
#endif
    }
}

#if WITH_BINDLESS_DESCRIPTOR_STATS
void FDX12Device::UpdateTransientBindlessFrameStatsLocked()
{
    const uint32_t FreeCount = static_cast<uint32_t>(FreeTransientBindlessDescriptorIndices.size());
    const uint32_t RetiredCount = static_cast<uint32_t>(RetiredTransientBindlessDescriptorIndices.size());
    const uint32_t HeapAllocatedCount = static_cast<uint32_t>(TransientBindlessDescriptorHeapAllocationCount.load());
    const uint32_t CurrentLiveCount = (HeapAllocatedCount >= (FreeCount + RetiredCount))
        ? (HeapAllocatedCount - FreeCount - RetiredCount)
        : 0u;

    MinFreeTransientThisFrame = (std::min)(MinFreeTransientThisFrame, FreeCount);
    PeakTransientLiveThisFrame = (std::max)(PeakTransientLiveThisFrame, CurrentLiveCount);
}

void FDX12Device::MaybeLogBindlessDescriptorPressure(const char* Reason, uint32_t UsedCount)
{
    if (BindlessDescriptorCount == 0)
    {
        return;
    }

    static constexpr uint32_t Thresholds[] = { 75u, 85u, 90u, 95u, 98u };
    const uint32_t UsagePercent = static_cast<uint32_t>((static_cast<uint64_t>(UsedCount) * 100ull) / static_cast<uint64_t>(BindlessDescriptorCount));

    uint32_t TargetLevel = 0u;
    while (TargetLevel < _countof(Thresholds) && UsagePercent >= Thresholds[TargetLevel])
    {
        ++TargetLevel;
    }

    if (TargetLevel == 0u)
    {
        return;
    }

    uint32_t ObservedLevel = BindlessPressureLogLevel.load();
    while (TargetLevel > ObservedLevel)
    {
        if (BindlessPressureLogLevel.compare_exchange_weak(ObservedLevel, TargetLevel))
        {
            LogBindlessDescriptorStats(Reason);
            break;
        }
    }
}

void FDX12Device::LogBindlessDescriptorStats(const char* Reason) const
{
    uint32_t FreeTransientCount = 0u;
    uint32_t RetiredTransientCount = 0u;
    {
        std::lock_guard<std::mutex> Lock(TransientBindlessDescriptorMutex);
        FreeTransientCount = static_cast<uint32_t>(FreeTransientBindlessDescriptorIndices.size());
        RetiredTransientCount = static_cast<uint32_t>(RetiredTransientBindlessDescriptorIndices.size());
    }

    const FDX12CommandQueue* Queue = GraphicsQueue.get();
    const uint64_t CompletedFenceValue = Queue ? Queue->GetCompletedFenceValue() : 0u;
    const uint64_t LastSignaledFenceValue = Queue ? Queue->GetLastSignaledFenceValue() : 0u;
    const uint32_t NextIndex = BindlessDescriptorNextIndex.load();
    const uint32_t UsedPercent = (BindlessDescriptorCount > 0u)
        ? static_cast<uint32_t>((static_cast<uint64_t>((std::min)(NextIndex, BindlessDescriptorCount)) * 100ull) / static_cast<uint64_t>(BindlessDescriptorCount))
        : 0u;

    std::ostringstream Stream;
    Stream << "Bindless descriptor stats [" << (Reason ? Reason : "unknown") << "]: "
        << "next=" << NextIndex << "/" << BindlessDescriptorCount
        << " (" << UsedPercent << "%)"
        << ", permanent_allocs=" << PermanentBindlessDescriptorAllocationCount.load()
        << ", transient_from_heap=" << TransientBindlessDescriptorHeapAllocationCount.load()
        << ", transient_reuse=" << TransientBindlessDescriptorReuseCount.load()
        << ", transient_retired=" << TransientBindlessDescriptorRetireCount.load()
        << ", transient_reclaimed=" << TransientBindlessDescriptorReclaimCount.load()
        << ", free_transient=" << FreeTransientCount
        << ", retired_transient=" << RetiredTransientCount
        << ", completed_fence=" << CompletedFenceValue
        << ", last_signaled_fence=" << LastSignaledFenceValue;
    LogWarning(Stream.str());
}
#endif

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
