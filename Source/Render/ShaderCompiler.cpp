#include "ShaderCompiler.h"

#include <cstring>
#include <string>
#include <vector>
#include <Windows.h>

FShaderCompiler::FShaderCompiler()
{
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils));
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler));

    if (Utils)
    {
        Utils->CreateDefaultIncludeHandler(&IncludeHandler);
    }
}

bool FShaderCompiler::CompileFromFile(const std::wstring& FilePath, const std::wstring& EntryPoint, const std::wstring& Target, std::vector<uint8_t>& OutByteCode)
{
    if (!Utils || !Compiler)
    {
        return false;
    }

    Microsoft::WRL::ComPtr<IDxcBlobEncoding> SourceBlob;
    if (FAILED(Utils->LoadFile(FilePath.c_str(), nullptr, &SourceBlob)))
    {
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

    Microsoft::WRL::ComPtr<IDxcResult> CompileResult;
    HRESULT hr = Compiler->Compile(&SourceBuffer, Arguments.data(), static_cast<uint32_t>(Arguments.size()), IncludeHandler.Get(), IID_PPV_ARGS(&CompileResult));
    if (FAILED(hr))
    {
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
        }

        return false;
    }

    Microsoft::WRL::ComPtr<IDxcBlob> ShaderBlob;
    CompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&ShaderBlob), nullptr);
    if (!ShaderBlob)
    {
        return false;
    }

    OutByteCode.resize(ShaderBlob->GetBufferSize());
    memcpy(OutByteCode.data(), ShaderBlob->GetBufferPointer(), ShaderBlob->GetBufferSize());
    return true;
}
