#include "Cas.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include <d3dx12.h>
#include <vector>

using Microsoft::WRL::ComPtr;

bool FCas::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    (void)Owner;
    return CreateRootSignature(Device) && CreatePipeline(Device, BackBufferFormat);
}

bool FCas::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    (void)Device;
    return true;
}

void FCas::ImportPersistentResources(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FCasFrameResources& OutResources = Context.Resources.Cas;

    OutResources.TonemapOutputHandle = Graph.ImportTexture(
        "TonemapOutput",
        Owner.TonemapOutput.Get(),
        &Owner.TonemapOutput.State,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), Owner.BackBufferFormat });
}

bool FCas::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    WriteOrCreateBindlessTextureSrv(Device, Owner.TonemapOutput);
    return true;
}

void FCas::AddPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FCasPassData
    {
        bool bEnabled = false;
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        DirectX::XMFLOAT2 TexelDelta{};
        float Sharpness = 0.0f;
    };

    Context.Graph.AddPass<FCasPassData>("CAS", [&](FCasPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bCasActive;
        if (!Data.bEnabled)
        {
            return;
        }
        Data.OutputHandle = Context.RtvHandle;
        Data.TexelDelta = DirectX::XMFLOAT2(1.0f / Owner.Viewport.Width, 1.0f / Owner.Viewport.Height);
        Data.Sharpness = Sharpness;
        Builder.ReadTexture(Context.Resources.Cas.TonemapOutputHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }, [&, this](const FCasPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        Cmd.SetRenderTarget(Data.OutputHandle, nullptr);

        struct FCasConstants
        {
            DirectX::XMFLOAT2 TexelDelta;
            float Sharpness;
            float Padding;
        };

        const FCasConstants CasConstants =
        {
            Data.TexelDelta,
            Data.Sharpness,
            0.0f
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetPipelineState(Pipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(RootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(CasConstants) / sizeof(uint32_t), &CasConstants, 0);
        LocalCommandList->SetGraphicsRoot32BitConstant(1, Owner.TonemapOutput.SrvBindlessIndex, 0);
        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

bool FCas::IsReady() const
{
    return bEnabled && Pipeline && RootSignature;
}

bool FCas::CreateRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    RootParams[1].InitAsConstants(1, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
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

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams), RootParams,
        1, &SamplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RootSignature.GetAddressOf())));
    return true;
}

bool FCas::CreatePipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/Cas.hlsl", VSByteCode))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/Cas.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature.Get();
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
    PsoDesc.RTVFormats[0] = BackBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(Pipeline.GetAddressOf())));
    return true;
}
