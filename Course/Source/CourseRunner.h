#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <d3dx12.h>
#include <wrl.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>
#include <iostream>
#ifdef _DEBUG
#  include <pix3.h>
#endif

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------
// CourseLog – formats like printf, writes to OutputDebugString + log file.
// All course source files automatically redirect printf/fprintf here
// (unless COURSE_RUNNER_IMPL is defined, which CourseRunner.cpp sets).
// -----------------------------------------------------------------------
void CourseLog(const char* Fmt, ...);

#ifndef COURSE_RUNNER_IMPL
#  define printf(...)          CourseLog(__VA_ARGS__)
#  define fprintf(stream, ...) CourseLog(__VA_ARGS__)
#endif

// D3D12 Agility SDK exports – must appear in exactly one .cpp translation unit.
// Defined in CourseRunner.cpp.

#define HR_CHECK(expr) do { HRESULT _hr = (expr); if (FAILED(_hr)) { \
    CourseLog("D3D12 error 0x%08X at %s:%d\n", _hr, __FILE__, __LINE__); \
    throw std::runtime_error("D3D12 call failed"); } } while(0)

// -----------------------------------------------------------------------
// GpuBuffer – lightweight wrapper around an ID3D12Resource.
// -----------------------------------------------------------------------
struct GpuBuffer
{
    ComPtr<ID3D12Resource> Resource;
    uint64_t               Size = 0;

    bool IsValid() const { return Resource != nullptr; }
    D3D12_GPU_VIRTUAL_ADDRESS VA() const
    {
        return Resource ? Resource->GetGPUVirtualAddress() : 0;
    }
};

// -----------------------------------------------------------------------
// CourseRunner – minimal headless D3D12 compute context.
//
// Root signature layout used by all compute dispatches:
//   Param 0: Root Constants  (b0)  – up to kMaxConstantDwords dwords
//   Param 1..N: Root UAV (u0..u(N-1)) – GPU VA binding, no descriptor heap
// -----------------------------------------------------------------------
class CourseRunner
{
public:
    static constexpr uint32_t kMaxUavRoots       = 7;
    static constexpr uint32_t kMaxConstantDwords = 16;

    // ------------------------------------------------------------------
    // Initialisation / teardown
    // ------------------------------------------------------------------
    bool Initialize();
    void PrintAdapterInfo() const;

    // ------------------------------------------------------------------
    // Shader compilation (DXC, target cs_6_6)
    // ------------------------------------------------------------------
    bool CompileCS(
        const std::wstring& ShaderPath,
        const std::wstring& EntryPoint,
        std::vector<uint8_t>& OutBytecode,
        const std::vector<std::wstring>& Defines = {});

    // ------------------------------------------------------------------
    // Pipeline helpers
    // ------------------------------------------------------------------
    ComPtr<ID3D12RootSignature> CreateRootSignature(uint32_t NumUavs, uint32_t ConstantDwords);
    ComPtr<ID3D12PipelineState> CreateComputePSO(
        ID3D12RootSignature* RootSig,
        const std::vector<uint8_t>& Bytecode);

    // ------------------------------------------------------------------
    // Buffer management
    // ------------------------------------------------------------------
    // GPU-resident buffer (UAV-capable, for use as u0..uN).
    GpuBuffer CreateBuffer(uint64_t Bytes, const wchar_t* Name = nullptr);
    // CPU->GPU staging buffer.
    GpuBuffer CreateUploadBuffer(uint64_t Bytes, const wchar_t* Name = nullptr);
    // GPU->CPU readback buffer.
    GpuBuffer CreateReadbackBuffer(uint64_t Bytes, const wchar_t* Name = nullptr);

    // Upload data: write to upload buffer, copy to dst, flush.
    void Upload(const GpuBuffer& Dst, const void* Data, uint64_t Bytes);
    // Zero-fill a GPU buffer via a temp upload + copy.
    void ZeroBuffer(const GpuBuffer& Dst, uint64_t Bytes);
    // Copy a GPU buffer to a readback buffer and flush so data is CPU-readable.
    void Readback(const GpuBuffer& Dst, const GpuBuffer& Src, uint64_t Bytes);

    // Map a readback buffer for CPU reading (call UnmapReadback when done).
    const void* MapReadback(const GpuBuffer& Buf) const;
    void        UnmapReadback(const GpuBuffer& Buf) const;

    // ------------------------------------------------------------------
    // Dispatch
    // ------------------------------------------------------------------
    struct DispatchDesc
    {
        ID3D12RootSignature*       RootSig        = nullptr;
        ID3D12PipelineState*       PSO            = nullptr;
        uint32_t                   GroupsX        = 1;
        uint32_t                   GroupsY        = 1;
        uint32_t                   GroupsZ        = 1;
        const void*                Constants      = nullptr;
        uint32_t                   ConstantDwords = 0;
        D3D12_GPU_VIRTUAL_ADDRESS  Uavs[kMaxUavRoots] = {};
        uint32_t                   NumUavs        = 0;
        const wchar_t*             Label          = nullptr;  // shown in PIX event list
    };

    void Dispatch(const DispatchDesc& Desc);

    // Transition a buffer between resource states.
    void Transition(ID3D12Resource* Res, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After);

    // Submit all recorded commands and wait for GPU idle.
    void Flush();

    // PIX programmatic GPU capture (Debug only; no-ops in Release).
    // Call BeginCapture before dispatches you want to inspect, EndCapture after Flush().
    void BeginCapture(const wchar_t* CaptureName = nullptr);
    void EndCapture();

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------
    ID3D12Device* GetDevice() const { return Device.Get(); }

private:
    void EnsureRecording();
    void SubmitAndWait();

    ComPtr<ID3D12Device>              Device;
    ComPtr<ID3D12CommandQueue>        Queue;
    ComPtr<ID3D12CommandAllocator>    Allocator;
    ComPtr<ID3D12GraphicsCommandList> CmdList;
    ComPtr<ID3D12Fence>               Fence;
    HANDLE                            FenceEvent = nullptr;
    uint64_t                          FenceValue = 0;
    bool                              bRecording = false;

    ComPtr<IDxcUtils>          DxcUtils;
    ComPtr<IDxcCompiler3>      DxcCompiler;
    ComPtr<IDxcIncludeHandler> DxcIncHandler;
};
