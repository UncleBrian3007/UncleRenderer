#include "DeferredBasePass.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
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

namespace
{
    constexpr uint32_t GDeferredBasePassDoubleSidedBit = 1u << 8;
    constexpr uint32_t GDeferredBasePassClusterDagDebugBit = 1u << 9;

    bool IsClusterDagLightingVisualizationMode(EDeferredLightingVisualizationMode Mode)
    {
        return Mode == EDeferredLightingVisualizationMode::ClusterDagClusters
            || Mode == EDeferredLightingVisualizationMode::ClusterDagMip;
    }

    uint32_t BuildDeferredBasePassPipelineKey(uint32_t ModelPipelineKey, EDeferredLightingVisualizationMode VisualizationMode)
    {
        return (ModelPipelineKey & 0xFFu)
            | (((ModelPipelineKey >> 9) & 1u) << 8)
            | (IsClusterDagLightingVisualizationMode(VisualizationMode) ? GDeferredBasePassClusterDagDebugBit : 0u);
    }

    uint32_t BuildDeferredBasePassVsVariantIndex(bool bUseSkinning, bool bUseClusterDagDebugView)
    {
        return (bUseSkinning ? 1u : 0u) | (bUseClusterDagDebugView ? 2u : 0u);
    }
}

bool FDeferredBasePass::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* InDevice, DXGI_FORMAT LightingBufferFormat)
{
    this->Owner = &Owner;
    Device = InDevice;
    if (!CreateBasePassRootSignature(Device)
        || !CreateBasePassPipeline(Device, LightingBufferFormat)
        || !Owner.ObjectId->InitializePipelines(Device, BasePassRootSignature.Get())
        || !CreateDepthPrepassPipeline(Device)
        || !CreateVelocityRootSignature(Device)
        || !CreateVelocityPipeline(Device))
    {
        return false;
    }

    const std::vector<std::wstring> ShadowDefines;
    const std::vector<std::wstring> ShadowSkinnedDefines = { L"USE_SKINNING=1" };
    return Owner.CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowDefines, ShadowPipelines[0], false)
        && Owner.CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowSkinnedDefines, ShadowPipelinesSkinned[0], false)
        && Owner.CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowDefines, ShadowPipelines[1], true)
        && Owner.CreateShadowPipeline(Device, BasePassRootSignature.Get(), ShadowSkinnedDefines, ShadowPipelinesSkinned[1], true);
}

bool FDeferredBasePass::InitializeResources(FDeferredRenderer& Owner, FDX12Device* InDevice, uint32_t Width, uint32_t Height) const
{
    if (!Owner.CreateDepthResourcesPerFrame(InDevice, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT)
        || !Owner.ObjectId->InitializeResources(InDevice, Width, Height)
        || !Owner.CreateShadowResources(InDevice, Owner.ShadowMapWidth, Owner.ShadowMapHeight, Owner.ShadowMap, Owner.ShadowDSVHeap, Owner.ShadowDSVHandle)
        || !CreateGBufferResources(InDevice, Width, Height)
        || !CreateVelocityResources(InDevice, Width, Height))
    {
        return false;
    }

    return true;
}

