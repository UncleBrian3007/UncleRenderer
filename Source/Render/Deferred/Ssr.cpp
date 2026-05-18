#include "Ssr.h"
#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"
#include "Taa.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
#include <d3dx12.h>
namespace
{
    constexpr uint32_t SsrRayItemStride = 48u;
    constexpr uint32_t SsrPipelineRefineBit = 1u;
    constexpr uint32_t SsrPipelineHzbBit = 2u;
    constexpr uint32_t SsrPipelineHwFallbackBit = 4u;
    constexpr uint32_t SsrPipelineHzbFullResDepthBit = 8u;

    constexpr uint32_t kSsrTraceConstantsDwordCount              = 15;
    constexpr uint32_t kSsrTraceBindlessDwordCount               = 9;
    constexpr uint32_t kSsrDenoiseConstantsDwordCount            = 4;
    constexpr uint32_t kSsrDenoiseBindlessDwordCount             = 5;
    constexpr uint32_t kSsrRayGatherConstantsDwordCount          = 11;
    constexpr uint32_t kSsrRayGatherBindlessDwordCount           = 6;
    constexpr uint32_t kSsrSwTraceConstantsDwordCount            = 13;
    constexpr uint32_t kSsrSwTraceBindlessDwordCount             = 10;
    constexpr uint32_t kSsrBuildIndirectArgsConstantsDwordCount  = 2;
    constexpr uint32_t kSsrBuildIndirectArgsBindlessDwordCount   = 2;
    constexpr uint32_t kSsrResolveConstantsDwordCount            = 5;
    constexpr uint32_t kSsrResolveBindlessDwordCount             = 6;

    uint32_t BuildSsrPipelineIndex(bool bUseHzb, bool bUseRefine, bool bUseSwSsr, bool bUseHzbFullResDepth)
    {
        uint32_t PipelineIndex = 0u;
        if (bUseRefine)
        {
            PipelineIndex |= SsrPipelineRefineBit;
        }
        if (bUseHzb)
        {
            PipelineIndex |= SsrPipelineHzbBit;
        }
        if (!bUseSwSsr)
        {
            PipelineIndex |= SsrPipelineHwFallbackBit;
        }
        if (bUseHzb && bUseHzbFullResDepth)
        {
            PipelineIndex |= SsrPipelineHzbFullResDepthBit;
        }

        return PipelineIndex;
    }
}

bool FSsr::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    bPersistentInputsValid = false;

    return CreateSsrRootSignature(Device)
        && CreateSsrPipeline(Device)
        && CreateSsrDenoiseRootSignature(Device)
        && CreateSsrDenoisePipeline(Device)
        && CreateSsrRayGatherRootSignature(Device)
        && CreateSsrRayGatherPipeline(Device)
        && CreateSsrSwTraceRootSignature(Device)
        && CreateSsrSwTracePipeline(Device)
        && CreateSsrBuildIndirectArgsRootSignature(Device)
        && CreateSsrBuildIndirectArgsPipeline(Device)
        && CreateSsrResolveRootSignature(Device)
        && CreateSsrResolvePipeline(Device)
        && CreateSsrDispatchCommandSignature(Device);
}

bool FSsr::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    bPersistentInputsValid = false;

    return CreateSsrResources(Owner, Device, Width, Height);
}

void FSsr::ImportPersistentResources(FDeferredPassContext& Context)
{
    FRenderGraph& Graph = Context.Graph;
    FSsrFrameResources& OutResources = Context.Resources.Ssr;

    OutResources.SsrHandle = ImportBindlessTexture(Graph, "SSR", SsrTexture);
    OutResources.SsrDenoiseHandle = ImportBindlessTexture(Graph, "SSR Denoise", SsrDenoiseTexture);
    OutResources.SsrFallbackHandle = ImportBindlessTexture(Graph, "SSR Fallback", SsrFallbackTexture);
    OutResources.SsrResolveHandle = ImportBindlessTexture(Graph, "SSR Resolve", SsrResolveTexture);
}


void FSsr::RefreshPersistentInputValidation()
{
    bPersistentInputsValid =
        SsrTexture.IsFullyBound() &&
        SsrResolveTexture.IsFullyBound() &&
        SsrDenoiseTexture.HasSrv() &&
        SsrFallbackTexture.IsFullyBound() &&
        SsrRtvHeap &&
        SsrRootSignature &&
        SsrRayGatherPipeline && SsrRayGatherRootSignature &&
        SsrBuildIndirectArgsPipeline && SsrBuildIndirectArgsRootSignature &&
        SsrSwTraceRootSignature && SsrDispatchCommandSignature &&
        SsrResolvePipeline && SsrResolveRootSignature &&
        SsrDenoiseRootSignature && SsrDenoisePipeline;
}

void FSsr::AddPasses(FDeferredPassContext& Context)
{
    if (SsrMode == ESSRMode::CS)
    {
        AddSsrRayCounterClearPass(Context);
        AddSsrRayGatherPass(Context);
        AddSsrBuildIndirectArgsPass(Context, false);
        AddSsrSwTracePass(Context);
        AddSsrBuildIndirectArgsPass(Context, true);
        AddSsrHwTracePass(Context);
        AddSsrResolvePass(Context);
    }
    else
    {
        AddSsrPass(Context);
        AddSsrFallbackPass(Context);
    }

    const FRGResourceHandle SsrBaseHandle = Context.Resources.Ssr.GetBaseHandle(SsrMode);
    if (bSsrDenoiseEnabled)
    {
        AddSsrDenoisePass(Context, SsrBaseHandle);
    }
}

uint32_t FSsr::GetBaseOutputSrvBindlessIndex() const
{
    if (SsrMode == ESSRMode::CS)
    {
        return bPersistentInputsValid ? SsrResolveTexture.SrvBindlessIndex : UINT32_MAX;
    }

    return bPersistentInputsValid ? SsrTexture.SrvBindlessIndex : UINT32_MAX;
}

uint32_t FSsr::GetLightingSrvBindlessIndex() const
{
    return (bSsrDenoiseEnabled && bPersistentInputsValid) ? SsrDenoiseTexture.SrvBindlessIndex : GetBaseOutputSrvBindlessIndex();
}


bool FSsr::CreateSsrRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    // RootParams[0]: Scene constants (b0), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_PIXEL);
    // RootParams[1]: SSR constants (b1), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[1].InitAsConstants(kSsrTraceConstantsDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    // RootParams[2]: SSR bindless indices (b2), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[2].InitAsConstants(kSsrTraceBindlessDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrRootSignature.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/Ssr/SsrSWTracePS.hlsl", SsrGraphicsVsBytecode))
    {
        return false;
    }

    for (size_t PipelineIndex = 0; PipelineIndex < SsrPipelines.size(); ++PipelineIndex)
    {
        SsrPipelines[PipelineIndex].Reset();
        SsrGraphicsPsBytecodes[PipelineIndex].clear();
        SsrGraphicsPsCompiled[PipelineIndex] = false;
        SsrGraphicsFailureLogged[PipelineIndex] = false;
    }

    return true;
}

