#include "DeferredGeometryPasses.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../TextureLoader.h"
#include "../../Core/Logger.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"

#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <d3dx12.h>

bool FDeferredGeometryPasses::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT LightingBufferFormat) const
{
    if (!Owner.CreateBasePassRootSignature(Device)
        || !Owner.CreateBasePassPipeline(Device, LightingBufferFormat)
        || !Owner.CreateObjectIdPipeline(Device)
        || !Owner.CreateDepthPrepassPipeline(Device)
        || !Owner.CreateVelocityRootSignature(Device)
        || !Owner.CreateVelocityPipeline(Device))
    {
        return false;
    }

    const std::vector<std::wstring> ShadowDefines;
    const std::vector<std::wstring> ShadowSkinnedDefines = { L"USE_SKINNING=1" };
    return Owner.CreateShadowPipeline(Device, Owner.BasePassRootSignature.Get(), ShadowDefines, Owner.ShadowPipelines[0], false)
        && Owner.CreateShadowPipeline(Device, Owner.BasePassRootSignature.Get(), ShadowSkinnedDefines, Owner.ShadowPipelinesSkinned[0], false)
        && Owner.CreateShadowPipeline(Device, Owner.BasePassRootSignature.Get(), ShadowDefines, Owner.ShadowPipelines[1], true)
        && Owner.CreateShadowPipeline(Device, Owner.BasePassRootSignature.Get(), ShadowSkinnedDefines, Owner.ShadowPipelinesSkinned[1], true);
}

bool FDeferredGeometryPasses::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const
{
    if (!Owner.CreateDepthResourcesPerFrame(Device, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT)
        || !Owner.CreateObjectIdResources(Device, Width, Height)
        || !Owner.CreateShadowResources(Device, Owner.ShadowMapWidth, Owner.ShadowMapHeight, Owner.ShadowMap, Owner.ShadowDSVHeap, Owner.ShadowDSVHandle, Owner.ShadowMapState)
        || !Owner.CreateGBufferResources(Device, Width, Height)
        || !Owner.CreateVelocityResources(Device, Width, Height))
    {
        return false;
    }

    if (Owner.ObjectIdTexture)
    {
        Owner.ObjectIdTexture->SetName(L"ObjectIdTexture");
    }
    if (Owner.ObjectIdRtvHeap)
    {
        Owner.ObjectIdRtvHeap->SetName(L"ObjectIdRtvHeap");
    }
    if (Owner.ObjectIdReadback)
    {
        Owner.ObjectIdReadback->SetName(L"ObjectIdReadback");
    }

    return true;
}

bool FDeferredRenderer::CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    Microsoft::WRL::ComPtr<ID3D12Resource>* Targets[4] = { &GBufferA, &GBufferB, &GBufferC, &GBufferD };
    const wchar_t* GBufferNames[4] = { L"GBufferA", L"GBufferB", L"GBufferC", L"GBufferD" };

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    const UINT RtvDescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = {};
    RtvHandle.ptr = 0;

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 6;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(GBufferRTVHeap.GetAddressOf())));
    if (GBufferRTVHeap)
    {
        GBufferRTVHeap->SetName(L"GBufferRTVHeap");
    }

    RtvHandle = GBufferRTVHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < 4; ++i)
    {
        CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
            FDeferredRenderer::GBufferFormats[i],
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
        ClearValue.Color[3] = 1.0f;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &ClearValue,
            IID_PPV_ARGS(Targets[i]->GetAddressOf())));

        Targets[i]->Get()->SetName(GBufferNames[i]);

        GBufferRTVHandles[i] = RtvHandle;
        D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
        RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        RtvDesc.Format = FDeferredRenderer::GBufferFormats[i];
        Device->GetDevice()->CreateRenderTargetView(Targets[i]->Get(), &RtvDesc, RtvHandle);
        RtvHandle.ptr += RtvDescriptorSize;

        GBufferStates[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        LightingBufferFormat,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    D3D12_CLEAR_VALUE LightingClear = {};
    LightingClear.Format = Desc.Format;
    LightingClear.Color[0] = 0.0f;
    LightingClear.Color[1] = 0.0f;
    LightingClear.Color[2] = 0.0f;
    LightingClear.Color[3] = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        LightingBufferState,
        &LightingClear,
        IID_PPV_ARGS(LightingBuffer.GetAddressOf())));

    LightingBuffer->SetName(L"LightingBuffer");

    LightingRTVHandle = RtvHandle;
    D3D12_RENDER_TARGET_VIEW_DESC LightingRtvDesc = {};
    LightingRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    LightingRtvDesc.Format = LightingBufferFormat;
    Device->GetDevice()->CreateRenderTargetView(LightingBuffer.Get(), &LightingRtvDesc, RtvHandle);
    RtvHandle.ptr += RtvDescriptorSize;

    Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        BackBufferFormat,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    D3D12_CLEAR_VALUE TonemapClear = {};
    TonemapClear.Format = Desc.Format;
    TonemapClear.Color[0] = 0.0f;
    TonemapClear.Color[1] = 0.0f;
    TonemapClear.Color[2] = 0.0f;
    TonemapClear.Color[3] = 1.0f;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        TonemapOutputState,
        &TonemapClear,
        IID_PPV_ARGS(TonemapOutput.GetAddressOf())));

    TonemapOutput->SetName(L"TonemapOutput");

    TonemapOutputRtvHandle = RtvHandle;
    D3D12_RENDER_TARGET_VIEW_DESC TonemapRtvDesc = {};
    TonemapRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    TonemapRtvDesc.Format = BackBufferFormat;
    Device->GetDevice()->CreateRenderTargetView(TonemapOutput.Get(), &TonemapRtvDesc, RtvHandle);

    return true;
}

