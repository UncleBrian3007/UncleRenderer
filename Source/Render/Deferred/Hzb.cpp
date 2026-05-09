#include "Hzb.h"

#include <algorithm>
#include <string>
#include <vector>

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../GpuResource.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"
#include <d3dx12.h>

bool FHzb::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    return CreateRootSignature(Device) && CreatePipeline(Device);
}

bool FHzb::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t InWidth, uint32_t InHeight)
{
    return CreateResources(Device, InWidth, InHeight) && CreatePersistentDescriptors(Owner, Device);
}

void FHzb::ImportPersistentResources(FDeferredPassContext& Context)
{
    Context.Resources.Hzb.HzbHandle = ImportBindlessTexture(Context.Graph, "HZB", HzbTexture);
}

bool FHzb::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;

    WriteOrCreateBindlessTextureSrv(Device, HzbTexture);

    HzbSrvMipBindlessIndices.clear();
    HzbSrvMipBindlessIndices.reserve(MipCount);
    for (uint32_t Mip = 0; Mip < MipCount; ++Mip)
    {
        HzbSrvMipBindlessIndices.push_back(
            Device->CreateBindlessSrv(HzbTexture.Get(), CreateTexture2DSrvDesc(HzbTexture.Desc, Mip, 1)));
    }

    HzbUavBindlessIndices.clear();
    HzbUavBindlessIndices.reserve(MipCount);
    for (uint32_t Mip = 0; Mip < MipCount; ++Mip)
    {
        HzbUavBindlessIndices.push_back(
            Device->CreateBindlessUav(HzbTexture.Get(), nullptr, CreateTexture2DUavDesc(HzbTexture.Desc, Mip)));
    }

    HzbNullUavTexture.UavBindlessIndex = Device->CreateBindlessUav(
        HzbNullUavTexture.Get(), nullptr, CreateTexture2DUavDesc(HzbNullUavTexture.Desc));

    return true;
}

