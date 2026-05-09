#include "ClusterDagVisibilityPass.h"

#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "ClusterDagRuntime.h"
#include "DeferredPassContext.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12CommandContext.h"
#include "../../RHI/DX12Device.h"

#include <vector>
#include <d3dx12.h>

namespace
{
    constexpr DXGI_FORMAT GClusterDagVisibilityFormat0 = DXGI_FORMAT_R32G32_UINT;
    constexpr DXGI_FORMAT GClusterDagVisibilityFormat1 = DXGI_FORMAT_R16G16_FLOAT;
    constexpr uint32_t GClusterDagVisibilityDoubleSidedBit = 1u << 9;
}

bool FClusterDagVisibilityPass::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    this->Owner = &Owner;
    this->Device = Device;
    bPipelinesReady = false;
    bFeatureSupported = false;
    if (Device == nullptr)
    {
        return false;
    }

    bFeatureSupported = Device->IsBarycentricsSupported();
    if (!bFeatureSupported)
    {
        LogWarning("Cluster DAG visibility buffer disabled: device does not support barycentrics required by Shaders/ClusterDagVisibility.hlsl.");
        return true;
    }

    bPipelinesReady = CreateVisibilityRootSignature(Device)
        && CreateVisibilityPipeline(Device)
        && CreateResolveRootSignature(Device)
        && CreateResolvePipeline(Device)
        && CreateCommandSignature(Device);
    return bPipelinesReady;
}

bool FClusterDagVisibilityPass::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    this->Owner = &Owner;
    this->Device = Device;
    bResourcesReady = false;
    if (Device == nullptr)
    {
        return false;
    }
    if (!bFeatureSupported)
    {
        return true;
    }
    return CreateVisibilityResources(Device, Width, Height);
}

void FClusterDagVisibilityPass::ImportPersistentResources(FDeferredPassContext& Context)
{
    if (!IsReady())
    {
        return;
    }

    Context.Resources.ClusterDagVisibility.VisibilityHandle0 = Context.Graph.ImportTexture(
        "ClusterDagVisibility0",
        VisibilityTexture0.Get(),
        &VisibilityTexture0.State,
        { static_cast<uint32>(Owner->Viewport.Width), static_cast<uint32>(Owner->Viewport.Height), GClusterDagVisibilityFormat0 },
        VisibilityTexture0.SrvBindlessIndex);
    Context.Resources.ClusterDagVisibility.VisibilityHandle1 = Context.Graph.ImportTexture(
        "ClusterDagVisibility1",
        VisibilityTexture1.Get(),
        &VisibilityTexture1.State,
        { static_cast<uint32>(Owner->Viewport.Width), static_cast<uint32>(Owner->Viewport.Height), GClusterDagVisibilityFormat1 },
        VisibilityTexture1.SrvBindlessIndex);
}

void FClusterDagVisibilityPass::AddPasses(FDeferredPassContext& Context) const
{
    if (!IsReady())
    {
        return;
    }

    AddVisibilityPass(Context);
    AddResolvePass(Context);
}


