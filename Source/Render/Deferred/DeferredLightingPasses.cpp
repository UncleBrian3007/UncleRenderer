#include "DeferredLightingPasses.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include <algorithm>
#include <cstring>
#include <array>
#include <d3dx12.h>
namespace
{
    constexpr uint32_t SsrRayItemStride = 48u;

    uint32_t HilbertIndex(uint32_t PosX, uint32_t PosY)
    {
        constexpr uint32_t HilbertLevel = 6u;
        constexpr uint32_t HilbertWidth = 1u << HilbertLevel;
        uint32_t Index = 0u;

        for (uint32_t CurLevel = HilbertWidth / 2u; CurLevel > 0u; CurLevel /= 2u)
        {
            const uint32_t RegionX = (PosX & CurLevel) > 0u;
            const uint32_t RegionY = (PosY & CurLevel) > 0u;
            Index += CurLevel * CurLevel * ((3u * RegionX) ^ RegionY);

            if (RegionY == 0u)
            {
                if (RegionX == 1u)
                {
                    PosX = (HilbertWidth - 1u) - PosX;
                    PosY = (HilbertWidth - 1u) - PosY;
                }

                std::swap(PosX, PosY);
            }
        }

        return Index;
    }
}

bool FDeferredLightingPasses::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat) const
{
    return Owner.CreateLightingRootSignature(Device)
        && Owner.CreateLightingPipeline(Device, BackBufferFormat)
        && Owner.CreateLinearDepthRootSignature(Device)
        && Owner.CreateLinearDepthPipeline(Device)
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
    RootParams[1].Constants.Num32BitValues = 13;

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

bool FDeferredRenderer::CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCodes[4];

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> DefaultDefines;
    const std::vector<std::wstring> ShadowMaskDefines = { L"USE_SHADOW_MASK=1" };
    const std::vector<std::wstring> ResearchDefines = { L"USE_PBR_RESEARCH=1" };
    const std::vector<std::wstring> ShadowMaskResearchDefines = { L"USE_SHADOW_MASK=1", L"USE_PBR_RESEARCH=1" };

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[0], DefaultDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[1], ShadowMaskDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[2], ResearchDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCodes[3], ShadowMaskResearchDefines))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LightingRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCodes[0].data(), PSByteCodes[0].size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = LightingBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    for (size_t Index = 0; Index < LightingPipelines.size(); ++Index)
    {
        PsoDesc.PS = { PSByteCodes[Index].data(), PSByteCodes[Index].size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(LightingPipelines[Index].GetAddressOf())));
    }
    return true;
}

bool FDeferredRenderer::CreateGtaoRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Scene constants (b0) used in Gtao.hlsl.
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: GTAO bindless indices (b1) used in Gtao.hlsl.
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.Num32BitValues = 3;
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(GtaoRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateGtaoPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::array<std::vector<uint8_t>, 8> PSByteCodes;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/Gtao.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> JitterOffDefines = { L"GTAO_USE_JITTER=0" };
    const std::vector<std::wstring> JitterOnDefines = { L"GTAO_USE_JITTER=1" };
    if (!Compiler.CompileFromFile(L"Shaders/Gtao.hlsl", L"PSMain", PSTarget, PSByteCodes[0], JitterOffDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/Gtao.hlsl", L"PSMain", PSTarget, PSByteCodes[1], JitterOnDefines))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = GtaoRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCodes[0].data(), PSByteCodes[0].size() };
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
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(GtaoPipelines[0].GetAddressOf())));
    PsoDesc.PS = { PSByteCodes[1].data(), PSByteCodes[1].size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(GtaoPipelines[1].GetAddressOf())));
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

bool FDeferredRenderer::CreateGtaoResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R8_UNORM,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = Desc.Format;
    ClearValue.Color[0] = 1.0f;
    ClearValue.Color[1] = 1.0f;
    ClearValue.Color[2] = 1.0f;
    ClearValue.Color[3] = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(GtaoTexture.GetAddressOf())));

    if (GtaoTexture)
    {
        GtaoTexture->SetName(L"GTAO");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 1;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(GtaoRtvHeap.GetAddressOf())));
    if (GtaoRtvHeap)
    {
        GtaoRtvHeap->SetName(L"GTAO_RTVHeap");
    }

    GtaoRtvHandle = GtaoRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R8_UNORM;
    Device->GetDevice()->CreateRenderTargetView(GtaoTexture.Get(), &RtvDesc, GtaoRtvHandle);

    GtaoState = D3D12_RESOURCE_STATE_RENDER_TARGET;
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

bool FDeferredRenderer::CreateSsrResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
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

    const uint32_t Frames = GetFramesInFlight();
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


