#include "Tonemap.h"

#include "DeferredPassContext.h"
#include "AutoExposure.h"
#include "Cas.h"
#include "Taa.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include <string>
#include <vector>
#include <d3dx12.h>

bool FTonemap::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    (void)Owner;
    return CreateTonemapRootSignature(Device)
        && CreateTonemapPipeline(Device, BackBufferFormat);
}

bool FTonemap::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    (void)Device;
    return true;
}

void FTonemap::ImportPersistentResources(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    (void)Owner;
    (void)Context;
}

bool FTonemap::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    (void)Device;
    return true;
}

void FTonemap::AddPasses(FDeferredPassContext& Context) const
{
    AddTonemapPass(Context);
}


bool FTonemap::CreateTonemapRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    RootParams[1].InitAsConstants(2, 1, 0, D3D12_SHADER_VISIBILITY_PIXEL);

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
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
            | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(TonemapRootSignature.GetAddressOf())));
    return true;
}

bool FTonemap::CreateTonemapPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/Tonemap.hlsl", VSByteCode))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/Tonemap.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = TonemapRootSignature.Get();
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

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(TonemapPipeline.GetAddressOf())));
    return true;
}

void FTonemap::AddTonemapPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FTonemapPassData
    {
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        uint32_t InputBindlessIndex = UINT32_MAX;
        bool bUseCas = false;
        bool bUseTaa = false;
        uint32_t LuminanceIndex = 0;
    };

    Context.Graph.AddPass<FTonemapPassData>("Tonemap", [&](FTonemapPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseCas = Context.FrameState.bCasActive;
        Data.OutputHandle = Data.bUseCas ? Owner.TonemapOutputRtvHandle : Context.RtvHandle;
        Data.bUseTaa = Context.FrameState.bTaaActive && Owner.Taa;
        Data.LuminanceIndex = Owner.AutoExposure ? Owner.AutoExposure->LuminanceWriteIndex : 0u;
        Data.InputBindlessIndex = Data.bUseTaa ? Owner.Taa->GetHistorySrvBindlessIndex(Context.FrameState.TaaWriteIndex) : Owner.LightingBuffer.SrvBindlessIndex;
        if (Data.bUseTaa)
        {
            Builder.ReadTexture(Context.Resources.Taa.HistoryHandles[Context.FrameState.TaaWriteIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else
        {
            Builder.ReadTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (Data.bUseCas)
        {
            Builder.WriteTexture(Context.Resources.Cas.TonemapOutputHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (Owner.AutoExposure)
        {
            Builder.ReadTexture(Context.Resources.AutoExposure.LuminanceHandles[Data.LuminanceIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        for (int i = 0; i < 4; ++i)
        {
            Builder.WriteTexture(Context.Resources.GBufferHandles[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [&, this](const FTonemapPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        Cmd.SetRenderTarget(Data.OutputHandle, nullptr);

        struct FTonemapConstants
        {
            uint32_t Enabled;
            uint32_t AutoExposureEnabled;
            float Exposure;
            float Gamma;
        };

        const FTonemapConstants TonemapConstants =
        {
            bTonemapEnabled ? 1u : 0u,
            (Owner.AutoExposure && Owner.AutoExposure->IsEnabled()) ? 1u : 0u,
            TonemapExposure,
            TonemapGamma
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetPipelineState(TonemapPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(TonemapRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(TonemapConstants) / sizeof(uint32_t), &TonemapConstants, 0);
        const uint32_t TonemapBindlessIndices[] =
        {
            Data.InputBindlessIndex,
            Owner.AutoExposure ? Owner.AutoExposure->LuminanceTextures[Data.LuminanceIndex].SrvBindlessIndex : UINT32_MAX
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(TonemapBindlessIndices), TonemapBindlessIndices, 0);
        LocalCommandList->DrawInstanced(3, 1, 0, 0);

        Cmd.TransitionResource(Owner.LightingBuffer.Get(), Owner.LightingBuffer.State, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Owner.LightingBuffer.State = D3D12_RESOURCE_STATE_RENDER_TARGET;
    });
}