void FClusterDagVisibilityPass::AddVisibilityPass(FDeferredPassContext& Context) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle VisibilityHandle0{};
        FRGResourceHandle VisibilityHandle1{};
        FRGBufferHandle DagIndirectHandle{};
        ID3D12Resource* DagIndirectBuffer = nullptr;
        ID3D12Resource* DagRunCountBuffer = nullptr;
        const std::vector<FRenderer::FIndirectDrawRange>* DagRanges = nullptr;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagVisibility", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        FClusterDagRuntime* Runtime = Owner->ClusterDagRuntime.get();
        Data.DagIndirectBuffer = Runtime->IndirectCommandBuffers[Context.FrameIndex].Get();
        Data.DagRunCountBuffer = Runtime->RunCountBuffers[Context.FrameIndex].Get();
        Data.DagRanges = &Runtime->GetIndirectDrawRanges();
        Data.bEnabled =
            Context.Resources.ClusterDagVisibility.VisibilityHandle0
            && Context.Resources.ClusterDagVisibility.VisibilityHandle1
            && Data.DagIndirectBuffer != nullptr
            && Data.DagRunCountBuffer != nullptr
            && !Data.DagRanges->empty();
        Data.VisibilityHandle0 = Context.Resources.ClusterDagVisibility.VisibilityHandle0;
        Data.VisibilityHandle1 = Context.Resources.ClusterDagVisibility.VisibilityHandle1;
        Data.Camera = &Context.Camera;

        if (Data.bEnabled)
        {
            Data.DagIndirectHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagIndirectCommandsVisibility", Runtime->IndirectCommandBuffers[Context.FrameIndex]);
            Builder.WriteTexture(Data.VisibilityHandle0, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Builder.WriteTexture(Data.VisibilityHandle1, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Builder.WriteTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Builder.ReadBuffer(Data.DagIndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        }
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        Cmd.ClearRenderTarget(VisibilityRtvHandles[0], ClearValue);
        Cmd.ClearRenderTarget(VisibilityRtvHandles[1], ClearValue);

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner->Device->GetBindlessDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetGraphicsRootSignature(VisibilityRootSignature.Get());
        CommandList->RSSetViewports(1, &Owner->Viewport);
        CommandList->RSSetScissorRects(1, &Owner->ScissorRect);
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE DepthHandle = Owner->GetDSVHandle();
        CommandList->OMSetRenderTargets(static_cast<UINT>(VisibilityRtvHandles.size()), VisibilityRtvHandles.data(), FALSE, &DepthHandle);

        Owner->EnsureClusterDagSceneConstantsPrepared(*Data.Camera);

        for (size_t RangeIndex = 0; RangeIndex < Data.DagRanges->size(); ++RangeIndex)
        {
            const FRenderer::FIndirectDrawRange& Range = (*Data.DagRanges)[RangeIndex];
            const bool bDoubleSided = (Range.PipelineKey & GClusterDagVisibilityDoubleSidedBit) != 0;
            ID3D12PipelineState* Pipeline = VisibilityPipelines[bDoubleSided ? 1u : 0u].Get();
            CommandList->SetPipelineState(Pipeline);
            const uint64_t Offset = static_cast<uint64_t>(Range.Start) * sizeof(FIndirectDrawCommand);
            const uint64_t CountOffset = RangeIndex * sizeof(uint32_t);
            CommandList->ExecuteIndirect(CommandSignature.Get(), Range.Count, Data.DagIndirectBuffer, Offset, Data.DagRunCountBuffer, CountOffset);
        }
    });
}

