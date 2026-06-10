#include "DiffuseGIDenoiser.h"
#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include <algorithm>
#include <d3dx12.h>
#define A_CPU
#include "../../../Shaders/ffx_a.h"
#include "../../../Shaders/ffx_spd.h"
#undef A_CPU

using Microsoft::WRL::ComPtr;

constexpr uint32_t kDiffuseGIDenoiserConstantsDwordCount = 10;
constexpr uint32_t kDiffuseGIDenoiserBindlessDwordCount  = 16;

struct FDiffuseGIDenoiserConstants
{
    uint32_t Width            = 0;
    uint32_t Height           = 0;
    uint32_t HistoryValid     = 0;
    uint32_t PassIndex        = 0;
    float    DepthThresholdScale = 1.03f;
    float    NormalThreshold  = 0.9f;
    float    BlendStrength    = 1.0f;
    uint32_t MipLevel         = 0;
    float    Padding1         = 0.0f;
    float    Padding2         = 0.0f;
};
static_assert(sizeof(FDiffuseGIDenoiserConstants) / sizeof(uint32_t) <= kDiffuseGIDenoiserConstantsDwordCount);

bool FDiffuseGIDenoiser::ShouldResetHistoryForFreeze(const FDeferredRenderer& Owner) const
{
    if (!Owner.RestirGI->IsEnabled() || !IsEnabled() || !Owner.RestirGI->IsFreezeFrame())
    {
        return false;
    }

    if (FreezeHistoryResetPeriod == 0u)
    {
        return false;
    }

    const uint64_t FreezeFrameIndex = Owner.GetFrameNumber() - Owner.RestirGI->GetFreezeStartFrameNumber();
    return (FreezeFrameIndex % FreezeHistoryResetPeriod) == 0u;
}

bool FDiffuseGIDenoiser::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    const DXGI_FORMAT RestirGiRadianceFormat = Owner.ResolveRestirGiRadianceFormat(Device);
    bPersistentInputsValid = false;
    const uint32_t FrameCount = (std::max)(1u, Owner.GetFramesInFlight());

    HistoryIrradiance.clear();
    HistorySH.clear();
    HistoryCount.clear();
    PrevLinearDepth.clear();
    PrevNormal.clear();

    HistoryIrradiance.resize(FrameCount);
    HistorySH.resize(FrameCount);
    HistoryCount.resize(FrameCount);
    PrevLinearDepth.resize(FrameCount);
    PrevNormal.resize(FrameCount);

    for (uint32_t FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
    {
        const std::wstring Suffix = L"_Frame" + std::to_wstring(FrameIndex);
        CreateBindlessTexture(Device, (L"ReSTIR_GI_HistoryIrradiance" + Suffix).c_str(), { Width, Height, RestirGiRadianceFormat }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, HistoryIrradiance[FrameIndex], true, true);
        CreateBindlessTexture(Device, (L"ReSTIR_GI_HistorySH" + Suffix).c_str(), { Width, Height, DXGI_FORMAT_R32G32B32A32_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, HistorySH[FrameIndex], true, true);
        CreateBindlessTexture(Device, (L"ReSTIR_GI_HistoryCount" + Suffix).c_str(), { Width, Height, DXGI_FORMAT_R8_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, HistoryCount[FrameIndex], true, true);
        CreateBindlessTexture(Device, (L"ReSTIR_GI_PrevLinearDepth" + Suffix).c_str(), { Width, Height, DXGI_FORMAT_R16_FLOAT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PrevLinearDepth[FrameIndex], true, true);
        CreateBindlessTexture(Device, (L"ReSTIR_GI_PrevNormal" + Suffix).c_str(), { Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PrevNormal[FrameIndex], true, true);
    }

    HistoryValid.assign(FrameCount, false);
    PendingHistoryWrite.assign(FrameCount, false);
    CurrentOutputSlot = 0;
    CurrentReadSlot = 0;
    bHistoryValid = false;
    bPassesSubmittedThisFrame = false;
    return true;
}

bool FDiffuseGIDenoiser::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].InitAsConstants(kDiffuseGIDenoiserConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kDiffuseGIDenoiserBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[2].InitAsConstantBufferView(2, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RootSignature.ReleaseAndGetAddressOf())));

    FShaderCompiler Compiler;
    auto CreateDenoiserPso = [&](const wchar_t* ShaderPath, const wchar_t* EntryPoint, Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipeline) -> bool
    {
        std::vector<uint8_t> CSByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, ShaderPath, EntryPoint, CSByteCode))
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
        PsoDesc.pRootSignature = RootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(OutPipeline.ReleaseAndGetAddressOf())));
        return true;
    };

    if (!CreateDenoiserPso(L"Shaders/GIDenoiser/DiffuseGIDenoiser.hlsl", L"CSPreBlur", PreBlurPipeline)
        || !CreateDenoiserPso(L"Shaders/GIDenoiser/DiffuseGIDenoiser.hlsl", L"CSTemporalAccumulation", TemporalAccumulationPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGI/RestirGiMipGenSpd.hlsl", L"CSGenerateShMipsSpd", GenerateShMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGI/RestirGiLinearDepthMipGenSpd.hlsl", L"CSGenerateLinearDepthMipsSpd", GenerateLinearDepthMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/GIDenoiser/DiffuseGIDenoiser.hlsl", L"CSHistoryReconstruction", HistoryReconstructionPipeline)
        || !CreateDenoiserPso(L"Shaders/GIDenoiser/DiffuseGIDenoiser.hlsl", L"CSFinalBlur", FinalBlurPipeline))
    {
        return false;
    }
    return true;
}

