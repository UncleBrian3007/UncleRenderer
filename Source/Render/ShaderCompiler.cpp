#include "ShaderCompiler.h"

#include "Core/Logger.h"
#include "../Core/TaskSystem.h"

#include <cstring>
#include <string>
#include <vector>
#include <Windows.h>
#include <chrono>

namespace
{
    std::string WStringToUtf8(const std::wstring& WStr)
    {
        if (WStr.empty())
        {
            return std::string();
        }

        int RequiredSize = WideCharToMultiByte(CP_UTF8, 0, WStr.c_str(), static_cast<int>(WStr.size()), nullptr, 0, nullptr, nullptr);
        if (RequiredSize <= 0)
        {
            return std::string();
        }

        std::string Result(static_cast<size_t>(RequiredSize), '\0');
        WideCharToMultiByte(CP_UTF8, 0, WStr.c_str(), static_cast<int>(WStr.size()), Result.data(), RequiredSize, nullptr, nullptr);
        return Result;
    }
}

FShaderCompiler::FShaderCompiler()
{
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils));
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler));

    if (Utils)
    {
        Utils->CreateDefaultIncludeHandler(&IncludeHandler);
    }
}

bool FShaderCompiler::CompileFromFile(
    const std::wstring& FilePath,
    const std::wstring& EntryPoint,
    const std::wstring& Target,
    std::vector<uint8_t>& OutByteCode,
    const std::vector<std::wstring>& Defines)
{
    if (!Utils || !Compiler)
    {
        LogError("Shader compiler is not initialized.");
        return false;
    }

    Microsoft::WRL::ComPtr<IDxcBlobEncoding> SourceBlob;
    if (FAILED(Utils->LoadFile(FilePath.c_str(), nullptr, &SourceBlob)))
    {
        const std::string NarrowPath = WStringToUtf8(FilePath);
        LogError("Failed to load shader file: " + NarrowPath);
        return false;
    }

    DxcBuffer SourceBuffer = {};
    SourceBuffer.Ptr = SourceBlob->GetBufferPointer();
    SourceBuffer.Size = SourceBlob->GetBufferSize();
    SourceBuffer.Encoding = DXC_CP_ACP;

    std::wstring EntryPointArg = L"-E" + EntryPoint;
    std::wstring TargetArg = L"-T" + Target;

    std::vector<LPCWSTR> Arguments;
    Arguments.push_back(L"-Zpr");
    Arguments.push_back(EntryPointArg.c_str());
    Arguments.push_back(TargetArg.c_str());
    Arguments.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);
    Arguments.push_back(L"-IShaders");
    for (const std::wstring& Define : Defines)
    {
        Arguments.push_back(L"-D");
        Arguments.push_back(Define.c_str());
    }

#if defined(_DEBUG)
        // Enable rich shader debugging information for PIX captures.
        Arguments.push_back(L"-Zi");
        Arguments.push_back(L"-Qembed_debug");
        Arguments.push_back(L"-Od");
#endif

    Microsoft::WRL::ComPtr<IDxcResult> CompileResult;
    const std::string NarrowPath = WStringToUtf8(FilePath);
    const std::string EntryPointUtf8 = WStringToUtf8(EntryPoint);
    const std::string TargetUtf8 = WStringToUtf8(Target);
    LogInfo("Compiling shader from file: " + NarrowPath + ", entry: " + EntryPointUtf8 + ", target: " + TargetUtf8);

    HRESULT hr = Compiler->Compile(&SourceBuffer, Arguments.data(), static_cast<uint32_t>(Arguments.size()), IncludeHandler.Get(), IID_PPV_ARGS(&CompileResult));
    if (FAILED(hr))
    {
        LogError("DxcCompile failed for shader: " + NarrowPath);
        return false;
    }

    HRESULT Status = S_OK;
    CompileResult->GetStatus(&Status);
    if (FAILED(Status))
    {
        Microsoft::WRL::ComPtr<IDxcBlobUtf8> ErrorBlob;
        CompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&ErrorBlob), nullptr);

        if (ErrorBlob && ErrorBlob->GetStringLength() > 0)
        {
            OutputDebugStringA(ErrorBlob->GetStringPointer());
            LogError("Shader compilation errors: " + std::string(ErrorBlob->GetStringPointer(), ErrorBlob->GetStringLength()));
        }

        return false;
    }

    Microsoft::WRL::ComPtr<IDxcBlob> ShaderBlob;
    CompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&ShaderBlob), nullptr);
    if (!ShaderBlob)
    {
        LogError("Shader compilation produced no output blob for: " + NarrowPath);
        return false;
    }

    OutByteCode.resize(ShaderBlob->GetBufferSize());
    memcpy(OutByteCode.data(), ShaderBlob->GetBufferPointer(), ShaderBlob->GetBufferSize());
    return true;
}

bool FShaderCompiler::CompileShadersParallel(std::vector<FShaderCompileRequest>& Requests)
{
    if (Requests.empty())
    {
        return true;
    }

    const auto StartTime = std::chrono::high_resolution_clock::now();

    // NOTE: Parallel shader compilation is disabled due to DXC thread-safety issues.
    // DXC COM objects (IDxcCompiler3, IDxcUtils, IDxcIncludeHandler) are not thread-safe.
    // When multiple threads access these shared objects concurrently, it causes D3D12 errors:
    // "ID3D12CommandAllocator is being reset before previous executions have completed"
    // 
    // To enable true parallel compilation, each thread would need its own FShaderCompiler
    // instance with separate DXC COM objects, but the performance benefit is minimal
    // since DXC compilation is already quite fast and the overhead of thread coordination
    // would likely negate any gains.
    
    // Compile shaders serially for thread safety
    for (FShaderCompileRequest& Request : Requests)
    {
        Request.bSuccess = CompileFromFile(
            Request.FilePath,
            Request.EntryPoint,
            Request.Target,
            *Request.OutByteCode,
            Request.Defines);
    }

    const auto EndTime = std::chrono::high_resolution_clock::now();
    const auto Duration = std::chrono::duration_cast<std::chrono::milliseconds>(EndTime - StartTime);
    LogInfo("Compiled " + std::to_string(Requests.size()) + " shaders serially in " + std::to_string(Duration.count()) + " ms");

    // Check if all shaders compiled successfully
    bool bAllSuccess = true;
    for (const FShaderCompileRequest& Request : Requests)
    {
        if (!Request.bSuccess)
        {
            bAllSuccess = false;
            LogError("Failed to compile shader: " + WStringToUtf8(Request.FilePath));
        }
    }

    return bAllSuccess;
}

