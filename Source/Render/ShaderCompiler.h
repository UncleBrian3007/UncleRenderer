#pragma once

#include <string>
#include <vector>
#include <wrl.h>
#include <d3d12shader.h>
#include <dxcapi.h>

struct FShaderCompileRequest
{
    std::wstring FilePath;
    std::wstring EntryPoint;
    std::wstring Target;
    std::vector<std::wstring> Defines;
    std::vector<uint8_t>* OutByteCode = nullptr;
    bool bSuccess = false;
};

class FShaderCompiler
{
public:
    FShaderCompiler();

    bool CompileFromFile(
        const std::wstring& FilePath,
        const std::wstring& EntryPoint,
        const std::wstring& Target,
        std::vector<uint8_t>& OutByteCode,
        const std::vector<std::wstring>& Defines = {});

    /**
     * Compile multiple shaders in parallel using the task system.
     * @param Requests Vector of shader compilation requests
     * @return True if all shaders compiled successfully
     */
    bool CompileShadersParallel(std::vector<FShaderCompileRequest>& Requests);

private:
    Microsoft::WRL::ComPtr<IDxcUtils> Utils;
    Microsoft::WRL::ComPtr<IDxcCompiler3> Compiler;
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> IncludeHandler;
};