bool FDeferredRenderer::CreateHilbertLutResources(FDX12Device* Device)
{
    if (Device == nullptr)
    {
        return false;
    }

    constexpr uint32_t HilbertWidth = 64u;
    std::array<uint16_t, HilbertWidth * HilbertWidth> Data = {};
    for (uint32_t Y = 0; Y < HilbertWidth; ++Y)
    {
        for (uint32_t X = 0; X < HilbertWidth; ++X)
        {
            const uint32_t Index = HilbertIndex(X, Y);
            Data[X + HilbertWidth * Y] = static_cast<uint16_t>(Index);
        }
    }

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16_UINT,
        HilbertWidth,
        HilbertWidth,
        1,
        1);

    CD3DX12_HEAP_PROPERTIES DefaultHeap(D3D12_HEAP_TYPE_DEFAULT);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(HilbertLutTexture.GetAddressOf())));

    if (HilbertLutTexture)
    {
        HilbertLutTexture->SetName(L"GTAO_HilbertLUT");
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&Desc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    CD3DX12_HEAP_PROPERTIES UploadHeap(D3D12_HEAP_TYPE_UPLOAD);

    CD3DX12_RESOURCE_DESC UploadDesc = CD3DX12_RESOURCE_DESC::Buffer(UploadBufferSize);

    ComPtr<ID3D12Resource> UploadResource;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(UploadResource.GetAddressOf())));

    uint8_t* UploadData = nullptr;
    const D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&UploadData)));
    const size_t RowBytes = HilbertWidth * sizeof(uint16_t);
    for (uint32_t Row = 0; Row < NumRows; ++Row)
    {
        std::memcpy(UploadData + Row * Layout.Footprint.RowPitch, &Data[Row * HilbertWidth], RowBytes);
    }
    UploadResource->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"DeferredRenderer_HilbertUpload_CL");

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = HilbertLutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadResource.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = HilbertLutTexture.Get();
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

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

