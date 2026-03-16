#include "DeferredLightingPasses.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <d3dx12.h>
bool FDeferredLightingPasses::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat) const
{
    return Owner.CreateLightingRootSignature(Device)
        && Owner.CreateLightingPipeline(Device, BackBufferFormat)
        && Owner.CreateLinearDepthRootSignature(Device)
        && Owner.CreateLinearDepthPipeline(Device)
        && Owner.CreateExtractHalfDepthNormalRootSignature(Device)
        && Owner.CreateExtractHalfDepthNormalPipeline(Device)
        && Owner.CreateGtaoRootSignature(Device)
        && Owner.CreateGtaoPipeline(Device)
        && Owner.CreateSsrRootSignature(Device)
        && Owner.CreateSsrPipeline(Device)
        && Owner.CreateSsrDenoiseRootSignature(Device)
        && Owner.CreateSsrDenoisePipeline(Device)
        && Owner.CreateSsrRayGatherRootSignature(Device)
        && Owner.CreateSsrRayGatherPipeline(Device)
        && Owner.CreateSsrSwTraceRootSignature(Device)
        && Owner.CreateSsrSwTracePipeline(Device)
        && Owner.CreateSsrBuildIndirectArgsRootSignature(Device)
        && Owner.CreateSsrBuildIndirectArgsPipeline(Device)
        && Owner.CreateSsrResolveRootSignature(Device)
        && Owner.CreateSsrResolvePipeline(Device)
        && Owner.CreateSsrDispatchCommandSignature(Device)
        && Owner.CreateHZBRootSignature(Device)
        && Owner.CreateHZBPipeline(Device);
}

bool FDeferredLightingPasses::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const
{
    return Owner.CreateLinearDepthResources(Device, Width, Height)
        && Owner.CreateGtaoResources(Device, Width, Height)
        && Owner.CreateSsrResources(Device, Width, Height)
        && Owner.CreateHilbertLutResources(Device)
        && Owner.CreateHZBResources(Device, Width, Height);
}