bool FDeferredBasePass::CreateGBufferResources(FDX12Device* InDevice, uint32_t Width, uint32_t Height) const
{
    if (Owner == nullptr)
    {
        return false;
    }

    FBindlessTexture* Targets[4] = { &Owner->GBufferA, &Owner->GBufferB, &Owner->GBufferC, &Owner->GBufferD };
    const wchar_t* GBufferNames[4] = { L"GBufferA", L"GBufferB", L"GBufferC", L"GBufferD" };

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    const UINT RtvDescriptorSize = InDevice->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = {};
    RtvHandle.ptr = 0;

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 6;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(InDevice->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(Owner->GBufferRTVHeap.GetAddressOf())));
    if (Owner->GBufferRTVHeap)
    {
        Owner->GBufferRTVHeap->SetName(L"GBufferRTVHeap");
    }

    RtvHandle = Owner->GBufferRTVHeap->GetCPUDescriptorHandleForHeapStart();

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

        const FLOAT Color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        CD3DX12_CLEAR_VALUE ClearValue(Desc.Format, Color);

        HR_CHECK(InDevice->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &ClearValue,
            IID_PPV_ARGS(Targets[i]->GetAddressOf())));

        Targets[i]->Get()->SetName(GBufferNames[i]);
        InitializeBindlessTexture(*Targets[i], { Width, Height, FDeferredRenderer::GBufferFormats[i] }, D3D12_RESOURCE_STATE_RENDER_TARGET);

        Owner->GBufferRTVHandles[i] = RtvHandle;
        D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
        RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        RtvDesc.Format = FDeferredRenderer::GBufferFormats[i];
        InDevice->GetDevice()->CreateRenderTargetView(Targets[i]->Get(), &RtvDesc, RtvHandle);
        RtvHandle.ptr += RtvDescriptorSize;

        Owner->GBufferStates[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        FDeferredRenderer::LightingBufferFormat,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    const FLOAT LightingClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    CD3DX12_CLEAR_VALUE LightingClear(Desc.Format, LightingClearColor);

    HR_CHECK(InDevice->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &LightingClear,
        IID_PPV_ARGS(Owner->LightingBuffer.GetAddressOf())));

    Owner->LightingBuffer->SetName(L"LightingBuffer");
    InitializeBindlessTexture(Owner->LightingBuffer, { Width, Height, FDeferredRenderer::LightingBufferFormat }, D3D12_RESOURCE_STATE_RENDER_TARGET);

    Owner->LightingRTVHandle = RtvHandle;
    D3D12_RENDER_TARGET_VIEW_DESC LightingRtvDesc = {};
    LightingRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    LightingRtvDesc.Format = FDeferredRenderer::LightingBufferFormat;
    InDevice->GetDevice()->CreateRenderTargetView(Owner->LightingBuffer.Get(), &LightingRtvDesc, RtvHandle);
    RtvHandle.ptr += RtvDescriptorSize;

    Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        Owner->BackBufferFormat,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    const FLOAT TonemapClearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    CD3DX12_CLEAR_VALUE TonemapClear(Desc.Format, TonemapClearColor);

    HR_CHECK(InDevice->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &TonemapClear,
        IID_PPV_ARGS(Owner->TonemapOutput.GetAddressOf())));

    Owner->TonemapOutput->SetName(L"TonemapOutput");
    InitializeBindlessTexture(Owner->TonemapOutput, { Width, Height, Owner->BackBufferFormat }, D3D12_RESOURCE_STATE_RENDER_TARGET);

    Owner->TonemapOutputRtvHandle = RtvHandle;
    D3D12_RENDER_TARGET_VIEW_DESC TonemapRtvDesc = {};
    TonemapRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    TonemapRtvDesc.Format = Owner->BackBufferFormat;
    InDevice->GetDevice()->CreateRenderTargetView(Owner->TonemapOutput.Get(), &TonemapRtvDesc, RtvHandle);

    return true;
}

bool FDeferredBasePass::CreateVelocityResources(FDX12Device* InDevice, uint32_t Width, uint32_t Height) const
{
    if (Owner == nullptr)
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

    const FLOAT Color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_CLEAR_VALUE ClearValue(DXGI_FORMAT_R16G16B16A16_FLOAT, Color);

    HR_CHECK(InDevice->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(Owner->VelocityTexture.GetAddressOf())));

    if (Owner->VelocityTexture)
    {
        Owner->VelocityTexture->SetName(L"Velocity");
    }
    InitializeBindlessTexture(Owner->VelocityTexture, { Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT }, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 1;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(InDevice->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(Owner->VelocityRtvHeap.GetAddressOf())));

    if (Owner->VelocityRtvHeap)
    {
        Owner->VelocityRtvHeap->SetName(L"VelocityRtvHeap");
    }

    Owner->VelocityRtvHandle = Owner->VelocityRtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    InDevice->GetDevice()->CreateRenderTargetView(Owner->VelocityTexture.Get(), &RtvDesc, Owner->VelocityRtvHandle);

    Owner->bHasPreviousViewProjection = false;
    Owner->bHasPreviousUnjitteredViewProjection = false;
    return true;
}