bool FSsr::CompileSsrGraphicsPs(FDX12Device* Device, uint32_t PipelineIndex, std::vector<uint8_t>& OutPs)
{
    FShaderCompiler Compiler;

    const bool bUseHzb = (PipelineIndex & SsrPipelineHzbBit) != 0;
    const bool bUseRefine = (PipelineIndex & SsrPipelineRefineBit) != 0;
    const bool bUseSwSsr = (PipelineIndex & SsrPipelineHwFallbackBit) == 0;
    const bool bUseHzbFullResDepth = (PipelineIndex & SsrPipelineHzbFullResDepthBit) != 0;

    const std::vector<std::wstring> Defines =
    {
        bUseHzb ? L"HZB_ENABLED=1" : L"HZB_ENABLED=0",
        bUseRefine ? L"SSR_REFINE_ENABLED=1" : L"SSR_REFINE_ENABLED=0",
        bUseSwSsr ? L"SW_SSR_ENABLED=1" : L"SW_SSR_ENABLED=0",
        bUseHzbFullResDepth ? L"SSR_HZB_FULL_RES_DEPTH_ENABLED=1" : L"SSR_HZB_FULL_RES_DEPTH_ENABLED=0"
    };

    return RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/Ssr/SsrSWTracePS.hlsl", OutPs, Defines);
}