bool FDeferredRenderer::CreateLightingRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};
    // RootParams[0]: Lighting constants (b0), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Lighting bindless indices (b1), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.Num32BitValues = 14;

    // RootParams[2]: ReSTIR GI constants (b2), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[2].Constants.ShaderRegister = 2;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.Num32BitValues = 5;

    D3D12_STATIC_SAMPLER_DESC Samplers[3] = {};
    Samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    Samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    Samplers[0].MinLOD = 0.0f;
    Samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[0].ShaderRegister = 0;
    Samplers[0].RegisterSpace = 0;
    Samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    Samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    Samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    Samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    Samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    Samplers[1].MinLOD = 0.0f;
    Samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[1].ShaderRegister = 1;
    Samplers[1].RegisterSpace = 0;
    Samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    Samplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Samplers[2].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    Samplers[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    Samplers[2].MinLOD = 0.0f;
    Samplers[2].MaxLOD = D3D12_FLOAT32_MAX;
    Samplers[2].ShaderRegister = 2;
    Samplers[2].RegisterSpace = 0;
    Samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = _countof(Samplers);
    RootSigDesc.Desc_1_1.pStaticSamplers = Samplers;
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(LightingRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLinearDepthRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Linear depth constants (b0), used in Shaders/LinearDepth.hlsl VSMain and PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Depth bindless index (b1), used in Shaders/LinearDepth.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 1;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(LinearDepthRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLinearDepthPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/LinearDepth.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/LinearDepth.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LinearDepthRootSignature.Get();
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
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(LinearDepthPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateExtractHalfDepthNormalRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.ShaderRegister = 1;
    RootParams[0].Constants.RegisterSpace = 0;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(ExtractHalfDepthNormalRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateExtractHalfDepthNormalPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> CSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);
    if (!Compiler.CompileFromFile(L"Shaders/ExtractHalfDepthNormal.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = ExtractHalfDepthNormalRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };

    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(ExtractHalfDepthNormalPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    (void)BackBufferFormat;

    FShaderCompiler Compiler;
    std::vector<uint8_t> DirectVSByteCode;
    std::vector<uint8_t> DirectPSByteCodes[4];
    std::vector<uint8_t> CompositeVSByteCode;
    std::vector<uint8_t> CompositePSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredDirectLighting.hlsl", L"VSMain", VSTarget, DirectVSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> DefaultDefines;
    const std::vector<std::wstring> ShadowMaskDefines = { L"USE_SHADOW_MASK=1" };
    const std::vector<std::wstring> ResearchDefines = { L"USE_PBR_RESEARCH=1" };
    const std::vector<std::wstring> ShadowMaskResearchDefines = { L"USE_SHADOW_MASK=1", L"USE_PBR_RESEARCH=1" };

    if (!Compiler.CompileFromFile(L"Shaders/DeferredDirectLighting.hlsl", L"PSMain", PSTarget, DirectPSByteCodes[0], DefaultDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredDirectLighting.hlsl", L"PSMain", PSTarget, DirectPSByteCodes[1], ShadowMaskDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredDirectLighting.hlsl", L"PSMain", PSTarget, DirectPSByteCodes[2], ResearchDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredDirectLighting.hlsl", L"PSMain", PSTarget, DirectPSByteCodes[3], ShadowMaskResearchDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredCompositeLight.hlsl", L"VSMain", VSTarget, CompositeVSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredCompositeLight.hlsl", L"PSMain", PSTarget, CompositePSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LightingRootSignature.Get();
    PsoDesc.VS = { DirectVSByteCode.data(), DirectVSByteCode.size() };
    PsoDesc.PS = { DirectPSByteCodes[0].data(), DirectPSByteCodes[0].size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = LightingBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    for (size_t Index = 0; Index < DirectLightingPipelines.size(); ++Index)
    {
        PsoDesc.PS = { DirectPSByteCodes[Index].data(), DirectPSByteCodes[Index].size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DirectLightingPipelines[Index].GetAddressOf())));
    }

    PsoDesc.VS = { CompositeVSByteCode.data(), CompositeVSByteCode.size() };
    PsoDesc.PS = { CompositePSByteCode.data(), CompositePSByteCode.size() };
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(CompositeLightingPipeline.GetAddressOf())));

    return true;
}

bool FDeferredRenderer::CreateHZBRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    // RootParams[0]: HZB constants (mip counts, dimensions, source mip), used in Shaders/BuildHZB.hlsl BuildHZB
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 12;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: HZB bindless indices (b1), used in Shaders/BuildHZB.hlsl BuildHZB
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 6;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(HZBRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateHZBPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    for (size_t PipelineIndex = 0; PipelineIndex < HZBPipelines.size(); ++PipelineIndex)
    {
        std::vector<uint8_t> CSByteCode;
        const std::wstring Define = L"HZB_MIPS_PER_DISPATCH=" + std::to_wstring(PipelineIndex + 1);
        const std::vector<std::wstring> Defines = { Define };

        if (!Compiler.CompileFromFile(L"Shaders/BuildHZB.hlsl", L"BuildHZB", CSTarget, CSByteCode, Defines))
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
        PsoDesc.pRootSignature = HZBRootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };

        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(HZBPipelines[PipelineIndex].GetAddressOf())));
    }
    return true;
}

bool FDeferredRenderer::CreateLinearDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16_FLOAT,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

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
        IID_PPV_ARGS(LinearDepthTexture.GetAddressOf())));

    if (LinearDepthTexture)
    {
        LinearDepthTexture->SetName(L"LinearDepth");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 1;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(LinearDepthRtvHeap.GetAddressOf())));
    if (LinearDepthRtvHeap)
    {
        LinearDepthRtvHeap->SetName(L"LinearDepthRTVHeap");
    }

    LinearDepthRtvHandle = LinearDepthRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    Device->GetDevice()->CreateRenderTargetView(LinearDepthTexture.Get(), &RtvDesc, LinearDepthRtvHandle);

    LinearDepthState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
}