bool FDeferredBasePass::CreateBasePassRootSignature(FDX12Device* InDevice)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    // RootParams[0]: Scene constant buffer (b0), used in Shaders/DeferredBasePass.hlsl VSMain and PSMain
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[1]: Base pass bindless indices (b1), used in Shaders/DeferredBasePass.hlsl PSMain
    RootParams[1].InitAsConstants(10, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    // RootParams[2]: Per-draw index-buffer start (b2), used in Shaders/DeferredBasePass.hlsl VSMain
    RootParams[2].InitAsConstants(1, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX);


    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
        0,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f, 4,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams), RootParams,
        1, &SamplerDesc,
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

    HR_CHECK(InDevice->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(BasePassRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredBasePass::CreateBasePassPipeline(FDX12Device* InDevice, DXGI_FORMAT LightingFormat)
{
    FShaderCompiler Compiler;

    if (!RendererUtils::CompileVertexShader(Compiler, InDevice, L"Shaders/DeferredBasePass.hlsl", DeferredBasePassVsBytecodes[0]))
    {
        return false;
    }
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/DeferredBasePass.hlsl", DeferredBasePassVsBytecodes[1], { L"USE_SKINNING=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/DeferredBasePass.hlsl", DeferredBasePassVsBytecodes[2], { L"USE_CLUSTER_DAG_DEBUG_VIEW=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/DeferredBasePass.hlsl", DeferredBasePassVsBytecodes[3], { L"USE_SKINNING=1", L"USE_CLUSTER_DAG_DEBUG_VIEW=1" }))
    {
        return false;
    }

    DeferredBasePassLightingFormat = LightingFormat;
    for (size_t PipelineKey = 0; PipelineKey < BasePassPipelines.size(); ++PipelineKey)
    {
        BasePassPipelines[PipelineKey].Reset();
        BasePassPipelinesSkinned[PipelineKey].Reset();
        DeferredBasePassPsBytecodes[PipelineKey].clear();
        DeferredBasePassPsCompiled[PipelineKey] = false;
        DeferredBasePassFailureLogged[PipelineKey] = false;
    }

    return true;
}

bool FDeferredBasePass::CreateDepthPrepassPipeline(FDX12Device* InDevice)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> VSByteCodeSkinned;

    if (!RendererUtils::CompileVertexShader(Compiler, InDevice, L"Shaders/DeferredBasePass.hlsl", VSByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/DeferredBasePass.hlsl", VSByteCodeSkinned, { L"USE_SKINNING=1" }))
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
        HR_CHECK(InDevice->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipelines[DoubleSidedVariant].GetAddressOf())));
        PsoDesc.VS = { VSByteCodeSkinned.data(), VSByteCodeSkinned.size() };
        HR_CHECK(InDevice->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipelinesSkinned[DoubleSidedVariant].GetAddressOf())));
    }
    return true;
}

bool FDeferredBasePass::CompileDeferredBasePassPs(uint32_t PipelineKey, std::vector<uint8_t>& OutPs) const
{
    FShaderCompiler Compiler;

    const bool bUseNormal = (PipelineKey & 1u) != 0;
    const bool bUseMR = (PipelineKey & 2u) != 0;
    const bool bUseBaseColor = (PipelineKey & 4u) != 0;
    const bool bUseEmissive = (PipelineKey & 8u) != 0;
    const bool bUseAlphaMask = (PipelineKey & 16u) != 0;
    const bool bUseSheenModel = (PipelineKey & 32u) != 0;
    const bool bUseClearcoatModel = (PipelineKey & 64u) != 0;
    const bool bUseAnisotropyModel = (PipelineKey & 128u) != 0;
    const bool bUseDoubleSided = (PipelineKey & GDeferredBasePassDoubleSidedBit) != 0;
    const bool bUseClusterDagDebugView = (PipelineKey & GDeferredBasePassClusterDagDebugBit) != 0;

    std::vector<std::wstring> Defines;
    Defines.push_back(bUseNormal ? L"USE_NORMAL_MAP=1" : L"USE_NORMAL_MAP=0");
    Defines.push_back(bUseMR ? L"USE_METALLIC_ROUGHNESS_MAP=1" : L"USE_METALLIC_ROUGHNESS_MAP=0");
    Defines.push_back(bUseBaseColor ? L"USE_BASE_COLOR_MAP=1" : L"USE_BASE_COLOR_MAP=0");
    Defines.push_back(bUseEmissive ? L"USE_EMISSIVE_MAP=1" : L"USE_EMISSIVE_MAP=0");
    Defines.push_back(bUseSheenModel ? L"SHADINGMODEL_SHEEN=1" : L"SHADINGMODEL_SHEEN=0");
    Defines.push_back(bUseClearcoatModel ? L"SHADINGMODEL_CLEARCOAT=1" : L"SHADINGMODEL_CLEARCOAT=0");
    Defines.push_back(bUseAnisotropyModel ? L"SHADINGMODEL_ANISOTROPY=1" : L"SHADINGMODEL_ANISOTROPY=0");
    Defines.push_back(bUseDoubleSided ? L"USE_DOUBLE_SIDED=1" : L"USE_DOUBLE_SIDED=0");
    Defines.push_back(bUseClusterDagDebugView ? L"USE_CLUSTER_DAG_DEBUG_VIEW=1" : L"USE_CLUSTER_DAG_DEBUG_VIEW=0");
    if (bUseAlphaMask)
    {
        Defines.push_back(L"USE_ALPHA_MASK=1");
    }

    return RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/DeferredBasePass.hlsl", OutPs, Defines);
}