void FHzb::AddPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle HzbHandle = Context.Resources.Hzb.HzbHandle;

    struct FHZBPassData
    {
        uint32_t Width = 0;
        uint32_t Height = 0;
        uint32_t MipCount = 0;
        uint32_t SourceWidth = 0;
        uint32_t SourceHeight = 0;
        uint32_t DepthBindlessIndex = UINT32_MAX;
        std::vector<uint32_t> HZBSrvMips;
        std::vector<uint32_t> HZBUavs;
        uint32_t HZBNullUav = UINT32_MAX;
    };

    if (!bEnabled || !FrameState.bBuildHZB)
    {
        return;
    }

    Graph.AddPass<FHZBPassData>("Build HZB", [this, &Owner, DepthHandle, HzbHandle](FHZBPassData& Data, FRGPassBuilder& Builder)
    {
        Data.Width = Width;
        Data.Height = Height;
        Data.MipCount = MipCount;
        ID3D12Resource* DepthBuffer = Owner.GetDepthBuffer();
        const D3D12_RESOURCE_DESC DepthDesc = DepthBuffer ? DepthBuffer->GetDesc() : D3D12_RESOURCE_DESC{};
        Data.SourceWidth = static_cast<uint32>(DepthDesc.Width);
        Data.SourceHeight = DepthDesc.Height;
        const uint32_t DepthIndex = Owner.DepthBindlessIndices.empty()
            ? 0u
            : (Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size()));
        Data.DepthBindlessIndex = Owner.DepthBindlessIndices.empty() ? UINT32_MAX : Owner.DepthBindlessIndices[DepthIndex];
        Data.HZBSrvMips = HzbSrvMipBindlessIndices;
        Data.HZBUavs = HzbUavBindlessIndices;
        Data.HZBNullUav = HzbNullUavTexture.UavBindlessIndex;

        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(HzbHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FHZBPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!HzbRootSignature || Data.MipCount == 0)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        if (Data.DepthBindlessIndex == UINT32_MAX || Data.HZBNullUav == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(HzbRootSignature.Get());

        struct FHZBConstants
        {
            uint32_t SourceWidth;
            uint32_t SourceHeight;
            uint32_t DestWidth;
            uint32_t DestHeight;
            uint32_t DestWidth1;
            uint32_t DestHeight1;
            uint32_t DestWidth2;
            uint32_t DestHeight2;
            uint32_t DestWidth3;
            uint32_t DestHeight3;
            uint32_t SourceMip;
            uint32_t SourceIsDepth;
        };

        uint32_t CurrentWidth = Data.Width;
        uint32_t CurrentHeight = Data.Height;
        std::vector<D3D12_RESOURCE_STATES> MipStates(Data.MipCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        uint32_t MipIndex = 0;
        while (MipIndex < Data.MipCount)
        {
            const uint32_t RemainingMips = Data.MipCount - MipIndex;
            const uint32_t MipsThisDispatch = (std::min)(4u, RemainingMips);
            const bool bHasSecondMip = MipsThisDispatch > 1;
            const bool bHasThirdMip = MipsThisDispatch > 2;
            const bool bHasFourthMip = MipsThisDispatch > 3;

            const uint32_t SourceWidth = (MipIndex == 0) ? Data.SourceWidth : (std::max)(1u, CurrentWidth);
            const uint32_t SourceHeight = (MipIndex == 0) ? Data.SourceHeight : (std::max)(1u, CurrentHeight);

            const uint32_t DestWidth = (MipIndex == 0) ? CurrentWidth : (std::max)(1u, CurrentWidth / 2);
            const uint32_t DestHeight = (MipIndex == 0) ? CurrentHeight : (std::max)(1u, CurrentHeight / 2);
            const uint32_t DestWidth1 = bHasSecondMip ? (std::max)(1u, DestWidth / 2) : 0u;
            const uint32_t DestHeight1 = bHasSecondMip ? (std::max)(1u, DestHeight / 2) : 0u;
            const uint32_t DestWidth2 = bHasThirdMip ? (std::max)(1u, DestWidth1 / 2) : 0u;
            const uint32_t DestHeight2 = bHasThirdMip ? (std::max)(1u, DestHeight1 / 2) : 0u;
            const uint32_t DestWidth3 = bHasFourthMip ? (std::max)(1u, DestWidth2 / 2) : 0u;
            const uint32_t DestHeight3 = bHasFourthMip ? (std::max)(1u, DestHeight2 / 2) : 0u;

            FHZBConstants Constants = {};
            Constants.SourceWidth = SourceWidth;
            Constants.SourceHeight = SourceHeight;
            Constants.DestWidth = DestWidth;
            Constants.DestHeight = DestHeight;
            Constants.DestWidth1 = DestWidth1;
            Constants.DestHeight1 = DestHeight1;
            Constants.DestWidth2 = DestWidth2;
            Constants.DestHeight2 = DestHeight2;
            Constants.DestWidth3 = DestWidth3;
            Constants.DestHeight3 = DestHeight3;
            Constants.SourceMip = (MipIndex > 0) ? (MipIndex - 1) : 0u;
            Constants.SourceIsDepth = (MipIndex == 0) ? 1u : 0u;

            uint32_t SourceBindlessIndex = Data.DepthBindlessIndex;
            if (MipIndex > 0)
            {
                const uint32_t SourceMipIndex = MipIndex - 1;
                SourceBindlessIndex = HzbTexture.SrvBindlessIndex;

                if (SourceMipIndex < MipStates.size() && MipStates[SourceMipIndex] != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                {
                    const D3D12_RESOURCE_BARRIER ToSrvBarrier = CD3DX12_RESOURCE_BARRIER::Transition(HzbTexture.Get(), MipStates[SourceMipIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12CalcSubresource(SourceMipIndex, 0, 0, Data.MipCount, 1));
                    if (Owner.bLogResourceBarriers)
                    {
                        LogInfo("HZB Barrier: Mip " + std::to_string(SourceMipIndex) + " "
                            + RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateBefore) + " -> "
                            + RendererUtils::ResourceStateToString(ToSrvBarrier.Transition.StateAfter));
                    }
                    LocalCommandList->ResourceBarrier(1, &ToSrvBarrier);
                    MipStates[SourceMipIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                }
            }
            const uint32_t DestIndex0 = (MipIndex < Data.HZBUavs.size()) ? Data.HZBUavs[MipIndex] : UINT32_MAX;
            const uint32_t DestIndex1 = (bHasSecondMip && (MipIndex + 1) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 1]
                : Data.HZBNullUav;
            const uint32_t DestIndex2 = (bHasThirdMip && (MipIndex + 2) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 2]
                : Data.HZBNullUav;
            const uint32_t DestIndex3 = (bHasFourthMip && (MipIndex + 3) < Data.HZBUavs.size())
                ? Data.HZBUavs[MipIndex + 3]
                : Data.HZBNullUav;

            if (SourceBindlessIndex == UINT32_MAX || DestIndex0 == UINT32_MAX || DestIndex1 == UINT32_MAX
                || DestIndex2 == UINT32_MAX || DestIndex3 == UINT32_MAX)
            {
                break;
            }

            ID3D12PipelineState* SelectedPipeline = HzbPipelines[MipsThisDispatch - 1].Get();
            if (!SelectedPipeline)
            {
                break;
            }

            LocalCommandList->SetPipelineState(SelectedPipeline);
            LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
            const uint32_t HZBBindlessIndices[] = { Data.DepthBindlessIndex, SourceBindlessIndex, DestIndex0, DestIndex1, DestIndex2, DestIndex3 };
            LocalCommandList->SetComputeRoot32BitConstants(1, _countof(HZBBindlessIndices), HZBBindlessIndices, 0);

            const uint32_t GroupX = (Constants.DestWidth + 7) / 8;
            const uint32_t GroupY = (Constants.DestHeight + 7) / 8;
            LocalCommandList->Dispatch(GroupX, GroupY, 1);

            if (bHasFourthMip)
            {
                CurrentWidth = DestWidth3;
                CurrentHeight = DestHeight3;
            }
            else if (bHasThirdMip)
            {
                CurrentWidth = DestWidth2;
                CurrentHeight = DestHeight2;
            }
            else if (bHasSecondMip)
            {
                CurrentWidth = DestWidth1;
                CurrentHeight = DestHeight1;
            }
            else
            {
                CurrentWidth = DestWidth;
                CurrentHeight = DestHeight;
            }

            std::vector<D3D12_RESOURCE_BARRIER> Barriers;
            Barriers.reserve(MipsThisDispatch + 1);

            if (Owner.bLogResourceBarriers)
            {
                LogInfo("HZB Barrier: UAV sync");
            }
            Barriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(HzbTexture.Get()));

            for (uint32_t LocalMip = 0; LocalMip < MipsThisDispatch; ++LocalMip)
            {
                const uint32_t TargetMip = MipIndex + LocalMip;
                if (TargetMip >= Data.MipCount)
                {
                    break;
                }

                const D3D12_RESOURCE_BARRIER Barrier = CD3DX12_RESOURCE_BARRIER::Transition(HzbTexture.Get(), MipStates[TargetMip], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12CalcSubresource(TargetMip, 0, 0, Data.MipCount, 1));
                if (Owner.bLogResourceBarriers)
                {
                    LogInfo("HZB Barrier: Mip " + std::to_string(TargetMip) + " "
                        + RendererUtils::ResourceStateToString(Barrier.Transition.StateBefore) + " -> "
                        + RendererUtils::ResourceStateToString(Barrier.Transition.StateAfter));
                }
                Barriers.push_back(Barrier);
                MipStates[TargetMip] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            }

            if (!Barriers.empty())
            {
                LocalCommandList->ResourceBarrier(static_cast<UINT>(Barriers.size()), Barriers.data());
            }

            MipIndex += MipsThisDispatch;
        }

        HzbTexture.State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        bReady = true;
    });
}

bool FHzb::CreateRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: HZB constants (mip counts, dimensions, source mip), used in Shaders/BuildHZB.hlsl BuildHZB
    RootParams[0].InitAsConstants(12, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[1]: HZB bindless indices (b1), used in Shaders/BuildHZB.hlsl BuildHZB
    RootParams[1].InitAsConstants(6, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(HzbRootSignature.GetAddressOf())));
    return true;
}

