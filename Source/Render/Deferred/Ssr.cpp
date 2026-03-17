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
#include <cstring>
#include <mutex>
#include <string>
#include <d3dx12.h>
namespace
{
    constexpr uint32_t SsrRayItemStride = 48u;
}

bool FSsr::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
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
    return CreateSsrResources(Owner, Device, Width, Height);
}

void FSsr::ImportPersistentResources(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FSsrFrameResources& OutResources = Context.Resources.Ssr;

    OutResources.SsrHandle = Graph.ImportTexture(
        "SSR",
        SsrTexture.Get(),
        &SsrState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrDenoiseHandle = Graph.ImportTexture(
        "SSR Denoise",
        SsrDenoiseTexture.Get(),
        &SsrDenoiseState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrFallbackHandle = Graph.ImportTexture(
        "SSR Fallback",
        SsrFallbackTexture.Get(),
        &SsrFallbackState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });

    OutResources.SsrResolveHandle = Graph.ImportTexture(
        "SSR Resolve",
        SsrResolveTexture.Get(),
        &SsrResolveState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });
}

bool FSsr::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    if (!Device)
    {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC SsrSrvDesc = {};
    SsrSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    SsrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SsrSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    SsrSrvDesc.Texture2D.MipLevels = 1;
    SsrBindlessIndex = Device->CreateBindlessSrv(SsrTexture.Get(), SsrSrvDesc);
    SsrDenoiseBindlessIndex = Device->CreateBindlessSrv(SsrDenoiseTexture.Get(), SsrSrvDesc);
    SsrFallbackBindlessIndex = Device->CreateBindlessSrv(SsrFallbackTexture.Get(), SsrSrvDesc);
    SsrResolveBindlessIndex = Device->CreateBindlessSrv(SsrResolveTexture.Get(), SsrSrvDesc);

    D3D12_UNORDERED_ACCESS_VIEW_DESC SsrUavDesc = {};
    SsrUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    SsrUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    SsrUavDesc.Texture2D.MipSlice = 0;
    SsrUavBindlessIndex = Device->CreateBindlessUav(SsrTexture.Get(), nullptr, SsrUavDesc);

    D3D12_UNORDERED_ACCESS_VIEW_DESC SsrResolveUavDesc = {};
    SsrResolveUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    SsrResolveUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    SsrResolveUavDesc.Texture2D.MipSlice = 0;
    SsrResolveUavBindlessIndex = Device->CreateBindlessUav(SsrResolveTexture.Get(), nullptr, SsrResolveUavDesc);

    D3D12_UNORDERED_ACCESS_VIEW_DESC SsrFallbackUavDesc = {};
    SsrFallbackUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    SsrFallbackUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    SsrFallbackUavDesc.Texture2D.MipSlice = 0;
    SsrFallbackUavBindlessIndex = Device->CreateBindlessUav(SsrFallbackTexture.Get(), nullptr, SsrFallbackUavDesc);
    return true;
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
    return (SsrMode == ESSRMode::CS) ? SsrResolveBindlessIndex : SsrBindlessIndex;
}

uint32_t FSsr::GetLightingSrvBindlessIndex() const
{
    return bSsrDenoiseEnabled ? SsrDenoiseBindlessIndex : GetBaseOutputSrvBindlessIndex();
}