bool FDeferredBasePass::BuildDeferredBasePassPsoDesc(uint32_t PipelineKey, bool bUseSkinning, D3D12_GRAPHICS_PIPELINE_STATE_DESC& OutDesc) const
{
    if (DeferredBasePassLightingFormat == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }

    OutDesc = {};
    OutDesc.pRootSignature = BasePassRootSignature.Get();
    OutDesc.InputLayout = { nullptr, 0 };
    const bool bUseClusterDagDebugView = (PipelineKey & GDeferredBasePassClusterDagDebugBit) != 0;
    const std::vector<uint8_t>& VsBytecode = DeferredBasePassVsBytecodes[BuildDeferredBasePassVsVariantIndex(bUseSkinning, bUseClusterDagDebugView)];
    OutDesc.VS = { VsBytecode.data(), VsBytecode.size() };
    OutDesc.PS = { DeferredBasePassPsBytecodes[PipelineKey].data(), DeferredBasePassPsBytecodes[PipelineKey].size() };
    OutDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    OutDesc.SampleDesc.Count = 1;
    OutDesc.SampleMask = UINT_MAX;

    OutDesc.RasterizerState = {};
    OutDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    OutDesc.RasterizerState.CullMode = (PipelineKey & GDeferredBasePassDoubleSidedBit) != 0 ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
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

bool FDeferredBasePass::EnsureBasePassPipeline(uint32_t PipelineKey, bool bUseSkinning) const
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

bool FDeferredBasePass::EnsureBasePassPipelineOrFail(uint32_t PipelineKey, bool bUseSkinning, const char* PassContext) const
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

    if (Owner)
    {
        Owner->SetRenderFatalError(std::string("Deferred BasePass fatal failure. context=")
            + (PassContext ? PassContext : "Unknown")
            + ", key=" + std::to_string(PipelineKey)
            + ", skinned=" + (bUseSkinning ? "1" : "0"));
    }
    return false;
}

bool FDeferredBasePass::CreateVelocityRootSignature(FDX12Device* InDevice)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[4] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(10, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    RootParams[2].InitAsConstants(1, 2, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    RootParams[3].InitAsConstants(33, 3, 0, D3D12_SHADER_VISIBILITY_PIXEL);


    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
        0,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f, 4,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams), RootParams,
        1, &SamplerDesc,
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

    HR_CHECK(InDevice->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(VelocityRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredBasePass::CreateVelocityPipeline(FDX12Device* InDevice)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> VSByteCodeSkinned;

    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", InDevice->GetShaderModel());

    if (!RendererUtils::CompileVertexShader(Compiler, InDevice, L"Shaders/DeferredBasePass.hlsl", VSByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/DeferredBasePass.hlsl", VSByteCodeSkinned, { L"USE_SKINNING=1" }))
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
            HR_CHECK(InDevice->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(VelocityPipelines[PipelineIndex].GetAddressOf())));

            InitializeVelocityDesc(PsoDesc, VSByteCodeSkinned);
            PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            PsoDesc.PS = { PSByteCodes[PipelineIndex].data(), PSByteCodes[PipelineIndex].size() };
            HR_CHECK(InDevice->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(VelocityPipelinesSkinned[PipelineIndex].GetAddressOf())));
        }
    }

    return true;
}

