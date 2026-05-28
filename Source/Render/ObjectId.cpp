#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ObjectId.h"

#include "Deferred/DeferredBasePass.h"
#include "Deferred/DeferredPassContext.h"
#include "DeferredRenderer.h"
#include "RenderGraph.h"
#include "RendererUtils.h"
#include "SceneModelResource.h"
#include "ShaderCompiler.h"
#include "../Core/GpuDebugMarkers.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12Commons.h"
#include <d3dx12.h>

bool FObjectId::InitializePipelines(FDX12Device* Device, ID3D12RootSignature* RootSignature)
{
    if (!Device || !RootSignature)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ObjectId.hlsl", VSByteCode))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/ObjectId.hlsl", PSByteCode))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature;
    PsoDesc.InputLayout = { InputLayout, _countof(InputLayout) };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
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
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(Pipeline.ReleaseAndGetAddressOf())));
    return true;
}

bool FObjectId::InitializeResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (!Device)
    {
        return false;
    }

    const FLOAT ClearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CreateRenderTarget2D(Device, L"ObjectIdRenderTarget", Width, Height, DXGI_FORMAT_R32_UINT, ClearColor, RenderTarget);

    RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    Footprint.Offset = 0;
    Footprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
    Footprint.Footprint.Width = 1;
    Footprint.Footprint.Height = 1;
    Footprint.Footprint.Depth = 1;
    Footprint.Footprint.RowPitch = RowPitch;

    if (!CreateReadbackBuffer(Device, L"ObjectIdReadback", RowPitch, Readback))
    {
        return false;
    }

    State = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
}

FRGResourceHandle FObjectId::ImportResource(FRenderGraph& Graph, uint32_t Width, uint32_t Height)
{
    return Graph.ImportTexture(
        "ObjectId",
        RenderTarget.Get(),
        &State,
        { Width, Height, DXGI_FORMAT_R32_UINT });
}

void FObjectId::AddPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    const FObjectId* ObjectId = this;

    struct FObjectIdPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FObjectIdPassData>("ObjectId", [&Context, ObjectId](FObjectIdPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = ObjectId->IsReadbackRequested() && ObjectId->IsReady();
        Data.Camera = &Context.Camera;
        if (Data.bEnabled)
        {
            Builder.WriteTexture(Context.Resources.ObjectIdHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Builder.ReadTexture(Context.Resources.DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
        }
    }, [&Owner, ObjectId](const FObjectIdPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        LocalCommandList->SetPipelineState(ObjectId->GetPipeline());
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetGraphicsRootSignature(Owner.BasePass->GetBasePassRootSignature());
        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);
        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_CPU_DESCRIPTOR_HANDLE& DepthHandle = Owner.GetDSVHandle();
        const D3D12_CPU_DESCRIPTOR_HANDLE ObjectIdRtvHandle = ObjectId->GetRtvHandle();
        LocalCommandList->OMSetRenderTargets(1, &ObjectIdRtvHandle, FALSE, &DepthHandle);

        const UINT ClearValue[4] = { 0, 0, 0, 0 };
        LocalCommandList->ClearRenderTargetView(ObjectId->GetRtvHandle(), reinterpret_cast<const float*>(ClearValue), 0, nullptr);

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
            if (Model.Material.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
            {
                continue;
            }
            const uint64_t ConstantBufferOffset = Owner.SceneConstantBufferStride * ModelIndex;

            LocalCommandList->IASetVertexBuffers(0, Model.Geometry.VertexBufferCount, Model.Geometry.VertexBufferViews.data());
            LocalCommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
            LocalCommandList->SetGraphicsRootConstantBufferView(0, ConstantBufferAddress + ConstantBufferOffset);
            LocalCommandList->SetGraphicsRoot32BitConstants(1, 1, &Model.ObjectId, 0);

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
        const uint32_t ReadX = (std::min)(ObjectId->GetReadbackX(), Width > 0 ? Width - 1 : 0);
        const uint32_t ReadY = (std::min)(ObjectId->GetReadbackY(), Height > 0 ? Height - 1 : 0);

        D3D12_RESOURCE_BARRIER Barrier = CD3DX12_RESOURCE_BARRIER::Transition(ObjectId->GetRenderTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        LocalCommandList->ResourceBarrier(1, &Barrier);

        D3D12_TEXTURE_COPY_LOCATION Src = {};
        Src.pResource = ObjectId->GetRenderTarget();
        Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Src.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION Dst = {};
        Dst.pResource = ObjectId->GetReadbackResource();
        Dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Dst.PlacedFootprint = ObjectId->GetFootprint();

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

        const_cast<FObjectId*>(ObjectId)->SetReadbackRecorded();
    });
}

void FObjectId::RequestReadback(uint32_t X, uint32_t Y)
{
    bReadbackRequested = true;
    bReadbackRecorded = false;
    ReadbackX = X;
    ReadbackY = Y;
}

bool FObjectId::ConsumeReadback(uint32_t& OutObjectId)
{
    if (!bReadbackRecorded || !Readback)
    {
        return false;
    }

    void* MappedData = nullptr;
    D3D12_RANGE ReadRange = { 0, RowPitch };
    if (FAILED(Readback->Map(0, &ReadRange, &MappedData)) || !MappedData)
    {
        return false;
    }

    OutObjectId = *static_cast<const uint32_t*>(MappedData);
    D3D12_RANGE WriteRange = { 0, 0 };
    Readback->Unmap(0, &WriteRange);
    bReadbackRequested = false;
    bReadbackRecorded = false;
    return true;
}
