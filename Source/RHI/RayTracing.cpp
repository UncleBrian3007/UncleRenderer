#include "RayTracing.h"
#include "../Core/Logger.h"
#include <fstream>
#include <string>
#include <filesystem>

namespace
{
    std::string WStringToUtf8(const std::wstring& Value)
    {
        return std::filesystem::path(Value).u8string();
    }
}

bool FRayTracingPipelineState::Initialize(ID3D12Device5* Device, const D3D12_STATE_OBJECT_DESC& Desc)
{
    if (!Device)
    {
        LogError("Ray tracing pipeline state initialization failed: device is null.");
        return false;
    }

    StateObject.Reset();
    StateObjectProperties.Reset();

    if (FAILED(Device->CreateStateObject(&Desc, IID_PPV_ARGS(StateObject.ReleaseAndGetAddressOf()))))
    {
        LogError("Ray tracing pipeline state initialization failed: CreateStateObject failed.");
        return false;
    }

    StateObject.As(&StateObjectProperties);
    return true;
}

bool FRayTracingShaderLibrary::LoadFromFile(const std::wstring& FilePath)
{
    std::ifstream File(FilePath, std::ios::binary | std::ios::ate);
    if (!File.is_open())
    {
        LogWarning("Ray tracing shader library load failed: " + WStringToUtf8(FilePath));
        return false;
    }

    const std::streamsize Size = File.tellg();
    if (Size <= 0)
    {
        LogWarning("Ray tracing shader library load failed: file is empty.");
        return false;
    }

    Bytecode.resize(static_cast<size_t>(Size));
    File.seekg(0, std::ios::beg);
    if (!File.read(reinterpret_cast<char*>(Bytecode.data()), Size))
    {
        LogWarning("Ray tracing shader library load failed: read error.");
        Bytecode.clear();
        UpdateBytecodeDesc();
        return false;
    }

    UpdateBytecodeDesc();
    return true;
}

void FRayTracingShaderLibrary::SetBytecode(std::vector<uint8_t> InBytecode)
{
    Bytecode = std::move(InBytecode);
    UpdateBytecodeDesc();
}

void FRayTracingShaderLibrary::UpdateBytecodeDesc()
{
    BytecodeDesc.pShaderBytecode = Bytecode.empty() ? nullptr : Bytecode.data();
    BytecodeDesc.BytecodeLength = Bytecode.size();
}

bool FRayTracingDevice::Initialize(ID3D12Device* InDevice)
{
    Device.Reset();

    if (!InDevice)
    {
        LogWarning("Ray tracing device initialization failed: device is null.");
        return false;
    }

    if (FAILED(InDevice->QueryInterface(IID_PPV_ARGS(Device.ReleaseAndGetAddressOf()))))
    {
        LogWarning("Ray tracing device initialization failed: ID3D12Device5 not available.");
        return false;
    }

    return true;
}

bool FRayTracingDevice::CreateStateObject(const D3D12_STATE_OBJECT_DESC& Desc, FRayTracingPipelineState& OutPipelineState) const
{
    if (!Device)
    {
        LogWarning("Ray tracing state object creation failed: device not initialized.");
        return false;
    }

    return OutPipelineState.Initialize(Device.Get(), Desc);
}