void FDiffuseGIDenoiser::AddPasses(FDeferredPassContext& Context, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle) const
{
    bPassesSubmittedThisFrame = false;
    if (!IsReady() || !InputSHHandle || !VarianceHandle)
    {
        bHistoryValid = false;
        std::fill(HistoryValid.begin(), HistoryValid.end(), false);
        std::fill(PendingHistoryWrite.begin(), PendingHistoryWrite.end(), false);
        return;
    }
    bPassesSubmittedThisFrame = true;
    if (CurrentOutputSlot < PendingHistoryWrite.size())
    {
        PendingHistoryWrite[CurrentOutputSlot] = true;
        HistoryValid[CurrentOutputSlot] = false;
    }

    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredGBufferHandles& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle           = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle        = Context.Resources.VelocityHandle;
    const FRGResourceHandle LinearDepthHandle     = Context.Resources.LinearDepthHandle;

    FDiffuseGIDenoiserFrameResources& DenoiserResources = Context.Resources.DiffuseGIDenoiser;
    const FRGResourceHandle HistoryIrradianceHandle     = DenoiserResources.HistoryIrradianceHandle;
    const FRGResourceHandle HistorySHReadHandle          = DenoiserResources.HistorySHReadHandle;
    const FRGResourceHandle HistorySHWriteHandle         = DenoiserResources.HistorySHWriteHandle;
    const FRGResourceHandle HistoryCountReadHandle       = DenoiserResources.HistoryCountReadHandle;
    const FRGResourceHandle HistoryCountWriteHandle      = DenoiserResources.HistoryCountWriteHandle;
    const FRGResourceHandle PrevLinearDepthReadHandle    = DenoiserResources.PrevLinearDepthReadHandle;
    const FRGResourceHandle PrevLinearDepthWriteHandle   = DenoiserResources.PrevLinearDepthWriteHandle;
    const FRGResourceHandle PrevNormalReadHandle         = DenoiserResources.PrevNormalReadHandle;
    const FRGResourceHandle PrevNormalWriteHandle        = DenoiserResources.PrevNormalWriteHandle;
    FRGResourceHandle& PreBlurSHHandle        = DenoiserResources.PreBlurSHHandle        = {};
    FRGResourceHandle& TemporalSHHandle       = DenoiserResources.TemporalSHHandle       = {};
    FRGResourceHandle& ShMipHandle            = DenoiserResources.ShMipHandle            = {};
    FRGResourceHandle& LinearDepthMipHandle   = DenoiserResources.LinearDepthMipHandle   = {};
    FRGBufferHandle&   SpdAtomicCounterHandle = DenoiserResources.SpdAtomicCounterHandle = {};

    AddPreBlurPass(Owner, Graph, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle);
    AddTemporalAccumulationPass(
        Owner, Graph, GBufferHandles,
        DepthHandle, VelocityHandle, LinearDepthHandle,
        InputSHHandle, VarianceHandle, PreBlurSHHandle, TemporalSHHandle,
        HistorySHReadHandle, HistoryCountReadHandle, HistoryCountWriteHandle,
        PrevLinearDepthReadHandle, PrevLinearDepthWriteHandle,
        PrevNormalReadHandle, PrevNormalWriteHandle);
    AddShMipGenPass(Owner, Graph, TemporalSHHandle, ShMipHandle, SpdAtomicCounterHandle);
    AddLinearDepthMipGenPass(Owner, Graph, LinearDepthHandle, LinearDepthMipHandle, SpdAtomicCounterHandle);
    AddHistoryReconstructionPass(
        Owner, Graph, GBufferHandles,
        LinearDepthHandle, InputSHHandle, VarianceHandle,
        HistorySHReadHandle, HistoryCountWriteHandle, TemporalSHHandle,
        ShMipHandle, LinearDepthMipHandle);
    AddFinalBlurPass(
        Owner, Graph, GBufferHandles,
        LinearDepthHandle, InputSHHandle, VarianceHandle,
        TemporalSHHandle, HistoryIrradianceHandle,
        HistorySHWriteHandle, HistoryCountWriteHandle);
}

