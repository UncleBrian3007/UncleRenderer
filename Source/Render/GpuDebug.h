#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

#include "GpuResource.h"

class FDX12Device;
class FDX12CommandContext;
struct FDeferredPassContext;

namespace GpuDebug
{
    struct FGpuDebugPrintEntry
    {
        uint32_t X = 0;
        uint32_t Y = 0;
        uint32_t Code = 0;
        uint32_t Color = 0;
    };

    inline constexpr uint32_t GpuDebugPrintMaxEntries = 4096;
    inline constexpr uint32_t GpuDebugPrintHeaderSize = sizeof(uint32_t);
    inline constexpr uint32_t GpuDebugPrintEntryStride = sizeof(FGpuDebugPrintEntry);
    inline constexpr uint64_t GpuDebugPrintBufferSize = GpuDebugPrintHeaderSize + static_cast<uint64_t>(GpuDebugPrintMaxEntries) * GpuDebugPrintEntryStride;
    inline constexpr uint32_t GpuDebugPrintStatsCount = 32;

    struct FGpuDebugLineEntry
    {
        DirectX::XMFLOAT3 P0{ 0.0f, 0.0f, 0.0f };
        float Padding0 = 0.0f;
        DirectX::XMFLOAT3 P1{ 0.0f, 0.0f, 0.0f };
        uint32_t PackedColor = 0;
    };

    inline constexpr uint32_t GpuDebugLineMaxEntries = 32768;
    inline constexpr uint32_t GpuDebugLineHeaderCount = 2;
    inline constexpr uint32_t GpuDebugLineHeaderSize = sizeof(uint32_t) * GpuDebugLineHeaderCount;
    inline constexpr uint32_t GpuDebugLineEntryStride = sizeof(FGpuDebugLineEntry);
    inline constexpr uint64_t GpuDebugLineBufferSize = GpuDebugLineHeaderSize + static_cast<uint64_t>(GpuDebugLineMaxEntries) * GpuDebugLineEntryStride;

    struct FGpuDebugBoxEntry
    {
        DirectX::XMFLOAT3 Center{ 0.0f, 0.0f, 0.0f };
        float HalfExtentX = 0.0f;
        DirectX::XMFLOAT3 AxisX{ 1.0f, 0.0f, 0.0f };
        float HalfExtentY = 0.0f;
        DirectX::XMFLOAT3 AxisY{ 0.0f, 1.0f, 0.0f };
        float HalfExtentZ = 0.0f;
        DirectX::XMFLOAT3 AxisZ{ 0.0f, 0.0f, 1.0f };
        uint32_t PackedColor = 0;
    };

    inline constexpr uint32_t GpuDebugBoxMaxEntries = 16384;
    inline constexpr uint32_t GpuDebugBoxHeaderCount = 2;
    inline constexpr uint32_t GpuDebugBoxHeaderSize = sizeof(uint32_t) * GpuDebugBoxHeaderCount;
    inline constexpr uint32_t GpuDebugBoxEntryStride = sizeof(FGpuDebugBoxEntry);
    inline constexpr uint64_t GpuDebugBoxBufferSize = GpuDebugBoxHeaderSize + static_cast<uint64_t>(GpuDebugBoxMaxEntries) * GpuDebugBoxEntryStride;
}

class FGpuDebug
{
public:
    ~FGpuDebug();

    void SetPrintEnabled(bool bEnabled) { bPrintEnabled = bEnabled; }
    bool IsPrintEnabled() const { return bPrintEnabled; }

    void SetCpuDebugLines(const std::vector<GpuDebug::FGpuDebugLineEntry>& Lines);
    bool HasCpuDebugLines() const { return !CpuDebugLines.empty(); }
    void SetCpuDebugBoxes(const std::vector<GpuDebug::FGpuDebugBoxEntry>& Boxes);
    bool HasCpuDebugBoxes() const { return !CpuDebugBoxes.empty(); }