bool FSsr::CreateSsrRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};

    // RootParams[0]: Scene constants (b0), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: SSR constants (b1), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 15;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    // RootParams[2]: SSR bindless indices (b2), used in Shaders/SsrSWTracePS.hlsl PSMain
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[2].Constants.Num32BitValues = 9;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/SsrSWTracePS.hlsl", L"VSMain", VSTarget, SsrGraphicsVsBytecode))
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
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    const bool bUseHzb = (PipelineIndex & 2u) != 0;
    const bool bUseRefine = (PipelineIndex & 1u) != 0;
    const bool bUseSwSsr = (PipelineIndex & 4u) == 0;

    const std::vector<std::wstring> Defines =
    {
        bUseHzb ? L"HZB_ENABLED=1" : L"HZB_ENABLED=0",
        bUseRefine ? L"SSR_REFINE_ENABLED=1" : L"SSR_REFINE_ENABLED=0",
        bUseSwSsr ? L"SW_SSR_ENABLED=1" : L"SW_SSR_ENABLED=0"
    };

    return Compiler.CompileFromFile(L"Shaders/SsrSWTracePS.hlsl", L"PSMain", PSTarget, OutPs, Defines);
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
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 5;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/SsrDenoise.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/SsrDenoise.hlsl", L"PSMain", PSTarget, PSByteCode))
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
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 11;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].Constants.Num32BitValues = 6;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/SsrRayGather.hlsl", L"CSMain", CSTarget, CSByteCode))
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
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 13;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].Constants.Num32BitValues = 10;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    const bool bUseHzb = (PipelineIndex & 2u) != 0;
    const bool bUseRefine = (PipelineIndex & 1u) != 0;
    const bool bUseSwSsr = (PipelineIndex & 4u) == 0;

    const std::vector<std::wstring> Defines =
    {
        bUseHzb ? L"HZB_ENABLED=1" : L"HZB_ENABLED=0",
        bUseRefine ? L"SSR_REFINE_ENABLED=1" : L"SSR_REFINE_ENABLED=0",
        bUseSwSsr ? L"SW_SSR_ENABLED=1" : L"SW_SSR_ENABLED=0"
    };

    return Compiler.CompileFromFile(L"Shaders/SsrSWTraceCS.hlsl", L"CSMain", CSTarget, OutCs, Defines);
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
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 2;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 2;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/SsrBuildIndirectArgs.hlsl", L"CSMain", CSTarget, CSByteCode))
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
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 5;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 1;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 6;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/SsrResolve.hlsl", L"CSMain", CSTarget, CSByteCode))
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
    if (Device == nullptr)
    {
        return false;
    }

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
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = Desc.Format;
    ClearValue.Color[0] = 0.0f;
    ClearValue.Color[1] = 0.0f;
    ClearValue.Color[2] = 0.0f;
    ClearValue.Color[3] = 0.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(SsrTexture.GetAddressOf())));

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(SsrDenoiseTexture.GetAddressOf())));

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        &ClearValue,
        IID_PPV_ARGS(SsrResolveTexture.GetAddressOf())));

    CD3DX12_RESOURCE_DESC FallbackDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &FallbackDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(SsrFallbackTexture.GetAddressOf())));

    if (SsrTexture)
    {
        SsrTexture->SetName(L"SSR");
    }

    if (SsrDenoiseTexture)
    {
        SsrDenoiseTexture->SetName(L"SSR_Denoise");
    }

    if (SsrResolveTexture)
    {
        SsrResolveTexture->SetName(L"SSR_Resolve");
    }

    if (SsrFallbackTexture)
    {
        SsrFallbackTexture->SetName(L"SSR_Fallback");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 2;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(SsrRtvHeap.GetAddressOf())));
    if (SsrRtvHeap)
    {
        SsrRtvHeap->SetName(L"SSR_RTVHeap");
    }

    SsrRtvHandle = SsrRtvHeap->GetCPUDescriptorHandleForHeapStart();
    const UINT RtvDescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    SsrDenoiseRtvHandle = SsrRtvHandle;
    SsrDenoiseRtvHandle.ptr += RtvDescriptorSize;
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Device->GetDevice()->CreateRenderTargetView(SsrTexture.Get(), &RtvDesc, SsrRtvHandle);
    Device->GetDevice()->CreateRenderTargetView(SsrDenoiseTexture.Get(), &RtvDesc, SsrDenoiseRtvHandle);

    SsrState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    SsrDenoiseState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    SsrFallbackState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    SsrResolveState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    const uint32_t Frames = Owner.GetFramesInFlight();
    SsrMaxRayCount = Width * Height;
    SsrRayListBuffers.clear();
    SsrRayListBuffers.resize(Frames);
    SsrRayCounterBuffers.clear();
    SsrRayCounterBuffers.resize(Frames);
    SsrRayListSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayCounterStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayListPrimaryBuffers.clear();
    SsrRayListPrimaryBuffers.resize(Frames);
    SsrRayCounterPrimaryBuffers.clear();
    SsrRayCounterPrimaryBuffers.resize(Frames);
    SsrRayListHwMissBuffers.clear();
    SsrRayListHwMissBuffers.resize(Frames);
    SsrRayCounterHwMissBuffers.clear();
    SsrRayCounterHwMissBuffers.resize(Frames);
    SsrRayListPrimarySrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListPrimaryUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterPrimarySrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterPrimaryUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListHwMissSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListHwMissUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterHwMissSrvBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayCounterHwMissUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrRayListPrimaryStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayCounterPrimaryStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayListHwMissStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrRayCounterHwMissStates.assign(Frames, D3D12_RESOURCE_STATE_COMMON);
    SsrIndirectArgsPrimaryBuffers.clear();
    SsrIndirectArgsPrimaryBuffers.resize(Frames);
    SsrIndirectArgsHwMissBuffers.clear();
    SsrIndirectArgsHwMissBuffers.resize(Frames);
    SsrIndirectArgsPrimaryUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrIndirectArgsHwMissUavBindlessIndices.assign(Frames, UINT32_MAX);
    SsrIndirectArgsPrimaryStates.assign(Frames, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SsrIndirectArgsHwMissStates.assign(Frames, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const uint64_t RayListSize = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
    CD3DX12_HEAP_PROPERTIES DefaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC RayListDesc = CD3DX12_RESOURCE_DESC::Buffer(RayListSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_RESOURCE_DESC CounterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CD3DX12_RESOURCE_DESC IndirectArgsDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_DISPATCH_ARGUMENTS), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RayListDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayListBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayListBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayList_Frame" + std::to_wstring(FrameIndex);
            SsrRayListBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &CounterDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayCounterBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayCounterBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayCounter_Frame" + std::to_wstring(FrameIndex);
            SsrRayCounterBuffers[FrameIndex]->SetName(Name.c_str());
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC RayListSrvDesc = {};
        RayListSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        RayListSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        RayListSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        RayListSrvDesc.Buffer.FirstElement = 0;
        RayListSrvDesc.Buffer.NumElements = SsrMaxRayCount;
        RayListSrvDesc.Buffer.StructureByteStride = SsrRayItemStride;
        RayListSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        SsrRayListSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayListBuffers[FrameIndex].Get(), RayListSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC RayListUavDesc = {};
        RayListUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        RayListUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        RayListUavDesc.Buffer.FirstElement = 0;
        RayListUavDesc.Buffer.NumElements = SsrMaxRayCount;
        RayListUavDesc.Buffer.StructureByteStride = SsrRayItemStride;
        RayListUavDesc.Buffer.CounterOffsetInBytes = 0;
        RayListUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        SsrRayListUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayListBuffers[FrameIndex].Get(), nullptr, RayListUavDesc);

        D3D12_SHADER_RESOURCE_VIEW_DESC CounterSrvDesc = {};
        CounterSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        CounterSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        CounterSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        CounterSrvDesc.Buffer.FirstElement = 0;
        CounterSrvDesc.Buffer.NumElements = 1;
        CounterSrvDesc.Buffer.StructureByteStride = 0;
        CounterSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        SsrRayCounterSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayCounterBuffers[FrameIndex].Get(), CounterSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC CounterUavDesc = {};
        CounterUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        CounterUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        CounterUavDesc.Buffer.FirstElement = 0;
        CounterUavDesc.Buffer.NumElements = 1;
        CounterUavDesc.Buffer.StructureByteStride = 0;
        CounterUavDesc.Buffer.CounterOffsetInBytes = 0;
        CounterUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        SsrRayCounterUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayCounterBuffers[FrameIndex].Get(), nullptr, CounterUavDesc);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RayListDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayListPrimaryBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayListPrimaryBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayListPrimary_Frame" + std::to_wstring(FrameIndex);
            SsrRayListPrimaryBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &CounterDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayCounterPrimaryBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayCounterPrimaryBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayCounterPrimary_Frame" + std::to_wstring(FrameIndex);
            SsrRayCounterPrimaryBuffers[FrameIndex]->SetName(Name.c_str());
        }

        SsrRayListPrimarySrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayListPrimaryBuffers[FrameIndex].Get(), RayListSrvDesc);
        SsrRayListPrimaryUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayListPrimaryBuffers[FrameIndex].Get(), nullptr, RayListUavDesc);
        SsrRayCounterPrimarySrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayCounterPrimaryBuffers[FrameIndex].Get(), CounterSrvDesc);
        SsrRayCounterPrimaryUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayCounterPrimaryBuffers[FrameIndex].Get(), nullptr, CounterUavDesc);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RayListDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayListHwMissBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayListHwMissBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayListHwMiss_Frame" + std::to_wstring(FrameIndex);
            SsrRayListHwMissBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &CounterDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(SsrRayCounterHwMissBuffers[FrameIndex].GetAddressOf())));
        if (SsrRayCounterHwMissBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_RayCounterHwMiss_Frame" + std::to_wstring(FrameIndex);
            SsrRayCounterHwMissBuffers[FrameIndex]->SetName(Name.c_str());
        }

        SsrRayListHwMissSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayListHwMissBuffers[FrameIndex].Get(), RayListSrvDesc);
        SsrRayListHwMissUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayListHwMissBuffers[FrameIndex].Get(), nullptr, RayListUavDesc);
        SsrRayCounterHwMissSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(SsrRayCounterHwMissBuffers[FrameIndex].Get(), CounterSrvDesc);
        SsrRayCounterHwMissUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrRayCounterHwMissBuffers[FrameIndex].Get(), nullptr, CounterUavDesc);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &IndirectArgsDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(SsrIndirectArgsPrimaryBuffers[FrameIndex].GetAddressOf())));
        if (SsrIndirectArgsPrimaryBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_IndirectArgsPrimary_Frame" + std::to_wstring(FrameIndex);
            SsrIndirectArgsPrimaryBuffers[FrameIndex]->SetName(Name.c_str());
        }

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &IndirectArgsDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(SsrIndirectArgsHwMissBuffers[FrameIndex].GetAddressOf())));
        if (SsrIndirectArgsHwMissBuffers[FrameIndex])
        {
            const std::wstring Name = L"SSR_IndirectArgsHwMiss_Frame" + std::to_wstring(FrameIndex);
            SsrIndirectArgsHwMissBuffers[FrameIndex]->SetName(Name.c_str());
        }

        D3D12_UNORDERED_ACCESS_VIEW_DESC IndirectArgsUavDesc = {};
        IndirectArgsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        IndirectArgsUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        IndirectArgsUavDesc.Buffer.FirstElement = 0;
        IndirectArgsUavDesc.Buffer.NumElements = sizeof(D3D12_DISPATCH_ARGUMENTS) / sizeof(uint32_t);
        IndirectArgsUavDesc.Buffer.StructureByteStride = 0;
        IndirectArgsUavDesc.Buffer.CounterOffsetInBytes = 0;
        IndirectArgsUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        SsrIndirectArgsPrimaryUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(), nullptr, IndirectArgsUavDesc);
        SsrIndirectArgsHwMissUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(SsrIndirectArgsHwMissBuffers[FrameIndex].Get(), nullptr, IndirectArgsUavDesc);
    }

    return true;
}


void FSsr::AddSsrRayCounterClearPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;

    struct FSsrRayCounterClearPassData
    {
    };

    Graph.AddPass<FSsrRayCounterClearPassData>("SSR RayCounter Clear", [this, &Owner, FrameIndex, &Graph](FSsrRayCounterClearPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        if (FrameIndex >= SsrRayCounterPrimaryBuffers.size() || FrameIndex >= SsrRayCounterHwMissBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle PrimaryHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle HwMissHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);

        Builder.WriteBuffer(PrimaryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FSsrRayCounterClearPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrClearEvent(LocalCommandList, L"SSR RayCounter Clear");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= SsrRayCounterHwMissBuffers.size())
        {
            return;
        }

        ID3D12Resource* PrimaryCounterBuffer = SsrRayCounterPrimaryBuffers[LocalFrameIndex].Get();
        ID3D12Resource* HwMissCounterBuffer = SsrRayCounterHwMissBuffers[LocalFrameIndex].Get();
        const uint32_t PrimaryCounterUavIndex = SsrRayCounterPrimaryUavBindlessIndices[LocalFrameIndex];
        const uint32_t HwMissCounterUavIndex = SsrRayCounterHwMissUavBindlessIndices[LocalFrameIndex];

        if (!PrimaryCounterBuffer || !HwMissCounterBuffer || PrimaryCounterUavIndex == UINT32_MAX || HwMissCounterUavIndex == UINT32_MAX)
        {
            return;
        }

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
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;

    struct FSsrRayGatherPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrRayGatherPassData>("SSR Ray Gather", [this, &Owner, FrameIndex, GBufferHandles, LinearDepthHandle, &Graph](FSsrRayGatherPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bEnabled = SsrRayGatherPipeline && SsrRayGatherRootSignature;
        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= SsrRayCounterPrimaryBuffers.size() || FrameIndex >= SsrRayListPrimaryBuffers.size())
        {
            return;
        }

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListPrimary",
            SsrRayListPrimaryBuffers[FrameIndex].Get(),
            &SsrRayListPrimaryStates[FrameIndex],
            RayListDesc);

        Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FSsrRayGatherPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= SsrRayListPrimaryBuffers.size())
        {
            return;
        }

        const uint32_t RayCounterUavIndex = SsrRayCounterPrimaryUavBindlessIndices[LocalFrameIndex];
        const uint32_t RayListUavIndex = SsrRayListPrimaryUavBindlessIndices[LocalFrameIndex];
        if (RayCounterUavIndex == UINT32_MAX || RayListUavIndex == UINT32_MAX)
        {
            return;
        }

        if (Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrGatherEvent(LocalCommandList, L"SSR Ray Gather");

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
        LocalCommandList->SetComputeRoot32BitConstants(1, sizeof(FSsrRayGatherConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t BindlessIndices[] =
        {
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.LinearDepthBindlessIndex,
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
        Data.bEnabled = SsrBuildIndirectArgsPipeline && SsrBuildIndirectArgsRootSignature;
        Data.bHwMiss = bHwMiss;
        if (!Data.bEnabled)
        {
            return;
        }

        const bool bValidFrame = bHwMiss
            ? (FrameIndex < SsrRayCounterHwMissBuffers.size() && FrameIndex < SsrIndirectArgsHwMissBuffers.size())
            : (FrameIndex < SsrRayCounterPrimaryBuffers.size() && FrameIndex < SsrIndirectArgsPrimaryBuffers.size());
        if (!bValidFrame)
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle CounterHandle = Graph.ImportBuffer(
            bHwMiss ? "SSR_RayCounterHwMiss" : "SSR_RayCounterPrimary",
            bHwMiss ? SsrRayCounterHwMissBuffers[FrameIndex].Get() : SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            bHwMiss ? &SsrRayCounterHwMissStates[FrameIndex] : &SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            bHwMiss ? "SSR_IndirectArgsHwMiss" : "SSR_IndirectArgsPrimary",
            bHwMiss ? SsrIndirectArgsHwMissBuffers[FrameIndex].Get() : SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(),
            bHwMiss ? &SsrIndirectArgsHwMissStates[FrameIndex] : &SsrIndirectArgsPrimaryStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(CounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(IndirectHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, PassLabel](const FSsrBuildIndirectArgsPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent BuildEvent(LocalCommandList, PassLabel);

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (Data.bHwMiss)
        {
            if (LocalFrameIndex >= SsrRayCounterHwMissBuffers.size() || LocalFrameIndex >= SsrIndirectArgsHwMissBuffers.size())
            {
                return;
            }
        }
        else
        {
            if (LocalFrameIndex >= SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= SsrIndirectArgsPrimaryBuffers.size())
            {
                return;
            }
        }

        ID3D12Resource* RayCounterBuffer = Data.bHwMiss ? SsrRayCounterHwMissBuffers[LocalFrameIndex].Get() : SsrRayCounterPrimaryBuffers[LocalFrameIndex].Get();
        ID3D12Resource* IndirectArgsBuffer = Data.bHwMiss ? SsrIndirectArgsHwMissBuffers[LocalFrameIndex].Get() : SsrIndirectArgsPrimaryBuffers[LocalFrameIndex].Get();
        const uint32_t RayCounterSrvIndex = Data.bHwMiss ? SsrRayCounterHwMissSrvBindlessIndices[LocalFrameIndex] : SsrRayCounterPrimarySrvBindlessIndices[LocalFrameIndex];
        const uint32_t IndirectArgsUavIndex = Data.bHwMiss ? SsrIndirectArgsHwMissUavBindlessIndices[LocalFrameIndex] : SsrIndirectArgsPrimaryUavBindlessIndices[LocalFrameIndex];

        if (!RayCounterBuffer || !IndirectArgsBuffer || RayCounterSrvIndex == UINT32_MAX || IndirectArgsUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->SetPipelineState(SsrBuildIndirectArgsPipeline.Get());
        LocalCommandList->SetComputeRootSignature(SsrBuildIndirectArgsRootSignature.Get());

        constexpr uint32_t ThreadGroupSizeX = 64u;
        const uint32_t Constants[] = { ThreadGroupSizeX, SsrMaxRayCount };
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

        const uint32_t BindlessIndices[] = { RayCounterSrvIndex, IndirectArgsUavIndex };
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
    const FRGResourceHandle HZBHandle = Context.Resources.HZBHandle;
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
        Data.bUseHzb = bSsrHzbEnabled && Owner.bHZBReady && Owner.HZBSrvBindlessIndex != UINT32_MAX;
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = (Data.bUseHzb ? 2u : 0u) + (bSsrRefineEnabled ? 1u : 0u) + (bSsrSwEnabled ? 0u : 4u);
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && SsrSwTraceRootSignature;

        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= SsrRayCounterPrimaryBuffers.size() || FrameIndex >= SsrRayListPrimaryBuffers.size()
            || FrameIndex >= SsrRayCounterHwMissBuffers.size() || FrameIndex >= SsrRayListHwMissBuffers.size()
            || FrameIndex >= SsrIndirectArgsPrimaryBuffers.size())
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

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListPrimary",
            SsrRayListPrimaryBuffers[FrameIndex].Get(),
            &SsrRayListPrimaryStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle HwMissCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle HwMissListHandle = Graph.ImportBuffer(
            "SSR_RayListHwMiss",
            SsrRayListHwMissBuffers[FrameIndex].Get(),
            &SsrRayListHwMissStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            "SSR_IndirectArgsPrimary",
            SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(),
            &SsrIndirectArgsPrimaryStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(HwMissCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }, [this, &Owner, &Graph](const FSsrSwTracePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (Owner.GBufferBindlessIndices[2] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX || SsrUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= SsrRayListPrimaryBuffers.size()
            || LocalFrameIndex >= SsrRayCounterHwMissBuffers.size() || LocalFrameIndex >= SsrRayListHwMissBuffers.size()
            || LocalFrameIndex >= SsrIndirectArgsPrimaryBuffers.size())
        {
            return;
        }

        const uint32_t RayCounterPrimarySrvIndex = SsrRayCounterPrimarySrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayListPrimarySrvIndex = SsrRayListPrimarySrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayCounterHwMissUavIndex = SsrRayCounterHwMissUavBindlessIndices[LocalFrameIndex];
        const uint32_t RayListHwMissUavIndex = SsrRayListHwMissUavBindlessIndices[LocalFrameIndex];
        if (RayCounterPrimarySrvIndex == UINT32_MAX || RayListPrimarySrvIndex == UINT32_MAX
            || RayCounterHwMissUavIndex == UINT32_MAX || RayListHwMissUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12Resource* IndirectArgsBuffer = SsrIndirectArgsPrimaryBuffers[LocalFrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        if (!SsrDispatchCommandSignature)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER UavBarrier = {};
        UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        UavBarrier.UAV.pResource = IndirectArgsBuffer;
        Cmd.GetCommandList()->ResourceBarrier(1, &UavBarrier);

        ID3D12Resource* SsrOutput = Graph.GetTextureResource(Data.SsrHandle);
        if (!SsrOutput)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrSwTraceEvent(LocalCommandList, L"SSR SW Trace");
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const D3D12_GPU_DESCRIPTOR_HANDLE OutputGpuHandle = Owner.GetBindlessGpuHandle(SsrUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE OutputCpuHandle = Owner.GetBindlessCpuClearHandle(SsrUavBindlessIndex);
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
            Owner.HZBWidth,
            Owner.HZBHeight,
            Owner.HZBMipCount,
            Data.bUseHzb ? 1u : 0u,
            SsrMaxRayCount
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, sizeof(FSsrSwTraceConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t SceneColorIndex = Data.bUseHistory && Owner.Taa
            ? Owner.Taa->GetHistorySrvBindlessIndex(Data.HistoryIndex)
            : Owner.GBufferBindlessIndices[2];
        if (SceneColorIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t HzbIndex = Data.bUseHzb && Owner.HZBSrvBindlessIndex != UINT32_MAX ? Owner.HZBSrvBindlessIndex : Owner.LinearDepthBindlessIndex;
        const uint32_t BindlessIndices[] =
        {
            Owner.LinearDepthBindlessIndex,
            SceneColorIndex,
            RayCounterPrimarySrvIndex,
            RayListPrimarySrvIndex,
            RayCounterHwMissUavIndex,
            RayListHwMissUavIndex,
            SsrUavBindlessIndex,
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
        Data.bEnabled = bSsrHwEnabled && Owner.bRayTracingPipelineReady && Owner.RayQueryRootSignature && Owner.RayQuerySsrHwPipeline;
        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= SsrRayCounterHwMissBuffers.size() || FrameIndex >= SsrRayListHwMissBuffers.size()
            || FrameIndex >= SsrIndirectArgsHwMissBuffers.size())
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

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListHwMiss",
            SsrRayListHwMissBuffers[FrameIndex].Get(),
            &SsrRayListHwMissStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            "SSR_IndirectArgsHwMiss",
            SsrIndirectArgsHwMissBuffers[FrameIndex].Get(),
            &SsrIndirectArgsHwMissStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }, [this, &Owner, &Graph](const FSsrHwTracePassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FScopedPixEvent SsrHWTraceEvent(CommandList4, L"SSR HW Trace");

        if (Data.Camera == nullptr)
        {
            return;
        }

        if (Owner.GBufferBindlessIndices[2] == UINT32_MAX || SsrUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t LocalFrameIndex = CmdContext.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.TlasResultBuffers.size() || !Owner.TlasResultBuffers[LocalFrameIndex])
        {
            return;
        }

        const uint32_t RayCounterHwMissIndex = SsrRayCounterHwMissSrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayListHwMissIndex = SsrRayListHwMissSrvBindlessIndices[LocalFrameIndex];
        if (RayCounterHwMissIndex == UINT32_MAX || RayListHwMissIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12Resource* IndirectArgsBuffer = SsrIndirectArgsHwMissBuffers[LocalFrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        if (!SsrDispatchCommandSignature)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER UavBarrier = {};
        UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        UavBarrier.UAV.pResource = IndirectArgsBuffer;
        CommandList4->ResourceBarrier(1, &UavBarrier);

        ID3D12Resource* SsrOutput = Graph.GetTextureResource(Data.SsrHandle);
        if (!SsrOutput)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t SceneColorIndex = Data.bUseHistory && Owner.Taa
            ? Owner.Taa->GetHistorySrvBindlessIndex(Data.HistoryIndex)
            : Owner.GBufferBindlessIndices[2];
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

        CommandList4->SetPipelineState(Owner.RayQuerySsrHwPipeline.Get());
        CommandList4->SetComputeRootSignature(Owner.RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, Owner.TlasResultBuffers[LocalFrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (LocalFrameIndex >= Owner.PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = Owner.PathTracingInstanceDataBindlessIndices[LocalFrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        std::array<uint32_t, 13> BindlessIndices =
        {
            RayListHwMissIndex,
            RayCounterHwMissIndex,
            SsrUavBindlessIndex,
            SceneColorIndex,
            PathTracingInstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            Owner.Device->GetLinearClampSamplerIndex(),
            SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            0u,
            0u,
            0u
        };
        static_assert(sizeof(float) == sizeof(uint32_t), "Float size mismatch.");
        std::memcpy(&BindlessIndices[10], &SsrIntensity, sizeof(float));
        std::memcpy(&BindlessIndices[11], &SsrRoughnessCutoff, sizeof(float));
        CommandList4->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);

        CommandList4->ExecuteIndirect(SsrDispatchCommandSignature.Get(), 1, IndirectArgsBuffer, 0, nullptr, 0);
    });
}

void FSsr::AddSsrResolvePass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
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
        Data.bEnabled = SsrResolvePipeline && SsrResolveRootSignature;
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
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (SsrBindlessIndex == UINT32_MAX || SsrResolveUavBindlessIndex == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX
            || Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrResolveEvent(LocalCommandList, L"SSR Resolve");
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
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FSsrResolveConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t BindlessIndices[] =
        {
            SsrBindlessIndex,
            SsrResolveUavBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.LinearDepthBindlessIndex,
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
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.Taa.HistoryHandles;
    const FRGResourceHandle HZBHandle = Context.Resources.HZBHandle;
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
        Data.bUseHzb = bSsrHzbEnabled && Owner.bHZBReady && Owner.HZBSrvBindlessIndex != UINT32_MAX;
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = (Data.bUseHzb ? 2u : 0u) + (bSsrRefineEnabled ? 1u : 0u) + (bSsrSwEnabled ? 0u : 4u);
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && SsrRootSignature;

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

        if (FrameIndex >= SsrRayCounterBuffers.size() || FrameIndex >= SsrRayListBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounter",
            SsrRayCounterBuffers[FrameIndex].Get(),
            &SsrRayCounterStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayList",
            SsrRayListBuffers[FrameIndex].Get(),
            &SsrRayListStates[FrameIndex],
            RayListDesc);

        Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FSsrPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrEvent(LocalCommandList, L"SSR");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(SsrRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(SsrRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        if (Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t HistoryIndex = Data.bUseHistory && Owner.Taa
            ? Owner.Taa->GetHistorySrvBindlessIndex(Data.HistoryIndex)
            : Owner.GBufferBindlessIndices[2];
        if (HistoryIndex == UINT32_MAX)
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
        if (LocalFrameIndex < SsrRayCounterBuffers.size() && LocalFrameIndex < SsrRayListBuffers.size())
        {
            RayCounterBuffer = SsrRayCounterBuffers[LocalFrameIndex].Get();
            RayCounterUavIndex = SsrRayCounterUavBindlessIndices[LocalFrameIndex];
            RayListUavIndex = SsrRayListUavBindlessIndices[LocalFrameIndex];
        }

        if (RayCounterBuffer && RayCounterUavIndex != UINT32_MAX && RayListUavIndex != UINT32_MAX)
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
            (bSsrHwEnabled && RayCounterUavIndex != UINT32_MAX && RayListUavIndex != UINT32_MAX) ? SsrMaxRayCount : 0u,
            Data.bUseHistory ? 1u : 0u,
            Owner.HZBWidth,
            Owner.HZBHeight,
            Owner.HZBMipCount,
            Data.bUseHzb ? 1u : 0u,
            bSsrHwEnabled ? 1u : 0u
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, sizeof(FSsrConstants) / sizeof(uint32_t), &SsrConstants, 0);

        const uint32_t HzbIndex = (Owner.HZBSrvBindlessIndex != UINT32_MAX) ? Owner.HZBSrvBindlessIndex : Owner.LinearDepthBindlessIndex;
        const uint32_t SsrBindlessIndices[] =
        {
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.LinearDepthBindlessIndex,
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
        Data.bEnabled = static_cast<bool>(SsrFallbackHandle);
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

        if (!Data.bDoRayTracing)
        {
            return;
        }

        if (FrameIndex >= SsrRayListBuffers.size() || FrameIndex >= SsrRayCounterBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayList",
            SsrRayListBuffers[FrameIndex].Get(),
            &SsrRayListStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounter",
            SsrRayCounterBuffers[FrameIndex].Get(),
            &SsrRayCounterStates[FrameIndex],
            CounterDesc);

        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }, [this, &Owner, &Graph](const FSsrFallbackPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12Resource* FallbackTexture = Graph.GetTextureResource(Data.FallbackHandle);
        if (!FallbackTexture)
        {
            return;
        }

        const uint32_t FallbackUavIndex = SsrFallbackUavBindlessIndex;
        if (FallbackUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FScopedPixEvent SsrFallbackEvent(CommandList4, L"SSR Fallback");

        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
        const D3D12_GPU_DESCRIPTOR_HANDLE UavGpuHandle = Owner.GetBindlessGpuHandle(FallbackUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE UavCpuHandle = Owner.GetBindlessCpuClearHandle(FallbackUavIndex);
        const float ClearValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CommandList4->ClearUnorderedAccessViewFloat(UavGpuHandle, UavCpuHandle, FallbackTexture, ClearValues, 0, nullptr);

        if (!Data.bDoRayTracing || !Owner.bRayTracingPipelineReady || !Owner.RayQuerySsrFallbackPipeline || !Owner.RayQueryRootSignature)
        {
            return;
        }

        if (Owner.SceneModels.empty() || Data.Camera == nullptr)
        {
            return;
        }

        ID3D12Resource* SceneColor = Graph.GetTextureResource(Data.SceneColorHandle);
        if (!SceneColor)
        {
            return;
        }

        const uint32_t LocalFrameIndex = CmdContext.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.TlasResultBuffers.size() || !Owner.TlasResultBuffers[LocalFrameIndex])
        {
            return;
        }

        if (LocalFrameIndex >= SsrRayListBuffers.size() || LocalFrameIndex >= SsrRayCounterBuffers.size())
        {
            return;
        }

        const uint32_t RayListIndex = SsrRayListSrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayCounterIndex = SsrRayCounterSrvBindlessIndices[LocalFrameIndex];
        if (RayListIndex == UINT32_MAX || RayCounterIndex == UINT32_MAX)
        {
            return;
        }

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

        CommandList4->SetPipelineState(Owner.RayQuerySsrFallbackPipeline.Get());
        CommandList4->SetComputeRootSignature(Owner.RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, Owner.TlasResultBuffers[LocalFrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (LocalFrameIndex >= Owner.PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = Owner.PathTracingInstanceDataBindlessIndices[LocalFrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        std::array<uint32_t, 13> BindlessIndices =
        {
            RayListIndex,
            RayCounterIndex,
            FallbackUavIndex,
            SceneColorIndex,
            PathTracingInstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            Owner.Device->GetLinearClampSamplerIndex(),
            SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            0u,
            0u,
            0u
        };
        static_assert(sizeof(float) == sizeof(uint32_t), "Float size mismatch.");
        std::memcpy(&BindlessIndices[10], &SsrIntensity, sizeof(float));
        std::memcpy(&BindlessIndices[11], &SsrRoughnessCutoff, sizeof(float));
        CommandList4->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);
        CommandList4->Dispatch(DispatchCount, 1, 1);
    });
}

void FSsr::AddSsrDenoisePass(FDeferredPassContext& Context, FRGResourceHandle InputHandle)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle SsrDenoiseHandle = Context.Resources.Ssr.SsrDenoiseHandle;

    struct FSsrDenoisePassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrDenoisePassData>("SSR Denoise", [this, &Owner, InputHandle, GBufferHandles, LinearDepthHandle, SsrDenoiseHandle](FSsrDenoisePassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("SSR");
        Data.bEnabled = (bSsrSwEnabled || bSsrHwEnabled) && bSsrDenoiseEnabled && SsrDenoiseRootSignature && SsrDenoisePipeline;

        Builder.ReadTexture(InputHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SsrDenoiseHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [this, &Owner](const FSsrDenoisePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrDenoiseEvent(LocalCommandList, L"SSR Denoise");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(SsrDenoiseRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(SsrDenoiseRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        const uint32_t SsrInputIndex = (SsrMode == ESSRMode::CS) ? SsrResolveBindlessIndex : SsrBindlessIndex;
        if (SsrInputIndex == UINT32_MAX || Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX)
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
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(FSsrDenoiseConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t DenoiseBindlessIndices[] =
        {
            SsrInputIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.LinearDepthBindlessIndex,
            Owner.Device->GetPointClampSamplerIndex(),
            Owner.Device->GetLinearClampSamplerIndex()
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(DenoiseBindlessIndices), DenoiseBindlessIndices, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}