bool FSsr::BuildSsrGraphicsPsoDesc(uint32_t PipelineIndex, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const
{
    OutDesc = {};
    OutDesc.pRootSignature = SsrRootSignature.Get();
    OutDesc.VS = { SsrGraphicsVsBytecode.data(), SsrGraphicsVsBytecode.size() };
    OutDesc.PS = { SsrGraphicsPsBytecodes[PipelineIndex].data(), SsrGraphicsPsBytecodes[PipelineIndex].size() };
    OutDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    OutDesc.SampleDesc.Count = 1;
    OutDesc.SampleMask = UINT_MAX;

    OutDesc.RasterizerState = {};
    OutDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    OutDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    OutDesc.RasterizerState.FrontCounterClockwise = TRUE;
    OutDesc.RasterizerState.DepthClipEnable = TRUE;

    OutDesc.BlendState = {};
    OutDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    OutDesc.DepthStencilState = {};
    OutDesc.DepthStencilState.DepthEnable = FALSE;
    OutDesc.DepthStencilState.StencilEnable = FALSE;
    OutDesc.NumRenderTargets = 1;
    OutDesc.RTVFormats[0] = FDeferredRenderer::LightingBufferFormat;
    OutDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    return true;
}

bool FSsr::EnsureSsrGraphicsPipeline(FDX12Device* Device, uint32_t PipelineIndex)
{
    if (PipelineIndex >= SsrPipelines.size())
    {
        return false;
    }

    if (SsrPipelines[PipelineIndex])
    {
        return true;
    }

    std::lock_guard<std::mutex> Lock(SsrGraphicsPipelineMutex);
    if (SsrPipelines[PipelineIndex])
    {
        return true;
    }

    if (!SsrGraphicsPsCompiled[PipelineIndex])
    {
        if (!CompileSsrGraphicsPs(Device, PipelineIndex, SsrGraphicsPsBytecodes[PipelineIndex]))
        {
            return false;
        }
        SsrGraphicsPsCompiled[PipelineIndex] = true;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = {};
    if (!BuildSsrGraphicsPsoDesc(PipelineIndex, Desc))
    {
        return false;
    }

    HRESULT Hr = Device->GetDevice()->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(SsrPipelines[PipelineIndex].GetAddressOf()));
    if (FAILED(Hr))
    {
        return false;
    }

    LogInfo("SSR graphics pipeline created. index=" + std::to_string(PipelineIndex));
    return true;
}

bool FSsr::EnsureSsrGraphicsPipelineOrFail(FDX12Device* Device, uint32_t PipelineIndex, const char* PassContext)
{
    if (EnsureSsrGraphicsPipeline(Device, PipelineIndex))
    {
        return true;
    }

    if (PipelineIndex < SsrGraphicsFailureLogged.size() && !SsrGraphicsFailureLogged[PipelineIndex])
    {
        SsrGraphicsFailureLogged[PipelineIndex] = true;
        LogError(std::string("SSR graphics pipeline creation failed. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", index=" + std::to_string(PipelineIndex));
    }

    LogError(std::string("SSR graphics fatal failure. context=")
        + (PassContext ? PassContext : "Unknown")
        + ", index=" + std::to_string(PipelineIndex));
    return false;
}

bool FSsr::CreateSsrDenoiseRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(kSsrDenoiseConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    RootParams[1].InitAsConstants(kSsrDenoiseBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrDenoiseRootSignature.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrDenoisePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/Ssr/SsrDenoise.hlsl", VSByteCode))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/Ssr/SsrDenoise.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrDenoiseRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(SsrDenoisePipeline.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrRayGatherRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kSsrRayGatherConstantsDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[2].InitAsConstants(kSsrRayGatherBindlessDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrRayGatherRootSignature.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrRayGatherPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;

    std::vector<uint8_t> CSByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/Ssr/SsrRayGather.hlsl", CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrRayGatherRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SsrRayGatherPipeline.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrSwTraceRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kSsrSwTraceConstantsDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[2].InitAsConstants(kSsrSwTraceBindlessDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrSwTraceRootSignature.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrSwTracePipeline(FDX12Device* Device)
{
    (void)Device;

    for (size_t PipelineIndex = 0; PipelineIndex < SsrSwTracePipelines.size(); ++PipelineIndex)
    {
        SsrSwTracePipelines[PipelineIndex].Reset();
        SsrSwTraceCsBytecodes[PipelineIndex].clear();
        SsrSwTraceCsCompiled[PipelineIndex] = false;
        SsrSwTraceFailureLogged[PipelineIndex] = false;
    }

    return true;
}

bool FSsr::CompileSsrSwTraceCs(FDX12Device* Device, uint32_t PipelineIndex, std::vector<uint8_t>& OutCs)
{
    FShaderCompiler Compiler;

    const bool bUseHzb = (PipelineIndex & SsrPipelineHzbBit) != 0;
    const bool bUseRefine = (PipelineIndex & SsrPipelineRefineBit) != 0;
    const bool bUseSwSsr = (PipelineIndex & SsrPipelineHwFallbackBit) == 0;
    const bool bUseHzbFullResDepth = (PipelineIndex & SsrPipelineHzbFullResDepthBit) != 0;

    const std::vector<std::wstring> Defines =
    {
        bUseHzb ? L"HZB_ENABLED=1" : L"HZB_ENABLED=0",
        bUseRefine ? L"SSR_REFINE_ENABLED=1" : L"SSR_REFINE_ENABLED=0",
        bUseSwSsr ? L"SW_SSR_ENABLED=1" : L"SW_SSR_ENABLED=0",
        bUseHzbFullResDepth ? L"SSR_HZB_FULL_RES_DEPTH_ENABLED=1" : L"SSR_HZB_FULL_RES_DEPTH_ENABLED=0"
    };

    return RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/Ssr/SsrSWTraceCS.hlsl", OutCs, Defines);
}

bool FSsr::EnsureSsrSwTracePipeline(FDX12Device* Device, uint32_t PipelineIndex)
{
    if (PipelineIndex >= SsrSwTracePipelines.size())
    {
        return false;
    }

    if (SsrSwTracePipelines[PipelineIndex])
    {
        return true;
    }

    std::lock_guard<std::mutex> Lock(SsrSwTracePipelineMutex);
    if (SsrSwTracePipelines[PipelineIndex])
    {
        return true;
    }

    if (!SsrSwTraceCsCompiled[PipelineIndex])
    {
        if (!CompileSsrSwTraceCs(Device, PipelineIndex, SsrSwTraceCsBytecodes[PipelineIndex]))
        {
            return false;
        }
        SsrSwTraceCsCompiled[PipelineIndex] = true;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC Desc = {};
    Desc.pRootSignature = SsrSwTraceRootSignature.Get();
    Desc.CS = { SsrSwTraceCsBytecodes[PipelineIndex].data(), SsrSwTraceCsBytecodes[PipelineIndex].size() };
    HRESULT Hr = Device->GetDevice()->CreateComputePipelineState(&Desc, IID_PPV_ARGS(SsrSwTracePipelines[PipelineIndex].GetAddressOf()));
    if (FAILED(Hr))
    {
        return false;
    }

    LogInfo("SSR SW trace pipeline created. index=" + std::to_string(PipelineIndex));
    return true;
}

bool FSsr::EnsureSsrSwTracePipelineOrFail(FDX12Device* Device, uint32_t PipelineIndex, const char* PassContext)
{
    if (EnsureSsrSwTracePipeline(Device, PipelineIndex))
    {
        return true;
    }

    if (PipelineIndex < SsrSwTraceFailureLogged.size() && !SsrSwTraceFailureLogged[PipelineIndex])
    {
        SsrSwTraceFailureLogged[PipelineIndex] = true;
        LogError(std::string("SSR SW trace pipeline creation failed. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", index=" + std::to_string(PipelineIndex));
    }

    LogError(std::string("SSR SW trace fatal failure. context=")
        + (PassContext ? PassContext : "Unknown")
        + ", index=" + std::to_string(PipelineIndex));
    return false;
}

bool FSsr::CreateSsrBuildIndirectArgsRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(kSsrBuildIndirectArgsConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kSsrBuildIndirectArgsBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrBuildIndirectArgsRootSignature.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrBuildIndirectArgsPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;

    std::vector<uint8_t> CSByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/Ssr/SsrBuildIndirectArgs.hlsl", CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrBuildIndirectArgsRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SsrBuildIndirectArgsPipeline.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrResolveRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(kSsrResolveConstantsDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kSsrResolveBindlessDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SsrResolveRootSignature.GetAddressOf())));
    return true;
}

bool FSsr::CreateSsrResolvePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;

    std::vector<uint8_t> CSByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/Ssr/SsrResolve.hlsl", CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = SsrResolveRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SsrResolvePipeline.GetAddressOf())));
    return true;
}


bool FSsr::CreateSsrDispatchCommandSignature(FDX12Device* Device)
{
    D3D12_INDIRECT_ARGUMENT_DESC ArgumentDesc = {};
    ArgumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    CommandDesc.NumArgumentDescs = 1;
    CommandDesc.pArgumentDescs = &ArgumentDesc;

    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, nullptr, IID_PPV_ARGS(SsrDispatchCommandSignature.ReleaseAndGetAddressOf())));
    return true;
}


bool FSsr::CreateSsrResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    const D3D12_RESOURCE_FLAGS SharedFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const FRGTextureDesc SharedDesc =
    {
        Width,
        Height,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        1
    };

    const FLOAT Color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_CLEAR_VALUE ClearValue(SharedDesc.Format, Color);

    CreateBindlessTexture(
        Device,
        L"SSR",
        SharedDesc,
        SharedFlags,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        SsrTexture,
        true,
        true,
        &ClearValue);
    CreateBindlessTexture(
        Device,
        L"SSR_Denoise",
        SharedDesc,
        SharedFlags,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        SsrDenoiseTexture,
        true,
        false,
        &ClearValue);
    CreateBindlessTexture(
        Device,
        L"SSR_Resolve",
        SharedDesc,
        SharedFlags,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        SsrResolveTexture,
        true,
        true,
        &ClearValue);

    const D3D12_RESOURCE_FLAGS FallbackFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const FRGTextureDesc FallbackDesc =
    {
        Width,
        Height,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        1
    };
    CreateBindlessTexture(
        Device,
        L"SSR_Fallback",
        FallbackDesc,
        FallbackFlags,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        SsrFallbackTexture,
        true,
        true);

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 2;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(SsrRtvHeap.GetAddressOf())));
    SsrRtvHeap->SetName(L"SSR_RTVHeap");

    SsrRtvHandle = SsrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const uint32_t RtvDescriptorSize = Device->GetRtvDescriptorStride();
    SsrDenoiseRtvHandle = SsrRtvHandle;
    SsrDenoiseRtvHandle.ptr += RtvDescriptorSize;
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Device->GetDevice()->CreateRenderTargetView(SsrTexture.Get(), &RtvDesc, SsrRtvHandle);
    Device->GetDevice()->CreateRenderTargetView(SsrDenoiseTexture.Get(), &RtvDesc, SsrDenoiseRtvHandle);

    const uint32_t Frames = Owner.GetFramesInFlight();
    SsrMaxRayCount = Width * Height;
    const auto ResizeFrameBuffers = [Frames](std::vector<FBindlessBuffer>& Buffers)
    {
        Buffers.clear();
        Buffers.resize(Frames);
    };
    ResizeFrameBuffers(SsrRayListBuffers);
    ResizeFrameBuffers(SsrRayCounterBuffers);
    ResizeFrameBuffers(SsrRayListPrimaryBuffers);
    ResizeFrameBuffers(SsrRayCounterPrimaryBuffers);
    ResizeFrameBuffers(SsrRayListHwMissBuffers);
    ResizeFrameBuffers(SsrRayCounterHwMissBuffers);
    ResizeFrameBuffers(SsrIndirectArgsPrimaryBuffers);
    ResizeFrameBuffers(SsrIndirectArgsHwMissBuffers);

    const uint64_t RayListSize = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
    FRGBufferDesc RayListRgDesc = {};
    RayListRgDesc.Size = RayListSize;
    RayListRgDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    RayListRgDesc.NumElements = SsrMaxRayCount;
    RayListRgDesc.StructureByteStride = SsrRayItemStride;

    FRGBufferDesc CounterRgDesc = {};
    CounterRgDesc.Size = sizeof(uint32_t);
    CounterRgDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CounterRgDesc.ViewFormat = DXGI_FORMAT_R32_TYPELESS;
    CounterRgDesc.NumElements = 1;
    CounterRgDesc.SrvFlags = D3D12_BUFFER_SRV_FLAG_RAW;
    CounterRgDesc.UavFlags = D3D12_BUFFER_UAV_FLAG_RAW;

    FRGBufferDesc IndirectArgsRgDesc = {};
    IndirectArgsRgDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
    IndirectArgsRgDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    IndirectArgsRgDesc.ViewFormat = DXGI_FORMAT_R32_TYPELESS;
    IndirectArgsRgDesc.NumElements = sizeof(D3D12_DISPATCH_ARGUMENTS) / sizeof(uint32_t);
    IndirectArgsRgDesc.SrvFlags = D3D12_BUFFER_SRV_FLAG_RAW;
    IndirectArgsRgDesc.UavFlags = D3D12_BUFFER_UAV_FLAG_RAW;

    const auto CreateRayPair = [&](uint32_t FrameIndex, FBindlessBuffer& RayList, FBindlessBuffer& RayCounter, const wchar_t* NameSuffix)
    {
        CreateBindlessBuffer(
            Device,
            std::wstring(L"SSR_RayList") + NameSuffix + L"_Frame" + std::to_wstring(FrameIndex),
            RayListRgDesc,
            D3D12_RESOURCE_STATE_COMMON,
            RayList,
            true,
            true);
        CreateBindlessBuffer(
            Device,
            std::wstring(L"SSR_RayCounter") + NameSuffix + L"_Frame" + std::to_wstring(FrameIndex),
            CounterRgDesc,
            D3D12_RESOURCE_STATE_COMMON,
            RayCounter,
            true,
            true);
    };

    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        CreateRayPair(FrameIndex, SsrRayListBuffers[FrameIndex], SsrRayCounterBuffers[FrameIndex], L"");
        CreateRayPair(FrameIndex, SsrRayListPrimaryBuffers[FrameIndex], SsrRayCounterPrimaryBuffers[FrameIndex], L"Primary");
        CreateRayPair(FrameIndex, SsrRayListHwMissBuffers[FrameIndex], SsrRayCounterHwMissBuffers[FrameIndex], L"HwMiss");

        CreateBindlessBuffer(
            Device,
            L"SSR_IndirectArgsPrimary_Frame" + std::to_wstring(FrameIndex),
            IndirectArgsRgDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            SsrIndirectArgsPrimaryBuffers[FrameIndex],
            false,
            true);
        CreateBindlessBuffer(
            Device,
            L"SSR_IndirectArgsHwMiss_Frame" + std::to_wstring(FrameIndex),
            IndirectArgsRgDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            SsrIndirectArgsHwMissBuffers[FrameIndex],
            false,
            true);
    }

    RefreshPersistentInputValidation();
    return true;
}


void FSsr::AddSsrRayCounterClearPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;

    struct FSsrRayCounterClearPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrRayCounterClearPassData>("SSR RayCounter Clear", [this, &Owner, FrameIndex, &Graph](FSsrRayCounterClearPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bEnabled = FrameIndex < SsrRayCounterPrimaryBuffers.size() && FrameIndex < SsrRayCounterHwMissBuffers.size();
        if (!Data.bEnabled)
        {
            return;
        }

        const FRGBufferHandle PrimaryHandle = ImportBindlessBuffer(Graph, "SSR_RayCounterPrimary", SsrRayCounterPrimaryBuffers[FrameIndex]);
        const FRGBufferHandle HwMissHandle = ImportBindlessBuffer(Graph, "SSR_RayCounterHwMiss", SsrRayCounterHwMissBuffers[FrameIndex]);

        Builder.WriteBuffer(PrimaryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FSsrRayCounterClearPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        ID3D12Resource* PrimaryCounterBuffer = SsrRayCounterPrimaryBuffers[LocalFrameIndex].Get();
        ID3D12Resource* HwMissCounterBuffer = SsrRayCounterHwMissBuffers[LocalFrameIndex].Get();
        const uint32_t PrimaryCounterUavIndex = SsrRayCounterPrimaryBuffers[LocalFrameIndex].UavBindlessIndex;
        const uint32_t HwMissCounterUavIndex = SsrRayCounterHwMissBuffers[LocalFrameIndex].UavBindlessIndex;

        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE PrimaryGpuHandle = Owner.GetBindlessGpuHandle(PrimaryCounterUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE PrimaryCpuHandle = Owner.GetBindlessCpuClearHandle(PrimaryCounterUavIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(PrimaryGpuHandle, PrimaryCpuHandle, PrimaryCounterBuffer, ClearValues, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HwMissGpuHandle = Owner.GetBindlessGpuHandle(HwMissCounterUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HwMissCpuHandle = Owner.GetBindlessCpuClearHandle(HwMissCounterUavIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HwMissGpuHandle, HwMissCpuHandle, HwMissCounterBuffer, ClearValues, 0, nullptr);
    });
}

void FSsr::AddSsrRayGatherPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredGBufferHandles& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;

    struct FSsrRayGatherPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrRayGatherPassData>("SSR Ray Gather", [this, &Owner, FrameIndex, GBufferHandles, LinearDepthHandle, &Graph](FSsrRayGatherPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bEnabled = bPersistentInputsValid
            && FrameIndex < SsrRayCounterPrimaryBuffers.size()
            && FrameIndex < SsrRayListPrimaryBuffers.size();
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const FRGBufferHandle RayCounterHandle = ImportBindlessBuffer(Graph, "SSR_RayCounterPrimary", SsrRayCounterPrimaryBuffers[FrameIndex]);
        const FRGBufferHandle RayListHandle = ImportBindlessBuffer(Graph, "SSR_RayListPrimary", SsrRayListPrimaryBuffers[FrameIndex]);

        Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FSsrRayGatherPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t RayCounterUavIndex = SsrRayCounterPrimaryBuffers[LocalFrameIndex].UavBindlessIndex;
        const uint32_t RayListUavIndex = SsrRayListPrimaryBuffers[LocalFrameIndex].UavBindlessIndex;

        if (Owner.GBufferA.SrvBindlessIndex == UINT32_MAX || Owner.GBufferB.SrvBindlessIndex == UINT32_MAX || Owner.LinearDepthTexture.SrvBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetPipelineState(SsrRayGatherPipeline.Get());
        LocalCommandList->SetComputeRootSignature(SsrRayGatherRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        struct FSsrRayGatherConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            uint32_t FrameIndex = 0;
            uint32_t SamplesPerQuad = 0;
            uint32_t MaxRayCount = 0;
            float MaxDistance = 0.0f;
            float RoughnessCutoff = 0.0f;
            float NormalBiasScale = 0.0f;
            float TMinBias = 0.0f;
            uint32_t PatternRotate = 0;
            uint32_t Padding = 0;
        };

        const FSsrRayGatherConstants Constants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            LocalFrameIndex,
            SsrSamplesPerQuad,
            SsrMaxRayCount,
            SsrMaxDistance,
            SsrRoughnessCutoff,
            0.001f,
            0.01f,
            LocalFrameIndex & 3u,
            0u
        };
        static_assert(sizeof(FSsrRayGatherConstants) / sizeof(uint32_t) <= kSsrRayGatherConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(1, sizeof(FSsrRayGatherConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t BindlessIndices[kSsrRayGatherBindlessDwordCount] =
        {
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            RayCounterUavIndex,
            RayListUavIndex,
            Owner.Device->GetPointClampSamplerIndex()
        };
        LocalCommandList->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        const uint32_t DispatchX = (Constants.OutputWidth + 7u) / 8u;
        const uint32_t DispatchY = (Constants.OutputHeight + 7u) / 8u;
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FSsr::AddSsrBuildIndirectArgsPass(FDeferredPassContext& Context, bool bHwMiss)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;

    struct FSsrBuildIndirectArgsPassData
    {
        bool bEnabled = false;
        bool bHwMiss = false;
    };

    const wchar_t* PassLabel = bHwMiss ? L"SSR Build IndirectArgs HW Miss" : L"SSR Build IndirectArgs Primary";
    const char* PassName = bHwMiss ? "SSR Build IndirectArgs HW Miss" : "SSR Build IndirectArgs Primary";
    Graph.AddPass<FSsrBuildIndirectArgsPassData>(PassName, [this, &Owner, FrameIndex, bHwMiss, &Graph](FSsrBuildIndirectArgsPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bHwMiss = bHwMiss;
        Data.bEnabled = bPersistentInputsValid && (bHwMiss
            ? (FrameIndex < SsrRayCounterHwMissBuffers.size() && FrameIndex < SsrIndirectArgsHwMissBuffers.size())
            : (FrameIndex < SsrRayCounterPrimaryBuffers.size() && FrameIndex < SsrIndirectArgsPrimaryBuffers.size()));
        if (!Data.bEnabled)
        {
            return;
        }

        FBindlessBuffer& CounterBuffer = bHwMiss ? SsrRayCounterHwMissBuffers[FrameIndex] : SsrRayCounterPrimaryBuffers[FrameIndex];
        FBindlessBuffer& IndirectBuffer = bHwMiss ? SsrIndirectArgsHwMissBuffers[FrameIndex] : SsrIndirectArgsPrimaryBuffers[FrameIndex];
        const FRGBufferHandle CounterHandle = ImportBindlessBuffer(Graph, bHwMiss ? "SSR_RayCounterHwMiss" : "SSR_RayCounterPrimary", CounterBuffer);
        const FRGBufferHandle IndirectHandle = ImportBindlessBuffer(Graph, bHwMiss ? "SSR_IndirectArgsHwMiss" : "SSR_IndirectArgsPrimary", IndirectBuffer);

        Builder.ReadBuffer(CounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(IndirectHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, PassLabel](const FSsrBuildIndirectArgsPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t RayCounterSrvIndex = Data.bHwMiss ? SsrRayCounterHwMissBuffers[LocalFrameIndex].SrvBindlessIndex : SsrRayCounterPrimaryBuffers[LocalFrameIndex].SrvBindlessIndex;
        const uint32_t IndirectArgsUavIndex = Data.bHwMiss ? SsrIndirectArgsHwMissBuffers[LocalFrameIndex].UavBindlessIndex : SsrIndirectArgsPrimaryBuffers[LocalFrameIndex].UavBindlessIndex;

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->SetPipelineState(SsrBuildIndirectArgsPipeline.Get());
        LocalCommandList->SetComputeRootSignature(SsrBuildIndirectArgsRootSignature.Get());

        constexpr uint32_t ThreadGroupSizeX = 64u;
        const uint32_t Constants[kSsrBuildIndirectArgsConstantsDwordCount] = { ThreadGroupSizeX, SsrMaxRayCount };
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

        const uint32_t BindlessIndices[kSsrBuildIndirectArgsBindlessDwordCount] = { RayCounterSrvIndex, IndirectArgsUavIndex };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

        LocalCommandList->Dispatch(1, 1, 1);
    });
}

void FSsr::AddSsrSwTracePass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.Taa.HistoryHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle HZBHandle = Context.Resources.Hzb.HzbHandle;
    const FRGResourceHandle SsrHandle = Context.Resources.Ssr.SsrHandle;

    struct FSsrSwTracePassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        bool bUseHzb = false;
        uint32_t HistoryIndex = 0;
        uint32_t PipelineIndex = 0;
        FRGResourceHandle SceneColorHandle{};
        FRGResourceHandle SsrHandle{};
        FRGResourceHandle HzbHandle{};
    };

    Graph.AddPass<FSsrSwTracePassData>("SSR SW Trace", [this, &Owner, FrameIndex, FrameState, TaaHandles, LinearDepthHandle, HZBHandle, SsrHandle, &Graph](FSsrSwTracePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bUseHzb = bSsrHzbEnabled && Owner.Hzb->IsEnabled() && Owner.Hzb->IsReady() && IsValidBindlessIndex(Owner.Hzb->GetSrvBindlessIndex());
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = BuildSsrPipelineIndex(Data.bUseHzb, bSsrRefineEnabled, bSsrSwEnabled, bSsrHzbFullResDepthEnabled);
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && bPersistentInputsValid
            && FrameIndex < SsrRayCounterPrimaryBuffers.size() && FrameIndex < SsrRayListPrimaryBuffers.size()
            && FrameIndex < SsrRayCounterHwMissBuffers.size() && FrameIndex < SsrRayListHwMissBuffers.size()
            && FrameIndex < SsrIndirectArgsPrimaryBuffers.size();

        if (!Data.bEnabled)
        {
            return;
        }

        if (Data.bUseHistory)
        {
            Data.SceneColorHandle = TaaHandles[Data.HistoryIndex];
            Builder.ReadTexture(Data.SceneColorHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (Data.bUseHzb)
        {
            Data.HzbHandle = HZBHandle;
            Builder.ReadTexture(Data.HzbHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Data.SsrHandle = SsrHandle;
        Builder.WriteTexture(Data.SsrHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const FRGBufferHandle RayCounterHandle = ImportBindlessBuffer(Graph, "SSR_RayCounterPrimary", SsrRayCounterPrimaryBuffers[FrameIndex]);
        const FRGBufferHandle RayListHandle = ImportBindlessBuffer(Graph, "SSR_RayListPrimary", SsrRayListPrimaryBuffers[FrameIndex]);
        const FRGBufferHandle HwMissCounterHandle = ImportBindlessBuffer(Graph, "SSR_RayCounterHwMiss", SsrRayCounterHwMissBuffers[FrameIndex]);
        const FRGBufferHandle HwMissListHandle = ImportBindlessBuffer(Graph, "SSR_RayListHwMiss", SsrRayListHwMissBuffers[FrameIndex]);
        const FRGBufferHandle IndirectHandle = ImportBindlessBuffer(Graph, "SSR_IndirectArgsPrimary", SsrIndirectArgsPrimaryBuffers[FrameIndex]);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(HwMissCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.UavBarrier(IndirectHandle);
    }, [this, &Owner, &Graph](const FSsrSwTracePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (!IsValidBindlessIndex(Owner.GBufferC.SrvBindlessIndex) || !IsValidBindlessIndex(Owner.LinearDepthTexture.SrvBindlessIndex))
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t RayCounterPrimarySrvIndex = SsrRayCounterPrimaryBuffers[LocalFrameIndex].SrvBindlessIndex;
        const uint32_t RayListPrimarySrvIndex = SsrRayListPrimaryBuffers[LocalFrameIndex].SrvBindlessIndex;
        const uint32_t RayCounterHwMissUavIndex = SsrRayCounterHwMissBuffers[LocalFrameIndex].UavBindlessIndex;
        const uint32_t RayListHwMissUavIndex = SsrRayListHwMissBuffers[LocalFrameIndex].UavBindlessIndex;

        ID3D12Resource* IndirectArgsBuffer = SsrIndirectArgsPrimaryBuffers[LocalFrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        ID3D12Resource* SsrOutput = Graph.GetTextureResource(Data.SsrHandle);
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const D3D12_GPU_DESCRIPTOR_HANDLE OutputGpuHandle = Owner.GetBindlessGpuHandle(SsrTexture.UavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE OutputCpuHandle = Owner.GetBindlessCpuClearHandle(SsrTexture.UavBindlessIndex);
        const float ClearValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearUnorderedAccessViewFloat(OutputGpuHandle, OutputCpuHandle, SsrOutput, ClearValues, 0, nullptr);

        if (!EnsureSsrSwTracePipelineOrFail(Owner.Device, Data.PipelineIndex, "SSR SW Trace"))
        {
            return;
        }

        LocalCommandList->SetPipelineState(SsrSwTracePipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetComputeRootSignature(SsrSwTraceRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        struct FSsrSwTraceConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            uint32_t MaxSteps = 0;
            float MaxDistance = 0.0f;
            float Thickness = 0.0f;
            float Stride = 0.0f;
            float RoughnessCutoff = 0.0f;
            float Intensity = 0.0f;
            uint32_t HZBWidth = 0;
            uint32_t HZBHeight = 0;
            uint32_t HZBMipCount = 0;
            uint32_t HZBAvailable = 0;
            uint32_t MaxRayCount = 0;
        };

        const FSsrSwTraceConstants Constants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            SsrMaxSteps,
            SsrMaxDistance,
            SsrThickness,
            SsrStride,
            SsrRoughnessCutoff,
            SsrIntensity,
            Owner.Hzb->GetWidth(),
            Owner.Hzb->GetHeight(),
            Owner.Hzb->GetMipCount(),
            Data.bUseHzb ? 1u : 0u,
            SsrMaxRayCount
        };
        static_assert(sizeof(FSsrSwTraceConstants) / sizeof(uint32_t) <= kSsrSwTraceConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(1, sizeof(FSsrSwTraceConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t SceneColorIndex = Data.bUseHistory && Owner.Taa
            ? Owner.Taa->GetHistorySrvBindlessIndex(Data.HistoryIndex)
            : Owner.GBufferC.SrvBindlessIndex;
        if (!IsValidBindlessIndex(SceneColorIndex))
        {
            return;
        }

        const uint32_t HzbIndex = Data.bUseHzb && IsValidBindlessIndex(Owner.Hzb->GetSrvBindlessIndex()) ? Owner.Hzb->GetSrvBindlessIndex() : Owner.LinearDepthTexture.SrvBindlessIndex;
        const uint32_t BindlessIndices[kSsrSwTraceBindlessDwordCount] =
        {
            Owner.LinearDepthTexture.SrvBindlessIndex,
            SceneColorIndex,
            RayCounterPrimarySrvIndex,
            RayListPrimarySrvIndex,
            RayCounterHwMissUavIndex,
            RayListHwMissUavIndex,
            SsrTexture.UavBindlessIndex,
            HzbIndex,
            Owner.Device->GetPointClampSamplerIndex(),
            Owner.Device->GetLinearClampSamplerIndex()
        };
        LocalCommandList->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        LocalCommandList->ExecuteIndirect(SsrDispatchCommandSignature.Get(), 1, IndirectArgsBuffer, 0, nullptr, 0);
    });
}

void FSsr::AddSsrHwTracePass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FCamera& Camera = Context.Camera;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.Taa.HistoryHandles;
    const FRGResourceHandle SsrHandle = Context.Resources.Ssr.SsrHandle;

    struct FSsrHwTracePassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        uint32_t HistoryIndex = 0;
        FRGResourceHandle SceneColorHandle{};
        FRGResourceHandle SsrHandle{};
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSsrHwTracePassData>("SSR HW Trace", [this, &Owner, FrameIndex, FrameState, &Camera, TaaHandles, SsrHandle, &Graph](FSsrHwTracePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bEnabled = bSsrHwEnabled && bPersistentInputsValid
            && Owner.GetRayTracingRuntime().bRayTracingPipelineReady
            && Owner.GetRayTracingRuntime().RayQueryRootSignature
            && Owner.GetRayTracingRuntime().RayQuerySsrHwPipeline
            && FrameIndex < SsrRayCounterHwMissBuffers.size()
            && FrameIndex < SsrRayListHwMissBuffers.size()
            && FrameIndex < SsrIndirectArgsHwMissBuffers.size();
        if (!Data.bEnabled)
        {
            return;
        }

        Data.SsrHandle = SsrHandle;
        Data.Camera = &Camera;
        if (Data.bUseHistory)
        {
            Data.SceneColorHandle = TaaHandles[Data.HistoryIndex];
            Builder.ReadTexture(Data.SceneColorHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteTexture(Data.SsrHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const FRGBufferHandle RayCounterHandle = ImportBindlessBuffer(Graph, "SSR_RayCounterHwMiss", SsrRayCounterHwMissBuffers[FrameIndex]);
        const FRGBufferHandle RayListHandle = ImportBindlessBuffer(Graph, "SSR_RayListHwMiss", SsrRayListHwMissBuffers[FrameIndex]);
        const FRGBufferHandle IndirectHandle = ImportBindlessBuffer(Graph, "SSR_IndirectArgsHwMiss", SsrIndirectArgsHwMissBuffers[FrameIndex]);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        Builder.UavBarrier(IndirectHandle);
    }, [this, &Owner, &Graph](const FSsrHwTracePassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (!IsValidBindlessIndex(Owner.GBufferC.SrvBindlessIndex))
        {
            return;
        }

        const uint32_t LocalFrameIndex = CmdContext.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.GetRayTracingRuntime().TlasResultBuffers.size() || !Owner.GetRayTracingRuntime().TlasResultBuffers[LocalFrameIndex])
        {
            return;
        }

        const uint32_t RayCounterHwMissIndex = SsrRayCounterHwMissBuffers[LocalFrameIndex].SrvBindlessIndex;
        const uint32_t RayListHwMissIndex = SsrRayListHwMissBuffers[LocalFrameIndex].SrvBindlessIndex;

        ID3D12Resource* IndirectArgsBuffer = SsrIndirectArgsHwMissBuffers[LocalFrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        ID3D12Resource* SsrOutput = Graph.GetTextureResource(Data.SsrHandle);
        ID3D12GraphicsCommandList* LocalCommandList = CmdContext.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t SceneColorIndex = Data.bUseHistory && Owner.Taa
            ? Owner.Taa->GetHistorySrvBindlessIndex(Data.HistoryIndex)
            : Owner.GBufferC.SrvBindlessIndex;
        if (!IsValidBindlessIndex(SceneColorIndex))
        {
            return;
        }

        const uint32_t OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        if (OutputWidth == 0 || OutputHeight == 0 || SsrMaxRayCount == 0)
        {
            return;
        }

        LocalCommandList->SetPipelineState(Owner.GetRayTracingRuntime().RayQuerySsrHwPipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.GetRayTracingRuntime().RayQueryRootSignature.Get());
        LocalCommandList->SetComputeRootShaderResourceView(0, Owner.GetRayTracingRuntime().TlasResultBuffers[LocalFrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        LocalCommandList->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (LocalFrameIndex >= Owner.GetRayTracingRuntime().PathTracingInstanceDataBuffers.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = Owner.GetRayTracingRuntime().PathTracingInstanceDataBuffers[LocalFrameIndex].SrvBindlessIndex;
        if (!IsValidBindlessIndex(PathTracingInstanceDataBindlessIndex))
        {
            return;
        }

        auto FloatBits = [](float f) noexcept { uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u; };
        std::array<uint32_t, FRayTracingRuntime::RayQueryRootConstantDwordCount> BindlessIndices =
        {
            RayListHwMissIndex,
            RayCounterHwMissIndex,
            SsrTexture.UavBindlessIndex,
            SceneColorIndex,
            PathTracingInstanceDataBindlessIndex,
            Owner.GetEnvironmentCubeSrvIndex(),
            Owner.Device->GetLinearClampSamplerIndex(),
            SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            FloatBits(SsrIntensity),
            FloatBits(SsrRoughnessCutoff),
            0u
        };
        static_assert(std::tuple_size_v<decltype(BindlessIndices)> <= FRayTracingRuntime::RayQueryRootConstantDwordCount, "Ray query root constants exceed root signature capacity.");
        LocalCommandList->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);

        LocalCommandList->ExecuteIndirect(SsrDispatchCommandSignature.Get(), 1, IndirectArgsBuffer, 0, nullptr, 0);
    });
}

void FSsr::AddSsrResolvePass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle SsrInputHandle = Context.Resources.Ssr.SsrHandle;
    const FRGResourceHandle SsrResolveHandle = Context.Resources.Ssr.SsrResolveHandle;

    struct FSsrResolvePassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrResolvePassData>("SSR Resolve", [this, &Owner, GBufferHandles, LinearDepthHandle, SsrInputHandle, SsrResolveHandle](FSsrResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bEnabled = bPersistentInputsValid;
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(SsrInputHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SsrResolveHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FSsrResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (!IsValidBindlessIndex(Owner.LinearDepthTexture.SrvBindlessIndex)
            || !IsValidBindlessIndex(Owner.GBufferA.SrvBindlessIndex) || !IsValidBindlessIndex(Owner.GBufferB.SrvBindlessIndex))
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetPipelineState(SsrResolvePipeline.Get());
        LocalCommandList->SetComputeRootSignature(SsrResolveRootSignature.Get());

        struct FSsrResolveConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            float DepthWeight = 1.0f;
            float NormalWeight = 1.0f;
            float RoughnessWeight = 1.0f;
        };

        const FSsrResolveConstants Constants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            1.0f,
            1.0f,
            0.5f
        };
        static_assert(sizeof(FSsrResolveConstants) / sizeof(uint32_t) <= kSsrResolveConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FSsrResolveConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t BindlessIndices[kSsrResolveBindlessDwordCount] =
        {
            SsrTexture.SrvBindlessIndex,
            SsrResolveTexture.UavBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.Device->GetPointClampSamplerIndex()
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

        const uint32_t DispatchX = (Constants.OutputWidth + 7u) / 8u;
        const uint32_t DispatchY = (Constants.OutputHeight + 7u) / 8u;
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FSsr::AddSsrPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FDeferredGBufferHandles& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.Taa.HistoryHandles;
    const FRGResourceHandle HZBHandle = Context.Resources.Hzb.HzbHandle;
    const FRGResourceHandle SsrHandle = Context.Resources.Ssr.SsrHandle;

    struct FSsrPassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        uint32_t HistoryIndex = 0;
        bool bUseHzb = false;
        uint32_t PipelineIndex = 0;
    };

    Graph.AddPass<FSsrPassData>("SSR", [this, &Owner, FrameIndex, GBufferHandles, LinearDepthHandle, TaaHandles, HZBHandle, SsrHandle, FrameState, &Graph](FSsrPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bUseHzb = bSsrHzbEnabled && Owner.Hzb->IsEnabled() && Owner.Hzb->IsReady() && IsValidBindlessIndex(Owner.Hzb->GetSrvBindlessIndex());
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = BuildSsrPipelineIndex(Data.bUseHzb, bSsrRefineEnabled, bSsrSwEnabled, bSsrHzbFullResDepthEnabled);
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && bPersistentInputsValid
            && FrameIndex < SsrRayCounterBuffers.size() && FrameIndex < SsrRayListBuffers.size();

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (Data.bUseHzb)
        {
            Builder.ReadTexture(HZBHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (Data.bUseHistory)
        {
            Builder.ReadTexture(TaaHandles[Data.HistoryIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteTexture(SsrHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);

        if (Data.bEnabled)
        {
            const FRGBufferHandle RayCounterHandle = ImportBindlessBuffer(Graph, "SSR_RayCounter", SsrRayCounterBuffers[FrameIndex]);
            const FRGBufferHandle RayListHandle = ImportBindlessBuffer(Graph, "SSR_RayList", SsrRayListBuffers[FrameIndex]);
            Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [this, &Owner](const FSsrPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(SsrRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(SsrRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        if (!IsValidBindlessIndex(Owner.GBufferA.SrvBindlessIndex) || !IsValidBindlessIndex(Owner.GBufferB.SrvBindlessIndex) || !IsValidBindlessIndex(Owner.LinearDepthTexture.SrvBindlessIndex))
        {
            return;
        }

        const uint32_t HistoryIndex = Data.bUseHistory && Owner.Taa
            ? Owner.Taa->GetHistorySrvBindlessIndex(Data.HistoryIndex)
            : Owner.GBufferC.SrvBindlessIndex;
        if (!IsValidBindlessIndex(HistoryIndex))
        {
            return;
        }

        if (!EnsureSsrGraphicsPipelineOrFail(Owner.Device, Data.PipelineIndex, "SSR"))
        {
            return;
        }

        LocalCommandList->SetPipelineState(SsrPipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(SsrRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        struct FSsrConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            uint32_t MaxSteps = 0;
            float Thickness = 0.0f;
            float MaxDistance = 0.0f;
            float Stride = 0.0f;
            float RoughnessCutoff = 0.0f;
            float Intensity = 0.0f;
            uint32_t MaxRayCount = 0;
            uint32_t UseHistory = 0;
            uint32_t HZBWidth = 0;
            uint32_t HZBHeight = 0;
            uint32_t HZBMipCount = 0;
            uint32_t HZBAvailable = 0;
            uint32_t HwEnabled = 0;
        };

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        uint32_t RayCounterUavIndex = UINT32_MAX;
        uint32_t RayListUavIndex = UINT32_MAX;
        ID3D12Resource* RayCounterBuffer = nullptr;
        bool bRayOutputAvailable = false;
        if (LocalFrameIndex < SsrRayCounterBuffers.size() && LocalFrameIndex < SsrRayListBuffers.size())
        {
            RayCounterBuffer = SsrRayCounterBuffers[LocalFrameIndex].Get();
            RayCounterUavIndex = SsrRayCounterBuffers[LocalFrameIndex].UavBindlessIndex;
            RayListUavIndex = SsrRayListBuffers[LocalFrameIndex].UavBindlessIndex;
            bRayOutputAvailable = RayCounterBuffer != nullptr;
        }

        if (bRayOutputAvailable)
        {
            const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = Owner.GetBindlessGpuHandle(RayCounterUavIndex);
            const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = Owner.GetBindlessCpuClearHandle(RayCounterUavIndex);
            const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
            LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, RayCounterBuffer, ClearValues, 0, nullptr);
        }

        const FSsrConstants SsrConstants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            SsrMaxSteps,
            SsrThickness,
            SsrMaxDistance,
            SsrStride,
            SsrRoughnessCutoff,
            SsrIntensity,
            (bSsrHwEnabled && bRayOutputAvailable) ? SsrMaxRayCount : 0u,
            Data.bUseHistory ? 1u : 0u,
            Owner.Hzb->GetWidth(),
            Owner.Hzb->GetHeight(),
            Owner.Hzb->GetMipCount(),
            Data.bUseHzb ? 1u : 0u,
            bSsrHwEnabled ? 1u : 0u
        };
        static_assert(sizeof(FSsrConstants) / sizeof(uint32_t) <= kSsrTraceConstantsDwordCount);
        LocalCommandList->SetGraphicsRoot32BitConstants(1, sizeof(FSsrConstants) / sizeof(uint32_t), &SsrConstants, 0);

        const uint32_t HzbIndex = IsValidBindlessIndex(Owner.Hzb->GetSrvBindlessIndex()) ? Owner.Hzb->GetSrvBindlessIndex() : Owner.LinearDepthTexture.SrvBindlessIndex;
        const uint32_t SsrBindlessIndices[kSsrTraceBindlessDwordCount] =
        {
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            HistoryIndex,
            HzbIndex,
            Owner.Device->GetPointClampSamplerIndex(),
            Owner.Device->GetLinearClampSamplerIndex(),
            RayCounterUavIndex,
            RayListUavIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(2, _countof(SsrBindlessIndices), SsrBindlessIndices, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FSsr::AddSsrFallbackPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FCamera& Camera = Context.Camera;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.Taa.HistoryHandles;
    const FRGResourceHandle SsrFallbackHandle = Context.Resources.Ssr.SsrFallbackHandle;

    struct FSsrFallbackPassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        bool bDoRayTracing = false;
        uint32_t HistoryIndex = 0;
        FRGResourceHandle SceneColorHandle{};
        FRGResourceHandle FallbackHandle{};
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSsrFallbackPassData>("SSR Fallback", [this, &Owner, FrameIndex, FrameState, &Camera, TaaHandles, SsrFallbackHandle, &Graph](FSsrFallbackPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bEnabled = bPersistentInputsValid;
        Data.bDoRayTracing = bSsrHwEnabled && Data.bUseHistory;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.FallbackHandle = SsrFallbackHandle;
        Data.Camera = &Camera;
        if (Data.bDoRayTracing)
        {
            Data.SceneColorHandle = TaaHandles[Data.HistoryIndex];
            Builder.ReadTexture(Data.SceneColorHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Builder.WriteTexture(Data.FallbackHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        Data.bDoRayTracing = Data.bDoRayTracing
            && FrameIndex < SsrRayListBuffers.size() && FrameIndex < SsrRayCounterBuffers.size()
            && static_cast<bool>(Owner.GetRayTracingRuntime().RayQuerySsrFallbackPipeline)
            && static_cast<bool>(Owner.GetRayTracingRuntime().RayQueryRootSignature);
        if (!Data.bDoRayTracing)
        {
            return;
        }

        const FRGBufferHandle RayListHandle = ImportBindlessBuffer(Graph, "SSR_RayList", SsrRayListBuffers[FrameIndex]);
        const FRGBufferHandle RayCounterHandle = ImportBindlessBuffer(Graph, "SSR_RayCounter", SsrRayCounterBuffers[FrameIndex]);

        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }, [this, &Owner, &Graph](const FSsrFallbackPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12Resource* FallbackTexture = Graph.GetTextureResource(Data.FallbackHandle);
        if (!FallbackTexture)
        {
            return;
        }

        const uint32_t FallbackUavIndex = SsrFallbackTexture.UavBindlessIndex;

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = CmdContext.GetCommandList();
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        const D3D12_GPU_DESCRIPTOR_HANDLE UavGpuHandle = Owner.GetBindlessGpuHandle(FallbackUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE UavCpuHandle = Owner.GetBindlessCpuClearHandle(FallbackUavIndex);
        const float ClearValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearUnorderedAccessViewFloat(UavGpuHandle, UavCpuHandle, FallbackTexture, ClearValues, 0, nullptr);

        if (!Data.bDoRayTracing || !Owner.GetRayTracingRuntime().bRayTracingPipelineReady)
        {
            return;
        }

        if (Owner.SceneModels.empty())
        {
            return;
        }

        ID3D12Resource* SceneColor = Graph.GetTextureResource(Data.SceneColorHandle);
        const uint32_t LocalFrameIndex = CmdContext.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.GetRayTracingRuntime().TlasResultBuffers.size() || !Owner.GetRayTracingRuntime().TlasResultBuffers[LocalFrameIndex])
        {
            return;
        }

        const uint32_t RayListIndex = SsrRayListBuffers[LocalFrameIndex].SrvBindlessIndex;
        const uint32_t RayCounterIndex = SsrRayCounterBuffers[LocalFrameIndex].SrvBindlessIndex;

        const uint32_t SceneColorIndex = Owner.Taa
            ? Owner.Taa->GetHistorySrvBindlessIndex(Data.HistoryIndex)
            : UINT32_MAX;
        if (SceneColorIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        if (OutputWidth == 0 || OutputHeight == 0 || SsrMaxRayCount == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 64;
        const uint32_t DispatchCount = (SsrMaxRayCount + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        LocalCommandList->SetPipelineState(Owner.GetRayTracingRuntime().RayQuerySsrFallbackPipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.GetRayTracingRuntime().RayQueryRootSignature.Get());
        LocalCommandList->SetComputeRootShaderResourceView(0, Owner.GetRayTracingRuntime().TlasResultBuffers[LocalFrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        LocalCommandList->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (LocalFrameIndex >= Owner.GetRayTracingRuntime().PathTracingInstanceDataBuffers.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = Owner.GetRayTracingRuntime().PathTracingInstanceDataBuffers[LocalFrameIndex].SrvBindlessIndex;
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        auto FloatBits = [](float f) noexcept { uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u; };
        std::array<uint32_t, FRayTracingRuntime::RayQueryRootConstantDwordCount> BindlessIndices =
        {
            RayListIndex,
            RayCounterIndex,
            FallbackUavIndex,
            SceneColorIndex,
            PathTracingInstanceDataBindlessIndex,
            Owner.GetEnvironmentCubeSrvIndex(),
            Owner.Device->GetLinearClampSamplerIndex(),
            SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            FloatBits(SsrIntensity),
            FloatBits(SsrRoughnessCutoff),
            0u
        };
        static_assert(std::tuple_size_v<decltype(BindlessIndices)> <= FRayTracingRuntime::RayQueryRootConstantDwordCount, "Ray query root constants exceed root signature capacity.");
        LocalCommandList->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);
        LocalCommandList->Dispatch(DispatchCount, 1, 1);
    });
}

void FSsr::AddSsrDenoisePass(FDeferredPassContext& Context, FRGResourceHandle InputHandle)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle SsrDenoiseHandle = Context.Resources.Ssr.SsrDenoiseHandle;

    struct FSsrDenoisePassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrDenoisePassData>("SSR Denoise", [this, &Owner, InputHandle, GBufferHandles, LinearDepthHandle, SsrDenoiseHandle](FSsrDenoisePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && bSsrDenoiseEnabled && bPersistentInputsValid;

        Builder.ReadTexture(InputHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SsrDenoiseHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [this, &Owner](const FSsrDenoisePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(SsrDenoiseRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(SsrDenoiseRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        const uint32_t SsrInputIndex = (SsrMode == ESSRMode::CS) ? SsrResolveTexture.SrvBindlessIndex : SsrTexture.SrvBindlessIndex;
        if (Owner.GBufferA.SrvBindlessIndex == UINT32_MAX || Owner.LinearDepthTexture.SrvBindlessIndex == UINT32_MAX)
        {
            return;
        }

        LocalCommandList->SetPipelineState(SsrDenoisePipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(SsrDenoiseRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        struct FSsrDenoiseConstants
        {
            uint32_t OutputWidth = 0;
            uint32_t OutputHeight = 0;
            float DepthSigma = 0.0f;
            float NormalPower = 0.0f;
        };

        const FSsrDenoiseConstants Constants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            0.5f,
            32.0f
        };
        static_assert(sizeof(FSsrDenoiseConstants) / sizeof(uint32_t) <= kSsrDenoiseConstantsDwordCount);
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(FSsrDenoiseConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t DenoiseBindlessIndices[kSsrDenoiseBindlessDwordCount] =
        {
            SsrInputIndex,
            Owner.GBufferA.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.Device->GetPointClampSamplerIndex(),
            Owner.Device->GetLinearClampSamplerIndex()
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(DenoiseBindlessIndices), DenoiseBindlessIndices, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}
