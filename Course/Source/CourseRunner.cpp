#define COURSE_RUNNER_IMPL  // suppress printf/fprintf redirects in this TU
#include "CourseRunner.h"
#include <cstdio>
#include <cstdarg>

#include <dxgi1_6.h>
#include <stdexcept>
#include <cstring>
#include <filesystem>

extern "C"
{
    __declspec(dllexport) extern const UINT  D3D12SDKVersion = D3D12_SDK_VERSION;
    __declspec(dllexport) extern const char* D3D12SDKPath    = ".\\D3D12\\";
}
// -----------------------------------------------------------------------
static FILE* s_LogFile = nullptr;

void CourseLog(const char* Fmt, ...)
{
    char Buf[2048];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    va_end(Args);

    OutputDebugStringA(Buf);

    if (!s_LogFile)
        fopen_s(&s_LogFile, "CourseTests.log", "w");
    if (s_LogFile)
    {
        fputs(Buf, s_LogFile);
        fflush(s_LogFile);
    }
}

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE Type)
{
    D3D12_HEAP_PROPERTIES P = {};
    P.Type                 = Type;
    P.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    P.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    P.CreationNodeMask     = 1;
    P.VisibleNodeMask      = 1;
    return P;
}

static D3D12_RESOURCE_DESC BufferDesc(uint64_t Bytes, D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC D = {};
    D.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    D.Width              = Bytes;
    D.Height             = 1;
    D.DepthOrArraySize   = 1;
    D.MipLevels          = 1;
    D.Format             = DXGI_FORMAT_UNKNOWN;
    D.SampleDesc.Count   = 1;
    D.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D.Flags              = Flags;
    return D;
}

// -----------------------------------------------------------------------
// Initialize
// -----------------------------------------------------------------------
bool CourseRunner::Initialize()
{
#ifdef _DEBUG
    // Load WinPixGpuCapturer.dll from the PIX installation automatically.
    // This enables PIXBeginCapture/PIXEndCapture without having to launch from PIX.
    if (GetModuleHandle(L"WinPixGpuCapturer.dll") == nullptr)
    {
        HMODULE Mod = PIXLoadLatestWinPixGpuCapturerLibrary();
        if (Mod)
            CourseLog("[PIX] WinPixGpuCapturer.dll loaded – programmatic capture enabled.\n");
        else
            CourseLog("[PIX] WinPixGpuCapturer.dll not found. Launch from PIX to enable capture.\n");
    }
#endif

    // Create DXGI factory and pick the first hardware adapter.
    ComPtr<IDXGIFactory6> Factory;
    HR_CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory)));

    ComPtr<IDXGIAdapter1> Adapter;
    for (UINT i = 0;
         SUCCEEDED(Factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(Adapter.ReleaseAndGetAddressOf())));
         ++i)
    {
        DXGI_ADAPTER_DESC1 Desc;
        Adapter->GetDesc1(&Desc);
        if (Desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr)))
            break;
    }

    HR_CHECK(D3D12CreateDevice(Adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&Device)));

    // Verify SM 6.0 (required for wave intrinsics).
    D3D12_FEATURE_DATA_SHADER_MODEL SmData = { D3D_SHADER_MODEL_6_0 };
    HR_CHECK(Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &SmData, sizeof(SmData)));

    // Compute-only command queue.
    D3D12_COMMAND_QUEUE_DESC QDesc = {};
    QDesc.Type  = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    QDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    HR_CHECK(Device->CreateCommandQueue(&QDesc, IID_PPV_ARGS(&Queue)));

    HR_CHECK(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&Allocator)));
    HR_CHECK(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, Allocator.Get(), nullptr, IID_PPV_ARGS(&CmdList)));
    CmdList->Close();
    bRecording = false;

    HR_CHECK(Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence)));
    FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!FenceEvent) return false;

    // DXC initialisation.
    HR_CHECK(DxcCreateInstance(CLSID_DxcUtils,     IID_PPV_ARGS(&DxcUtils)));
    HR_CHECK(DxcCreateInstance(CLSID_DxcCompiler,  IID_PPV_ARGS(&DxcCompiler)));
    HR_CHECK(DxcUtils->CreateDefaultIncludeHandler(&DxcIncHandler));

    return true;
}