bool FDeferredRenderer::CreateHZBResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t BaseWidth = (std::max)(1u, (Width + 1) / 2);
    const uint32_t BaseHeight = (std::max)(1u, (Height + 1) / 2);

    HZBWidth = BaseWidth;
    HZBHeight = BaseHeight;
    HZBMipCount = 1;

    uint32_t MipWidth = BaseWidth;
    uint32_t MipHeight = BaseHeight;
    while (MipWidth > 1 || MipHeight > 1)
    {
        MipWidth = (std::max)(1u, MipWidth / 2);
        MipHeight = (std::max)(1u, MipHeight / 2);
        ++HZBMipCount;
    }

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R32G32_FLOAT,
        BaseWidth,
        BaseHeight,
        1,
        static_cast<UINT16>(HZBMipCount),
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        HZBState,
        nullptr,
        IID_PPV_ARGS(HierarchicalZBuffer.GetAddressOf())));

    HierarchicalZBuffer->SetName(L"HierarchicalZBuffer");

    {
        D3D12_RESOURCE_DESC NullDesc = {};
        NullDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        NullDesc.Alignment = 0;
        NullDesc.Width = 1;
        NullDesc.Height = 1;
        NullDesc.DepthOrArraySize = 1;
        NullDesc.MipLevels = 1;
        NullDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        NullDesc.SampleDesc.Count = 1;
        NullDesc.SampleDesc.Quality = 0;
        NullDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        NullDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &NullDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(HZBNullUavResource.GetAddressOf())));

        HZBNullUavResource->SetName(L"HZBNullUavResource");
    }

    return true;
}

void FDeferredLightingPasses::AddLinearDepthPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;

    struct FLinearDepthPassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FLinearDepthPassData>("LinearDepth", [&Owner, DepthHandle, LinearDepthHandle](FLinearDepthPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.LinearDepthPipeline && Owner.LinearDepthRootSignature;
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner](const FLinearDepthPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent LinearDepthEvent(LocalCommandList, L"LinearDepth");

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.LinearDepthRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(Owner.LinearDepthRtvHandle, ClearColor, 0, nullptr);

        LocalCommandList->SetPipelineState(Owner.LinearDepthPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.LinearDepthRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());

        const uint32_t DepthIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[DepthIndex];
        LocalCommandList->SetGraphicsRoot32BitConstant(1, DepthBindlessIndex, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredLightingPasses::AddExtractHalfDepthNormalPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GBufferAHandle = Context.Resources.GBufferHandles[0];
    FRGResourceHandle& HalfDepthNormalHandle = Context.Resources.RestirGI.RestirGIHalfDepthNormalHandle;

    struct FExtractHalfDepthNormalPassData
    {
        bool bEnabled = false;
        FRGResourceHandle HalfDepthNormalHandle{};
    };

    HalfDepthNormalHandle = {};

    Graph.AddPass<FExtractHalfDepthNormalPassData>("ExtractHalfDepthNormal", [&, DepthHandle, GBufferAHandle](FExtractHalfDepthNormalPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.ExtractHalfDepthNormalRootSignature && Owner.ExtractHalfDepthNormalPipeline;
        if (!Data.bEnabled)
        {
            return;
        }

        const uint32_t HalfWidth = (static_cast<uint32_t>(Owner.Viewport.Width) + 1u) / 2u;
        const uint32_t HalfHeight = (static_cast<uint32_t>(Owner.Viewport.Height) + 1u) / 2u;
        Data.HalfDepthNormalHandle = Builder.CreateTexture("HalfDepthNormal", { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT });
        HalfDepthNormalHandle = Data.HalfDepthNormalHandle;
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Data.HalfDepthNormalHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, DepthHandle, GBufferAHandle](const FExtractHalfDepthNormalPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap() || !Owner.Device->GetSamplerDescriptorHeap())
        {
            return;
        }

        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size())];
        const uint32_t HalfDepthNormalUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Data.HalfDepthNormalHandle);
        if (DepthBindlessIndex == UINT32_MAX || Owner.GBufferBindlessIndices[0] == UINT32_MAX || HalfDepthNormalUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList = Cmd.GetCommandList4();
        if (!CommandList)
        {
            return;
        }

        FScopedPixEvent ExtractEvent(CommandList, L"ExtractHalfDepthNormal");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(Owner.ExtractHalfDepthNormalRootSignature.Get());
        CommandList->SetPipelineState(Owner.ExtractHalfDepthNormalPipeline.Get());

		const uint32_t GlobalFrameNumber = static_cast<uint32_t>(Owner.GetFrameNumber());
		const uint32_t SequenceFrame = Owner.IsRestirGIFreezeFrame() ? Owner.GetRestirGIFrozenSequenceFrame() : GlobalFrameNumber;
        const uint32_t Constants[4] =
        {
            DepthBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            HalfDepthNormalUavBindlessIndex,
            SequenceFrame
        };
        CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

        const uint32_t HalfWidth = (static_cast<uint32_t>(Owner.Viewport.Width) + 1u) / 2u;
        const uint32_t HalfHeight = (static_cast<uint32_t>(Owner.Viewport.Height) + 1u) / 2u;
        constexpr uint32_t GroupSize = 8u;
        CommandList->Dispatch((HalfWidth + GroupSize - 1u) / GroupSize, (HalfHeight + GroupSize - 1u) / GroupSize, 1u);
    });
}

