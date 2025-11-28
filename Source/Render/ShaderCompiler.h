#pragma once

#include <string>
#include <vector>
#include <wrl.h>
#include <d3d12shader.h>
#include <dxcapi.h>

class FShaderCompiler
{
public:
    FShaderCompiler();

    bool CompileFromFile(const std::wstring& FilePath, const std::wstring& EntryPoint, const std::wstring& Target, std::vector<uint8_t>& OutByteCode);

private:
    Microsoft::WRL::ComPtr<IDxcUtils> Utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> Compiler;
};