void FDeferredBasePass::AddShadowPass(FDeferredPassContext& Context) const
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
    }, [this, &Owner](const FShadowPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        Cmd.ClearDepth(Owner.ShadowDSVHandle, 1.0f);

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        ID3D12PipelineState* CurrentShadowPipeline = nullptr;
        const auto SetShadowPipeline = [&](bool bUseSkinning, bool bDoubleSided)
        {
            ID3D12PipelineState* Pipeline = bUseSkinning ? ShadowPipelinesSkinned[bDoubleSided ? 1u : 0u].Get() : ShadowPipelines[bDoubleSided ? 1u : 0u].Get();
            if (Pipeline != CurrentShadowPipeline)
            {
                LocalCommandList->SetPipelineState(Pipeline);
                CurrentShadowPipeline = Pipeline;
            }
        };
        SetShadowPipeline(false, false);

        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());

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
            if (!ShadowVisibility[ModelIndex])
            {
                continue;
            }

            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            if (Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
            SetShadowPipeline(bUseSkinning, Model.bDoubleSided);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t BindlessIndices[] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);
            LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
            }
        }

    });
}

void FDeferredBasePass::AddDepthPrepass(FDeferredPassContext& Context) const
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
    }, [this, &Owner](const FDepthPrepassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();


        Cmd.ClearDepth(Owner.GetDSVHandle());

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthHandle);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            const bool bUseClusterDagIndexBuffer = Owner.ClusterDagRuntime && Owner.ClusterDagRuntime->UsesRuntimePath(Owner, Model);
            if (bUseClusterDagIndexBuffer)
            {
                Owner.UpdateClusterDagSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
            }
            else
            {
                Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
            }
        }

        if (Owner.ClusterDagRuntime && Owner.IndirectCommandSignature && Owner.ClusterDagRuntime->HasResources())
        {
            ID3D12Resource* DagIndirectBuffer = Owner.ClusterDagRuntime->GetIndirectCommandBuffer(Owner);
            ID3D12Resource* DagRunCountBuffer = Owner.ClusterDagRuntime->GetRunCountBuffer(Owner);
            const std::vector<FRenderer::FIndirectDrawRange>& DagRanges = Owner.ClusterDagRuntime->GetIndirectDrawRanges();
            if (DagIndirectBuffer && DagRunCountBuffer && !DagRanges.empty())
            {
                ID3D12PipelineState* CurrentDagPipeline = nullptr;
                const uint32_t ZeroBindlessIndices[10] = {};
                for (size_t RangeIndex = 0; RangeIndex < DagRanges.size(); ++RangeIndex)
                {
                    const FRenderer::FIndirectDrawRange& Range = DagRanges[RangeIndex];
                    const bool bDoubleSided = (Range.PipelineKey & (1u << 9)) != 0;
                    ID3D12PipelineState* DesiredPipeline = DepthPrepassPipelines[bDoubleSided ? 1u : 0u].Get();
                    if (DesiredPipeline != CurrentDagPipeline)
                    {
                        CurrentDagPipeline = DesiredPipeline;
                        LocalCommandList->SetPipelineState(CurrentDagPipeline);
                    }

                    LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(ZeroBindlessIndices), ZeroBindlessIndices, 0);
                    const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
                    const uint64_t CountOffset = RangeIndex * sizeof(uint32_t);
                    LocalCommandList->ExecuteIndirect(Owner.IndirectCommandSignature.Get(), Range.Count, DagIndirectBuffer, Offset, DagRunCountBuffer, CountOffset);
                }
            }
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
            if (Owner.ClusterDagRuntime && Owner.ClusterDagRuntime->UsesRuntimePath(Owner, Model))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
            ID3D12PipelineState* DesiredPipeline = bUseSkinning ? DepthPrepassPipelinesSkinned[Model.bDoubleSided ? 1u : 0u].Get() : DepthPrepassPipelines[Model.bDoubleSided ? 1u : 0u].Get();
            if (DesiredPipeline != CurrentPipeline)
            {
                CurrentPipeline = DesiredPipeline;
                LocalCommandList->SetPipelineState(CurrentPipeline);
            }

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
            const uint32_t BindlessIndices[] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
            LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(BindlessIndices), BindlessIndices, 0);
            LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);

            if (AreModelPixEventsEnabled())
            {
                const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
            }
            else
            {
                LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
            }
        }
    });
}