void FDeferredLightingPasses::AddGtaoPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle GtaoHandle = Context.Resources.GtaoHandle;

    struct FGtaoPassData
    {
        bool bEnabled = false;
        uint32_t PipelineIndex = 0;
    };

    Graph.AddPass<FGtaoPassData>("GTAO", [&Owner, GBufferHandles, LinearDepthHandle, GtaoHandle](FGtaoPassData& Data, FRGPassBuilder& Builder)
    {
        Data.PipelineIndex = Owner.bGtaoJitterEnabled ? 1u : 0u;
        Data.bEnabled = Owner.bGtaoEnabled && Owner.GtaoRootSignature && Owner.GtaoPipelines[Data.PipelineIndex];
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(GtaoHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner](const FGtaoPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent GtaoEvent(LocalCommandList, L"GTAO");

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.GtaoRtvHandle, nullptr);

        const float ClearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        LocalCommandList->ClearRenderTargetView(Owner.GtaoRtvHandle, ClearColor, 0, nullptr);

        LocalCommandList->SetPipelineState(Owner.GtaoPipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.GtaoRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const uint32_t GtaoBindlessIndices[] =
        {
            Owner.GBufferBindlessIndices[0],
            Owner.LinearDepthBindlessIndex,
            Owner.HilbertLutBindlessIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(GtaoBindlessIndices), GtaoBindlessIndices, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredLightingPasses::AddSsrRayCounterClearPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;

    struct FSsrRayCounterClearPassData
    {
    };

    Graph.AddPass<FSsrRayCounterClearPassData>("SSR RayCounter Clear", [&Owner, FrameIndex, &Graph](FSsrRayCounterClearPassData& Data, FRGPassBuilder& Builder)
    {
        if (FrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || FrameIndex >= Owner.SsrRayCounterHwMissBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle PrimaryHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            Owner.SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle HwMissHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            Owner.SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);

        Builder.WriteBuffer(PrimaryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FSsrRayCounterClearPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrClearEvent(LocalCommandList, L"SSR RayCounter Clear");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= Owner.SsrRayCounterHwMissBuffers.size())
        {
            return;
        }

        ID3D12Resource* PrimaryCounterBuffer = Owner.SsrRayCounterPrimaryBuffers[LocalFrameIndex].Get();
        ID3D12Resource* HwMissCounterBuffer = Owner.SsrRayCounterHwMissBuffers[LocalFrameIndex].Get();
        const uint32_t PrimaryCounterUavIndex = Owner.SsrRayCounterPrimaryUavBindlessIndices[LocalFrameIndex];
        const uint32_t HwMissCounterUavIndex = Owner.SsrRayCounterHwMissUavBindlessIndices[LocalFrameIndex];

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

void FDeferredLightingPasses::AddSsrRayGatherPass(FDeferredPassContext& Context) const
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

    Graph.AddPass<FSsrRayGatherPassData>("SSR Ray Gather", [&Owner, FrameIndex, GBufferHandles, LinearDepthHandle, &Graph](FSsrRayGatherPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.SsrRayGatherPipeline && Owner.SsrRayGatherRootSignature;
        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || FrameIndex >= Owner.SsrRayListPrimaryBuffers.size())
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
        RayListDesc.Size = static_cast<uint64_t>(Owner.SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            Owner.SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListPrimary",
            Owner.SsrRayListPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayListPrimaryStates[FrameIndex],
            RayListDesc);

        Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FSsrRayGatherPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= Owner.SsrRayListPrimaryBuffers.size())
        {
            return;
        }

        const uint32_t RayCounterUavIndex = Owner.SsrRayCounterPrimaryUavBindlessIndices[LocalFrameIndex];
        const uint32_t RayListUavIndex = Owner.SsrRayListPrimaryUavBindlessIndices[LocalFrameIndex];
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
        LocalCommandList->SetPipelineState(Owner.SsrRayGatherPipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.SsrRayGatherRootSignature.Get());
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
            Owner.SsrSamplesPerQuad,
            Owner.SsrMaxRayCount,
            Owner.SsrMaxDistance,
            Owner.SsrRoughnessCutoff,
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

void FDeferredLightingPasses::AddSsrBuildIndirectArgsPass(FDeferredPassContext& Context, bool bHwMiss) const
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
    Graph.AddPass<FSsrBuildIndirectArgsPassData>(PassName, [&Owner, FrameIndex, bHwMiss, &Graph](FSsrBuildIndirectArgsPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.SsrBuildIndirectArgsPipeline && Owner.SsrBuildIndirectArgsRootSignature;
        Data.bHwMiss = bHwMiss;
        if (!Data.bEnabled)
        {
            return;
        }

        const bool bValidFrame = bHwMiss
            ? (FrameIndex < Owner.SsrRayCounterHwMissBuffers.size() && FrameIndex < Owner.SsrIndirectArgsHwMissBuffers.size())
            : (FrameIndex < Owner.SsrRayCounterPrimaryBuffers.size() && FrameIndex < Owner.SsrIndirectArgsPrimaryBuffers.size());
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
            bHwMiss ? Owner.SsrRayCounterHwMissBuffers[FrameIndex].Get() : Owner.SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            bHwMiss ? &Owner.SsrRayCounterHwMissStates[FrameIndex] : &Owner.SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            bHwMiss ? "SSR_IndirectArgsHwMiss" : "SSR_IndirectArgsPrimary",
            bHwMiss ? Owner.SsrIndirectArgsHwMissBuffers[FrameIndex].Get() : Owner.SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(),
            bHwMiss ? &Owner.SsrIndirectArgsHwMissStates[FrameIndex] : &Owner.SsrIndirectArgsPrimaryStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(CounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(IndirectHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner, PassLabel](const FSsrBuildIndirectArgsPassData& Data, FDX12CommandContext& Cmd)
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
            if (LocalFrameIndex >= Owner.SsrRayCounterHwMissBuffers.size() || LocalFrameIndex >= Owner.SsrIndirectArgsHwMissBuffers.size())
            {
                return;
            }
        }
        else
        {
            if (LocalFrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= Owner.SsrIndirectArgsPrimaryBuffers.size())
            {
                return;
            }
        }

        ID3D12Resource* RayCounterBuffer = Data.bHwMiss ? Owner.SsrRayCounterHwMissBuffers[LocalFrameIndex].Get() : Owner.SsrRayCounterPrimaryBuffers[LocalFrameIndex].Get();
        ID3D12Resource* IndirectArgsBuffer = Data.bHwMiss ? Owner.SsrIndirectArgsHwMissBuffers[LocalFrameIndex].Get() : Owner.SsrIndirectArgsPrimaryBuffers[LocalFrameIndex].Get();
        const uint32_t RayCounterSrvIndex = Data.bHwMiss ? Owner.SsrRayCounterHwMissSrvBindlessIndices[LocalFrameIndex] : Owner.SsrRayCounterPrimarySrvBindlessIndices[LocalFrameIndex];
        const uint32_t IndirectArgsUavIndex = Data.bHwMiss ? Owner.SsrIndirectArgsHwMissUavBindlessIndices[LocalFrameIndex] : Owner.SsrIndirectArgsPrimaryUavBindlessIndices[LocalFrameIndex];

        if (!RayCounterBuffer || !IndirectArgsBuffer || RayCounterSrvIndex == UINT32_MAX || IndirectArgsUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->SetPipelineState(Owner.SsrBuildIndirectArgsPipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.SsrBuildIndirectArgsRootSignature.Get());

        constexpr uint32_t ThreadGroupSizeX = 64u;
        const uint32_t Constants[] = { ThreadGroupSizeX, Owner.SsrMaxRayCount };
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

        const uint32_t BindlessIndices[] = { RayCounterSrvIndex, IndirectArgsUavIndex };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

        LocalCommandList->Dispatch(1, 1, 1);
    });
}

void FDeferredLightingPasses::AddSsrSwTracePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.TaaHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle HZBHandle = Context.Resources.HZBHandle;
    const FRGResourceHandle SsrHandle = Context.Resources.SsrHandle;

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

    Graph.AddPass<FSsrSwTracePassData>("SSR SW Trace", [&Owner, FrameIndex, FrameState, TaaHandles, LinearDepthHandle, HZBHandle, SsrHandle, &Graph](FSsrSwTracePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseHzb = Owner.bSsrHzbEnabled && Owner.bHZBReady && Owner.HZBSrvBindlessIndex != UINT32_MAX;
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = (Data.bUseHzb ? 2u : 0u) + (Owner.bSsrRefineEnabled ? 1u : 0u) + (Owner.bSsrSwEnabled ? 0u : 4u);
        Data.bEnabled = (Owner.bSsrSwEnabled || Owner.bSsrHwEnabled) && Owner.SsrSwTraceRootSignature;

        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || FrameIndex >= Owner.SsrRayListPrimaryBuffers.size()
            || FrameIndex >= Owner.SsrRayCounterHwMissBuffers.size() || FrameIndex >= Owner.SsrRayListHwMissBuffers.size()
            || FrameIndex >= Owner.SsrIndirectArgsPrimaryBuffers.size())
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
        RayListDesc.Size = static_cast<uint64_t>(Owner.SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterPrimary",
            Owner.SsrRayCounterPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterPrimaryStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListPrimary",
            Owner.SsrRayListPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrRayListPrimaryStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle HwMissCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            Owner.SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle HwMissListHandle = Graph.ImportBuffer(
            "SSR_RayListHwMiss",
            Owner.SsrRayListHwMissBuffers[FrameIndex].Get(),
            &Owner.SsrRayListHwMissStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            "SSR_IndirectArgsPrimary",
            Owner.SsrIndirectArgsPrimaryBuffers[FrameIndex].Get(),
            &Owner.SsrIndirectArgsPrimaryStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteBuffer(HwMissCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(HwMissListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }, [&Owner, &Graph](const FSsrSwTracePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (Owner.GBufferBindlessIndices[2] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX || Owner.SsrUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t LocalFrameIndex = Cmd.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.SsrRayCounterPrimaryBuffers.size() || LocalFrameIndex >= Owner.SsrRayListPrimaryBuffers.size()
            || LocalFrameIndex >= Owner.SsrRayCounterHwMissBuffers.size() || LocalFrameIndex >= Owner.SsrRayListHwMissBuffers.size()
            || LocalFrameIndex >= Owner.SsrIndirectArgsPrimaryBuffers.size())
        {
            return;
        }

        const uint32_t RayCounterPrimarySrvIndex = Owner.SsrRayCounterPrimarySrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayListPrimarySrvIndex = Owner.SsrRayListPrimarySrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayCounterHwMissUavIndex = Owner.SsrRayCounterHwMissUavBindlessIndices[LocalFrameIndex];
        const uint32_t RayListHwMissUavIndex = Owner.SsrRayListHwMissUavBindlessIndices[LocalFrameIndex];
        if (RayCounterPrimarySrvIndex == UINT32_MAX || RayListPrimarySrvIndex == UINT32_MAX
            || RayCounterHwMissUavIndex == UINT32_MAX || RayListHwMissUavIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12Resource* IndirectArgsBuffer = Owner.SsrIndirectArgsPrimaryBuffers[LocalFrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        if (!Owner.SsrDispatchCommandSignature)
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

        const D3D12_GPU_DESCRIPTOR_HANDLE OutputGpuHandle = Owner.GetBindlessGpuHandle(Owner.SsrUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE OutputCpuHandle = Owner.GetBindlessCpuClearHandle(Owner.SsrUavBindlessIndex);
        const float ClearValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearUnorderedAccessViewFloat(OutputGpuHandle, OutputCpuHandle, SsrOutput, ClearValues, 0, nullptr);

        if (!Owner.EnsureSsrSwTracePipelineOrFail(Data.PipelineIndex, "SSR SW Trace"))
        {
            return;
        }

        LocalCommandList->SetPipelineState(Owner.SsrSwTracePipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetComputeRootSignature(Owner.SsrSwTraceRootSignature.Get());
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
            Owner.SsrMaxSteps,
            Owner.SsrMaxDistance,
            Owner.SsrThickness,
            Owner.SsrStride,
            Owner.SsrRoughnessCutoff,
            Owner.SsrIntensity,
            Owner.HZBWidth,
            Owner.HZBHeight,
            Owner.HZBMipCount,
            Data.bUseHzb ? 1u : 0u,
            Owner.SsrMaxRayCount
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, sizeof(FSsrSwTraceConstants) / sizeof(uint32_t), &Constants, 0);

        const uint32_t SceneColorIndex = Data.bUseHistory && Data.HistoryIndex < Owner.TaaSrvBindlessIndices.size()
            ? Owner.TaaSrvBindlessIndices[Data.HistoryIndex]
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
            Owner.SsrUavBindlessIndex,
            HzbIndex,
            Owner.Device->GetPointClampSamplerIndex(),
            Owner.Device->GetLinearClampSamplerIndex()
        };
        LocalCommandList->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        LocalCommandList->ExecuteIndirect(Owner.SsrDispatchCommandSignature.Get(), 1, IndirectArgsBuffer, 0, nullptr, 0);
    });
}

void FDeferredLightingPasses::AddSsrHwTracePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FCamera& Camera = Context.Camera;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.TaaHandles;
    const FRGResourceHandle SsrHandle = Context.Resources.SsrHandle;

    struct FSsrHwTracePassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        uint32_t HistoryIndex = 0;
        FRGResourceHandle SceneColorHandle{};
        FRGResourceHandle SsrHandle{};
        const FCamera* Camera = nullptr;
    };

    Graph.AddPass<FSsrHwTracePassData>("SSR HW Trace", [&Owner, FrameIndex, FrameState, &Camera, TaaHandles, SsrHandle, &Graph](FSsrHwTracePassData& Data, FRGPassBuilder& Builder)
    {
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bEnabled = Owner.bSsrHwEnabled && Owner.bRayTracingPipelineReady && Owner.RayQueryRootSignature && Owner.RayQuerySsrHwPipeline;
        if (!Data.bEnabled)
        {
            return;
        }

        if (FrameIndex >= Owner.SsrRayCounterHwMissBuffers.size() || FrameIndex >= Owner.SsrRayListHwMissBuffers.size()
            || FrameIndex >= Owner.SsrIndirectArgsHwMissBuffers.size())
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
        RayListDesc.Size = static_cast<uint64_t>(Owner.SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc IndirectDesc = {};
        IndirectDesc.Size = sizeof(D3D12_DISPATCH_ARGUMENTS);
        IndirectDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounterHwMiss",
            Owner.SsrRayCounterHwMissBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterHwMissStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayListHwMiss",
            Owner.SsrRayListHwMissBuffers[FrameIndex].Get(),
            &Owner.SsrRayListHwMissStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle IndirectHandle = Graph.ImportBuffer(
            "SSR_IndirectArgsHwMiss",
            Owner.SsrIndirectArgsHwMissBuffers[FrameIndex].Get(),
            &Owner.SsrIndirectArgsHwMissStates[FrameIndex],
            IndirectDesc);

        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }, [&Owner, &Graph](const FSsrHwTracePassData& Data, FDX12CommandContext& CmdContext)
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

        if (Owner.GBufferBindlessIndices[2] == UINT32_MAX || Owner.SsrUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t LocalFrameIndex = CmdContext.GetCurrentFrameIndex();
        if (LocalFrameIndex >= Owner.TlasResultBuffers.size() || !Owner.TlasResultBuffers[LocalFrameIndex])
        {
            return;
        }

        const uint32_t RayCounterHwMissIndex = Owner.SsrRayCounterHwMissSrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayListHwMissIndex = Owner.SsrRayListHwMissSrvBindlessIndices[LocalFrameIndex];
        if (RayCounterHwMissIndex == UINT32_MAX || RayListHwMissIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12Resource* IndirectArgsBuffer = Owner.SsrIndirectArgsHwMissBuffers[LocalFrameIndex].Get();
        if (!IndirectArgsBuffer)
        {
            return;
        }

        if (!Owner.SsrDispatchCommandSignature)
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

        const uint32_t SceneColorIndex = Data.bUseHistory && Data.HistoryIndex < Owner.TaaSrvBindlessIndices.size()
            ? Owner.TaaSrvBindlessIndices[Data.HistoryIndex]
            : Owner.GBufferBindlessIndices[2];
        if (SceneColorIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        if (OutputWidth == 0 || OutputHeight == 0 || Owner.SsrMaxRayCount == 0)
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
            Owner.SsrUavBindlessIndex,
            SceneColorIndex,
            PathTracingInstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            Owner.Device->GetLinearClampSamplerIndex(),
            Owner.SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            0u,
            0u,
            0u
        };
        static_assert(sizeof(float) == sizeof(uint32_t), "Float size mismatch.");
        std::memcpy(&BindlessIndices[10], &Owner.SsrIntensity, sizeof(float));
        std::memcpy(&BindlessIndices[11], &Owner.SsrRoughnessCutoff, sizeof(float));
        CommandList4->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);

        CommandList4->ExecuteIndirect(Owner.SsrDispatchCommandSignature.Get(), 1, IndirectArgsBuffer, 0, nullptr, 0);
    });
}

void FDeferredLightingPasses::AddSsrResolvePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle SsrInputHandle = Context.Resources.SsrHandle;
    const FRGResourceHandle SsrResolveHandle = Context.Resources.SsrResolveHandle;

    struct FSsrResolvePassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrResolvePassData>("SSR Resolve", [&Owner, GBufferHandles, LinearDepthHandle, SsrInputHandle, SsrResolveHandle](FSsrResolvePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.SsrResolvePipeline && Owner.SsrResolveRootSignature;
        if (!Data.bEnabled)
        {
            return;
        }

        Builder.ReadTexture(SsrInputHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SsrResolveHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FSsrResolvePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (Owner.SsrBindlessIndex == UINT32_MAX || Owner.SsrResolveUavBindlessIndex == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX
            || Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrResolveEvent(LocalCommandList, L"SSR Resolve");
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetPipelineState(Owner.SsrResolvePipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.SsrResolveRootSignature.Get());

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
            Owner.SsrBindlessIndex,
            Owner.SsrResolveUavBindlessIndex,
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

void FDeferredLightingPasses::AddSsrPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.TaaHandles;
    const FRGResourceHandle HZBHandle = Context.Resources.HZBHandle;
    const FRGResourceHandle SsrHandle = Context.Resources.SsrHandle;

    struct FSsrPassData
    {
        bool bEnabled = false;
        bool bUseHistory = false;
        uint32_t HistoryIndex = 0;
        bool bUseHzb = false;
        uint32_t PipelineIndex = 0;
    };

    Graph.AddPass<FSsrPassData>("SSR", [&Owner, FrameIndex, GBufferHandles, LinearDepthHandle, TaaHandles, HZBHandle, SsrHandle, FrameState, &Graph](FSsrPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseHzb = Owner.bSsrHzbEnabled && Owner.bHZBReady && Owner.HZBSrvBindlessIndex != UINT32_MAX;
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bUseHzb = Data.bUseHzb && static_cast<bool>(HZBHandle);
        Data.PipelineIndex = (Data.bUseHzb ? 2u : 0u) + (Owner.bSsrRefineEnabled ? 1u : 0u) + (Owner.bSsrSwEnabled ? 0u : 4u);
        Data.bEnabled = (Owner.bSsrSwEnabled || Owner.bSsrHwEnabled) && Owner.SsrRootSignature;

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

        if (FrameIndex >= Owner.SsrRayCounterBuffers.size() || FrameIndex >= Owner.SsrRayListBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(Owner.SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounter",
            Owner.SsrRayCounterBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterStates[FrameIndex],
            CounterDesc);
        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayList",
            Owner.SsrRayListBuffers[FrameIndex].Get(),
            &Owner.SsrRayListStates[FrameIndex],
            RayListDesc);

        Builder.WriteBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(RayListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FSsrPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrEvent(LocalCommandList, L"SSR");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.SsrRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(Owner.SsrRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        if (Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t HistoryIndex = Data.bUseHistory && Data.HistoryIndex < Owner.TaaSrvBindlessIndices.size()
            ? Owner.TaaSrvBindlessIndices[Data.HistoryIndex]
            : Owner.GBufferBindlessIndices[2];
        if (HistoryIndex == UINT32_MAX)
        {
            return;
        }

        if (!Owner.EnsureSsrGraphicsPipelineOrFail(Data.PipelineIndex, "SSR"))
        {
            return;
        }

        LocalCommandList->SetPipelineState(Owner.SsrPipelines[Data.PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.SsrRootSignature.Get());

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
        if (LocalFrameIndex < Owner.SsrRayCounterBuffers.size() && LocalFrameIndex < Owner.SsrRayListBuffers.size())
        {
            RayCounterBuffer = Owner.SsrRayCounterBuffers[LocalFrameIndex].Get();
            RayCounterUavIndex = Owner.SsrRayCounterUavBindlessIndices[LocalFrameIndex];
            RayListUavIndex = Owner.SsrRayListUavBindlessIndices[LocalFrameIndex];
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
            Owner.SsrMaxSteps,
            Owner.SsrThickness,
            Owner.SsrMaxDistance,
            Owner.SsrStride,
            Owner.SsrRoughnessCutoff,
            Owner.SsrIntensity,
            (Owner.bSsrHwEnabled && RayCounterUavIndex != UINT32_MAX && RayListUavIndex != UINT32_MAX) ? Owner.SsrMaxRayCount : 0u,
            Data.bUseHistory ? 1u : 0u,
            Owner.HZBWidth,
            Owner.HZBHeight,
            Owner.HZBMipCount,
            Data.bUseHzb ? 1u : 0u,
            Owner.bSsrHwEnabled ? 1u : 0u
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

void FDeferredLightingPasses::AddSsrFallbackPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const uint32_t FrameIndex = Context.FrameIndex;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FCamera& Camera = Context.Camera;
    const std::vector<FRGResourceHandle>& TaaHandles = Context.Resources.TaaHandles;
    const FRGResourceHandle SsrFallbackHandle = Context.Resources.SsrFallbackHandle;

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

    Graph.AddPass<FSsrFallbackPassData>("SSR Fallback", [&Owner, FrameIndex, FrameState, &Camera, TaaHandles, SsrFallbackHandle, &Graph](FSsrFallbackPassData& Data, FRGPassBuilder& Builder)
    {
        Data.HistoryIndex = FrameState.TaaReadIndex;
        Data.bUseHistory = FrameState.bTaaHistoryReady && Data.HistoryIndex < TaaHandles.size();
        Data.bEnabled = static_cast<bool>(SsrFallbackHandle);
        Data.bDoRayTracing = Owner.bSsrHwEnabled && Data.bUseHistory;
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

        if (FrameIndex >= Owner.SsrRayListBuffers.size() || FrameIndex >= Owner.SsrRayCounterBuffers.size())
        {
            return;
        }

        FRGBufferDesc CounterDesc = {};
        CounterDesc.Size = sizeof(uint32_t);
        CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        FRGBufferDesc RayListDesc = {};
        RayListDesc.Size = static_cast<uint64_t>(Owner.SsrMaxRayCount) * SsrRayItemStride;
        RayListDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const FRGBufferHandle RayListHandle = Graph.ImportBuffer(
            "SSR_RayList",
            Owner.SsrRayListBuffers[FrameIndex].Get(),
            &Owner.SsrRayListStates[FrameIndex],
            RayListDesc);
        const FRGBufferHandle RayCounterHandle = Graph.ImportBuffer(
            "SSR_RayCounter",
            Owner.SsrRayCounterBuffers[FrameIndex].Get(),
            &Owner.SsrRayCounterStates[FrameIndex],
            CounterDesc);

        Builder.ReadBuffer(RayListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(RayCounterHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }, [&Owner, &Graph](const FSsrFallbackPassData& Data, FDX12CommandContext& CmdContext)
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

        const uint32_t FallbackUavIndex = Owner.SsrFallbackUavBindlessIndex;
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

        if (LocalFrameIndex >= Owner.SsrRayListBuffers.size() || LocalFrameIndex >= Owner.SsrRayCounterBuffers.size())
        {
            return;
        }

        const uint32_t RayListIndex = Owner.SsrRayListSrvBindlessIndices[LocalFrameIndex];
        const uint32_t RayCounterIndex = Owner.SsrRayCounterSrvBindlessIndices[LocalFrameIndex];
        if (RayListIndex == UINT32_MAX || RayCounterIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t SceneColorIndex = Data.HistoryIndex < Owner.TaaSrvBindlessIndices.size()
            ? Owner.TaaSrvBindlessIndices[Data.HistoryIndex]
            : UINT32_MAX;
        if (SceneColorIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t OutputWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t OutputHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        if (OutputWidth == 0 || OutputHeight == 0 || Owner.SsrMaxRayCount == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 64;
        const uint32_t DispatchCount = (Owner.SsrMaxRayCount + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

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
            Owner.SsrMaxRayCount,
            OutputWidth,
            OutputHeight,
            0u,
            0u,
            0u
        };
        static_assert(sizeof(float) == sizeof(uint32_t), "Float size mismatch.");
        std::memcpy(&BindlessIndices[10], &Owner.SsrIntensity, sizeof(float));
        std::memcpy(&BindlessIndices[11], &Owner.SsrRoughnessCutoff, sizeof(float));
        CommandList4->SetComputeRoot32BitConstants(2, static_cast<UINT>(BindlessIndices.size()), BindlessIndices.data(), 0);
        CommandList4->Dispatch(DispatchCount, 1, 1);
    });
}

void FDeferredLightingPasses::AddSsrDenoisePass(FDeferredPassContext& Context, FRGResourceHandle InputHandle) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle SsrDenoiseHandle = Context.Resources.SsrDenoiseHandle;

    struct FSsrDenoisePassData
    {
        bool bEnabled = false;
    };

    Graph.AddPass<FSsrDenoisePassData>("SSR Denoise", [&Owner, InputHandle, GBufferHandles, LinearDepthHandle, SsrDenoiseHandle](FSsrDenoisePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = (Owner.bSsrSwEnabled || Owner.bSsrHwEnabled) && Owner.bSsrDenoiseEnabled && Owner.SsrDenoiseRootSignature && Owner.SsrDenoisePipeline;

        Builder.ReadTexture(InputHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(SsrDenoiseHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner](const FSsrDenoisePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent SsrDenoiseEvent(LocalCommandList, L"SSR Denoise");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.SsrDenoiseRtvHandle, nullptr);

        const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        LocalCommandList->ClearRenderTargetView(Owner.SsrDenoiseRtvHandle, ClearColor, 0, nullptr);

        if (!Data.bEnabled)
        {
            return;
        }

        const uint32_t SsrInputIndex = (Owner.SsrMode == ESSRMode::CS) ? Owner.SsrResolveBindlessIndex : Owner.SsrBindlessIndex;
        if (SsrInputIndex == UINT32_MAX || Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.LinearDepthBindlessIndex == UINT32_MAX)
        {
            return;
        }

        LocalCommandList->SetPipelineState(Owner.SsrDenoisePipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.SsrDenoiseRootSignature.Get());

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

void FDeferredLightingPasses::AddLightingPass(FDeferredPassContext& Context, FRGResourceHandle SsrHandle) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GtaoHandle = Context.Resources.GtaoHandle;
    const FRGResourceHandle RestirGIHandle = Context.Resources.RestirGiHistoryIrradianceHandle;
    const FRGResourceHandle SsrFallbackHandle = Context.Resources.SsrFallbackHandle;
    const FRGResourceHandle ShadowHandle = Context.Resources.ShadowHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;

    struct FLightingPassData
    {
        bool bUseShadows = false;
    };

    Graph.AddPass<FLightingPassData>("Lighting", [&](FLightingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseShadows = FrameState.bRenderShadows;

        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[3], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GtaoHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SsrHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SsrFallbackHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (Data.bUseShadows)
        {
            Builder.ReadTexture(ShadowHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner](const FLightingPassData&, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent LightingEvent(LocalCommandList, L"Lighting");

        if (!Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        const uint32_t DepthIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[DepthIndex];
        const uint32_t BaseSsrIndex = (Owner.SsrMode == ESSRMode::CS) ? Owner.SsrResolveBindlessIndex : Owner.SsrBindlessIndex;
        const uint32_t SsrLightingBindlessIndex = Owner.bSsrDenoiseEnabled ? Owner.SsrDenoiseBindlessIndex : BaseSsrIndex;
        const uint32_t SsrFallbackIndex = Owner.SsrFallbackBindlessIndex;
        if (DepthBindlessIndex == UINT32_MAX || Owner.GtaoBindlessIndex == UINT32_MAX || Owner.RestirGIBindlessIndex == UINT32_MAX || SsrLightingBindlessIndex == UINT32_MAX || SsrFallbackIndex == UINT32_MAX || Owner.ShadowMapBindlessIndex == UINT32_MAX
            || Owner.EnvironmentCubeBindlessIndex == UINT32_MAX || Owner.BrdfLutBindlessIndex == UINT32_MAX
            || Owner.GBufferBindlessIndices[0] == UINT32_MAX || Owner.GBufferBindlessIndices[1] == UINT32_MAX || Owner.GBufferBindlessIndices[2] == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.LightingRTVHandle, nullptr);

        const bool bUseShadowMask = Owner.bShadowsEnabled && Owner.bRayTracedShadowsEnabled && Owner.bRayTracingPipelineReady && Owner.ShadowMaskBindlessIndex != UINT32_MAX;
        const uint32_t PipelineIndex = (bUseShadowMask ? 1u : 0u) | (Owner.bEnablePbrResearch ? 2u : 0u);
        LocalCommandList->SetPipelineState(Owner.LightingPipelines[PipelineIndex].Get());
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
            Owner.GtaoBindlessIndex,
            Owner.RestirGIBindlessIndex,
            SsrLightingBindlessIndex,
            SsrFallbackIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(LightingBindlessIndices), LightingBindlessIndices, 0);

        struct FRestirGIConstants
        {
            float Intensity = 0.0f;
            uint32_t Enabled = 0;
            uint32_t SamplesPerPixel = 0;
            uint32_t ShowOnly = 0;
            uint32_t Padding = 0;
        };

        const float EffectiveRestirGIIntensity = (std::max)(0.0f, Owner.RestirGIIntensity);
        const uint32_t EffectiveRestirGISamples = std::clamp(Owner.RestirGISamplesPerPixel, 1u, 32u);

        const FRestirGIConstants RestirGIConstants =
        {
            1.0f,
            Owner.bRestirGIEnabled ? 1u : 0u,
            EffectiveRestirGISamples,
            Owner.bRestirGIShowOnly ? 1u : 0u,
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


