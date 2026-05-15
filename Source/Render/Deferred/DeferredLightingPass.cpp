#include "DeferredLightingPass.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../Deferred/RestirGI.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../Core/RendererConfig.h"
#include "../../RHI/DX12Device.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <d3dx12.h>

namespace
{
    constexpr uint32_t kLightingPassBindlessDwordCount          = 14;
    constexpr uint32_t kLightingPassConstantsDwordCount         = 7;
    constexpr uint32_t kLinearDepthBindlessDwordCount           = 1;
    constexpr uint32_t kExtractHalfDepthConstantsDwordCount     = 4;

    enum class ECompositeDiffuseSource : uint32_t
    {
        Environment = 0,
        RestirGI = 1
    };

    enum class ECompositeVisualizationPermutation : uint32_t
    {
        Off = 0,
        On = 1
    };

    constexpr uint32_t CompositeDiffuseSourceCount = 2u;
    constexpr uint32_t CompositeVisualizationPermutationCount = 2u;
    constexpr uint32_t CompositeLightingPipelineCount = CompositeDiffuseSourceCount * CompositeVisualizationPermutationCount;

    uint32_t GetCompositeLightingPipelineIndex(ECompositeDiffuseSource DiffuseSource, ECompositeVisualizationPermutation VisualizationPermutation)
    {
        return static_cast<uint32_t>(DiffuseSource) * CompositeVisualizationPermutationCount + static_cast<uint32_t>(VisualizationPermutation);
    }

    ECompositeDiffuseSource ResolveCompositeDiffuseSource(const FDeferredRenderer& Owner)
    {
        if (Owner.GetRestirGI()->IsEnabled() && Owner.GetRestirGI()->GetIntensity() > 0.0f)
        {
            return ECompositeDiffuseSource::RestirGI;
        }

        return ECompositeDiffuseSource::Environment;
    }

    ECompositeVisualizationPermutation ResolveCompositeVisualizationPermutation(EDeferredLightingVisualizationMode VisualizationMode)
    {
        return (VisualizationMode == EDeferredLightingVisualizationMode::Off)
            ? ECompositeVisualizationPermutation::Off
            : ECompositeVisualizationPermutation::On;
    }

    std::vector<std::wstring> BuildCompositeLightingDefines(ECompositeDiffuseSource DiffuseSource, ECompositeVisualizationPermutation VisualizationPermutation)
    {
        std::vector<std::wstring> Defines;

        switch (DiffuseSource)
        {
        case ECompositeDiffuseSource::Environment:
            Defines.push_back(L"COMPOSITE_DIFFUSE_SOURCE_ENV=1");
            break;
        case ECompositeDiffuseSource::RestirGI:
            Defines.push_back(L"COMPOSITE_DIFFUSE_SOURCE_RESTIR=1");
            break;
        }

        if (VisualizationPermutation == ECompositeVisualizationPermutation::On)
        {
            Defines.push_back(L"COMPOSITE_VISUALIZATION_ON=1");
        }
        else
        {
            Defines.push_back(L"COMPOSITE_VISUALIZATION_OFF=1");
        }

        return Defines;
    }
}

void FDeferredLightingPass::ApplyLightingPassConfig(const FRendererConfig& Config)
{
    bEnablePbrResearch = Config.bEnablePbrResearch;
    DeferredLightingVisualizationMode = Config.DeferredLightingVisualizationMode;
}

EDeferredLightingVisualizationMode FDeferredLightingPass::GetDeferredLightingVisualizationMode() const
{
    return DeferredLightingVisualizationMode;
}

bool FDeferredLightingPass::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    this->Owner = &Owner;
    return CreateLightingRootSignature(Device)
        && CreateLightingPipeline(Device, BackBufferFormat)
        && CreateLinearDepthRootSignature(Device)
        && CreateLinearDepthPipeline(Device)
        && CreateExtractHalfDepthNormalRootSignature(Device)
        && CreateExtractHalfDepthNormalPipeline(Device)
        && Owner.Hzb->InitializePipelines(Owner, Device);
}

bool FDeferredLightingPass::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height) const
{
    return CreateLinearDepthResources(Device, Width, Height)
        && Owner.Hzb->InitializeResources(Owner, Device, Width, Height);
}