void CourseRunner::PrintAdapterInfo() const
{
    ComPtr<IDXGIFactory4> Factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&Factory));

    DXGI_ADAPTER_DESC1 Desc = {};
    ComPtr<IDXGIAdapter1> Adapter;
    if (SUCCEEDED(Factory->EnumAdapters1(0, &Adapter)))
        Adapter->GetDesc1(&Desc);

    wprintf(L"GPU: %s\n", Desc.Description);
    printf("VRAM: %zu MB\n", Desc.DedicatedVideoMemory / (1024 * 1024));
}

// -----------------------------------------------------------------------
// Shader compilation
// -----------------------------------------------------------------------
bool CourseRunner::CompileCS(
    const std::wstring& ShaderPath,
    const std::wstring& EntryPoint,
    std::vector<uint8_t>& OutBytecode,
    const std::vector<std::wstring>& Defines)
{
    ComPtr<IDxcBlobEncoding> Source;
    if (FAILED(DxcUtils->LoadFile(ShaderPath.c_str(), nullptr, &Source)))
    {
        fprintf(stderr, "Failed to load shader: %ls\n", ShaderPath.c_str());
        return false;
    }

    DxcBuffer SourceBuf = {};
    SourceBuf.Ptr      = Source->GetBufferPointer();
    SourceBuf.Size     = Source->GetBufferSize();
    SourceBuf.Encoding = DXC_CP_ACP;

    std::vector<LPCWSTR> Args;
    std::wstring EntryArg = L"-E" + EntryPoint;
    Args.push_back(ShaderPath.c_str());
    Args.push_back(L"-Tcs_6_6");
    Args.push_back(EntryArg.c_str());
    Args.push_back(L"-Zpr");
    Args.push_back(L"-ICourse/Shaders");
    Args.push_back(DXC_ARG_WARNINGS_ARE_ERRORS);

    std::vector<std::wstring> DefineArgs;
    for (const auto& D : Defines)
    {
        DefineArgs.push_back(L"-D" + D);
        Args.push_back(DefineArgs.back().c_str());
    }

#if defined(_DEBUG)
    Args.push_back(L"-Zi");
    Args.push_back(L"-Qembed_debug");
    Args.push_back(L"-Od");
#endif

    ComPtr<IDxcResult> Result;
    HRESULT Hr = DxcCompiler->Compile(&SourceBuf, Args.data(), (uint32_t)Args.size(), DxcIncHandler.Get(), IID_PPV_ARGS(&Result));
    if (FAILED(Hr))
    {
        fprintf(stderr, "DXC compile call failed for %ls\n", ShaderPath.c_str());
        return false;
    }

    HRESULT Status;
    Result->GetStatus(&Status);
    if (FAILED(Status))
    {
        ComPtr<IDxcBlobUtf8> Errors;
        Result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&Errors), nullptr);
        if (Errors && Errors->GetStringLength() > 0)
            fprintf(stderr, "Shader errors (%ls):\n%s\n", ShaderPath.c_str(), Errors->GetStringPointer());
        return false;
    }

    ComPtr<IDxcBlob> Blob;
    Result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&Blob), nullptr);
    if (!Blob) return false;

    OutBytecode.resize(Blob->GetBufferSize());
    memcpy(OutBytecode.data(), Blob->GetBufferPointer(), Blob->GetBufferSize());
    return true;
}

