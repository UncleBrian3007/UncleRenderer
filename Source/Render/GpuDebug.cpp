#include "Renderer.h"

#include "Deferred/DeferredPassContext.h"
#include "DeferredRenderer.h"
#include "DebugPrintFont.h"
#include "ShaderCompiler.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include <d3dx12.h>

#include <algorithm>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

FGpuDebug::~FGpuDebug()
{
    ResetUploadMappings();
}

void FGpuDebug::ResetUploadMappings()
{
    if (LineUpload && LineUploadMapped)
    {
        LineUpload.Resource->Unmap(0, nullptr);
        LineUploadMapped = nullptr;
    }

    if (BoxUpload && BoxUploadMapped)
    {
        BoxUpload.Resource->Unmap(0, nullptr);
        BoxUploadMapped = nullptr;
    }
}

void FGpuDebug::SetCpuDebugLines(const std::vector<GpuDebug::FGpuDebugLineEntry>& Lines)
{
    CpuDebugLines = Lines;
    if (CpuDebugLines.size() > GpuDebug::GpuDebugLineMaxEntries)
    {
        CpuDebugLines.resize(GpuDebug::GpuDebugLineMaxEntries);
    }
}

void FGpuDebug::SetCpuDebugBoxes(const std::vector<GpuDebug::FGpuDebugBoxEntry>& Boxes)
{
    CpuDebugBoxes = Boxes;
    if (CpuDebugBoxes.size() > GpuDebug::GpuDebugBoxMaxEntries)
    {
        CpuDebugBoxes.resize(GpuDebug::GpuDebugBoxMaxEntries);
    }
}

void FGpuDebug::RefreshPersistentValidation()
{
    bGpuDrivenCullingPersistentInputsValid =
        PrintBuffer.HasUav() &&
        PrintStatsBuffer.HasUav() &&
        LineBuffer.HasUav();

    bPrintPersistentInputsValid =
        PrintPipeline &&
        PrintRootSignature &&
        PrintGlyphBuffer &&
        PrintFontTexture &&
        PrintBuffer.HasSrv() &&
        AreAllBindlessIndicesValid(PrintGlyphBindlessIndex, PrintFontBindlessIndex);

    bPrintStatsPersistentInputsValid =
        PrintStatsPipeline &&
        PrintStatsRootSignature &&
        PrintStatsBuffer.HasSrv() &&
        PrintBuffer.HasUav() &&
        LineBuffer.HasSrv();

    bLinePersistentInputsValid =
        LinePipeline &&
        LineRootSignature &&
        LineBuffer.HasSrv();

    bBoxPersistentInputsValid =
        BoxPipeline &&
        BoxRootSignature &&
        BoxBuffer.HasSrv();
}