bool FDeferredRenderer::CreateBasePassRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Scene constant buffer (b0), used in Shaders/DeferredBasePass.hlsl VSMain and PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Base pass bindless indices (b1), used in Shaders/DeferredBasePass.hlsl PSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.Num32BitValues = 10;


    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.MipLODBias = 0.0f;
    SamplerDesc.MaxAnisotropy = 4;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(BasePassRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateBasePassPipeline(FDX12Device* Device, DXGI_FORMAT LightingFormat)
{
    FShaderCompiler Compiler;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, DeferredBasePassVsBytecodes[0]))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, DeferredBasePassVsBytecodes[1], { L"USE_SKINNING=1" }))
    {
        return false;
    }

    DeferredBasePassLightingFormat = LightingFormat;
    for (uint32_t PipelineKey = 0; PipelineKey < 512; ++PipelineKey)
    {
        BasePassPipelines[PipelineKey].Reset();
        BasePassPipelinesSkinned[PipelineKey].Reset();
        DeferredBasePassPsBytecodes[PipelineKey].clear();
        DeferredBasePassPsCompiled[PipelineKey] = false;
        DeferredBasePassFailureLogged[PipelineKey] = false;
    }

    return true;
}

bool FDeferredRenderer::CreateDepthPrepassPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> VSByteCodeSkinned;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCodeSkinned, { L"USE_SKINNING=1" }))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = BasePassRootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Single-sided and double-sided depth prepass pipelines are created per variant
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PsoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;
    PsoDesc.RasterizerState.MultisampleEnable = FALSE;
    PsoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    PsoDesc.RasterizerState.ForcedSampleCount = 0;
    PsoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    PsoDesc.BlendState.IndependentBlendEnable = FALSE;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    PsoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    PsoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    PsoDesc.DepthStencilState.BackFace = PsoDesc.DepthStencilState.FrontFace;
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
    {
        PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipelines[DoubleSidedVariant].GetAddressOf())));
        PsoDesc.VS = { VSByteCodeSkinned.data(), VSByteCodeSkinned.size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipelinesSkinned[DoubleSidedVariant].GetAddressOf())));
    }
    return true;
}

bool FDeferredRenderer::CompileDeferredBasePassPs(uint32_t PipelineKey, std::vector<uint8_t>& OutPs)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    const bool bUseNormal = (PipelineKey & 1u) != 0;
    const bool bUseMR = (PipelineKey & 2u) != 0;
    const bool bUseBaseColor = (PipelineKey & 4u) != 0;
    const bool bUseEmissive = (PipelineKey & 8u) != 0;
    const bool bUseAlphaMask = (PipelineKey & 16u) != 0;
    const bool bUseSheenModel = (PipelineKey & 32u) != 0;
    const bool bUseClearcoatModel = (PipelineKey & 64u) != 0;
    const bool bUseAnisotropyModel = (PipelineKey & 128u) != 0;
    const bool bUseDoubleSided = (PipelineKey & 256u) != 0;

    std::vector<std::wstring> Defines;
    Defines.push_back(bUseNormal ? L"USE_NORMAL_MAP=1" : L"USE_NORMAL_MAP=0");
    Defines.push_back(bUseMR ? L"USE_METALLIC_ROUGHNESS_MAP=1" : L"USE_METALLIC_ROUGHNESS_MAP=0");
    Defines.push_back(bUseBaseColor ? L"USE_BASE_COLOR_MAP=1" : L"USE_BASE_COLOR_MAP=0");
    Defines.push_back(bUseEmissive ? L"USE_EMISSIVE_MAP=1" : L"USE_EMISSIVE_MAP=0");
    Defines.push_back(bUseSheenModel ? L"SHADINGMODEL_SHEEN=1" : L"SHADINGMODEL_SHEEN=0");
    Defines.push_back(bUseClearcoatModel ? L"SHADINGMODEL_CLEARCOAT=1" : L"SHADINGMODEL_CLEARCOAT=0");
    Defines.push_back(bUseAnisotropyModel ? L"SHADINGMODEL_ANISOTROPY=1" : L"SHADINGMODEL_ANISOTROPY=0");
    Defines.push_back(bUseDoubleSided ? L"USE_DOUBLE_SIDED=1" : L"USE_DOUBLE_SIDED=0");
    if (bUseAlphaMask)
    {
        Defines.push_back(L"USE_ALPHA_MASK=1");
    }

    return Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, OutPs, Defines);
}