void FDiffuseGIDenoiser::AddShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle DestinationHandle{};
        FRGBufferHandle AtomicCounterHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser SH Mip SPD", [this, &Owner, SourceHandle, &DestinationHandle, &AtomicCounterHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Diffuse GI Denoiser");
        Data.bEnabled = IsEnabled();
        if (!Data.bEnabled)
        {
            return;
        }
        const uint32_t HalfWidth = (static_cast<uint32_t>(Owner.Viewport.Width) + 1u) / 2u;
        const uint32_t HalfHeight = (static_cast<uint32_t>(Owner.Viewport.Height) + 1u) / 2u;
        Data.DestinationHandle = Builder.CreateTexture("ReSTIR GI SH Mips", { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32B32A32_UINT, 4u });
        DestinationHandle = Data.DestinationHandle;
        if (!AtomicCounterHandle)
        {
            FRGBufferDesc CounterDesc = {};
            CounterDesc.Size = sizeof(uint32_t) * 4u;
            CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            CounterDesc.ViewFormat = DXGI_FORMAT_R32_TYPELESS;
            CounterDesc.NumElements = static_cast<uint32_t>(CounterDesc.Size / sizeof(uint32_t));
            CounterDesc.UavFlags = D3D12_BUFFER_UAV_FLAG_RAW;
            AtomicCounterHandle = Builder.CreateBuffer("ReSTIR GI SPD Atomic Counter", CounterDesc);
        }
        Data.AtomicCounterHandle = AtomicCounterHandle;
        Builder.ReadTexture(SourceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Data.DestinationHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.AtomicCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &Graph, SourceHandle](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        AU1 DispatchThreadGroupCountXY[2] = { 0u, 0u };
        AU1 WorkGroupOffset[2] = { 0u, 0u };
        AU1 NumWorkGroupsAndMips[2] = { 0u, 0u };
        AU1 RectInfo[4] = { 0u, 0u, static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Width)), static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Height)) };
        SpdSetup(DispatchThreadGroupCountXY, WorkGroupOffset, NumWorkGroupsAndMips, RectInfo, AU1_(4));

        const uint32_t TemporalShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(SourceHandle);
        const uint32_t AtomicCounterUavBindlessIndex = Graph.GetBufferUavBindlessIndex(Data.AtomicCounterHandle);
        const uint32_t ShMipUav0 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 0u);
        const uint32_t ShMipUav1 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 1u);
        const uint32_t ShMipUav2 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 2u);
        const uint32_t ShMipUav3 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 3u);
        uint32_t SpdConstants[kDiffuseGIDenoiserConstantsDwordCount] =
        {
            TemporalShSrvBindlessIndex,
            AtomicCounterUavBindlessIndex,
            ShMipUav0,
            ShMipUav1,
            ShMipUav2,
            ShMipUav3,
            NumWorkGroupsAndMips[1],
            NumWorkGroupsAndMips[0],
            WorkGroupOffset[0],
            WorkGroupOffset[1]
        };
        if (!AreAllBindlessIndicesValid(SpdConstants[0], SpdConstants[2], SpdConstants[3], SpdConstants[4], SpdConstants[5]))
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(GenerateShMipsPipeline.Get());
        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        ID3D12Resource* AtomicCounterResource = Graph.GetBufferResource(Data.AtomicCounterHandle);
        if (!AtomicCounterResource || !IsValidBindlessIndex(AtomicCounterUavBindlessIndex))
        {
            return;
        }
        const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = Owner.GetBindlessGpuHandle(AtomicCounterUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = Owner.GetBindlessCpuClearHandle(AtomicCounterUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, AtomicCounterResource, ClearValues, 0, nullptr);

        const auto CounterBarrier = CD3DX12_RESOURCE_BARRIER::UAV(AtomicCounterResource);
        LocalCommandList->ResourceBarrier(1, &CounterBarrier);
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(SpdConstants), SpdConstants, 0);
        LocalCommandList->Dispatch(DispatchThreadGroupCountXY[0], DispatchThreadGroupCountXY[1], 1);

        ID3D12Resource* DestinationResource = Graph.GetTextureResource(Data.DestinationHandle);
        if (DestinationResource)
        {
            const auto MipBarrier = CD3DX12_RESOURCE_BARRIER::UAV(DestinationResource);
            LocalCommandList->ResourceBarrier(1, &MipBarrier);
        }
    });
}

