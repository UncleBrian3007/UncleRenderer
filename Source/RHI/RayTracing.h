#pragma once

#include "DX12Commons.h"
#include <string>
#include <vector>

class FRayTracingPipelineState
{
public:
    bool Initialize(ID3D12Device5* Device, const D3D12_STATE_OBJECT_DESC& Desc);
    ID3D12StateObject* GetStateObject() const { return StateObject.Get(); }
    ID3D12StateObjectProperties* GetStateObjectProperties() const { return StateObjectProperties.Get(); }
    bool IsValid() const { return StateObject != nullptr; }

private:
    ComPtr<ID3D12StateObject> StateObject;
    ComPtr<ID3D12StateObjectProperties> StateObjectProperties;
};

class FRayTracingShaderLibrary
{
public:
    bool LoadFromFile(const std::wstring& FilePath);
    void SetBytecode(std::vector<uint8_t> InBytecode);
    const D3D12_SHADER_BYTECODE& GetBytecode() const { return BytecodeDesc; }
    bool IsValid() const { return !Bytecode.empty(); }

private:
    void UpdateBytecodeDesc();

private:
    std::vector<uint8_t> Bytecode;
    D3D12_SHADER_BYTECODE BytecodeDesc{};
};

class FRayTracingDevice
{
public:
    bool Initialize(ID3D12Device* InDevice);
    ID3D12Device5* GetDevice() const { return Device.Get(); }
    bool IsValid() const { return Device != nullptr; }
    bool CreateStateObject(const D3D12_STATE_OBJECT_DESC& Desc, FRayTracingPipelineState& OutPipelineState) const;

private:
    ComPtr<ID3D12Device5> Device;
};