void FDeferredBasePass::AddBasePass(FDeferredPassContext& Context, bool bClearTargets, bool bClearDepth, const char* PassName, bool bAllowSkinningFallback) const
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
    }, [this, &Owner, PassLabel](const FBasePassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();


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
        LocalCommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        LocalCommandList->OMSetRenderTargets(_countof(BasePassRTVs), BasePassRTVs, FALSE, &DepthHandle);

        for (size_t ModelIndex = 0; ModelIndex < Owner.SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = Owner.SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;
            const bool bUseClusterDagIndexBuffer = Owner.ClusterDagRuntime && Owner.ClusterDagRuntime->UsesRuntimePath(Owner, Model);
            if (bUseClusterDagIndexBuffer)
            {
                Owner.UpdateClusterDagSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
            }
            else
            {
                Owner.UpdateSceneConstants(*Data.Camera, Model, ModelIndex, ConstantBufferOffset);
            }
        }

        if (Owner.ClusterDagRuntime && Owner.IndirectCommandSignature && Owner.ClusterDagRuntime->HasResources())
        {
            ID3D12Resource* DagIndirectBuffer = Owner.ClusterDagRuntime->GetIndirectCommandBuffer(Owner);
            ID3D12Resource* DagRunCountBuffer = Owner.ClusterDagRuntime->GetRunCountBuffer(Owner);
            const std::vector<FRenderer::FIndirectDrawRange>& DagRanges = Owner.ClusterDagRuntime->GetIndirectDrawRanges();
            if (DagIndirectBuffer && DagRunCountBuffer && !DagRanges.empty())
            {
                auto SelectPipelineByKey = [&](uint32_t Key) -> ID3D12PipelineState*
                {
                    const bool bUseSkinning = (Key & (1u << 8)) != 0;
                    const uint32_t MaterialKey = BuildDeferredBasePassPipelineKey(Key, Owner.GetDeferredLightingVisualizationMode());
                    if (!EnsureBasePassPipelineOrFail(MaterialKey, bUseSkinning, "DeferredBasePass/ClusterDagIndirect"))
                    {
                        return nullptr;
                    }
                    return bUseSkinning ? BasePassPipelinesSkinned[MaterialKey].Get() : BasePassPipelines[MaterialKey].Get();
                };

                for (size_t RangeIndex = 0; RangeIndex < DagRanges.size(); ++RangeIndex)
                {
                    const FRenderer::FIndirectDrawRange& Range = DagRanges[RangeIndex];
                    ID3D12PipelineState* Pipeline = SelectPipelineByKey(Range.PipelineKey);
                    if (!Pipeline)
                    {
                        return;
                    }
                    LocalCommandList->SetPipelineState(Pipeline);
                    LocalCommandList->SetGraphicsRoot32BitConstants(1, static_cast<UINT>(Range.MaterialBindlessIndices.size()), Range.MaterialBindlessIndices.data(), 0);

                    const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
                    const uint64_t CountOffset = RangeIndex * sizeof(uint32_t);
                    LocalCommandList->ExecuteIndirect(Owner.IndirectCommandSignature.Get(), Range.Count, DagIndirectBuffer, Offset, DagRunCountBuffer, CountOffset);
                }
            }
        }

        ID3D12Resource* IndirectBuffer = Owner.GetIndirectCommandBuffer();
        ID3D12Resource* RunCountBuffer = Owner.GetMeshletRunCountBuffer();
        if (Owner.bEnableIndirectDraw && Owner.IndirectCommandSignature && IndirectBuffer && RunCountBuffer && !Owner.IndirectDrawRanges.empty())
        {
            auto SelectPipelineByKey = [&](uint32_t Key) -> ID3D12PipelineState*
            {
                const bool bUseSkinning = (Key & (1u << 8)) != 0;
                const uint32_t MaterialKey = (Key & 0xFFu) | (((Key >> 9) & 1u) << 8);
                if (!EnsureBasePassPipelineOrFail(MaterialKey, bUseSkinning, "DeferredBasePass/Indirect"))
                {
                    return nullptr;
                }
                return bUseSkinning ? BasePassPipelinesSkinned[MaterialKey].Get() : BasePassPipelines[MaterialKey].Get();
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
                    if (Owner.ClusterDagRuntime && Owner.ClusterDagRuntime->UsesRuntimePath(Owner, Model))
                    {
                        continue;
                    }

                    const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
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
                    LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);

                    const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                    const uint32_t PipelineKey = BuildDeferredBasePassPipelineKey(ModelPipelineKey, Owner.GetDeferredLightingVisualizationMode());
                    if (!EnsureBasePassPipelineOrFail(PipelineKey, true, "DeferredBasePass/SkinningFallback"))
                    {
                        return;
                    }
                    LocalCommandList->SetPipelineState(BasePassPipelinesSkinned[PipelineKey].Get());

                    if (AreModelPixEventsEnabled())
                    {
                        const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                        FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
                    }
                    else
                    {
                        LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
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
                if (Owner.ClusterDagRuntime && Owner.ClusterDagRuntime->UsesRuntimePath(Owner, Model))
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
                LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);

                const uint32_t ModelPipelineKey = RendererUtils::BuildPipelineKey(Model);
                const bool bUseSkinning = (ModelPipelineKey & (1u << 8)) != 0;
                const uint32_t PipelineKey = BuildDeferredBasePassPipelineKey(ModelPipelineKey, Owner.GetDeferredLightingVisualizationMode());
                if (!EnsureBasePassPipelineOrFail(PipelineKey, bUseSkinning, "DeferredBasePass/Direct"))
                {
                    return;
                }
                LocalCommandList->SetPipelineState(bUseSkinning ? BasePassPipelinesSkinned[PipelineKey].Get() : BasePassPipelines[PipelineKey].Get());

                if (AreModelPixEventsEnabled())
                {
                    const std::wstring ModelLabel = Model.Name.empty() ? L"Model" : std::wstring(Model.Name.begin(), Model.Name.end());
                    FScopedPixEvent ModelEvent(LocalCommandList, ModelLabel.c_str());
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
                }
                else
                {
                    LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
                }
            }
        }

    });
}