void FDiffuseGIDenoiser::AddLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle DestinationHandle{};
        FRGBufferHandle AtomicCounterHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser Depth Mip SPD", [this, &Owner, SourceHandle, &DestinationHandle, &AtomicCounterHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Diffuse GI Denoiser");
        Data.bEnabled = IsEnabled();
        if (!Data.bEnabled)
        {
            return;
        }
        const uint32_t HalfWidth = (static_cast<uint32_t>(Owner.Viewport.Width) + 1u) / 2u;
        const uint32_t HalfHeight = (static_cast<uint32_t>(Owner.Viewport.Height) + 1u) / 2u;
        Data.DestinationHandle = Builder.CreateTexture("ReSTIR GI LinearDepth Mips", { HalfWidth, HalfHeight, DXGI_FORMAT_R16_FLOAT, 4u });
        DestinationHandle = Data.DestinationHandle;
        if (!AtomicCounterHandle)
        {
            FRGBufferDesc CounterDesc = {};
            CounterDesc.Size = sizeof(uint32_t) * 4u;
            CounterDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            CounterDesc.ViewFormat = DXGI_FORMAT_R32_TYPELESS;
            CounterDesc.NumElements = static_cast<uint32_t>(CounterDesc.Size / sizeof(uint32_t));
            CounterDesc.UavFlags = D3D12_BUFFER_UAV_FLAG_RAW;
            AtomicCounterHandle = Builder.CreateBuffer("ReSTIR GI SPD Atomic Counter", CounterDesc);
        }
        Data.AtomicCounterHandle = AtomicCounterHandle;
        Builder.ReadTexture(SourceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Data.DestinationHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(Data.AtomicCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &Graph](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        AU1 DispatchThreadGroupCountXY[2] = { 0u, 0u };
        AU1 WorkGroupOffset[2] = { 0u, 0u };
        AU1 NumWorkGroupsAndMips[2] = { 0u, 0u };
        AU1 RectInfo[4] = { 0u, 0u, static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Width)), static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Height)) };
        SpdSetup(DispatchThreadGroupCountXY, WorkGroupOffset, NumWorkGroupsAndMips, RectInfo, AU1_(4));

        const uint32_t AtomicCounterUavBindlessIndex = Graph.GetBufferUavBindlessIndex(Data.AtomicCounterHandle);
        const uint32_t DepthMipUav0 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 0u);
        const uint32_t DepthMipUav1 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 1u);
        const uint32_t DepthMipUav2 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 2u);
        const uint32_t DepthMipUav3 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 3u);
        uint32_t SpdConstants[kDiffuseGIDenoiserConstantsDwordCount] =
        {
            Owner.LinearDepthTexture.SrvBindlessIndex,
            AtomicCounterUavBindlessIndex,
            DepthMipUav0,
            DepthMipUav1,
            DepthMipUav2,
            DepthMipUav3,
            NumWorkGroupsAndMips[1],
            NumWorkGroupsAndMips[0],
            WorkGroupOffset[0],
            WorkGroupOffset[1]
        };

        if (!AreAllBindlessIndicesValid(SpdConstants[0], SpdConstants[2], SpdConstants[3], SpdConstants[4], SpdConstants[5])) 
        { 
            return; 
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(GenerateLinearDepthMipsPipeline.Get());

        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        ID3D12Resource* AtomicCounterResource = Graph.GetBufferResource(Data.AtomicCounterHandle);
        if (!AtomicCounterResource || !IsValidBindlessIndex(AtomicCounterUavBindlessIndex))
        {
            return;
        }

        const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = Owner.GetBindlessGpuHandle(AtomicCounterUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = Owner.GetBindlessCpuClearHandle(AtomicCounterUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, AtomicCounterResource, ClearValues, 0, nullptr);

        const auto CounterBarrier = CD3DX12_RESOURCE_BARRIER::UAV(AtomicCounterResource);
        LocalCommandList->ResourceBarrier(1, &CounterBarrier);
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(SpdConstants), SpdConstants, 0);
        LocalCommandList->Dispatch(DispatchThreadGroupCountXY[0], DispatchThreadGroupCountXY[1], 1);

        ID3D12Resource* DestinationResource = Graph.GetTextureResource(Data.DestinationHandle);
        if (DestinationResource)
        {
            const auto MipBarrier = CD3DX12_RESOURCE_BARRIER::UAV(DestinationResource);
            LocalCommandList->ResourceBarrier(1, &MipBarrier);
        }
    });
}