bool FDeferredRenderer::BuildDeferredBasePassPsoDesc(uint32_t PipelineKey, bool bUseSkinning, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const
{
    if (DeferredBasePassLightingFormat == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    OutDesc = {};
    OutDesc.pRootSignature = BasePassRootSignature.Get();
    OutDesc.InputLayout = { nullptr, 0 };
    const std::vector<uint8_t>& VsBytecode = DeferredBasePassVsBytecodes[bUseSkinning ? 1u : 0u];
    OutDesc.VS = { VsBytecode.data(), VsBytecode.size() };
    OutDesc.PS = { DeferredBasePassPsBytecodes[PipelineKey].data(), DeferredBasePassPsBytecodes[PipelineKey].size() };
    OutDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    OutDesc.SampleDesc.Count = 1;
    OutDesc.SampleMask = UINT_MAX;

    OutDesc.RasterizerState = {};
    OutDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    OutDesc.RasterizerState.CullMode = (PipelineKey & 256u) != 0 ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
    OutDesc.RasterizerState.FrontCounterClockwise = TRUE;
    OutDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    OutDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    OutDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    OutDesc.RasterizerState.DepthClipEnable = TRUE;
    OutDesc.RasterizerState.MultisampleEnable = FALSE;
    OutDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    OutDesc.RasterizerState.ForcedSampleCount = 0;
    OutDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    OutDesc.BlendState = {};
    OutDesc.BlendState.AlphaToCoverageEnable = FALSE;
    OutDesc.BlendState.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 5; ++i)
    {
        D3D12_RENDER_TARGET_BLEND_DESC RtBlend = {};
        RtBlend.BlendEnable = FALSE;
        RtBlend.LogicOpEnable = FALSE;
        RtBlend.SrcBlend = D3D12_BLEND_ONE;
        RtBlend.DestBlend = D3D12_BLEND_ZERO;
        RtBlend.BlendOp = D3D12_BLEND_OP_ADD;
        RtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
        RtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
        RtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        RtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
        RtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        OutDesc.BlendState.RenderTarget[i] = RtBlend;
    }

    OutDesc.DepthStencilState = {};
    OutDesc.DepthStencilState.DepthEnable = TRUE;
    OutDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    OutDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    OutDesc.DepthStencilState.StencilEnable = FALSE;
    OutDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    OutDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    OutDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    OutDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    OutDesc.DepthStencilState.BackFace = OutDesc.DepthStencilState.FrontFace;
    OutDesc.NumRenderTargets = 5;
    OutDesc.RTVFormats[0] = FDeferredRenderer::GBufferFormats[0];
    OutDesc.RTVFormats[1] = FDeferredRenderer::GBufferFormats[1];
    OutDesc.RTVFormats[2] = FDeferredRenderer::GBufferFormats[2];
    OutDesc.RTVFormats[3] = FDeferredRenderer::GBufferFormats[3];
    OutDesc.RTVFormats[4] = DeferredBasePassLightingFormat;
    OutDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    OutDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return true;
}

bool FDeferredRenderer::EnsureBasePassPipeline(uint32_t PipelineKey, bool bUseSkinning)
{
    auto& TargetPipeline = bUseSkinning ? BasePassPipelinesSkinned[PipelineKey] : BasePassPipelines[PipelineKey];
    if (TargetPipeline)
    {
        return true;
    }

    std::lock_guard<std::mutex> Lock(DeferredBasePassPipelineMutex);
    if (TargetPipeline)
    {
        return true;
    }

    if (!DeferredBasePassPsCompiled[PipelineKey])
    {
        if (!CompileDeferredBasePassPs(PipelineKey, DeferredBasePassPsBytecodes[PipelineKey]))
        {
            return false;
        }
        DeferredBasePassPsCompiled[PipelineKey] = true;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc = {};
    if (!BuildDeferredBasePassPsoDesc(PipelineKey, bUseSkinning, Desc))
    {
        return false;
    }

    HRESULT Hr = Device->GetDevice()->CreateGraphicsPipelineState(&Desc, IID_PPV_ARGS(TargetPipeline.GetAddressOf()));
    if (FAILED(Hr))
    {
        return false;
    }

    LogInfo(std::string("Deferred BasePass pipeline created. key=") + std::to_string(PipelineKey) + ", skinned=" + (bUseSkinning ? "1" : "0"));
    return true;
}

bool FDeferredRenderer::EnsureBasePassPipelineOrFail(uint32_t PipelineKey, bool bUseSkinning, const char* PassContext)
{
    if (EnsureBasePassPipeline(PipelineKey, bUseSkinning))
    {
        return true;
    }

    if (!DeferredBasePassFailureLogged[PipelineKey])
    {
        DeferredBasePassFailureLogged[PipelineKey] = true;
        LogError(std::string("Deferred BasePass pipeline creation failed. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", key=" + std::to_string(PipelineKey)
            + ", skinned=" + (bUseSkinning ? "1" : "0"));
    }

    SetRenderFatalError(std::string("Deferred BasePass fatal failure. context=")
        + (PassContext ? PassContext : "Unknown")
        + ", key=" + std::to_string(PipelineKey)
        + ", skinned=" + (bUseSkinning ? "1" : "0"));
    return false;
}

bool FDeferredRenderer::CreateObjectIdResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    const bool bCreated = RendererUtils::CreateObjectIdResources(
        Device,
        Width,
        Height,
        ObjectIdTexture,
        ObjectIdRtvHeap,
        ObjectIdRtvHandle,
        ObjectIdReadback,
        ObjectIdFootprint,
        ObjectIdRowPitch);
    if (bCreated)
    {
        ObjectIdState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    return bCreated;
}

bool FDeferredRenderer::CreateObjectIdPipeline(FDX12Device* Device)
{
    return RendererUtils::CreateObjectIdPipeline(Device, BasePassRootSignature.Get(), ObjectIdPipeline);
}

bool FDeferredRenderer::CreateVelocityRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.Num32BitValues = 10;

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[2].Constants.ShaderRegister = 2;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.Num32BitValues = 33;


    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_ANISOTROPIC;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.MipLODBias = 0.0f;
    SamplerDesc.MaxAnisotropy = 4;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(VelocityRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateVelocityPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> VSByteCodeSkinned;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCodeSkinned, { L"USE_SKINNING=1" }))
    {
        return false;
    }

    std::array<std::vector<uint8_t>, 4> PSByteCodes;
    for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
    {
        for (uint32_t Permutation = 0; Permutation < 2; ++Permutation)
        {
            const uint32_t PipelineIndex = Permutation | (DoubleSidedVariant << 1);
            std::vector<std::wstring> Defines;
            Defines.push_back(Permutation != 0 ? L"USE_ALPHA_MASK=1" : L"USE_ALPHA_MASK=0");
            Defines.push_back(DoubleSidedVariant != 0 ? L"USE_DOUBLE_SIDED=1" : L"USE_DOUBLE_SIDED=0");
            if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMainVelocity", PSTarget, PSByteCodes[PipelineIndex], Defines))
            {
                return false;
            }
        }
    }

    auto InitializeVelocityDesc = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, const std::vector<uint8_t>& VertexShader)
    {
        Desc = {};
        Desc.pRootSignature = VelocityRootSignature.Get();
        Desc.InputLayout = { nullptr, 0 };
        Desc.VS = { VertexShader.data(), VertexShader.size() };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.SampleDesc.Count = 1;
        Desc.SampleMask = UINT_MAX;

        Desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        Desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        Desc.RasterizerState.FrontCounterClockwise = TRUE;

        Desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        Desc.BlendState.IndependentBlendEnable = FALSE;

        Desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        Desc.DepthStencilState.DepthEnable = TRUE;
        Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        Desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

        Desc.NumRenderTargets = 1;
        Desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        Desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
    {
        for (uint32_t Permutation = 0; Permutation < 2; ++Permutation)
        {
            const uint32_t PipelineIndex = Permutation | (DoubleSidedVariant << 1);
            InitializeVelocityDesc(PsoDesc, VSByteCode);
            PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            PsoDesc.PS = { PSByteCodes[PipelineIndex].data(), PSByteCodes[PipelineIndex].size() };
            HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(VelocityPipelines[PipelineIndex].GetAddressOf())));

            InitializeVelocityDesc(PsoDesc, VSByteCodeSkinned);
            PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            PsoDesc.PS = { PSByteCodes[PipelineIndex].data(), PSByteCodes[PipelineIndex].size() };
            HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(VelocityPipelinesSkinned[PipelineIndex].GetAddressOf())));
        }
    }

    return true;
}