bool FHzb::CreatePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    for (size_t PipelineIndex = 0; PipelineIndex < HzbPipelines.size(); ++PipelineIndex)
    {
        std::vector<uint8_t> CSByteCode;
        const std::wstring Define = L"HZB_MIPS_PER_DISPATCH=" + std::to_wstring(PipelineIndex + 1);
        const std::vector<std::wstring> Defines = { Define };

        if (!Compiler.CompileFromFile(L"Shaders/BuildHZB.hlsl", L"BuildHZB", CSTarget, CSByteCode, Defines))
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
        PsoDesc.pRootSignature = HzbRootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };

        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(HzbPipelines[PipelineIndex].GetAddressOf())));
    }
    return true;
}

bool FHzb::CreateResources(FDX12Device* Device, uint32_t InWidth, uint32_t InHeight)
{
    const uint32_t BaseWidth = (std::max)(1u, (InWidth + 1) / 2);
    const uint32_t BaseHeight = (std::max)(1u, (InHeight + 1) / 2);

    Width = BaseWidth;
    Height = BaseHeight;
    MipCount = 1;

    uint32_t MipWidth = BaseWidth;
    uint32_t MipHeight = BaseHeight;
    while (MipWidth > 1 || MipHeight > 1)
    {
        MipWidth = (std::max)(1u, MipWidth / 2);
        MipHeight = (std::max)(1u, MipHeight / 2);
        ++MipCount;
    }

    FRGTextureDesc HzbDesc;
    HzbDesc.Width = BaseWidth;
    HzbDesc.Height = BaseHeight;
    HzbDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    HzbDesc.MipLevels = static_cast<uint16_t>(MipCount);

    const D3D12_RESOURCE_DESC ResourceDesc = CreateTexture2DResourceDesc(HzbDesc, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_HEAP_PROPERTIES HeapProps = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

    InitializeBindlessTexture(HzbTexture, HzbDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ResourceDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(HzbTexture.ReleaseAndGetAddressOf())));
    HzbTexture->SetName(L"HierarchicalZBuffer");

    FRGTextureDesc NullDesc;
    NullDesc.Width = 1;
    NullDesc.Height = 1;
    NullDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    NullDesc.MipLevels = 1;

    const D3D12_RESOURCE_DESC NullResourceDesc = CreateTexture2DResourceDesc(NullDesc, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    InitializeBindlessTexture(HzbNullUavTexture, NullDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &NullResourceDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(HzbNullUavTexture.ReleaseAndGetAddressOf())));
    HzbNullUavTexture->SetName(L"HZBNullUavResource");

    return true;
}