void FDiffuseGIDenoiser::AddHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredGBufferHandles& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
    };
    Graph.AddPass<FPassData>("Denoiser HistoryBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, HistorySHHandle, HistoryCountHandle, TemporalSHHandle, ShMipHandle, DepthMipHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Diffuse GI Denoiser");
        Data.bEnabled = IsEnabled();
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(InputSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VarianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistorySHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(ShMipHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthMipHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &Graph, InputSHHandle, VarianceHandle, HistorySHHandle, HistoryCountHandle, TemporalSHHandle, ShMipHandle, DepthMipHandle](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t TemporalShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(TemporalSHHandle);
        const uint32_t LinearDepthMipSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(DepthMipHandle);
        const uint32_t ShMipSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(ShMipHandle);
        const uint32_t HistoryShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(HistorySHHandle);
        const uint32_t HistoryShUavBindlessIndex = CurrentOutputSlot < HistorySH.size() ? HistorySH[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t HistoryCountSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(HistoryCountHandle);
        const uint32_t HistoryCountUavBindlessIndex = CurrentOutputSlot < HistoryCount.size() ? HistoryCount[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t HistoryIrradianceUavBindlessIndex = CurrentOutputSlot < HistoryIrradiance.size() ? HistoryIrradiance[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t PrevLinearDepthSrvBindlessIndex = CurrentReadSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalSrvBindlessIndex = CurrentReadSlot < PrevNormal.size() ? PrevNormal[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalUavBindlessIndex = CurrentOutputSlot < PrevNormal.size() ? PrevNormal[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;

        const bool bInputsValid = AreAllBindlessIndicesValid(
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            TemporalShUavBindlessIndex,
            LinearDepthMipSrvBindlessIndex,
            ShMipSrvBindlessIndex,
            HistoryShSrvBindlessIndex,
            HistoryCountSrvBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        FDiffuseGIDenoiserConstants Constants = {};
        Constants.Width   = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.Height  = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.PassIndex = 4u;
        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        const uint32_t Bindless[kDiffuseGIDenoiserBindlessDwordCount] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            PrevNormalSrvBindlessIndex,
            HistoryShSrvBindlessIndex,
            HistoryCountSrvBindlessIndex,
            TemporalShUavBindlessIndex,
            HistoryIrradianceUavBindlessIndex,
            HistoryShUavBindlessIndex,
            HistoryCountUavBindlessIndex,
            LinearDepthMipSrvBindlessIndex,
            PrevNormalUavBindlessIndex,
            ShMipSrvBindlessIndex
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(HistoryReconstructionPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FDiffuseGIDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FDiffuseGIDenoiser::AddFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredGBufferHandles& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
    };
    Graph.AddPass<FPassData>("Denoiser FinalBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Diffuse GI Denoiser");
        Data.bEnabled = IsEnabled();
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(InputSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VarianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(HistoryIrradianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistorySHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &Graph, InputSHHandle, VarianceHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountHandle](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t TemporalShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(TemporalSHHandle);
        const uint32_t HistoryIrradianceUavBindlessIndex = Graph.GetTextureUavBindlessIndex(HistoryIrradianceHandle);
        const uint32_t HistoryShSrvBindlessIndex = CurrentReadSlot < HistorySH.size() ? HistorySH[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t HistoryShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(HistorySHHandle);
        const uint32_t HistoryCountSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(HistoryCountHandle);
        const uint32_t HistoryCountUavBindlessIndex = Graph.GetTextureUavBindlessIndex(HistoryCountHandle);
        const uint32_t PrevLinearDepthSrvBindlessIndex = CurrentReadSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevLinearDepthUavBindlessIndex = CurrentOutputSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalSrvBindlessIndex = CurrentReadSlot < PrevNormal.size() ? PrevNormal[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalUavBindlessIndex = CurrentOutputSlot < PrevNormal.size() ? PrevNormal[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;

        const bool bInputsValid = AreAllBindlessIndicesValid(
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            TemporalShSrvBindlessIndex,
            HistoryIrradianceUavBindlessIndex,
            HistoryShUavBindlessIndex,
            HistoryCountSrvBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        FDiffuseGIDenoiserConstants Constants = {};
        Constants.Width   = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.Height  = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.PassIndex = 5u;

        const uint32_t Bindless[kDiffuseGIDenoiserBindlessDwordCount] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            PrevNormalSrvBindlessIndex,
            HistoryShSrvBindlessIndex,
            HistoryCountSrvBindlessIndex,
            TemporalShSrvBindlessIndex,
            HistoryIrradianceUavBindlessIndex,
            HistoryShUavBindlessIndex,
            HistoryCountUavBindlessIndex,
            PrevLinearDepthUavBindlessIndex,
            PrevNormalUavBindlessIndex,
            UINT32_MAX
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(FinalBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FDiffuseGIDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FDiffuseGIDenoiser::AddPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredGBufferHandles& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle& PreBlurSHHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle PreBlurSHHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser PreBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, &PreBlurSHHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Diffuse GI Denoiser");
        Data.bEnabled = IsEnabled();
        if (!Data.bEnabled)
        {
            return;
        }
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Data.PreBlurSHHandle = Builder.CreateTexture("ReSTIR GI PreBlur SH", { FullWidth, FullHeight, DXGI_FORMAT_R32G32B32A32_UINT });
        PreBlurSHHandle = Data.PreBlurSHHandle;
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(InputSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VarianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Data.PreBlurSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &Graph, InputSHHandle, VarianceHandle](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t PreBlurShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Data.PreBlurSHHandle);

        const bool bInputsValid = AreAllBindlessIndicesValid(
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            PreBlurShUavBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        FDiffuseGIDenoiserConstants Constants = {};
        Constants.Width  = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.Height = static_cast<uint32_t>(Owner.Viewport.Height);
        const uint32_t HistoryIrradianceUavBindlessIndex = CurrentOutputSlot < HistoryIrradiance.size() ? HistoryIrradiance[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t HistoryShSrvBindlessIndex = CurrentReadSlot < HistorySH.size() ? HistorySH[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t HistoryShUavBindlessIndex = CurrentOutputSlot < HistorySH.size() ? HistorySH[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t HistoryCountSrvBindlessIndex = CurrentReadSlot < HistoryCount.size() ? HistoryCount[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t HistoryCountUavBindlessIndex = CurrentOutputSlot < HistoryCount.size() ? HistoryCount[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t PrevLinearDepthSrvBindlessIndex = CurrentReadSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevLinearDepthUavBindlessIndex = CurrentOutputSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalSrvBindlessIndex = CurrentReadSlot < PrevNormal.size() ? PrevNormal[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalUavBindlessIndex = CurrentOutputSlot < PrevNormal.size() ? PrevNormal[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());

        const uint32_t PreBlurBindless[kDiffuseGIDenoiserBindlessDwordCount] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            PrevNormalSrvBindlessIndex,
            HistoryShSrvBindlessIndex,
            HistoryCountSrvBindlessIndex,
            UINT32_MAX,
            HistoryIrradianceUavBindlessIndex,
            HistoryShUavBindlessIndex,
            HistoryCountUavBindlessIndex,
            PrevLinearDepthUavBindlessIndex,
            PrevNormalUavBindlessIndex,
            PreBlurShUavBindlessIndex
        };

        LocalCommandList->SetPipelineState(PreBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FDiffuseGIDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(PreBlurBindless), PreBlurBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FDiffuseGIDenoiser::AddTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredGBufferHandles& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle& TemporalSHHandle, FRGResourceHandle HistorySHReadHandle, FRGResourceHandle HistoryCountReadHandle, FRGResourceHandle HistoryCountWriteHandle, FRGResourceHandle PrevLinearDepthReadHandle, FRGResourceHandle PrevLinearDepthWriteHandle, FRGResourceHandle PrevNormalReadHandle, FRGResourceHandle PrevNormalWriteHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle TemporalSHHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser TemporalAccum", [this, &Owner, GBufferHandles, DepthHandle, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle, HistorySHReadHandle, HistoryCountReadHandle, &TemporalSHHandle, HistoryCountWriteHandle, PrevLinearDepthReadHandle, PrevLinearDepthWriteHandle, PrevNormalReadHandle, PrevNormalWriteHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Diffuse GI Denoiser");
        Data.bEnabled = IsEnabled();
        if (!Data.bEnabled)
        {
            return;
        }
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Data.TemporalSHHandle = Builder.CreateTexture("ReSTIR GI Temporal SH", { FullWidth, FullHeight, DXGI_FORMAT_R32G32B32A32_UINT });
        TemporalSHHandle = Data.TemporalSHHandle;
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(InputSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VarianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PreBlurSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistorySHReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevNormalReadHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Data.TemporalSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountWriteHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevLinearDepthWriteHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevNormalWriteHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &Graph, InputSHHandle, VarianceHandle, PreBlurSHHandle](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        const uint32_t DepthBindlessIndex = Owner.GetCurrentDepthSrvBindlessIndex();
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t PreBlurShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(PreBlurSHHandle);
        const uint32_t TemporalShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Data.TemporalSHHandle);
        const uint32_t HistoryShSrvBindlessIndex = CurrentReadSlot < HistorySH.size() ? HistorySH[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t HistoryCountSrvBindlessIndex = CurrentReadSlot < HistoryCount.size() ? HistoryCount[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t HistoryCountUavBindlessIndex = CurrentOutputSlot < HistoryCount.size() ? HistoryCount[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t PrevLinearDepthSrvBindlessIndex = CurrentReadSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevLinearDepthUavBindlessIndex = CurrentOutputSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalSrvBindlessIndex = CurrentReadSlot < PrevNormal.size() ? PrevNormal[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
        const uint32_t PrevNormalUavBindlessIndex = CurrentOutputSlot < PrevNormal.size() ? PrevNormal[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;

        const bool bInputsValid = AreAllBindlessIndicesValid(
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            PreBlurShSrvBindlessIndex,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            TemporalShUavBindlessIndex,
            HistoryShSrvBindlessIndex,
            HistoryCountSrvBindlessIndex,
            HistoryCountUavBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            PrevLinearDepthUavBindlessIndex,
            PrevNormalSrvBindlessIndex,
            PrevNormalUavBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        FDiffuseGIDenoiserConstants Constants = {};
        Constants.Width  = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.Height = static_cast<uint32_t>(Owner.Viewport.Height);
        const bool bResetHistoryThisFrame = ShouldResetHistoryForFreeze(Owner);
        Constants.HistoryValid = (bHistoryValid && !bResetHistoryThisFrame) ? 1u : 0u;
        Constants.PassIndex = 1u;
        Constants.MipLevel = 0u;

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());

        const uint32_t TemporalBindless[kDiffuseGIDenoiserBindlessDwordCount] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferA.SrvBindlessIndex,
            PrevNormalSrvBindlessIndex,
            HistoryShSrvBindlessIndex,
            HistoryCountSrvBindlessIndex,
            TemporalShUavBindlessIndex,
            DepthBindlessIndex,
            TemporalShUavBindlessIndex,
            HistoryCountUavBindlessIndex,
            PrevLinearDepthUavBindlessIndex,
            PrevNormalUavBindlessIndex,
            PreBlurShSrvBindlessIndex
        };

        LocalCommandList->SetPipelineState(TemporalAccumulationPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FDiffuseGIDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(TemporalBindless), TemporalBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FDiffuseGIDenoiser::ImportPersistentResources(FDeferredPassContext& Context)
{
    FRenderGraph& Graph = Context.Graph;
    FDiffuseGIDenoiserFrameResources& OutResources = Context.Resources.DiffuseGIDenoiser;
    const uint32_t FrameCount = static_cast<uint32_t>(HistoryIrradiance.size());
    if (FrameCount == 0u)
    {
        bHistoryValid = false;
        return;
    }

    CurrentOutputSlot = GetFrameSlot(Context.FrameIndex);
    CurrentReadSlot = GetFrameSlot(Context.FrameIndex + FrameCount - 1u);
    bHistoryValid = CurrentReadSlot < HistoryValid.size() ? HistoryValid[CurrentReadSlot] : false;

    OutResources.HistoryIrradianceHandle = ImportBindlessTexture(Graph, "ReSTIR GI Denoised Irradiance", HistoryIrradiance[CurrentOutputSlot]);
    OutResources.HistorySHReadHandle = ImportBindlessTexture(Graph, "ReSTIR GI History SH Read", HistorySH[CurrentReadSlot]);
    OutResources.HistorySHWriteHandle = ImportBindlessTexture(Graph, "ReSTIR GI History SH Write", HistorySH[CurrentOutputSlot]);
    OutResources.HistoryCountReadHandle = ImportBindlessTexture(Graph, "ReSTIR GI History Count Read", HistoryCount[CurrentReadSlot]);
    OutResources.HistoryCountWriteHandle = ImportBindlessTexture(Graph, "ReSTIR GI History Count Write", HistoryCount[CurrentOutputSlot]);
    OutResources.PrevLinearDepthReadHandle = ImportBindlessTexture(Graph, "ReSTIR GI Prev LinearDepth Read", PrevLinearDepth[CurrentReadSlot]);
    OutResources.PrevLinearDepthWriteHandle = ImportBindlessTexture(Graph, "ReSTIR GI Prev LinearDepth Write", PrevLinearDepth[CurrentOutputSlot]);
    OutResources.PrevNormalReadHandle = ImportBindlessTexture(Graph, "ReSTIR GI Prev Normal Read", PrevNormal[CurrentReadSlot]);
    OutResources.PrevNormalWriteHandle = ImportBindlessTexture(Graph, "ReSTIR GI Prev Normal Write", PrevNormal[CurrentOutputSlot]);
}

bool FDiffuseGIDenoiser::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    (void)Device;
    RefreshPersistentInputValidation();
    return true;
}

void FDiffuseGIDenoiser::RefreshPersistentInputValidation()
{
    const auto AllTexturesReady = [](const std::vector<FBindlessTexture>& Textures)
    {
        return !Textures.empty() && std::all_of(Textures.begin(), Textures.end(), [](const FBindlessTexture& Texture)
        {
            return Texture.IsFullyBound();
        });
    };

    bPersistentInputsValid =
        RootSignature &&
        PreBlurPipeline &&
        TemporalAccumulationPipeline &&
        GenerateShMipsPipeline &&
        GenerateLinearDepthMipsPipeline &&
        HistoryReconstructionPipeline &&
        FinalBlurPipeline &&
        AllTexturesReady(HistoryIrradiance) &&
        AllTexturesReady(HistorySH) &&
        AllTexturesReady(HistoryCount) &&
        AllTexturesReady(PrevLinearDepth) &&
        AllTexturesReady(PrevNormal);
}

bool FDiffuseGIDenoiser::IsReady() const
{
    return IsEnabled() && bPersistentInputsValid;
}

void FDiffuseGIDenoiser::FinalizeFrame(FDeferredRenderer& Owner)
{
    (void)Owner;
    bPassesSubmittedThisFrame = false;
}

void FDiffuseGIDenoiser::OnFrameFenceSignaled(uint32_t FrameIndex)
{
    if (!IsReady() || HistoryValid.empty())
    {
        return;
    }

    const uint32_t Slot = GetFrameSlot(FrameIndex);
    if (Slot < HistoryValid.size() && Slot < PendingHistoryWrite.size() && PendingHistoryWrite[Slot])
    {
        HistoryValid[Slot] = true;
        PendingHistoryWrite[Slot] = false;
    }
}

void FDiffuseGIDenoiser::InvalidateHistory()
{
    bHistoryValid = false;
    std::fill(HistoryValid.begin(), HistoryValid.end(), false);
    std::fill(PendingHistoryWrite.begin(), PendingHistoryWrite.end(), false);
}

uint32_t FDiffuseGIDenoiser::GetFrameSlot(uint32_t FrameIndex) const
{
    const uint32_t FrameCount = static_cast<uint32_t>(HistoryIrradiance.size());
    return FrameCount > 0u ? (FrameIndex % FrameCount) : 0u;
}

ID3D12Resource* FDiffuseGIDenoiser::GetCurrentOutputTexture() const
{
    return CurrentOutputSlot < HistoryIrradiance.size() ? HistoryIrradiance[CurrentOutputSlot].Get() : nullptr;
}

uint32_t FDiffuseGIDenoiser::GetCurrentOutputSrvBindlessIndex() const
{
    return CurrentOutputSlot < HistoryIrradiance.size() ? HistoryIrradiance[CurrentOutputSlot].SrvBindlessIndex : UINT32_MAX;
}

uint32_t FDiffuseGIDenoiser::GetCurrentOutputUavBindlessIndex() const
{
    return CurrentOutputSlot < HistoryIrradiance.size() ? HistoryIrradiance[CurrentOutputSlot].UavBindlessIndex : UINT32_MAX;
}

uint32_t FDiffuseGIDenoiser::GetPrevLinearDepthSrvBindlessIndex() const
{
    return CurrentReadSlot < PrevLinearDepth.size() ? PrevLinearDepth[CurrentReadSlot].SrvBindlessIndex : UINT32_MAX;
}