bool FDeferredLightingPass::CreateLightingRootSignature(FDX12Device* Device)
{
    FDeferredRenderer& Renderer = *Owner;
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    // RootParams[0]: Lighting constants (b0), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[1]: Lighting bindless indices (b1), used in Shaders/DeferredLighting.hlsl PSMain
    RootParams[1].InitAsConstants(kLightingPassBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    // RootParams[2]: Deferred lighting visualization/constants (b2), used in lighting pixel shaders
    RootParams[2].InitAsConstants(kLightingPassConstantsDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC Samplers[3];
    CD3DX12_STATIC_SAMPLER_DESC::Init(Samplers[0], 0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 0, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
        0.0f, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC::Init(Samplers[1], 1,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f, 0, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC::Init(Samplers[2], 2,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 0, D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
        0.0f, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams), RootParams,
        _countof(Samplers), Samplers,
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(Renderer.LightingRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredLightingPass::CreateLinearDepthRootSignature(FDX12Device* Device)
{
    FDeferredRenderer& Renderer = *Owner;
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: Linear depth constants (b0), used in Shaders/LinearDepth.hlsl VSMain and PSMain
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[1]: Depth bindless index (b1), used in Shaders/LinearDepth.hlsl PSMain
    RootParams[1].InitAsConstants(kLinearDepthBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
        0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 0,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(Renderer.LinearDepthRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredLightingPass::CreateLinearDepthPipeline(FDX12Device* Device)
{
    FDeferredRenderer& Renderer = *Owner;
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/LinearDepth.hlsl", VSByteCode))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/LinearDepth.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = Renderer.LinearDepthRootSignature.Get();
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

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(Renderer.LinearDepthPipeline.GetAddressOf())));
    return true;
}

bool FDeferredLightingPass::CreateExtractHalfDepthNormalRootSignature(FDX12Device* Device)
{
    FDeferredRenderer& Renderer = *Owner;
    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(kExtractHalfDepthConstantsDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(Renderer.ExtractHalfDepthNormalRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredLightingPass::CreateExtractHalfDepthNormalPipeline(FDX12Device* Device)
{
    FDeferredRenderer& Renderer = *Owner;
    FShaderCompiler Compiler;
    std::vector<uint8_t> CSByteCode;

    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ExtractHalfDepthNormal.hlsl", CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = Renderer.ExtractHalfDepthNormalRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };

    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(Renderer.ExtractHalfDepthNormalPipeline.GetAddressOf())));
    return true;
}

bool FDeferredLightingPass::CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FDeferredRenderer& Renderer = *Owner;
    (void)BackBufferFormat;

    FShaderCompiler Compiler;
    std::vector<uint8_t> DirectVSByteCode;
    std::vector<uint8_t> DirectPSByteCodes[4];
    std::vector<uint8_t> CompositeVSByteCode;
    std::array<std::vector<uint8_t>, CompositeLightingPipelineCount> CompositePSByteCodes;


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/DeferredDirectLighting.hlsl", DirectVSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> DefaultDefines;
    const std::vector<std::wstring> ShadowMaskDefines = { L"USE_SHADOW_MASK=1" };
    const std::vector<std::wstring> ResearchDefines = { L"USE_PBR_RESEARCH=1" };
    const std::vector<std::wstring> ShadowMaskResearchDefines = { L"USE_SHADOW_MASK=1", L"USE_PBR_RESEARCH=1" };

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/DeferredDirectLighting.hlsl", DirectPSByteCodes[0], DefaultDefines))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/DeferredDirectLighting.hlsl", DirectPSByteCodes[1], ShadowMaskDefines))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/DeferredDirectLighting.hlsl", DirectPSByteCodes[2], ResearchDefines))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/DeferredDirectLighting.hlsl", DirectPSByteCodes[3], ShadowMaskResearchDefines))
    {
        return false;
    }

    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/DeferredCompositeLight.hlsl", CompositeVSByteCode))
    {
        return false;
    }

    for (uint32_t DiffuseSourceIndex = 0; DiffuseSourceIndex < CompositeDiffuseSourceCount; ++DiffuseSourceIndex)
    {
        for (uint32_t VisualizationPermutationIndex = 0; VisualizationPermutationIndex < CompositeVisualizationPermutationCount; ++VisualizationPermutationIndex)
        {
            const ECompositeDiffuseSource DiffuseSource = static_cast<ECompositeDiffuseSource>(DiffuseSourceIndex);
            const ECompositeVisualizationPermutation VisualizationPermutation = static_cast<ECompositeVisualizationPermutation>(VisualizationPermutationIndex);
            const uint32_t PipelineIndex = GetCompositeLightingPipelineIndex(DiffuseSource, VisualizationPermutation);
            const std::vector<std::wstring> Defines = BuildCompositeLightingDefines(DiffuseSource, VisualizationPermutation);
            if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/DeferredCompositeLight.hlsl", CompositePSByteCodes[PipelineIndex], Defines))
            {
                return false;
            }
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = Renderer.LightingRootSignature.Get();
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
    PsoDesc.RTVFormats[0] = FDeferredRenderer::LightingBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    for (size_t Index = 0; Index < Renderer.DirectLightingPipelines.size(); ++Index)
    {
        PsoDesc.PS = { DirectPSByteCodes[Index].data(), DirectPSByteCodes[Index].size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(Renderer.DirectLightingPipelines[Index].GetAddressOf())));
    }

    PsoDesc.VS = { CompositeVSByteCode.data(), CompositeVSByteCode.size() };
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    for (uint32_t PipelineIndex = 0; PipelineIndex < CompositeLightingPipelineCount; ++PipelineIndex)
    {
        PsoDesc.PS = { CompositePSByteCodes[PipelineIndex].data(), CompositePSByteCodes[PipelineIndex].size() };
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(Renderer.CompositeLightingPipelines[PipelineIndex].GetAddressOf())));
    }

    return true;
}

bool FDeferredLightingPass::CreateLinearDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height) const
{
    FDeferredRenderer& Renderer = *Owner;
    const FRGTextureDesc LinearDepthDesc = { Width, Height, DXGI_FORMAT_R16_FLOAT };
    const FLOAT Color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CD3DX12_CLEAR_VALUE ClearValue(DXGI_FORMAT_R16_FLOAT, Color);
    CreateBindlessTexture(Device, L"LinearDepth", LinearDepthDesc, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, Renderer.LinearDepthTexture, false, false, &ClearValue);
    WriteOrCreateBindlessTextureSrv(Device, Renderer.LinearDepthTexture);
    CreateTexture2DRtv(Device, L"LinearDepthRTVHeap", Renderer.LinearDepthTexture.Get(), DXGI_FORMAT_R16_FLOAT, Renderer.LinearDepthRtvHeap, Renderer.LinearDepthRtvHandle);

    return true;
}

void FDeferredLightingPass::AddLinearDepthPass(FDeferredPassContext& Context) const
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

        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        static_assert(1u <= kLinearDepthBindlessDwordCount);
        LocalCommandList->SetGraphicsRoot32BitConstant(1, DepthBindlessIndex, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredLightingPass::AddExtractHalfDepthNormalPass(FDeferredPassContext& Context) const
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
        if (!Data.bEnabled)
        {
            return;
        }

        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        const uint32_t HalfDepthNormalUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Data.HalfDepthNormalHandle);
        if (DepthBindlessIndex == UINT32_MAX || Owner.GBufferA.SrvBindlessIndex == UINT32_MAX || HalfDepthNormalUavBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootSignature(Owner.ExtractHalfDepthNormalRootSignature.Get());
        CommandList->SetPipelineState(Owner.ExtractHalfDepthNormalPipeline.Get());

        const uint32_t GlobalFrameNumber = static_cast<uint32_t>(Owner.GetFrameNumber());
        const uint32_t SequenceFrame = Owner.RestirGI->IsFreezeFrame() ? Owner.RestirGI->GetFrozenSequenceFrame() : GlobalFrameNumber;
        const uint32_t Constants[kExtractHalfDepthConstantsDwordCount] =
        {
            DepthBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
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

void FDeferredLightingPass::AddDirectLightingPass(FDeferredPassContext& Context, FRGResourceHandle& OutDirectHandle) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FDeferredGBufferHandles& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle ShadowHandle = Context.Resources.ShadowHandle;
    const bool bPbrResearchEnabled = bEnablePbrResearch;
    const EDeferredLightingVisualizationMode VisualizationMode = DeferredLightingVisualizationMode;

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
    }, [&Owner, &Graph, bPbrResearchEnabled, VisualizationMode](const FDirectLightingPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

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

        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        if (!IsValidBindlessIndex(DepthBindlessIndex) || !IsValidBindlessIndex(Owner.ShadowMap.SrvBindlessIndex)
            || !IsValidBindlessIndex(Owner.GetEnvironmentCubeSrvIndex()) || !IsValidBindlessIndex(Owner.GetBrdfLutSrvIndex())
            || !IsValidBindlessIndex(Owner.GBufferA.SrvBindlessIndex) || !IsValidBindlessIndex(Owner.GBufferB.SrvBindlessIndex) || !IsValidBindlessIndex(Owner.GBufferC.SrvBindlessIndex))
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(DirectLightingRtvHandle, nullptr);

        const bool bUseShadowMask = Owner.bShadowsEnabled && Owner.bRayTracedShadowsEnabled && Owner.GetRayTracingRuntime().bRayTracingPipelineReady && IsValidBindlessIndex(Owner.ShadowMaskBindlessIndex);
        const uint32_t PipelineIndex = (bUseShadowMask ? 1u : 0u) | (bPbrResearchEnabled ? 2u : 0u);
        LocalCommandList->SetPipelineState(Owner.DirectLightingPipelines[PipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.LightingRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const uint32_t ResolvedShadowMaskIndex = bUseShadowMask ? Owner.ShadowMaskBindlessIndex : Owner.ShadowMap.SrvBindlessIndex;
        const uint32_t LightingBindlessIndices[kLightingPassBindlessDwordCount] =
        {
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.GBufferC.SrvBindlessIndex,
            Owner.GBufferD.SrvBindlessIndex,
            Owner.ShadowMap.SrvBindlessIndex,
            ResolvedShadowMaskIndex,
            Owner.GetEnvironmentCubeSrvIndex(),
            Owner.GetBrdfLutSrvIndex(),
            DepthBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(LightingBindlessIndices), LightingBindlessIndices, 0);

        struct FDeferredLightingConstants
        {
            float Intensity = 0.0f;
            uint32_t Enabled = 0;
            float SsrRoughnessCutoff = 0.0f;
            uint32_t VisualizationMode = 0;
            uint32_t Padding0 = 0;
            uint32_t Padding1 = 0;
            uint32_t Padding2 = 0;
        };

        const float EffectiveRestirGIIntensity = (std::max)(0.0f, Owner.RestirGI->GetIntensity());

        const FDeferredLightingConstants DeferredLightingConstants =
        {
            1.0f,
            Owner.RestirGI->IsEnabled() ? 1u : 0u,
            (Owner.Ssr != nullptr) ? Owner.Ssr->GetRoughnessCutoff() : 0.0f,
            static_cast<uint32_t>(VisualizationMode),
            0u,
            0u,
            0u
        };
        (void)EffectiveRestirGIIntensity;
        static_assert(sizeof(FDeferredLightingConstants) / sizeof(uint32_t) <= kLightingPassConstantsDwordCount);
        LocalCommandList->SetGraphicsRoot32BitConstants(2, sizeof(FDeferredLightingConstants) / sizeof(uint32_t), &DeferredLightingConstants, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredLightingPass::AddCompositeLightPass(FDeferredPassContext& Context, FRGResourceHandle DirectHandle) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GtaoHandle = Context.Resources.Gtao.GtaoHandle;
        const FRGResourceHandle RestirGIInputHandle = Owner.RestirGIDenoiser->IsEnabled() ? Context.Resources.RestirGIDenoiser.HistoryIrradianceHandle : Context.Resources.RestirGI.RestirGIHandle;
    const FRGResourceHandle SsrHandle = (Owner.Ssr != nullptr) ? Context.Resources.Ssr.GetLightingHandle(Owner.Ssr->GetMode(), Owner.Ssr->IsDenoiseEnabled()) : FRGResourceHandle{};
    const FRGResourceHandle SsrFallbackHandle = Context.Resources.Ssr.SsrFallbackHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;
    const EDeferredLightingVisualizationMode VisualizationMode = DeferredLightingVisualizationMode;
    const ECompositeDiffuseSource CompositeDiffuseSource = ResolveCompositeDiffuseSource(Owner);
    const ECompositeVisualizationPermutation VisualizationPermutation = ResolveCompositeVisualizationPermutation(VisualizationMode);
        const bool bUsesRestirDiffuse = CompositeDiffuseSource == ECompositeDiffuseSource::RestirGI;

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
            if (bUsesRestirDiffuse)
        {
            Builder.ReadTexture(RestirGIInputHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        Builder.ReadTexture(SsrHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(SsrFallbackHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.DirectLightingHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [&Owner, &Graph, VisualizationMode](const FCompositeLightPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        ID3D12Resource* DirectLightingResource = Graph.GetTextureResource(Data.DirectLightingHandle);
        if (!DirectLightingResource)
        {
            return;
        }

        const auto DirectLightingSrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(FDeferredRenderer::LightingBufferFormat, 1);
        if (Owner.DirectLightingBindlessIndex == UINT32_MAX)
        {
            Owner.DirectLightingBindlessIndex = Owner.Device->CreateBindlessSrv(DirectLightingResource, DirectLightingSrvDesc);
        }
        else if (Owner.DirectLightingResource != DirectLightingResource)
        {
            Owner.Device->WriteBindlessSrv(Owner.DirectLightingBindlessIndex, DirectLightingResource, DirectLightingSrvDesc);
        }
        Owner.DirectLightingResource = DirectLightingResource;

        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        const ECompositeDiffuseSource CompositeDiffuseSource = ResolveCompositeDiffuseSource(Owner);
        const ECompositeVisualizationPermutation VisualizationPermutation = ResolveCompositeVisualizationPermutation(VisualizationMode);
        const uint32_t CompositePipelineIndex = GetCompositeLightingPipelineIndex(CompositeDiffuseSource, VisualizationPermutation);
            const bool bUsesRestirDiffuse = CompositeDiffuseSource == ECompositeDiffuseSource::RestirGI;
        const uint32_t SsrLightingBindlessIndex = (Owner.Ssr != nullptr) ? Owner.Ssr->GetLightingSrvBindlessIndex() : UINT32_MAX;
        const uint32_t SsrFallbackIndex = (Owner.Ssr != nullptr) ? Owner.Ssr->GetFallbackSrvBindlessIndex() : UINT32_MAX;
            const uint32_t RestirGILightingBindlessIndex = Owner.RestirGIDenoiser->IsEnabled()
            ? ((Owner.RestirGIDenoiser != nullptr) ? Owner.RestirGIDenoiser->GetCurrentOutputSrvBindlessIndex() : UINT32_MAX)
            : ((Owner.RestirGI != nullptr) ? Owner.RestirGI->GetCurrentOutputSrvBindlessIndex() : UINT32_MAX);
        const uint32_t GtaoBindlessIndex = (Owner.Gtao != nullptr) ? Owner.Gtao->GetSrvBindlessIndex() : UINT32_MAX;
            if (!AreAllBindlessIndicesValid(
            DepthBindlessIndex,
            GtaoBindlessIndex,
            SsrLightingBindlessIndex,
            SsrFallbackIndex,
            Owner.ShadowMap.SrvBindlessIndex,
            Owner.GetEnvironmentCubeSrvIndex(),
            Owner.GetBrdfLutSrvIndex(),
            Owner.DirectLightingBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.GBufferC.SrvBindlessIndex))
        {
            return;
        }
        if (bUsesRestirDiffuse && !IsValidBindlessIndex(RestirGILightingBindlessIndex))
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        Cmd.SetRenderTarget(Owner.LightingRTVHandle, nullptr);

        LocalCommandList->SetPipelineState(Owner.CompositeLightingPipelines[CompositePipelineIndex].Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.LightingRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRootConstantBufferView(0, Owner.GetSceneConstantBufferAddress());
        const uint32_t LightingBindlessIndices[kLightingPassBindlessDwordCount] =
        {
            Owner.GBufferA.SrvBindlessIndex,
            Owner.GBufferB.SrvBindlessIndex,
            Owner.GBufferC.SrvBindlessIndex,
            Owner.GBufferD.SrvBindlessIndex,
            Owner.ShadowMap.SrvBindlessIndex,
            Owner.ShadowMap.SrvBindlessIndex,
            Owner.GetEnvironmentCubeSrvIndex(),
            Owner.GetBrdfLutSrvIndex(),
            DepthBindlessIndex,
            GtaoBindlessIndex,
            RestirGILightingBindlessIndex,
            SsrLightingBindlessIndex,
            SsrFallbackIndex,
            Owner.DirectLightingBindlessIndex
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(LightingBindlessIndices), LightingBindlessIndices, 0);

        struct FDeferredLightingConstants
        {
            float Intensity = 0.0f;
            uint32_t Enabled = 0;
            float SsrRoughnessCutoff = 0.0f;
            uint32_t VisualizationMode = 0;
            uint32_t Padding0 = 0;
            uint32_t Padding1 = 0;
            uint32_t Padding2 = 0;
        };

        const float EffectiveRestirGIIntensity = (std::max)(0.0f, Owner.RestirGI->GetIntensity());

        const FDeferredLightingConstants DeferredLightingConstants =
        {
            1.0f,
            Owner.RestirGI->IsEnabled() ? 1u : 0u,
            (Owner.Ssr != nullptr) ? Owner.Ssr->GetRoughnessCutoff() : 0.0f,
            static_cast<uint32_t>(VisualizationMode),
            0u,
            0u,
            0u
        };
        (void)EffectiveRestirGIIntensity;
        static_assert(sizeof(FDeferredLightingConstants) / sizeof(uint32_t) <= kLightingPassConstantsDwordCount);
        LocalCommandList->SetGraphicsRoot32BitConstants(2, sizeof(FDeferredLightingConstants) / sizeof(uint32_t), &DeferredLightingConstants, 0);

        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}