    bool IsGpuDrivenCullingReady() const { return !bPrintEnabled || bGpuDrivenCullingPersistentInputsValid; }
    bool IsPrintPassReady() const { return bPrintEnabled && bPrintPersistentInputsValid; }
    bool IsBoxPassReady() const { return bBoxPersistentInputsValid && HasCpuDebugBoxes(); }
    bool IsLinePassReady() const { return bLinePersistentInputsValid && (bPrintEnabled || HasCpuDebugLines()); }

    uint32_t GetPrintBufferUavBindlessIndex() const { return PrintBuffer.UavBindlessIndex; }
    uint32_t GetPrintStatsUavBindlessIndex() const { return PrintStatsBuffer.UavBindlessIndex; }
    uint32_t GetLineBufferUavBindlessIndex() const { return LineBuffer.UavBindlessIndex; }

    void RefreshPersistentValidation();

    bool CreateBufferResources(FDX12Device* Device);
    void AddUploadPreCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const;
    void UploadInitialData(ID3D12GraphicsCommandList* CommandList) const;
    void AddUploadPostCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const;
    void SetUploadCompletedStates();

    bool CreateResources(FDX12Device* Device);
    bool CreateLinePipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, DXGI_FORMAT SceneDepthFormat);
    bool CreateBoxPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, DXGI_FORMAT SceneDepthFormat);
    bool CreatePrintPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat);
    bool CreatePrintStatsPipeline(FDX12Device* Device);

    void PreparePrint(FDX12CommandContext& CmdContext);
    void PrepareLine(FDX12CommandContext& CmdContext);
    void PrepareBox(FDX12CommandContext& CmdContext);
    void AddPass(FDeferredPassContext& Context);
    void DispatchPrintStats(FDX12Device* Device, FDX12CommandContext& CmdContext);
    void RenderPrint(FDX12Device* Device, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& ScissorRect, FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle);
    void RenderLine(FDX12Device* Device, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& ScissorRect, D3D12_GPU_VIRTUAL_ADDRESS SceneConstantBufferAddress, const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle, FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle);
    void RenderBox(FDX12Device* Device, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& ScissorRect, D3D12_GPU_VIRTUAL_ADDRESS SceneConstantBufferAddress, const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle, FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle);

private:
    bool CreateLineResources(FDX12Device* Device);
    bool CreateBoxResources(FDX12Device* Device);
    void ResetUploadMappings();

    bool bPrintEnabled = false;
    bool bGpuDrivenCullingPersistentInputsValid = false;
    bool bPrintPersistentInputsValid = false;
    bool bPrintStatsPersistentInputsValid = false;
    bool bLinePersistentInputsValid = false;
    bool bBoxPersistentInputsValid = false;

    FBindlessBuffer PrintBuffer;
    FUploadBuffer   PrintUpload;
    FBindlessBuffer PrintStatsBuffer;
    FUploadBuffer   PrintStatsUpload;
    FBindlessBuffer LineBuffer;
    FUploadBuffer   LineUpload;
    FBindlessBuffer BoxBuffer;
    FUploadBuffer   BoxUpload;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> PrintRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PrintPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> PrintStatsRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> PrintStatsPipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> LineRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> LinePipeline;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> BoxRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> BoxPipeline;
    Microsoft::WRL::ComPtr<ID3D12Resource> PrintFontTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> PrintGlyphBuffer;

    uint32_t PrintGlyphBindlessIndex = UINT32_MAX;
    uint32_t PrintFontBindlessIndex = UINT32_MAX;

    uint32_t PrintAtlasWidth = 0;
    uint32_t PrintAtlasHeight = 0;
    uint32_t PrintFirstChar = 32;
    uint32_t PrintCharCount = 96;
    float PrintFontSize = 16.0f;

    std::vector<GpuDebug::FGpuDebugLineEntry> CpuDebugLines;
    std::vector<GpuDebug::FGpuDebugBoxEntry> CpuDebugBoxes;
    uint8_t* LineUploadMapped = nullptr;
    uint8_t* BoxUploadMapped = nullptr;
};