void FDeferredLightingPasses::AddDirectLightingPass(FDeferredPassContext& Context, FRGResourceHandle& OutDirectHandle) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle ShadowHandle = Context.Resources.ShadowHandle;

    struct FDirectLightingPassData
    {
        bool bUseShadows = false;
        FRGResourceHandle DirectLightingHandle{};
    };

    Graph.AddPass<FDirectLightingPassData>("DirectLighting", [&](FDirectLightingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseShadows = FrameState.bRenderShadows;
        const FRGTextureDesc DirectLightingDesc =
        {
            static_cast<uint32>(Owner.Viewport.Width),
            static_cast<uint32>(Owner.Viewport.Height),
            FDeferredRenderer::LightingBufferFormat
        };
        Data.DirectLightingHandle = Builder.CreateTexture("DirectLighting", DirectLightingDesc);
        Builder.WriteTexture(Data.DirectLightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        OutDirectHandle = Data.DirectLightingHandle;

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[3], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (Data.bUseShadows)
        {
            Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }, [&Owner, &Graph](const FDirectLightingPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent DirectLightingEvent(LocalCommandList, L"DirectLighting");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12Resource* DirectLightingResource = Graph.GetTextureResource(Data.DirectLightingHandle);
        if (!DirectLightingResource)
        {
            return;
        }

        ComPtr<ID3D12DescriptorHeap> DirectLightingRtvHeap;
        D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
        RtvHeapDesc.NumDescriptors = 1;
        RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(Owner.Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(DirectLightingRtvHeap.GetAddressOf()))))
        {
            return;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE DirectLightingRtvHandle = DirectLightingRtvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC DirectLightingRtvDesc = {};
        DirectLightingRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        DirectLightingRtvDesc.Format = FDeferredRenderer::LightingBufferFormat;
        Owner.Device->GetDevice()->CreateRenderTargetView(DirectLightingResource, &DirectLightingRtvDesc, DirectLightingRtvHandle);

        const uint32_t DepthIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[DepthIndex];
        if (DepthBindlessIndex == UINT32_MAX || Owner.ShadowMapBindlessIndex == UINT32_MAX
            || Owner.EnvironmentCubeBindlessIndex == UINT32_MAX || Owner.BrdfLutBindlessIndex == UINT32_MAX
            || Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX || Owner.GBufferBindlessIndices[2] == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(DirectLightingRtvHandle, nullptr);

        const bool bUseShadowMask = Owner.bShadowsEnabled && Owner.bRayTracedShadowsEnabled && Owner.bRayTracingPipelineReady && Owner.ShadowMaskBindlessIndex != UINT32_MAX;
        const uint32_t PipelineIndex = (bUseShadowMask ? 1u : 0u) | (Owner.bEnablePbrResearch ? 2u : 0u);
        LocalCommandList->SetPipelineState(Owner.DirectLightingPipelines[PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.LightingRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const uint32_t ResolvedShadowMaskIndex = bUseShadowMask ? Owner.ShadowMaskBindlessIndex : Owner.ShadowMapBindlessIndex;
        const uint32_t LightingBindlessIndices[] =
        {
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            Owner.GBufferBindlessIndices[3],
            Owner.ShadowMapBindlessIndex,
            ResolvedShadowMaskIndex,
            Owner.EnvironmentCubeBindlessIndex,
            Owner.BrdfLutBindlessIndex,
            DepthBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(LightingBindlessIndices), LightingBindlessIndices, 0);

        struct FRestirGIConstants
        {
            float Intensity = 0.0f;
            uint32_t Enabled = 0;
            float SsrRoughnessCutoff = 0.0f;
            uint32_t ShowOnly = 0;
            uint32_t Padding = 0;
        };

        const float EffectiveRestirGIIntensity = (std::max)(0.0f, Owner.GetRestirGIIntensity());

        const FRestirGIConstants RestirGIConstants =
        {
            1.0f,
            Owner.IsRestirGIEnabled() ? 1u : 0u,
            Owner.SsrRoughnessCutoff,
            Owner.IsRestirGIShowOnly() ? 1u : 0u,
            0u
        };
        (void)EffectiveRestirGIIntensity;
        LocalCommandList->SetGraphicsRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &RestirGIConstants, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredLightingPasses::AddCompositeLightPass(FDeferredPassContext& Context, FRGResourceHandle SsrHandle, FRGResourceHandle DirectHandle) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GtaoHandle = Context.Resources.GtaoHandle;
    const FRGResourceHandle RestirGIInputHandle = Owner.IsRestirGIDenoiserEnabled() ? Context.Resources.RestirGiHistoryIrradianceHandle : Context.Resources.RestirGI.RestirGIHandle;
    const FRGResourceHandle SsrFallbackHandle = Context.Resources.SsrFallbackHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;

    struct FCompositeLightPassData
    {
        FRGResourceHandle DirectLightingHandle{};
    };

    Graph.AddPass<FCompositeLightPassData>("CompositeLight", [&](FCompositeLightPassData& Data, FRGPassBuilder& Builder)
    {
        Data.DirectLightingHandle = DirectHandle;

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[3], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GtaoHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIInputHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SsrHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SsrFallbackHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.DirectLightingHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner, &Graph](const FCompositeLightPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent CompositeLightEvent(LocalCommandList, L"CompositeLight");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12Resource* DirectLightingResource = Graph.GetTextureResource(Data.DirectLightingHandle);
        if (!DirectLightingResource)
        {
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC DirectLightingSrvDesc = {};
        DirectLightingSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        DirectLightingSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DirectLightingSrvDesc.Format = FDeferredRenderer::LightingBufferFormat;
        DirectLightingSrvDesc.Texture2D.MipLevels = 1;
        DirectLightingSrvDesc.Texture2D.MostDetailedMip = 0;
        DirectLightingSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (Owner.DirectLightingBindlessIndex == UINT32_MAX)
        {
            Owner.DirectLightingBindlessIndex = Owner.Device->CreateBindlessSrv(DirectLightingResource, DirectLightingSrvDesc);
        }
        else if (Owner.DirectLightingResource != DirectLightingResource)
        {
            Owner.WriteBindlessSrv(Owner.DirectLightingBindlessIndex, DirectLightingResource, DirectLightingSrvDesc);
        }
        Owner.DirectLightingResource = DirectLightingResource;

        const uint32_t DepthIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[DepthIndex];
        const uint32_t BaseSsrIndex = (Owner.SsrMode == ESSRMode::CS) ? Owner.SsrResolveBindlessIndex : Owner.SsrBindlessIndex;
        const uint32_t SsrLightingBindlessIndex = Owner.bSsrDenoiseEnabled ? Owner.SsrDenoiseBindlessIndex : BaseSsrIndex;
        const uint32_t SsrFallbackIndex = Owner.SsrFallbackBindlessIndex;
        const uint32_t RestirGILightingBindlessIndex = Owner.IsRestirGIDenoiserEnabled()
            ? Owner.RestirGiHistoryIrradianceSrvBindlessIndex
            : ((Owner.RestirGI != nullptr) ? Owner.RestirGI->GetCurrentOutputSrvBindlessIndex() : UINT32_MAX);
        if (DepthBindlessIndex == UINT32_MAX || Owner.GtaoBindlessIndex == UINT32_MAX || RestirGILightingBindlessIndex == UINT32_MAX || SsrLightingBindlessIndex == UINT32_MAX || SsrFallbackIndex == UINT32_MAX || Owner.ShadowMapBindlessIndex == UINT32_MAX
            || Owner.EnvironmentCubeBindlessIndex == UINT32_MAX || Owner.BrdfLutBindlessIndex == UINT32_MAX || Owner.DirectLightingBindlessIndex == UINT32_MAX
            || Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX || Owner.GBufferBindlessIndices[2] == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.LightingRTVHandle, nullptr);

        LocalCommandList->SetPipelineState(Owner.CompositeLightingPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.LightingRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const uint32_t LightingBindlessIndices[] =
        {
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            Owner.GBufferBindlessIndices[3],
            Owner.ShadowMapBindlessIndex,
            Owner.ShadowMapBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            Owner.BrdfLutBindlessIndex,
            DepthBindlessIndex,
            Owner.GtaoBindlessIndex,
            RestirGILightingBindlessIndex,
            SsrLightingBindlessIndex,
            SsrFallbackIndex,
            Owner.DirectLightingBindlessIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(LightingBindlessIndices), LightingBindlessIndices, 0);

        struct FRestirGIConstants
        {
            float Intensity = 0.0f;
            uint32_t Enabled = 0;
            float SsrRoughnessCutoff = 0.0f;
            uint32_t ShowOnly = 0;
            uint32_t Padding = 0;
        };

        const float EffectiveRestirGIIntensity = (std::max)(0.0f, Owner.GetRestirGIIntensity());

        const FRestirGIConstants RestirGIConstants =
        {
            1.0f,
            Owner.IsRestirGIEnabled() ? 1u : 0u,
            Owner.SsrRoughnessCutoff,
            Owner.IsRestirGIShowOnly() ? 1u : 0u,
            0u
        };
        (void)EffectiveRestirGIIntensity;
        LocalCommandList->SetGraphicsRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &RestirGIConstants, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}


void FDeferredLightingPasses::AddSkyPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FCamera& Camera = Context.Camera;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;

    struct FSkyPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSkyPassData>("Sky", [&Owner, &Camera, DepthHandle, LightingHandle](FSkyPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.SkyPipelineState && Owner.SkyRootSignature && Owner.SkyGeometry.IndexCount > 0;
        Data.Camera = &Camera;

        if (Data.bEnabled)
        {
            Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
            Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [&Owner](const FSkyPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent SkyEvent(LocalCommandList, L"SkyAtmosphere");
        LocalCommandList->SetPipelineState(Owner.SkyPipelineState.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.SkyRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->IASetVertexBuffers(0, Owner.SkyGeometry.VertexBufferCount, Owner.SkyGeometry.VertexBufferViews.data());
        LocalCommandList->IASetIndexBuffer(&Owner.SkyGeometry.IndexBufferView);
        const D3D12_CPU_DESCRIPTOR_HANDLE& LocalDepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(1, &Owner.LightingRTVHandle, FALSE, &LocalDepthHandle);

        Owner.UpdateSkyConstants(*Data.Camera);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.SkyConstantBuffer->GetGPUVirtualAddress());
        LocalCommandList->DrawIndexedInstanced(Owner.SkyGeometry.IndexCount, 1, 0, 0, 0);
    });
}