bool FDeferredRenderer::CreateVelocityResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
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
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
        IID_PPV_ARGS(VelocityTexture.GetAddressOf())));

    if (VelocityTexture)
    {
        VelocityTexture->SetName(L"Velocity");
    }

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 1;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(VelocityRtvHeap.GetAddressOf())));

    if (VelocityRtvHeap)
    {
        VelocityRtvHeap->SetName(L"VelocityRtvHeap");
    }

    VelocityRtvHandle = VelocityRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Device->GetDevice()->CreateRenderTargetView(VelocityTexture.Get(), &RtvDesc, VelocityRtvHandle);

    VelocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bHasPreviousViewProjection = false;
    bHasPreviousUnjitteredViewProjection = false;
    return true;
}

void FDeferredGeometryPasses::AddShadowPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FShadowPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
    };

    Context.Graph.AddPass<FShadowPassData>("ShadowMap", [&Context](FShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bRenderShadows;
        Data.Camera = &Context.Camera;
        Data.LightViewProjection = Context.FrameState.LightViewProjection;

        if (Context.FrameState.bRenderShadows)
        {
            Builder.WriteTexture(Context.Resources.ShadowHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }, [&Owner](const FShadowPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ShadowEvent(LocalCommandList, L"ShadowMap");
        Cmd.ClearDepth(Owner.ShadowDSVHandle, 1.0f);

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12PipelineState* CurrentShadowPipeline = nullptr;
        const auto SetShadowPipeline = [&](bool bUseSkinning, bool bDoubleSided)
        {
            ID3D12PipelineState* Pipeline = bUseSkinning ? Owner.ShadowPipelinesSkinned[bDoubleSided ? 1u : 0u].Get() : Owner.ShadowPipelines[bDoubleSided ? 1u : 0u].Get();
            if (Pipeline != CurrentShadowPipeline)
            {
                LocalCommandList->SetPipelineState(Pipeline);
                CurrentShadowPipeline = Pipeline;
            }
        };
        SetShadowPipeline(false, false);
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.ShadowViewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ShadowScissor);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &Owner.ShadowDSVHandle);

        std::vector<bool> ShadowVisibility;
        ShadowVisibility.resize(Owner.SceneModels.size(), true);
        DirectX::XMVECTOR ShadowPlanes[6] = {};
        RendererUtils::BuildFrustumPlanesFromMatrix(Data.LightViewProjection, ShadowPlanes);
        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            ShadowVisibility[ModelIndex] = RendererUtils::IsAabbInCameraFrustum(ShadowPlanes, Model.BoundsMin, Model.BoundsMax);
        }

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!ShadowVisibility.empty() && !ShadowVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
            SetShadowPipeline(bUseSkinning, Model.bDoubleSided);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t BindlessIndices[] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
        }

    });
}

