#include "ShaderCompiler.h"

#include <cstring>

FShaderCompiler::FShaderCompiler()
{
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&Utils));
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&Compiler));
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

    DxcBuffer SourceBuffer;
    SourceBuffer.Ptr = SourceBlob->GetBufferPointer();
    SourceBuffer.Size = SourceBlob->GetBufferSize();
    SourceBuffer.Encoding = DXC_CP_ACP;

    LPCWSTR Arguments[] = { L"-Zpr" };
    Microsoft::WRL::ComPtr<IDxcResult> CompileResult;
    HRESULT hr = Compiler->Compile(&SourceBuffer, Arguments, _countof(Arguments), nullptr, IID_PPV_ARGS(&CompileResult));
    if (FAILED(hr))
    {
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