// -----------------------------------------------------------------------
// Root signature
// -----------------------------------------------------------------------
ComPtr<ID3D12RootSignature> CourseRunner::CreateRootSignature(uint32_t NumUavs, uint32_t ConstantDwords)
{
    assert(NumUavs <= kMaxUavRoots);
    assert(ConstantDwords <= kMaxConstantDwords);

    std::vector<CD3DX12_ROOT_PARAMETER1> Params(1 + NumUavs);

    // Param 0: root constants at b0.
    Params[0].InitAsConstants(ConstantDwords, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

    // Params 1..N: root UAVs at u0..u(N-1).
    for (uint32_t i = 0; i < NumUavs; ++i)
        Params[1 + i].InitAsUnorderedAccessView(i, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC Desc;
    Desc.Init_1_1((UINT)Params.size(), Params.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> Serialized, Errors;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&Desc, &Serialized, &Errors));
    if (Errors) fprintf(stderr, "Root sig errors: %s\n", (const char*)Errors->GetBufferPointer());

    ComPtr<ID3D12RootSignature> RS;
    HR_CHECK(Device->CreateRootSignature(0, Serialized->GetBufferPointer(), Serialized->GetBufferSize(), IID_PPV_ARGS(&RS)));
    return RS;
}

// -----------------------------------------------------------------------
// PSO
// -----------------------------------------------------------------------
ComPtr<ID3D12PipelineState> CourseRunner::CreateComputePSO(
    ID3D12RootSignature* RootSig,
    const std::vector<uint8_t>& Bytecode)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC Desc = {};
    Desc.pRootSignature = RootSig;
    Desc.CS = { Bytecode.data(), Bytecode.size() };

    ComPtr<ID3D12PipelineState> PSO;
    HR_CHECK(Device->CreateComputePipelineState(&Desc, IID_PPV_ARGS(&PSO)));
    return PSO;
}

// -----------------------------------------------------------------------
// Buffer helpers
// -----------------------------------------------------------------------
GpuBuffer CourseRunner::CreateBuffer(uint64_t Bytes, const wchar_t* Name)
{
    GpuBuffer Buf;
    Buf.Size = Bytes;
    auto HP  = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto RD  = BufferDesc(Bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HR_CHECK(Device->CreateCommittedResource(
        &HP, D3D12_HEAP_FLAG_NONE, &RD,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&Buf.Resource)));
    if (Name) Buf.Resource->SetName(Name);
    return Buf;
}

GpuBuffer CourseRunner::CreateUploadBuffer(uint64_t Bytes, const wchar_t* Name)
{
    GpuBuffer Buf;
    Buf.Size = Bytes;
    auto HP  = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    auto RD  = BufferDesc(Bytes);
    HR_CHECK(Device->CreateCommittedResource(
        &HP, D3D12_HEAP_FLAG_NONE, &RD,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&Buf.Resource)));
    if (Name) Buf.Resource->SetName(Name);
    return Buf;
}

GpuBuffer CourseRunner::CreateReadbackBuffer(uint64_t Bytes, const wchar_t* Name)
{
    GpuBuffer Buf;
    Buf.Size = Bytes;
    auto HP  = HeapProps(D3D12_HEAP_TYPE_READBACK);
    auto RD  = BufferDesc(Bytes);
    HR_CHECK(Device->CreateCommittedResource(
        &HP, D3D12_HEAP_FLAG_NONE, &RD,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&Buf.Resource)));
    if (Name) Buf.Resource->SetName(Name);
    return Buf;
}