void FDeferredGeometryPasses::AddDepthPrepass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FDepthPrepassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FDepthPrepassData>("DepthPrepass", [&Context](FDepthPrepassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bDoDepthPrepass;
        Data.Camera = &Context.Camera;

        if (Context.FrameState.bDoDepthPrepass)
        {
            Builder.WriteTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }, [&Owner](const FDepthPrepassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent DepthEvent(LocalCommandList, L"DepthPrepass");

        Cmd.ClearDepth(Owner.GetDSVHandle());

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthHandle);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        ID3D12PipelineState* CurrentPipeline = nullptr;
        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Mask)
                || Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
            ID3D12PipelineState* DesiredPipeline = bUseSkinning ? Owner.DepthPrepassPipelinesSkinned[Model.bDoubleSided ? 1u : 0u].Get() : Owner.DepthPrepassPipelines[Model.bDoubleSided ? 1u : 0u].Get();
            if (DesiredPipeline != CurrentPipeline)
            {
                CurrentPipeline = DesiredPipeline;
                LocalCommandList->SetPipelineState(CurrentPipeline);
            }

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t BindlessIndices[] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
            }
        }
    });
}
void FDeferredGeometryPasses::AddBasePass(FDeferredPassContext& Context, bool bClearTargets, bool bClearDepth, const char* PassName, bool bAllowSkinningFallback) const
{
    FDeferredRenderer& Owner = Context.Owner;

    const std::wstring PassLabel = PassName
        ? std::wstring(PassName, PassName + std::strlen(PassName))
        : L"GBuffer";
    struct FBasePassData
    {
        bool bDoDepthPrepass = false;
        bool bClearTargets = false;
        bool bClearDepth = false;
        bool bAllowSkinningFallback = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FBasePassData>(PassName, [&Context, bClearTargets, bClearDepth, bAllowSkinningFallback](FBasePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bDoDepthPrepass = Context.FrameState.bDoDepthPrepass;
        Data.bClearTargets = bClearTargets;
        Data.bClearDepth = bClearDepth;
        Data.bAllowSkinningFallback = bAllowSkinningFallback;
        Data.Camera = &Context.Camera;

        for (int i = 0; i < 4; ++i)
        {
            Builder.WriteTexture(Context.Resources.GBufferHandles[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        Builder.WriteTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Builder.WriteTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }, [&Owner, PassLabel](const FBasePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent GBufferEvent(LocalCommandList, PassLabel.c_str());

        D3D12_CPU_DESCRIPTOR_HANDLE BasePassRTVs[5] =
        {
            Owner.GBufferRTVHandles[0],
            Owner.GBufferRTVHandles[1],
            Owner.GBufferRTVHandles[2],
            Owner.GBufferRTVHandles[3],
            Owner.LightingRTVHandle
        };

        if (Data.bClearDepth)
        {
            Cmd.ClearDepth(Owner.GetDSVHandle());
        }

        if (Data.bClearTargets)
        {
            for (const D3D12_CPU_DESCRIPTOR_HANDLE& Handle : Owner.GBufferRTVHandles)
            {
                const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                Cmd.ClearRenderTarget(Handle, ClearValue);
            }

            const float SceneClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            Cmd.ClearRenderTarget(Owner.LightingRTVHandle, SceneClear);
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(_countof(BasePassRTVs), BasePassRTVs, FALSE, &DepthHandle);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        ID3D12Resource* IndirectBuffer = Owner.GetIndirectCommandBuffer();
        ID3D12Resource* RunCountBuffer = Owner.GetMeshletRunCountBuffer();
        if (Owner.bEnableIndirectDraw && Owner.IndirectCommandSignature && IndirectBuffer && RunCountBuffer && !Owner.IndirectDrawRanges.empty())
        {
            auto SelectPipelineByKey = [&](uint32_t Key) -> ID3D12PipelineState*
            {
                const bool bUseSkinning = (Key & (1u << 8)) != 0;
                const uint32_t MaterialKey = (Key & 0xFFu) | (((Key >> 9) & 1u) << 8);
                if (!Owner.EnsureBasePassPipelineOrFail(MaterialKey, bUseSkinning, "DeferredBasePass/Indirect"))
                {
                    return nullptr;
                }
                return bUseSkinning ? Owner.BasePassPipelinesSkinned[MaterialKey].Get() : Owner.BasePassPipelines[MaterialKey].Get();
            };

            for (size_t RangeIndex = 0; RangeIndex < Owner.IndirectDrawRanges.size(); ++RangeIndex)
            {
                const FRenderer::FIndirectDrawRange& Range = Owner.IndirectDrawRanges[RangeIndex];
                const bool bRangeSkinning = (Range.PipelineKey & (1u << 8)) != 0;
                if (bRangeSkinning && !Owner.bEnableSkinningIndirectDraw)
                {
                    continue;
                }
                ID3D12PipelineState* Pipeline = SelectPipelineByKey(Range.PipelineKey);
                if (!Pipeline)
                {
                    return;
                }
                LocalCommandList->SetPipelineState(Pipeline);
                LocalCommandList->SetGraphicsRoot32BitConstants(1, static_cast<UINT>(Range.MaterialBindlessIndices.size()), Range.MaterialBindlessIndices.data(), 0);

                const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
                const uint64_t CountOffset = RangeIndex * sizeof(uint32_t);
                if (AreModelPixEventsEnabled())
                {
                    const wchar_t* Label = Range.Name.empty() ? L"IndirectDrawRange" : Range.Name.c_str();
                    FScopedPixEvent ModelEvent(LocalCommandList, Label);
                    LocalCommandList->ExecuteIndirect(Owner.IndirectCommandSignature.Get(), Range.Count, IndirectBuffer, Offset, RunCountBuffer, CountOffset);
                }
                else
                {
                    LocalCommandList->ExecuteIndirect(Owner.IndirectCommandSignature.Get(), Range.Count, IndirectBuffer, Offset, RunCountBuffer, CountOffset);
                }
            }

            if (!Owner.bEnableSkinningIndirectDraw && Data.bAllowSkinningFallback)
            {
                for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
                {
                    if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
                    {
                        continue;
                    }

                    const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
                    if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
                    {
                        continue;
                    }

                    const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
                    if (!bUseSkinning)
                    {
                        continue;
                    }

                    const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

                    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
                    LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
                    const uint32_t BindlessIndices[] =
                    {
                        Model.BaseColorBindlessIndex,
                        Model.MetallicRoughnessBindlessIndex,
                        Model.NormalBindlessIndex,
                        Model.EmissiveBindlessIndex,
                        Model.SheenColorBindlessIndex,
                        Model.SheenRoughnessBindlessIndex,
                        Model.ClearcoatBindlessIndex,
                        Model.ClearcoatRoughnessBindlessIndex,
                        Model.ClearcoatNormalBindlessIndex,
                        Model.AnisotropyBindlessIndex
                    };
                    LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

                    const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                    const uint32_t PipelineKey = (ModelPipelineKey & 0xFFu) | (((ModelPipelineKey >> 9) & 1u) << 8);
                    if (!Owner.EnsureBasePassPipelineOrFail(PipelineKey, true, "DeferredBasePass/SkinningFallback"))
                    {
                        return;
                    }
                    LocalCommandList->SetPipelineState(Owner.BasePassPipelinesSkinned[PipelineKey].Get());

                    if (AreModelPixEventsEnabled())
                    {
                        const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                        FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                    }
                    else
                    {
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                    }
                }
            }
        }
        else
        {
            for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
            {
                if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
                {
                    continue;
                }

                const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
                if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
                {
                    continue;
                }
                const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

                const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
                LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
                const uint32_t BindlessIndices[] =
                {
                    Model.BaseColorBindlessIndex,
                    Model.MetallicRoughnessBindlessIndex,
                    Model.NormalBindlessIndex,
                    Model.EmissiveBindlessIndex,
                    Model.SheenColorBindlessIndex,
                    Model.SheenRoughnessBindlessIndex,
                    Model.ClearcoatBindlessIndex,
                    Model.ClearcoatRoughnessBindlessIndex,
                    Model.ClearcoatNormalBindlessIndex,
                    Model.AnisotropyBindlessIndex
                };
                LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);

                const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                const bool bUseSkinning = (ModelPipelineKey & (1u << 8)) != 0;
                const uint32_t PipelineKey = (ModelPipelineKey & 0xFFu) | (((ModelPipelineKey >> 9) & 1u) << 8);
                if (!Owner.EnsureBasePassPipelineOrFail(PipelineKey, bUseSkinning, "DeferredBasePass/Direct"))
                {
                    return;
                }
                LocalCommandList->SetPipelineState(bUseSkinning ? Owner.BasePassPipelinesSkinned[PipelineKey].Get() : Owner.BasePassPipelines[PipelineKey].Get());

                if (AreModelPixEventsEnabled())
                {
                    const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                    FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                }
                else
                {
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
                }
            }
        }

    });
}
void FDeferredGeometryPasses::AddObjectIdPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FObjectIdPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FObjectIdPassData>("ObjectId", [&Owner, &Context](FObjectIdPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.bObjectIdReadbackRequested && Owner.ObjectIdPipeline && Owner.ObjectIdTexture;
        Data.Camera = &Context.Camera;
        if (Data.bEnabled)
        {
            Builder.WriteTexture(Context.Resources.ObjectIdHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Builder.ReadTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
        }
    }, [&Owner](const FObjectIdPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent ObjectIdEvent(LocalCommandList, L"ObjectIdPass");

        LocalCommandList->SetPipelineState(Owner.ObjectIdPipeline.Get());
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePassRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(1, &Owner.ObjectIdRtvHandle, FALSE, &DepthHandle);

        const UINT ClearValue[4] = { 0, 0, 0, 0 };
        LocalCommandList->ClearRenderTargetView(Owner.ObjectIdRtvHandle, reinterpret_cast<const float*>(ClearValue), 0, nullptr);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            LocalCommandList->IASetVertexBuffers(0, Model.Geometry.VertexBufferCount, Model.Geometry.VertexBufferViews.data());
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
            else
            {
                LocalCommandList->DrawIndexedInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0, 0);
            }
        }

        const uint32_t Width = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t Height = static_cast<uint32_t>(Owner.Viewport.Height);
        const uint32_t ReadX = (std::min)(Owner.ObjectIdReadbackX, Width > 0 ? Width - 1 : 0);
        const uint32_t ReadY = (std::min)(Owner.ObjectIdReadbackY, Height > 0 ? Height - 1 : 0);

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = Owner.ObjectIdTexture.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        LocalCommandList->ResourceBarrier(1, &Barrier);

        D3D12_TEXTURE_COPY_LOCATION Src = {};
        Src.pResource = Owner.ObjectIdTexture.Get();
        Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst = {};
        Dst.pResource = Owner.ObjectIdReadback.Get();
        Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint = Owner.ObjectIdFootprint;

        D3D12_BOX SourceBox = {};
        SourceBox.left = ReadX;
        SourceBox.top = ReadY;
        SourceBox.front = 0;
        SourceBox.right = ReadX + 1;
        SourceBox.bottom = ReadY + 1;
        SourceBox.back = 1;

        LocalCommandList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &SourceBox);

        std::swap(Barrier.Transition.StateBefore, Barrier.Transition.StateAfter);
        LocalCommandList->ResourceBarrier(1, &Barrier);

        Owner.bObjectIdReadbackRecorded = true;
    });
}

void FDeferredGeometryPasses::AddVelocityPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FVelocityPassData
    {
        const FCamera* Camera = nullptr;
        bool bCameraMoved = false;
        bool bAnySkinningUpdated = false;
    };

    Context.Graph.AddPass<FVelocityPassData>("Velocity", [&Context](FVelocityPassData& Data, FRGPassBuilder& Builder)
    {
        Data.Camera = &Context.Camera;
        Data.bCameraMoved = Context.FrameState.bCameraMoved;
        Data.bAnySkinningUpdated = Context.FrameState.bAnySkinningUpdated;
        Builder.WriteTexture(Context.Resources.VelocityHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Builder.ReadTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
    }, [&Owner](const FVelocityPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Owner.VelocityRootSignature || !Owner.VelocityTexture)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent VelocityEvent(LocalCommandList, L"VelocityPass");

        const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cmd.ClearRenderTarget(Owner.VelocityRtvHandle, ClearValue);

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.VelocityRootSignature.Get());
        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthDsvHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(1, &Owner.VelocityRtvHandle, FALSE, &DepthDsvHandle);

        struct FVelocityPassConstants
        {
            DirectX::XMFLOAT4X4 CurrentUnjitteredViewProjection{};
            DirectX::XMFLOAT4X4 PreviousUnjitteredViewProjection{};
            uint32_t HasPreviousUnjitteredViewProjection = 0u;
        };

        FVelocityPassConstants VelocityConstants = {};
        VelocityConstants.CurrentUnjitteredViewProjection = Owner.CurrentUnjitteredViewProjectionMatrix;
        VelocityConstants.PreviousUnjitteredViewProjection = Owner.PreviousUnjitteredViewProjectionMatrix;
        VelocityConstants.HasPreviousUnjitteredViewProjection = Owner.bHasPreviousUnjitteredViewProjection ? 1u : 0u;
        LocalCommandList->SetGraphicsRoot32BitConstants(2, sizeof(FVelocityPassConstants) / sizeof(uint32_t), &VelocityConstants, 0);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
        }

        const auto IsWorldTransformChanged = [](const DirectX::XMFLOAT4X4& Current, const DirectX::XMFLOAT4X4& Previous)
        {
            const float* CurrentData = reinterpret_cast<const float*>(&Current);
            const float* PreviousData = reinterpret_cast<const float*>(&Previous);
            constexpr float MatrixEpsilon = 1e-6f;
            for (int ElementIndex = 0; ElementIndex < 16; ++ElementIndex)
            {
                if (std::abs(CurrentData[ElementIndex] - PreviousData[ElementIndex]) > MatrixEpsilon)
                {
                    return true;
                }
            }

            return false;
        };

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            if (!Owner.SceneModelVisibility.empty() && !Owner.SceneModelVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }

            bool bNeedsVelocity = Data.bCameraMoved;
            if (!bNeedsVelocity)
            {
                const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
                const bool bWorldMoved = Model.bHasPreviousWorldMatrix && IsWorldTransformChanged(Model.WorldMatrix, Model.PreviousWorldMatrix);
                const bool bSkinningMoved = bUseSkinning && Data.bAnySkinningUpdated && Model.bSkinningUpdatedThisFrame;
                bNeedsVelocity = bWorldMoved || bSkinningMoved;
            }

            if (!bNeedsVelocity)
            {
                continue;
            }

            const bool bUseAlphaMask = Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Mask);
            const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
            const uint32_t PipelineIndex = (bUseAlphaMask ? 1u : 0u) | (Model.bDoubleSided ? 2u : 0u);
            ID3D12PipelineState* Pipeline = bUseSkinning ? Owner.VelocityPipelinesSkinned[PipelineIndex].Get() : Owner.VelocityPipelines[PipelineIndex].Get();
            if (!Pipeline)
            {
                continue;
            }

            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetPipelineState(Pipeline);
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);

            const uint32_t BindlessIndices[] =
            {
                Model.BaseColorBindlessIndex,
                Model.MetallicRoughnessBindlessIndex,
                Model.NormalBindlessIndex,
                Model.EmissiveBindlessIndex,
                Model.SheenColorBindlessIndex,
                Model.SheenRoughnessBindlessIndex,
                Model.ClearcoatBindlessIndex,
                Model.ClearcoatRoughnessBindlessIndex,
                Model.ClearcoatNormalBindlessIndex,
                Model.AnisotropyBindlessIndex
            };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);
            LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, Model.DrawIndexStart, 0);
        }
    });
}

