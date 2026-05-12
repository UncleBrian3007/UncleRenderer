#include "RestirGIDenoiser.h"
#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include <d3dx12.h>
#define A_CPU
#include "../../../Shaders/ffx_a.h"
#include "../../../Shaders/ffx_spd.h"
#undef A_CPU

using Microsoft::WRL::ComPtr;

constexpr uint32_t kRestirGIDenoiserConstantsDwordCount = 10;
constexpr uint32_t kRestirGIDenoiserBindlessDwordCount  = 16;

bool FRestirGIDenoiser::ShouldResetHistoryForFreeze(const FDeferredRenderer& Owner) const
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

bool FRestirGIDenoiser::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    const DXGI_FORMAT RestirGiRadianceFormat = Owner.ResolveRestirGiRadianceFormat(Device);
    bPersistentInputsValid = false;

    CreateBindlessTexture(Device, L"ReSTIR_GI_HistoryIrradiance", { Width, Height, RestirGiRadianceFormat }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, HistoryIrradiance, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_HistorySH", { Width, Height, DXGI_FORMAT_R32G32B32A32_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, HistorySH, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_HistoryCountA", { Width, Height, DXGI_FORMAT_R8_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, HistoryCountA, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_HistoryCountB", { Width, Height, DXGI_FORMAT_R8_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, HistoryCountB, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_PrevLinearDepth", { Width, Height, DXGI_FORMAT_R16_FLOAT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PrevLinearDepth, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_PrevNormal", { Width, Height, DXGI_FORMAT_R16G16B16A16_FLOAT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, PrevNormal, true, true);
    return true;
}

bool FRestirGIDenoiser::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    CD3DX12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].InitAsConstants(kRestirGIDenoiserConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kRestirGIDenoiserBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
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

    if (!CreateDenoiserPso(L"Shaders/RestirGI/RestirGIDenoiser.hlsl", L"CSPreBlur", PreBlurPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGI/RestirGIDenoiser.hlsl", L"CSTemporalAccumulation", TemporalAccumulationPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGI/RestirGiMipGenSpd.hlsl", L"CSGenerateShMipsSpd", GenerateShMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGI/RestirGiLinearDepthMipGenSpd.hlsl", L"CSGenerateLinearDepthMipsSpd", GenerateLinearDepthMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGI/RestirGIDenoiser.hlsl", L"CSHistoryReconstruction", HistoryReconstructionPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGI/RestirGIDenoiser.hlsl", L"CSFinalBlur", FinalBlurPipeline))
    {
        return false;
    }
    return true;
}

void FRestirGIDenoiser::AddPasses(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle InputSHHandle = Context.Resources.RestirGI.RestirGiInputSHHandle;
    const FRGResourceHandle VarianceHandle = Context.Resources.RestirGI.RestirGiVarianceHandle;
    FRestirGIDenoiserFrameResources& DenoiserResources = Context.Resources.RestirGIDenoiser;
    const FRGResourceHandle HistorySHHandle = DenoiserResources.HistorySHHandle;
    const FRGResourceHandle HistoryIrradianceHandle = DenoiserResources.HistoryIrradianceHandle;
    const FRGResourceHandle HistoryCountAHandle = DenoiserResources.HistoryCountAHandle;
    const FRGResourceHandle HistoryCountBHandle = DenoiserResources.HistoryCountBHandle;
    const FRGResourceHandle PrevLinearDepthHandle = DenoiserResources.PrevLinearDepthHandle;
    const FRGResourceHandle PrevNormalHandle = DenoiserResources.PrevNormalHandle;
    FRGResourceHandle& ShMipHandle = DenoiserResources.ShMipHandle;
    FRGResourceHandle& LinearDepthMipHandle = DenoiserResources.LinearDepthMipHandle;
    FRGBufferHandle& SpdAtomicCounterHandle = DenoiserResources.SpdAtomicCounterHandle;

    if (!IsEnabled())
    {
        return;
    }

    FRGResourceHandle& PreBlurSHHandle = DenoiserResources.PreBlurSHHandle;
    FRGResourceHandle& TemporalSHHandle = DenoiserResources.TemporalSHHandle;
    PreBlurSHHandle = {};
    TemporalSHHandle = {};
    ShMipHandle = {};
    LinearDepthMipHandle = {};
    SpdAtomicCounterHandle = {};

    if (!IsReady())
    {
        return;
    }

    AddFreezeResetPass(Owner, Graph, HistorySHHandle, HistoryCountAHandle, HistoryCountBHandle);
    AddPreBlurPass(Owner, Graph, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle);
    AddTemporalAccumulationPass(Owner, Graph, GBufferHandles, DepthHandle, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle, TemporalSHHandle, HistorySHHandle, HistoryCountAHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle);
    AddShMipGenPass(Owner, Graph, TemporalSHHandle, ShMipHandle, SpdAtomicCounterHandle);
    AddLinearDepthMipGenPass(Owner, Graph, LinearDepthHandle, LinearDepthMipHandle, SpdAtomicCounterHandle);
    AddHistoryReconstructionPass(Owner, Graph, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, HistorySHHandle, HistoryCountBHandle, TemporalSHHandle, ShMipHandle, LinearDepthMipHandle);
    AddFinalBlurPass(Owner, Graph, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountBHandle);
}

void FRestirGIDenoiser::AddFreezeResetPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
    };
    Graph.AddPass<FPassData>("Denoiser Freeze Reset", [this, &Owner, HistorySHHandle, HistoryCountAHandle, HistoryCountBHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = ShouldResetHistoryForFreeze(Owner);
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.WriteTexture(HistorySHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t ClearUint4[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryShGpuHandle = Owner.GetBindlessGpuHandle(HistorySH.UavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryShCpuHandle = Owner.GetBindlessCpuClearHandle(HistorySH.UavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryShGpuHandle, HistoryShCpuHandle, HistorySH.Get(), ClearUint4, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryCountAGpuHandle = Owner.GetBindlessGpuHandle(HistoryCountA.UavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryCountACpuHandle = Owner.GetBindlessCpuClearHandle(HistoryCountA.UavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryCountAGpuHandle, HistoryCountACpuHandle, HistoryCountA.Get(), ClearUint4, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryCountBGpuHandle = Owner.GetBindlessGpuHandle(HistoryCountB.UavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryCountBCpuHandle = Owner.GetBindlessCpuClearHandle(HistoryCountB.UavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryCountBGpuHandle, HistoryCountBCpuHandle, HistoryCountB.Get(), ClearUint4, 0, nullptr);
    });
}

void FRestirGIDenoiser::AddShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle DestinationHandle{};
        FRGBufferHandle AtomicCounterHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser SH Mip SPD", [this, &Owner, SourceHandle, &DestinationHandle, &AtomicCounterHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.RestirGI->IsEnabled();
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
        uint32_t SpdConstants[10] =
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
        static_assert(_countof(SpdConstants) <= kRestirGIDenoiserConstantsDwordCount);
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

void FRestirGIDenoiser::AddLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle& DestinationHandle, FRGBufferHandle& AtomicCounterHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle DestinationHandle{};
        FRGBufferHandle AtomicCounterHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser Depth Mip SPD", [this, &Owner, SourceHandle, &DestinationHandle, &AtomicCounterHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.RestirGI->IsEnabled();
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
        uint32_t SpdConstants[10] =
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
        static_assert(_countof(SpdConstants) <= kRestirGIDenoiserConstantsDwordCount);
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

void FRestirGIDenoiser::AddHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
    };
    Graph.AddPass<FPassData>("Denoiser HistoryBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, HistorySHHandle, HistoryCountHandle, TemporalSHHandle, ShMipHandle, DepthMipHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.RestirGI->IsEnabled();
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
    }, [this, &Owner, &Graph, InputSHHandle, VarianceHandle, TemporalSHHandle, ShMipHandle, DepthMipHandle](const FPassData& Data, FDX12CommandContext& Cmd)
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

        const bool bInputsValid = AreAllBindlessIndicesValid(
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            TemporalShUavBindlessIndex,
            LinearDepthMipSrvBindlessIndex,
            ShMipSrvBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        struct FRestirGiDenoiserConstants
        {
            uint32_t Width;
            uint32_t Height;
            uint32_t HistoryValid;
            uint32_t PassIndex;
            float DepthThresholdScale;
            float NormalThreshold;
            float BlendStrength;
            uint32_t MipLevel;
            float Padding1;
            float Padding2;
        };
        FRestirGiDenoiserConstants Constants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            0u,
            4u,
            1.03f,
            0.9f,
            1.0f,
            0u,
            0.0f,
            0.0f
        };
        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        const uint32_t Bindless[16] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepth.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormal.SrvBindlessIndex,
            HistorySH.SrvBindlessIndex,
            HistoryCountB.SrvBindlessIndex,
            TemporalShUavBindlessIndex,
            HistoryIrradiance.UavBindlessIndex,
            HistorySH.UavBindlessIndex,
            HistoryCountB.UavBindlessIndex,
            LinearDepthMipSrvBindlessIndex,
            PrevNormal.UavBindlessIndex,
            ShMipSrvBindlessIndex
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(HistoryReconstructionPipeline.Get());
        static_assert(sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t) <= kRestirGIDenoiserConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        static_assert(_countof(Bindless) <= kRestirGIDenoiserBindlessDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FRestirGIDenoiser::AddFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
    };
    Graph.AddPass<FPassData>("Denoiser FinalBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.RestirGI->IsEnabled();
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
    }, [this, &Owner, &Graph, InputSHHandle, VarianceHandle, TemporalSHHandle](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t TemporalShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(TemporalSHHandle);

        const bool bInputsValid = AreAllBindlessIndicesValid(
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            TemporalShSrvBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        struct FRestirGiDenoiserConstants
        {
            uint32_t Width;
            uint32_t Height;
            uint32_t HistoryValid;
            uint32_t PassIndex;
            float DepthThresholdScale;
            float NormalThreshold;
            float BlendStrength;
            uint32_t MipLevel;
            float Padding1;
            float Padding2;
        };
        FRestirGiDenoiserConstants Constants =
        {
            static_cast<uint32_t>(Owner.Viewport.Width),
            static_cast<uint32_t>(Owner.Viewport.Height),
            0u,
            5u,
            1.03f,
            0.9f,
            1.0f,
            0u,
            0.0f,
            0.0f
        };

        const uint32_t Bindless[16] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepth.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormal.SrvBindlessIndex,
            HistorySH.SrvBindlessIndex,
            HistoryCountB.SrvBindlessIndex,
            TemporalShSrvBindlessIndex,
            HistoryIrradiance.UavBindlessIndex,
            HistorySH.UavBindlessIndex,
            HistoryCountB.UavBindlessIndex,
            PrevLinearDepth.UavBindlessIndex,
            PrevNormal.UavBindlessIndex,
            UINT32_MAX
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(FinalBlurPipeline.Get());
        static_assert(sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t) <= kRestirGIDenoiserConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        static_assert(_countof(Bindless) <= kRestirGIDenoiserBindlessDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}


void FRestirGIDenoiser::AddPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle& PreBlurSHHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle PreBlurSHHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser PreBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, &PreBlurSHHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.RestirGI->IsEnabled();
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
            Owner.GBufferBindlessIndices[0],
            PreBlurShUavBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        struct FRestirGiDenoiserConstants
        {
            uint32_t Width = 0;
            uint32_t Height = 0;
            uint32_t HistoryValid = 0;
            uint32_t PassIndex = 0;
            float DepthThresholdScale = 1.03f;
            float NormalThreshold = 0.9f;
            float BlendStrength = 1.0f;
            uint32_t MipLevel = 0;
            float Padding1 = 0.0f;
            float Padding2 = 0.0f;
        };

        FRestirGiDenoiserConstants Constants = {};
        Constants.Width = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.Height = static_cast<uint32_t>(Owner.Viewport.Height);
        Constants.PassIndex = 0u;
        Constants.MipLevel = 0u;

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());

        const uint32_t PreBlurBindless[16] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepth.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormal.SrvBindlessIndex,
            HistorySH.SrvBindlessIndex,
            HistoryCountA.SrvBindlessIndex,
            UINT32_MAX,
            HistoryIrradiance.UavBindlessIndex,
            HistorySH.UavBindlessIndex,
            HistoryCountB.UavBindlessIndex,
            PrevLinearDepth.UavBindlessIndex,
            PrevNormal.UavBindlessIndex,
            PreBlurShUavBindlessIndex
        };

        LocalCommandList->SetPipelineState(PreBlurPipeline.Get());
        static_assert(sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t) <= kRestirGIDenoiserConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        static_assert(_countof(PreBlurBindless) <= kRestirGIDenoiserBindlessDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(PreBlurBindless), PreBlurBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FRestirGIDenoiser::AddTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle& TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const
{
    struct FPassData
    {
        bool bEnabled = false;
        FRGResourceHandle TemporalSHHandle{};
    };
    Graph.AddPass<FPassData>("Denoiser TemporalAccum", [this, &Owner, GBufferHandles, DepthHandle, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle, HistorySHHandle, HistoryCountAHandle, &TemporalSHHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.RestirGI->IsEnabled();
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
        Builder.ReadTexture(HistorySHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevNormalHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Data.TemporalSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevNormalHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &Graph, InputSHHandle, VarianceHandle, PreBlurSHHandle](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty()
            ? UINT32_MAX
            : Owner.DepthBindlessIndices[Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size())];
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t PreBlurShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(PreBlurSHHandle);
        const uint32_t TemporalShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Data.TemporalSHHandle);

        const bool bInputsValid = AreAllBindlessIndicesValid(
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            PreBlurShSrvBindlessIndex,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            TemporalShUavBindlessIndex);
        if (!bInputsValid)
        {
            return;
        }

        struct FRestirGiDenoiserConstants
        {
            uint32_t Width = 0;
            uint32_t Height = 0;
            uint32_t HistoryValid = 0;
            uint32_t PassIndex = 0;
            float DepthThresholdScale = 1.03f;
            float NormalThreshold = 0.9f;
            float BlendStrength = 1.0f;
            uint32_t MipLevel = 0;
            float Padding1 = 0.0f;
            float Padding2 = 0.0f;
        };

        FRestirGiDenoiserConstants Constants = {};
        Constants.Width = static_cast<uint32_t>(Owner.Viewport.Width);
        Constants.Height = static_cast<uint32_t>(Owner.Viewport.Height);
        const bool bResetHistoryThisFrame = ShouldResetHistoryForFreeze(Owner);
        Constants.HistoryValid = (bHistoryValid && !bResetHistoryThisFrame) ? 1u : 0u;
        Constants.PassIndex = 1u;
        Constants.MipLevel = 0u;

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());

        const uint32_t TemporalBindless[16] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.LinearDepthTexture.SrvBindlessIndex,
            PrevLinearDepth.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormal.SrvBindlessIndex,
            HistorySH.SrvBindlessIndex,
            HistoryCountA.SrvBindlessIndex,
            TemporalShUavBindlessIndex,
            DepthBindlessIndex,
            TemporalShUavBindlessIndex,
            HistoryCountB.UavBindlessIndex,
            PrevLinearDepth.UavBindlessIndex,
            PrevNormal.UavBindlessIndex,
            PreBlurShSrvBindlessIndex
        };

        LocalCommandList->SetPipelineState(TemporalAccumulationPipeline.Get());
        static_assert(sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t) <= kRestirGIDenoiserConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        static_assert(_countof(TemporalBindless) <= kRestirGIDenoiserBindlessDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(TemporalBindless), TemporalBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FRestirGIDenoiser::ImportPersistentResources(FDeferredPassContext& Context)
{
    FRenderGraph& Graph = Context.Graph;
    FRestirGIDenoiserFrameResources& OutResources = Context.Resources.RestirGIDenoiser;

    OutResources.HistoryIrradianceHandle = ImportBindlessTexture(Graph, "ReSTIR GI Denoised Irradiance", HistoryIrradiance);
    OutResources.HistorySHHandle = ImportBindlessTexture(Graph, "ReSTIR GI History SH", HistorySH);
    OutResources.HistoryCountAHandle = ImportBindlessTexture(Graph, "ReSTIR GI History Count A", HistoryCountA);
    OutResources.HistoryCountBHandle = ImportBindlessTexture(Graph, "ReSTIR GI History Count B", HistoryCountB);
    OutResources.PrevLinearDepthHandle = ImportBindlessTexture(Graph, "ReSTIR GI Prev LinearDepth", PrevLinearDepth);
    OutResources.PrevNormalHandle = ImportBindlessTexture(Graph, "ReSTIR GI Prev Normal", PrevNormal);
}

bool FRestirGIDenoiser::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    (void)Device;
    RefreshPersistentInputValidation();
    return true;
}

void FRestirGIDenoiser::RefreshPersistentInputValidation()
{
    bPersistentInputsValid =
        RootSignature &&
        PreBlurPipeline &&
        TemporalAccumulationPipeline &&
        GenerateShMipsPipeline &&
        GenerateLinearDepthMipsPipeline &&
        HistoryReconstructionPipeline &&
        FinalBlurPipeline &&
        HistoryIrradiance.IsFullyBound() &&
        HistorySH.IsFullyBound() &&
        HistoryCountA.IsFullyBound() &&
        HistoryCountB.IsFullyBound() &&
        PrevLinearDepth.IsFullyBound() &&
        PrevNormal.IsFullyBound();
}

bool FRestirGIDenoiser::IsReady() const
{
    return IsEnabled() && bPersistentInputsValid;
}

void FRestirGIDenoiser::FinalizeFrame(FDeferredRenderer& Owner)
{
    const bool bReady = IsReady();

    if (HistoryCountA && HistoryCountB)
    {
        std::swap(HistoryCountA, HistoryCountB);
    }

    bHistoryValid = Owner.RestirGI->IsEnabled() && bReady;
}

void FRestirGIDenoiser::InvalidateHistory()
{
    bHistoryValid = false;

    if (HistoryCountA.HasSrv() && HistoryCountB.HasSrv())
    {
        if (HistoryCountA.SrvBindlessIndex > HistoryCountB.SrvBindlessIndex)
        {
            std::swap(HistoryCountA, HistoryCountB);
        }
    }
}