void CourseRunner::Upload(const GpuBuffer& Dst, const void* Data, uint64_t Bytes)
{
    GpuBuffer Upload = CreateUploadBuffer(Bytes, L"TmpUpload");

    void* Mapped = nullptr;
    D3D12_RANGE ReadRange = { 0, 0 };
    HR_CHECK(Upload.Resource->Map(0, &ReadRange, &Mapped));
    memcpy(Mapped, Data, (size_t)Bytes);
    Upload.Resource->Unmap(0, nullptr);

    EnsureRecording();
#ifdef _DEBUG
    PIXBeginEvent(CmdList.Get(), PIX_COLOR(255, 200, 0), L"Upload (%llu B)", Bytes);
#endif
    Transition(Dst.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    CmdList->CopyBufferRegion(Dst.Resource.Get(), 0, Upload.Resource.Get(), 0, Bytes);
    Transition(Dst.Resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#ifdef _DEBUG
    PIXEndEvent(CmdList.Get());
#endif
    Flush();
}

void CourseRunner::ZeroBuffer(const GpuBuffer& Dst, uint64_t Bytes)
{
    std::vector<uint8_t> Zeros(static_cast<size_t>(Bytes), 0);
    Upload(Dst, Zeros.data(), Bytes);
}

void CourseRunner::Readback(const GpuBuffer& Dst, const GpuBuffer& Src, uint64_t Bytes)
{
    EnsureRecording();
#ifdef _DEBUG
    PIXBeginEvent(CmdList.Get(), PIX_COLOR(0, 220, 100), L"Readback (%llu B)", Bytes);
#endif
    Transition(Src.Resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    CmdList->CopyBufferRegion(Dst.Resource.Get(), 0, Src.Resource.Get(), 0, Bytes);
    Transition(Src.Resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
#ifdef _DEBUG
    PIXEndEvent(CmdList.Get());
#endif
    Flush();
}

const void* CourseRunner::MapReadback(const GpuBuffer& Buf) const
{
    void* Ptr = nullptr;
    D3D12_RANGE ReadRange = { 0, (SIZE_T)Buf.Size };
    HR_CHECK(Buf.Resource->Map(0, &ReadRange, &Ptr));
    return Ptr;
}

void CourseRunner::UnmapReadback(const GpuBuffer& Buf) const
{
    D3D12_RANGE WriteRange = { 0, 0 };
    Buf.Resource->Unmap(0, &WriteRange);
}

// -----------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------
void CourseRunner::Dispatch(const DispatchDesc& Desc)
{
    EnsureRecording();

#ifdef _DEBUG
    {
        const wchar_t* Label = Desc.Label ? Desc.Label : L"Dispatch";
        PIXBeginEvent(CmdList.Get(), PIX_COLOR(64, 128, 255), L"%s (%u,%u,%u)",
                      Label, Desc.GroupsX, Desc.GroupsY, Desc.GroupsZ);
    }
#endif

    CmdList->SetComputeRootSignature(Desc.RootSig);
    CmdList->SetPipelineState(Desc.PSO);

    if (Desc.Constants && Desc.ConstantDwords > 0)
        CmdList->SetComputeRoot32BitConstants(0, Desc.ConstantDwords, Desc.Constants, 0);

    for (uint32_t i = 0; i < Desc.NumUavs; ++i)
        CmdList->SetComputeRootUnorderedAccessView(1 + i, Desc.Uavs[i]);

    CmdList->Dispatch(Desc.GroupsX, Desc.GroupsY, Desc.GroupsZ);
#ifdef _DEBUG
    PIXEndEvent(CmdList.Get());
#endif
}

void CourseRunner::Transition(ID3D12Resource* Res, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
{
    EnsureRecording();
    auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(Res, Before, After);
    CmdList->ResourceBarrier(1, &Barrier);
}

void CourseRunner::Flush()
{
    if (!bRecording) return;
    HR_CHECK(CmdList->Close());
    bRecording = false;

    ID3D12CommandList* Lists[] = { CmdList.Get() };
    Queue->ExecuteCommandLists(1, Lists);

    ++FenceValue;
    HR_CHECK(Queue->Signal(Fence.Get(), FenceValue));
    HR_CHECK(Fence->SetEventOnCompletion(FenceValue, FenceEvent));
    WaitForSingleObject(FenceEvent, INFINITE);

    HR_CHECK(Allocator->Reset());
}

void CourseRunner::BeginCapture(const wchar_t* CaptureName)
{
#ifdef _DEBUG
    const wchar_t* Name = CaptureName ? CaptureName : L"CourseCapture.wpix";

    // Ensure the parent directory exists.
    std::filesystem::path FilePath(Name);
    if (FilePath.has_parent_path())
    {
        std::error_code Ec;
        std::filesystem::create_directories(FilePath.parent_path(), Ec);
    }

    PIXCaptureParameters Params = {};
    Params.GpuCaptureParameters.FileName = Name;
    PIXBeginCapture(PIX_CAPTURE_GPU, &Params);
    CourseLog("[PIX] Capture started: %ls\n", Name);
#else
    (void)CaptureName;
#endif
}

void CourseRunner::EndCapture()
{
#ifdef _DEBUG
    PIXEndCapture(FALSE);
    CourseLog("[PIX] Capture ended.\n");
#endif
}

void CourseRunner::EnsureRecording()
{
    if (!bRecording)
    {
        HR_CHECK(CmdList->Reset(Allocator.Get(), nullptr));
        bRecording = true;
    }
}