bool FGpuDebug::CreateBufferResources(FDX12Device* Device)
{
    ResetUploadMappings();

    PrintBuffer = {};
    PrintStatsBuffer = {};
    LineBuffer = {};
    BoxBuffer = {};
    PrintUpload = {};
    PrintStatsUpload = {};
    LineUpload = {};
    BoxUpload = {};

    bGpuDrivenCullingPersistentInputsValid = false;
    bPrintPersistentInputsValid = false;
    bPrintStatsPersistentInputsValid = false;
    bLinePersistentInputsValid = false;
    bBoxPersistentInputsValid = false;

    const uint32_t ZeroPrint = 0;
    CreateBindlessBuffer(Device, L"GpuDebugPrintBuffer", CreateRawBufferDesc(GpuDebug::GpuDebugPrintBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON, PrintBuffer, false, false);
    CreateUploadBuffer(Device, L"GpuDebugPrintUpload", sizeof(uint32_t), PrintUpload, &ZeroPrint);

    const auto ZeroStats = std::vector<uint32_t>(GpuDebug::GpuDebugPrintStatsCount, 0u);
    CreateBindlessBuffer(Device, L"GpuDebugPrintStatsBuffer", CreateRawBufferDesc(sizeof(uint32_t) * GpuDebug::GpuDebugPrintStatsCount, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON, PrintStatsBuffer, false, false);
    CreateUploadBuffer(Device, L"GpuDebugPrintStatsUpload", sizeof(uint32_t) * GpuDebug::GpuDebugPrintStatsCount, PrintStatsUpload, ZeroStats.data());

    CreateBindlessBuffer(Device, L"GpuDebugLineBuffer", CreateRawBufferDesc(GpuDebug::GpuDebugLineBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON, LineBuffer, false, false);
    CreateUploadBuffer(Device, L"GpuDebugLineUpload", GpuDebug::GpuDebugLineBufferSize, LineUpload, nullptr);
    if (LineUpload)
    {
        const D3D12_RANGE EmptyRange = { 0, 0 };
        HR_CHECK(LineUpload.Resource->Map(0, &EmptyRange, reinterpret_cast<void**>(&LineUploadMapped)));
        if (LineUploadMapped)
        {
            std::memset(LineUploadMapped, 0, static_cast<size_t>(GpuDebug::GpuDebugLineBufferSize));
        }
    }

    CreateBindlessBuffer(Device, L"GpuDebugBoxBuffer", CreateRawBufferDesc(GpuDebug::GpuDebugBoxBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_COMMON, BoxBuffer, false, false);
    CreateUploadBuffer(Device, L"GpuDebugBoxUpload", GpuDebug::GpuDebugBoxBufferSize, BoxUpload, nullptr);
    if (BoxUpload)
    {
        const D3D12_RANGE EmptyRange = { 0, 0 };
        HR_CHECK(BoxUpload.Resource->Map(0, &EmptyRange, reinterpret_cast<void**>(&BoxUploadMapped)));
        if (BoxUploadMapped)
        {
            std::memset(BoxUploadMapped, 0, static_cast<size_t>(GpuDebug::GpuDebugBoxBufferSize));
        }
    }

    return PrintBuffer && PrintUpload && PrintStatsBuffer && PrintStatsUpload && LineBuffer && LineUpload && BoxBuffer && BoxUpload;
}

void FGpuDebug::AddUploadPreCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const
{
    auto AddBarrier = [&Barriers](ID3D12Resource* Resource)
    {
        if (!Resource)
        {
            return;
        }

        Barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
    };

    AddBarrier(PrintBuffer.Get());
    AddBarrier(PrintStatsBuffer.Get());
    AddBarrier(LineBuffer.Get());
    AddBarrier(BoxBuffer.Get());
}

void FGpuDebug::UploadInitialData(ID3D12GraphicsCommandList* CommandList) const
{
    if (PrintBuffer && PrintUpload)
    {
        CommandList->CopyBufferRegion(PrintBuffer.Get(), 0, PrintUpload.Get(), 0, sizeof(uint32_t));
    }
    if (PrintStatsBuffer && PrintStatsUpload)
    {
        CommandList->CopyBufferRegion(PrintStatsBuffer.Get(), 0, PrintStatsUpload.Get(), 0, sizeof(uint32_t) * GpuDebug::GpuDebugPrintStatsCount);
    }
    if (LineBuffer && LineUpload)
    {
        CommandList->CopyBufferRegion(LineBuffer.Get(), 0, LineUpload.Get(), 0, GpuDebug::GpuDebugLineHeaderSize);
    }
    if (BoxBuffer && BoxUpload)
    {
        CommandList->CopyBufferRegion(BoxBuffer.Get(), 0, BoxUpload.Get(), 0, GpuDebug::GpuDebugBoxHeaderSize);
    }
}

void FGpuDebug::AddUploadPostCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const
{
    auto AddBarrier = [&Barriers](ID3D12Resource* Resource)
    {
        if (!Resource)
        {
            return;
        }

        Barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    };

    AddBarrier(PrintBuffer.Get());
    AddBarrier(PrintStatsBuffer.Get());
    AddBarrier(LineBuffer.Get());
    AddBarrier(BoxBuffer.Get());
}

void FGpuDebug::SetUploadCompletedStates()
{
    PrintBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PrintStatsBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    LineBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    BoxBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void FGpuDebug::PreparePrint(FDX12CommandContext& CmdContext)
{
    if (!bPrintEnabled || !PrintBuffer || !PrintUpload || !PrintStatsBuffer || !PrintStatsUpload)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
	FScopedPixEvent DebugPrintEvent(CommandList, L"PrepareGpuDebugPrint");

    if (PrintBuffer.State != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintBuffer.Get(), PrintBuffer.State, D3D12_RESOURCE_STATE_COPY_DEST);
        CommandList->ResourceBarrier(1, &Barrier);
        PrintBuffer.State = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    CommandList->CopyBufferRegion(PrintBuffer.Get(), 0, PrintUpload.Get(), 0, sizeof(uint32_t));

    const auto PrintBarrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintBuffer.Get(), PrintBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CommandList->ResourceBarrier(1, &PrintBarrier);
    PrintBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (PrintStatsBuffer.State != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        const auto StatsBarrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintStatsBuffer.Get(), PrintStatsBuffer.State, D3D12_RESOURCE_STATE_COPY_DEST);
        CommandList->ResourceBarrier(1, &StatsBarrier);
        PrintStatsBuffer.State = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    CommandList->CopyBufferRegion(PrintStatsBuffer.Get(), 0, PrintStatsUpload.Get(), 0, sizeof(uint32_t) * GpuDebug::GpuDebugPrintStatsCount);

    const auto StatsBarrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintStatsBuffer.Get(), PrintStatsBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CommandList->ResourceBarrier(1, &StatsBarrier);
    PrintStatsBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void FGpuDebug::PrepareLine(FDX12CommandContext& CmdContext)
{
    if (!LineBuffer || !LineUpload || !LineUploadMapped)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugLineEvent(CommandList, L"PrepareGpuDebugLine");

    const uint32_t LineCount = static_cast<uint32_t>((std::min)(CpuDebugLines.size(), static_cast<size_t>(GpuDebug::GpuDebugLineMaxEntries)));
    const uint32_t DroppedCount = static_cast<uint32_t>(CpuDebugLines.size() > GpuDebug::GpuDebugLineMaxEntries ? CpuDebugLines.size() - GpuDebug::GpuDebugLineMaxEntries : 0ull);
    std::memcpy(LineUploadMapped + 0, &LineCount, sizeof(uint32_t));
    std::memcpy(LineUploadMapped + sizeof(uint32_t), &DroppedCount, sizeof(uint32_t));
    if (LineCount > 0u)
    {
        std::memcpy(
            LineUploadMapped + GpuDebug::GpuDebugLineHeaderSize,
            CpuDebugLines.data(),
            static_cast<size_t>(LineCount) * sizeof(GpuDebug::FGpuDebugLineEntry));
    }

    if (LineBuffer.State != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(LineBuffer.Get(), LineBuffer.State, D3D12_RESOURCE_STATE_COPY_DEST);
        CommandList->ResourceBarrier(1, &Barrier);
        LineBuffer.State = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    const uint64_t CopySize = GpuDebug::GpuDebugLineHeaderSize + static_cast<uint64_t>(LineCount) * sizeof(GpuDebug::FGpuDebugLineEntry);
    CommandList->CopyBufferRegion(LineBuffer.Get(), 0, LineUpload.Get(), 0, CopySize);

    const auto LineBarrier = CD3DX12_RESOURCE_BARRIER::Transition(LineBuffer.Get(), LineBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CommandList->ResourceBarrier(1, &LineBarrier);
    LineBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void FGpuDebug::PrepareBox(FDX12CommandContext& CmdContext)
{
    if (!BoxBuffer || !BoxUpload || !BoxUploadMapped)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugBoxEvent(CommandList, L"PrepareGpuDebugBox");

    const uint32_t BoxCount = static_cast<uint32_t>((std::min)(CpuDebugBoxes.size(), static_cast<size_t>(GpuDebug::GpuDebugBoxMaxEntries)));
    const uint32_t DroppedCount = static_cast<uint32_t>(CpuDebugBoxes.size() > GpuDebug::GpuDebugBoxMaxEntries ? CpuDebugBoxes.size() - GpuDebug::GpuDebugBoxMaxEntries : 0ull);
    std::memcpy(BoxUploadMapped + 0, &BoxCount, sizeof(uint32_t));
    std::memcpy(BoxUploadMapped + sizeof(uint32_t), &DroppedCount, sizeof(uint32_t));
    if (BoxCount > 0u)
    {
        std::memcpy(
            BoxUploadMapped + GpuDebug::GpuDebugBoxHeaderSize,
            CpuDebugBoxes.data(),
            static_cast<size_t>(BoxCount) * sizeof(GpuDebug::FGpuDebugBoxEntry));
    }

    if (BoxBuffer.State != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(BoxBuffer.Get(), BoxBuffer.State, D3D12_RESOURCE_STATE_COPY_DEST);
        CommandList->ResourceBarrier(1, &Barrier);
        BoxBuffer.State = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    const uint64_t CopySize = GpuDebug::GpuDebugBoxHeaderSize + static_cast<uint64_t>(BoxCount) * sizeof(GpuDebug::FGpuDebugBoxEntry);
    CommandList->CopyBufferRegion(BoxBuffer.Get(), 0, BoxUpload.Get(), 0, CopySize);

    const auto BoxBarrier = CD3DX12_RESOURCE_BARRIER::Transition(BoxBuffer.Get(), BoxBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CommandList->ResourceBarrier(1, &BoxBarrier);
    BoxBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void FGpuDebug::AddPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FDebugPrintPassData
    {
        bool bPrintEnabled = false;
        bool bBoxEnabled = false;
        bool bLineEnabled = false;
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
    };

    Context.Graph.AddPass<FDebugPrintPassData>("GpuDebugPrint", [&Owner, &Context](FDebugPrintPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bPrintEnabled = Owner.GpuDebugState.IsPrintPassReady();
        Data.bBoxEnabled = Owner.GpuDebugState.IsBoxPassReady();
        Data.bLineEnabled = Owner.GpuDebugState.IsLinePassReady();
        Data.OutputHandle = Context.RtvHandle;
        if (Data.bPrintEnabled || Data.bBoxEnabled || Data.bLineEnabled)
        {
            Builder.KeepAlive();
        }
    }, [this, &Owner](const FDebugPrintPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bPrintEnabled && !Data.bBoxEnabled && !Data.bLineEnabled)
        {
            return;
        }

        if (Data.bPrintEnabled)
        {
            DispatchPrintStats(Owner.Device, Cmd);
            RenderPrint(Owner.Device, Owner.Viewport, Owner.ScissorRect, Cmd, Data.OutputHandle);
        }
        if (Data.bBoxEnabled)
        {
            RenderBox(Owner.Device, Owner.Viewport, Owner.ScissorRect, Owner.GetSceneConstantBufferAddress(), Owner.GetDSVHandle(), Cmd, Data.OutputHandle);
        }
        if (Data.bLineEnabled)
        {
            RenderLine(Owner.Device, Owner.Viewport, Owner.ScissorRect, Owner.GetSceneConstantBufferAddress(), Owner.GetDSVHandle(), Cmd, Data.OutputHandle);
        }
    });
}

bool FGpuDebug::CreateLineResources(FDX12Device* Device)
{
    if (!LineBuffer)
    {
        LogError("GPU debug line buffer is missing.");
        return false;
    }

    CreateBindlessBufferViews(Device, LineBuffer, true, true);
    return true;
}

bool FGpuDebug::CreateBoxResources(FDX12Device* Device)
{
    if (!BoxBuffer)
    {
        LogError("GPU debug box buffer is missing.");
        return false;
    }

    CreateBindlessBufferViews(Device, BoxBuffer, true, true);
    return true;
}

bool FGpuDebug::CreateLinePipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, DXGI_FORMAT SceneDepthFormat)
{
    bLinePersistentInputsValid = false;

    CD3DX12_ROOT_PARAMETER1 Params[2] = {};
    Params[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX);
    Params[1].InitAsConstants(1, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc;
    VersionedRootDesc.Init_1_1(_countof(Params), Params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(LineRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/GpuDebug/GpuDebugLine.hlsl", VSByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/GpuDebug/GpuDebugLine.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LineRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.DSVFormat = SceneDepthFormat;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(LinePipeline.GetAddressOf())));
    RefreshPersistentValidation();
    return true;
}

bool FGpuDebug::CreateBoxPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat, DXGI_FORMAT SceneDepthFormat)
{
    bBoxPersistentInputsValid = false;

    CD3DX12_ROOT_PARAMETER1 Params[2] = {};
    Params[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX);
    Params[1].InitAsConstants(1, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc;
    VersionedRootDesc.Init_1_1(_countof(Params), Params, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(BoxRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/GpuDebug/GpuDebugBox.hlsl", VSByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/GpuDebug/GpuDebugBox.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = BoxRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.DSVFormat = SceneDepthFormat;

    PsoDesc.InputLayout = { nullptr, 0 };

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BoxPipeline.GetAddressOf())));
    RefreshPersistentValidation();
    return true;
}

bool FGpuDebug::CreateResources(FDX12Device* Device)
{
    bGpuDrivenCullingPersistentInputsValid = false;
    bPrintPersistentInputsValid = false;
    bPrintStatsPersistentInputsValid = false;
    bLinePersistentInputsValid = false;
    bBoxPersistentInputsValid = false;

    if (!PrintBuffer || !PrintStatsBuffer)
    {
        LogError("GPU debug print buffer is missing.");
        return false;
    }

    const std::wstring FontPath = L"Assets/Fonts/Roboto-Medium.ttf";
    const float FontSize = 16.0f;
    const uint32_t AtlasWidth = 512;
    const uint32_t AtlasHeight = 512;

    FDebugPrintFontResources FontResources;
    if (!CreateDebugPrintFontResources(Device, FontPath, FontSize, AtlasWidth, AtlasHeight, FontResources))
    {
        LogError("Failed to create GPU debug print font resources.");
        return false;
    }

    PrintFontTexture = FontResources.FontTexture;
    PrintGlyphBuffer = FontResources.GlyphBuffer;
    PrintAtlasWidth = FontResources.AtlasWidth;
    PrintAtlasHeight = FontResources.AtlasHeight;
    PrintFirstChar = FontResources.FirstChar;
    PrintCharCount = FontResources.CharCount;
    PrintFontSize = FontResources.FontSize;

    PrintGlyphBindlessIndex = Device->CreateBindlessSrv(PrintGlyphBuffer.Get(),
        CD3DX12_SHADER_RESOURCE_VIEW_DESC::StructuredBuffer(128, sizeof(FDebugPrintGlyph)));
    PrintFontBindlessIndex = Device->CreateBindlessSrv(PrintFontTexture.Get(),
        CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R8_UNORM, 1));

    CreateBindlessBufferViews(Device, PrintBuffer, true, true);
    CreateBindlessBufferViews(Device, PrintStatsBuffer, true, true);

    if (!CreateLineResources(Device))
    {
        return false;
    }

    if (!CreateBoxResources(Device))
    {
        return false;
    }

    RefreshPersistentValidation();
    return true;
}

bool FGpuDebug::CreatePrintPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    bPrintPersistentInputsValid = false;

    CD3DX12_ROOT_PARAMETER1 Params[2] = {};
    Params[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    Params[1].InitAsConstants(3, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC Sampler;
    Sampler.Init(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 0,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc;
    VersionedRootDesc.Init_1_1(
        _countof(Params), Params,
        1, &Sampler,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(PrintRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/GpuDebug/GpuDebugPrint.hlsl", VSByteCode))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/GpuDebug/GpuDebugPrint.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = PrintRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(PrintPipeline.GetAddressOf())));
    RefreshPersistentValidation();
    return true;
}

bool FGpuDebug::CreatePrintStatsPipeline(FDX12Device* Device)
{
    bPrintStatsPersistentInputsValid = false;

    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(3, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc;
    VersionedRootDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(PrintStatsRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;

    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDebug/GpuDebugPrintStats.hlsl", CsByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC CsDesc = {};
    CsDesc.pRootSignature = PrintStatsRootSignature.Get();
    CsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&CsDesc, IID_PPV_ARGS(PrintStatsPipeline.GetAddressOf())));
    RefreshPersistentValidation();
    return true;
}

void FGpuDebug::DispatchPrintStats(FDX12Device* Device, FDX12CommandContext& CmdContext)
{
    if (!bPrintEnabled || !bPrintStatsPersistentInputsValid)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugStatsEvent(CommandList, L"GpuDebugPrintStats");

    if (PrintStatsBuffer.State != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintStatsBuffer.Get(), PrintStatsBuffer.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CommandList->ResourceBarrier(1, &Barrier);
        PrintStatsBuffer.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    if (PrintBuffer.State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintBuffer.Get(), PrintBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CommandList->ResourceBarrier(1, &Barrier);
        PrintBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    if (LineBuffer.State != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(LineBuffer.Get(), LineBuffer.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CommandList->ResourceBarrier(1, &Barrier);
        LineBuffer.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(PrintStatsPipeline.Get());
    CommandList->SetComputeRootSignature(PrintStatsRootSignature.Get());
    const uint32_t BindlessIndices[] = { PrintStatsBuffer.SrvBindlessIndex, PrintBuffer.UavBindlessIndex, LineBuffer.SrvBindlessIndex };
    CommandList->SetComputeRoot32BitConstants(0, _countof(BindlessIndices), BindlessIndices, 0);
    CommandList->Dispatch(1, 1, 1);
}

void FGpuDebug::RenderPrint(FDX12Device* Device, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& ScissorRect, FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle)
{
    if (!bPrintEnabled || !bPrintPersistentInputsValid)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugEvent(CommandList, L"GpuDebugPrint");

    if (PrintBuffer.State != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintBuffer.Get(), PrintBuffer.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CommandList->ResourceBarrier(1, &Barrier);
        PrintBuffer.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    CmdContext.SetRenderTarget(OutputHandle, nullptr);

    struct FDebugPrintConstants
    {
        DirectX::XMFLOAT2 ScreenSize;
        uint32_t FirstChar;
        uint32_t CharCount;
    };

    struct FDebugPrintBindlessConstants
    {
        uint32_t GlyphBufferIndex;
        uint32_t FontAtlasIndex;
        uint32_t DebugPrintBufferIndex;
    };

    const FDebugPrintConstants Constants =
    {
        DirectX::XMFLOAT2(Viewport.Width, Viewport.Height),
        PrintFirstChar,
        PrintCharCount
    };

    const FDebugPrintBindlessConstants BindlessConstants =
    {
        PrintGlyphBindlessIndex,
        PrintFontBindlessIndex,
        PrintBuffer.SrvBindlessIndex
    };

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(PrintPipeline.Get());
    CommandList->SetGraphicsRootSignature(PrintRootSignature.Get());
    CommandList->RSSetViewports(1, &Viewport);
    CommandList->RSSetScissorRects(1, &ScissorRect);
    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
    CommandList->SetGraphicsRoot32BitConstants(1, sizeof(BindlessConstants) / sizeof(uint32_t), &BindlessConstants, 0);
    CommandList->DrawInstanced(6 * GpuDebug::GpuDebugPrintMaxEntries, 1, 0, 0);

    if (PrintBuffer.State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(PrintBuffer.Get(), PrintBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CommandList->ResourceBarrier(1, &Barrier);
        PrintBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
}

void FGpuDebug::RenderLine(FDX12Device* Device, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& ScissorRect, D3D12_GPU_VIRTUAL_ADDRESS SceneConstantBufferAddress, const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle, FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle)
{
    if (!bLinePersistentInputsValid)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugLineEvent(CommandList, L"GpuDebugLine");

    if (LineBuffer.State != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(LineBuffer.Get(), LineBuffer.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CommandList->ResourceBarrier(1, &Barrier);
        LineBuffer.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    CmdContext.SetRenderTarget(OutputHandle, &DepthHandle);

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(LinePipeline.Get());
    CommandList->SetGraphicsRootSignature(LineRootSignature.Get());
    CommandList->RSSetViewports(1, &Viewport);
    CommandList->RSSetScissorRects(1, &ScissorRect);
    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    CommandList->SetGraphicsRootConstantBufferView(0, SceneConstantBufferAddress);
    CommandList->SetGraphicsRoot32BitConstant(1, LineBuffer.SrvBindlessIndex, 0);
    CommandList->DrawInstanced(2 * GpuDebug::GpuDebugLineMaxEntries, 1, 0, 0);

    if (LineBuffer.State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(LineBuffer.Get(), LineBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CommandList->ResourceBarrier(1, &Barrier);
        LineBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
}

void FGpuDebug::RenderBox(FDX12Device* Device, const D3D12_VIEWPORT& Viewport, const D3D12_RECT& ScissorRect, D3D12_GPU_VIRTUAL_ADDRESS SceneConstantBufferAddress, const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle, FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle)
{
    if (!bBoxPersistentInputsValid)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugBoxEvent(CommandList, L"GpuDebugBox");

    if (BoxBuffer.State != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(BoxBuffer.Get(), BoxBuffer.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CommandList->ResourceBarrier(1, &Barrier);
        BoxBuffer.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    CmdContext.SetRenderTarget(OutputHandle, &DepthHandle);

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(BoxPipeline.Get());
    CommandList->SetGraphicsRootSignature(BoxRootSignature.Get());
    CommandList->RSSetViewports(1, &Viewport);
    CommandList->RSSetScissorRects(1, &ScissorRect);
    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->SetGraphicsRootConstantBufferView(0, SceneConstantBufferAddress);
    CommandList->SetGraphicsRoot32BitConstant(1, BoxBuffer.SrvBindlessIndex, 0);
    CommandList->DrawInstanced(36 * GpuDebug::GpuDebugBoxMaxEntries, 1, 0, 0);

    if (BoxBuffer.State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(BoxBuffer.Get(), BoxBuffer.State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CommandList->ResourceBarrier(1, &Barrier);
        BoxBuffer.State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
}