void FDeferredBasePass::AddVelocityPass(FDeferredPassContext& Context) const
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
    }, [this, &Owner](const FVelocityPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cmd.ClearRenderTarget(Owner.VelocityRtvHandle, ClearValue);

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(VelocityRootSignature.Get());
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
        LocalCommandList->SetGraphicsRoot32BitConstants(3, sizeof(FVelocityPassConstants) / sizeof(uint32_t), &VelocityConstants, 0);

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
                const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
                const bool bWorldMoved = Model.bHasPreviousWorldMatrix && IsWorldTransformChanged(Model.WorldMatrix, Model.PreviousWorldMatrix);
                const bool bSkinningMoved = bUseSkinning && Data.bAnySkinningUpdated && Model.bSkinningUpdatedThisFrame;
                bNeedsVelocity = bWorldMoved || bSkinningMoved;
            }

            if (!bNeedsVelocity)
            {
                continue;
            }

            const bool bUseAlphaMask = Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Mask);
            const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBindlessIndex) && Model.BoneMatrixCount > 0;
            const uint32_t PipelineIndex = (bUseAlphaMask ? 1u : 0u) | (Model.bDoubleSided ? 2u : 0u);
            ID3D12PipelineState* Pipeline = bUseSkinning ? VelocityPipelinesSkinned[PipelineIndex].Get() : VelocityPipelines[PipelineIndex].Get();

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
            LocalCommandList->SetGraphicsRoot32BitConstants(2, 1, &Model.DrawIndexStart, 0);
            LocalCommandList->DrawInstanced(Model.DrawIndexCount, 1, 0, 0);
        }
    });
}