void FClusterDagVisibilityPass::AddResolvePass(FDeferredPassContext& Context) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle VisibilityHandle0{};
        FRGResourceHandle VisibilityHandle1{};
        FRGBufferHandle DrawDataHandle{};
        FRGBufferHandle SceneDataHandle{};
        uint32_t VisibilitySrvIndex0 = UINT32_MAX;
        uint32_t VisibilitySrvIndex1 = UINT32_MAX;
        uint32_t DrawDataSrvIndex = UINT32_MAX;
        uint32_t SceneDataSrvIndex = UINT32_MAX;
    };

    Context.Graph.AddPass<FPassData>("ClusterDagResolve", [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
		FClusterDagRuntime* Runtime = Owner->ClusterDagRuntime.get();
		Data.bEnabled =
			Context.Resources.ClusterDagVisibility.VisibilityHandle0
			&& Context.Resources.ClusterDagVisibility.VisibilityHandle1;
        if (!Data.bEnabled)
        {
            return;
        }

        Data.VisibilityHandle0 = Context.Resources.ClusterDagVisibility.VisibilityHandle0;
        Data.VisibilityHandle1 = Context.Resources.ClusterDagVisibility.VisibilityHandle1;
        Data.DrawDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagResolveDrawData", Runtime->DrawDataBuffer);
        Data.SceneDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDagSceneData", Owner->ClusterDagSceneConstantBuffers[Context.FrameIndex]);
        Data.VisibilitySrvIndex0 = VisibilityTexture0.SrvBindlessIndex;
        Data.VisibilitySrvIndex1 = VisibilityTexture1.SrvBindlessIndex;
        Data.DrawDataSrvIndex = Runtime->DrawDataBuffer.SrvBindlessIndex;
        Data.SceneDataSrvIndex = Owner->ClusterDagSceneConstantBuffers[Context.FrameIndex].SrvBindlessIndex;

        Builder.ReadTexture(Data.VisibilityHandle0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.VisibilityHandle1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.DrawDataHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Builder.ReadBuffer(Data.SceneDataHandle, D3D12_RESOURCE_STATE_GENERIC_READ);
        for (FRGResourceHandle GBufferHandle : Context.Resources.GBufferHandles)
        {
            Builder.WriteTexture(GBufferHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        Builder.WriteTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
        ID3D12DescriptorHeap* Heaps[] = { Owner->Device->GetBindlessDescriptorHeap(), Owner->Device->GetSamplerDescriptorHeap() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetGraphicsRootSignature(ResolveRootSignature.Get());
        CommandList->SetPipelineState(ResolvePipeline.Get());
        CommandList->RSSetViewports(1, &Owner->Viewport);
        CommandList->RSSetScissorRects(1, &Owner->ScissorRect);
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const uint32_t ResolveIndices[] =
        {
            Data.VisibilitySrvIndex0,
            Data.VisibilitySrvIndex1,
            Data.DrawDataSrvIndex,
            Data.SceneDataSrvIndex
        };
        CommandList->SetGraphicsRoot32BitConstants(0, _countof(ResolveIndices), ResolveIndices, 0);

        D3D12_CPU_DESCRIPTOR_HANDLE ResolveRtvs[5] =
        {
            Owner->GBufferRTVHandles[0],
            Owner->GBufferRTVHandles[1],
            Owner->GBufferRTVHandles[2],
            Owner->GBufferRTVHandles[3],
            Owner->LightingRTVHandle
        };
        CommandList->OMSetRenderTargets(_countof(ResolveRtvs), ResolveRtvs, FALSE, nullptr);
        CommandList->DrawInstanced(3, 1, 0, 0);
    });
}

bool FClusterDagVisibilityPass::IsReady() const
{
    if (!bFeatureSupported)
    {
        return false;
    }

    return bEnabled
        && bPipelinesReady
        && bResourcesReady
        && Owner != nullptr
        && Owner->ClusterDagRuntime != nullptr
        && Owner->ClusterDagRuntime->HasResources();
}

bool FClusterDagVisibilityPass::CreateVisibilityRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(2, 2, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams),
        RootParams,
        0,
        nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(VisibilityRootSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateVisibilityPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VsByteCode;
    std::vector<uint8_t> PsByteCode;
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ClusterDagVisibility.hlsl", VsByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/ClusterDagVisibility.hlsl", PsByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = VisibilityRootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.VS = { VsByteCode.data(), VsByteCode.size() };
    PsoDesc.PS = { PsByteCode.data(), PsByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PsoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    PsoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    PsoDesc.NumRenderTargets = 2;
    PsoDesc.RTVFormats[0] = GClusterDagVisibilityFormat0;
    PsoDesc.RTVFormats[1] = GClusterDagVisibilityFormat1;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;

    for (uint32_t DoubleSidedVariant = 0; DoubleSidedVariant < 2; ++DoubleSidedVariant)
    {
        PsoDesc.RasterizerState.CullMode = (DoubleSidedVariant == 0u) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(VisibilityPipelines[DoubleSidedVariant].ReleaseAndGetAddressOf())));
    }

    return true;
}

bool FClusterDagVisibilityPass::CreateResolveRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
        0,
        D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f,
        4,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f,
        D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams),
        RootParams,
        1,
        &SamplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
            | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(ResolveRootSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateResolvePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VsByteCode;
    std::vector<uint8_t> PsByteCode;
    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ClusterDagResolve.hlsl", VsByteCode))
    {
        return false;
    }
    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/ClusterDagResolve.hlsl", PsByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = ResolveRootSignature.Get();
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.VS = { VsByteCode.data(), VsByteCode.size() };
    PsoDesc.PS = { PsByteCode.data(), PsByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;
    PsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    for (UINT TargetIndex = 0; TargetIndex < 5; ++TargetIndex)
    {
        PsoDesc.BlendState.RenderTarget[TargetIndex].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    PsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 5;
    PsoDesc.RTVFormats[0] = FDeferredRenderer::GBufferFormats[0];
    PsoDesc.RTVFormats[1] = FDeferredRenderer::GBufferFormats[1];
    PsoDesc.RTVFormats[2] = FDeferredRenderer::GBufferFormats[2];
    PsoDesc.RTVFormats[3] = FDeferredRenderer::GBufferFormats[3];
    PsoDesc.RTVFormats[4] = FDeferredRenderer::LightingBufferFormat;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(ResolvePipeline.ReleaseAndGetAddressOf())));
    return true;
}


bool FClusterDagVisibilityPass::CreateCommandSignature(FDX12Device* Device)
{
    D3D12_INDIRECT_ARGUMENT_DESC IndirectArgs[3] = {};
    IndirectArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    IndirectArgs[0].ConstantBufferView.RootParameterIndex = 0;
    IndirectArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    IndirectArgs[1].Constant.RootParameterIndex = 1;
    IndirectArgs[1].Constant.Num32BitValuesToSet = 2;
    IndirectArgs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.pArgumentDescs = IndirectArgs;
    CommandDesc.NumArgumentDescs = _countof(IndirectArgs);
    CommandDesc.ByteStride = sizeof(FIndirectDrawCommand);
    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, VisibilityRootSignature.Get(), IID_PPV_ARGS(CommandSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagVisibilityPass::CreateVisibilityResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Owner == nullptr || Device == nullptr)
    {
        return false;
    }

    bResourcesReady = false;

    const FRGTextureDesc Desc =
    {
        Width,
        Height,
        GClusterDagVisibilityFormat0
    };
    const FRGTextureDesc Desc1 =
    {
        Width,
        Height,
        GClusterDagVisibilityFormat1
    };

    const float ClearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const D3D12_CLEAR_VALUE ClearValue1 = CD3DX12_CLEAR_VALUE(Desc1.Format, ClearColor);
    CreateBindlessTexture(
        Device,
        L"ClusterDagVisibility0",
        Desc,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        VisibilityTexture0,
        false,
        false,
        nullptr);
    WriteOrCreateBindlessTextureSrv(Device, VisibilityTexture0);
    CreateBindlessTexture(
        Device,
        L"ClusterDagVisibility1",
        Desc1,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        VisibilityTexture1,
        false,
        false,
        &ClearValue1);
    WriteOrCreateBindlessTextureSrv(Device, VisibilityTexture1);

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 2;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(VisibilityRtvHeap.ReleaseAndGetAddressOf())));
    if (VisibilityRtvHeap)
    {
        VisibilityRtvHeap->SetName(L"ClusterDagVisibilityRtvHeap");
    }

    const UINT DescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    VisibilityRtvHandles[0] = VisibilityRtvHeap->GetCPUDescriptorHandleForHeapStart();
    VisibilityRtvHandles[1] = VisibilityRtvHandles[0];
    VisibilityRtvHandles[1].ptr += DescriptorSize;

    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    RtvDesc.Format = Desc.Format;
    Device->GetDevice()->CreateRenderTargetView(VisibilityTexture0.Get(), &RtvDesc, VisibilityRtvHandles[0]);
    RtvDesc.Format = Desc1.Format;
    Device->GetDevice()->CreateRenderTargetView(VisibilityTexture1.Get(), &RtvDesc, VisibilityRtvHandles[1]);

    bResourcesReady = VisibilityTexture0
        && VisibilityTexture1
        && VisibilityRtvHeap
        && IsValidBindlessIndex(VisibilityTexture0.SrvBindlessIndex)
        && IsValidBindlessIndex(VisibilityTexture1.SrvBindlessIndex);
    if (!bResourcesReady)
    {
        LogWarning("ClusterDag visibility resources are incomplete; visibility pass will remain disabled.");
    }

    return bResourcesReady;
}