bool FDeferredRenderer::CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models)
{
    if (!TextureLoader)
    {
        return false;
    }

    SceneTextures.clear();
    SceneTextures.reserve(Models.size());

    // Prepare all texture load requests
    std::vector<FTextureLoadRequest> Requests;
    Requests.reserve(Models.size() * 10); // 10 textures per model

    // Pre-allocate texture sets
    for (const FSceneModelResource& Model : Models)
    {
        FModelTextureSet TextureSet;
        SceneTextures.push_back(TextureSet);
    }

    // Build load requests for all textures
    for (size_t i = 0; i < Models.size(); ++i)
    {
        const FSceneModelResource& Model = Models[i];
        FModelTextureSet& TextureSet = SceneTextures[i];

        // Base color texture - skip when missing
        if (!Model.BaseColorTexturePath.empty())
        {
            FTextureLoadRequest BaseColorRequest;
            BaseColorRequest.Path = Model.BaseColorTexturePath;
            BaseColorRequest.bUseSolidColor = false;
            BaseColorRequest.bUseSRGB = true;
            BaseColorRequest.OutTexture = &TextureSet.BaseColor;
            Requests.push_back(BaseColorRequest);
        }

        // Metallic roughness texture - skip when missing
        if (!Model.MetallicRoughnessTexturePath.empty())
        {
            FTextureLoadRequest MetallicRoughnessRequest;
            MetallicRoughnessRequest.Path = Model.MetallicRoughnessTexturePath;
            MetallicRoughnessRequest.bUseSolidColor = false;
            MetallicRoughnessRequest.OutTexture = &TextureSet.MetallicRoughness;
            Requests.push_back(MetallicRoughnessRequest);
        }

		if (!Model.NormalTexturePath.empty())
		{
			FTextureLoadRequest NormalRequest;
            NormalRequest.Path = Model.NormalTexturePath;
            NormalRequest.bUseSolidColor = false;
            NormalRequest.OutTexture = &TextureSet.Normal;
			Requests.push_back(NormalRequest);
		}

        // Emissive texture - skip when missing
        if (!Model.EmissiveTexturePath.empty())
        {
            FTextureLoadRequest EmissiveRequest;
            EmissiveRequest.Path = Model.EmissiveTexturePath;
            EmissiveRequest.bUseSolidColor = false;
            EmissiveRequest.bUseSRGB = true;
            EmissiveRequest.OutTexture = &TextureSet.Emissive;
            Requests.push_back(EmissiveRequest);
        }

        if (!Model.SheenColorTexturePath.empty())
        {
            FTextureLoadRequest SheenColorRequest;
            SheenColorRequest.Path = Model.SheenColorTexturePath;
            SheenColorRequest.bUseSolidColor = false;
            SheenColorRequest.bUseSRGB = true;
            SheenColorRequest.OutTexture = &TextureSet.SheenColor;
            Requests.push_back(SheenColorRequest);
        }

        if (!Model.SheenRoughnessTexturePath.empty())
        {
            FTextureLoadRequest SheenRoughnessRequest;
            SheenRoughnessRequest.Path = Model.SheenRoughnessTexturePath;
            SheenRoughnessRequest.bUseSolidColor = false;
            SheenRoughnessRequest.OutTexture = &TextureSet.SheenRoughness;
            Requests.push_back(SheenRoughnessRequest);
        }

        if (!Model.ClearcoatTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatRequest;
            ClearcoatRequest.Path = Model.ClearcoatTexturePath;
            ClearcoatRequest.bUseSolidColor = false;
            ClearcoatRequest.OutTexture = &TextureSet.Clearcoat;
            Requests.push_back(ClearcoatRequest);
        }

        if (!Model.ClearcoatRoughnessTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatRoughnessRequest;
            ClearcoatRoughnessRequest.Path = Model.ClearcoatRoughnessTexturePath;
            ClearcoatRoughnessRequest.bUseSolidColor = false;
            ClearcoatRoughnessRequest.OutTexture = &TextureSet.ClearcoatRoughness;
            Requests.push_back(ClearcoatRoughnessRequest);
        }

        if (!Model.ClearcoatNormalTexturePath.empty())
        {
            FTextureLoadRequest ClearcoatNormalRequest;
            ClearcoatNormalRequest.Path = Model.ClearcoatNormalTexturePath;
            ClearcoatNormalRequest.bUseSolidColor = false;
            ClearcoatNormalRequest.OutTexture = &TextureSet.ClearcoatNormal;
            Requests.push_back(ClearcoatNormalRequest);
        }

        if (!Model.AnisotropyTexturePath.empty())
        {
            FTextureLoadRequest AnisotropyRequest;
            AnisotropyRequest.Path = Model.AnisotropyTexturePath;
            AnisotropyRequest.bUseSolidColor = false;
            AnisotropyRequest.OutTexture = &TextureSet.Anisotropy;
            Requests.push_back(AnisotropyRequest);
        }
    }

    // Load all textures in parallel
    LogInfo("Loading " + std::to_string(Requests.size()) + " textures in parallel for " + std::to_string(Models.size()) + " models");
    const bool bSuccess = TextureLoader->LoadTexturesParallel(Requests);

    if (!bSuccess)
    {
        LogError("Failed to load scene textures");
    }

    return bSuccess;
}
