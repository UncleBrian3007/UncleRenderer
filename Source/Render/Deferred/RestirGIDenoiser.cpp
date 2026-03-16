#include "RestirGIDenoiser.h"
#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include "../ShaderCompiler.h"
#include <d3dx12.h>
#define A_CPU
#include "../../../Shaders/ffx_a.h"
#include "../../../Shaders/ffx_spd.h"
#undef A_CPU

using Microsoft::WRL::ComPtr;

bool FRestirGIDenoiser::ShouldResetHistoryForFreeze(const FDeferredRenderer& Owner) const
{
    if (!Owner.IsRestirGIEnabled() || !IsEnabled() || !Owner.IsRestirGIFreezeFrame())
    {
        return false;
    }

    if (FreezeHistoryResetPeriod == 0u)
    {
        return false;
    }

    const uint64_t FreezeFrameIndex = Owner.GetFrameNumber() - Owner.GetRestirGIFreezeStartFrameNumber();
    return (FreezeFrameIndex % FreezeHistoryResetPeriod) == 0u;
}

bool FRestirGIDenoiser::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const DXGI_FORMAT RestirGiRadianceFormat = Owner.ResolveRestirGiRadianceFormat(Device);
    auto CreateTexture = [&](DXGI_FORMAT Format, uint32_t InWidth, uint32_t InHeight, uint16_t MipLevels, const wchar_t* Name, Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource)
    {
        CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
            Format,
            InWidth,
            InHeight,
            1,
            MipLevels,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(OutResource.ReleaseAndGetAddressOf())));

        if (OutResource)
        {
            OutResource->SetName(Name);
        }
    };

    CreateTexture(RestirGiRadianceFormat, Width, Height, 1, L"ReSTIR_GI_HistoryIrradiance", HistoryIrradianceTexture);
    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, 1, L"ReSTIR_GI_HistorySH", HistorySHTexture);
    CreateTexture(DXGI_FORMAT_R8_UINT, Width, Height, 1, L"ReSTIR_GI_HistoryCountA", HistoryCountATexture);
    CreateTexture(DXGI_FORMAT_R8_UINT, Width, Height, 1, L"ReSTIR_GI_HistoryCountB", HistoryCountBTexture);
    CreateTexture(DXGI_FORMAT_R16_FLOAT, Width, Height, 1, L"ReSTIR_GI_PrevLinearDepth", PrevLinearDepthTexture);
    CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, Width, Height, 1, L"ReSTIR_GI_PrevNormal", PrevNormalTexture);
    HistoryIrradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    HistorySHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    HistoryCountAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    HistoryCountBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PrevLinearDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PrevNormalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

bool FRestirGIDenoiser::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    if (!Device)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER1 RootParams[3] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 10;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 16;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].Descriptor.ShaderRegister = 2;
    RootParams[2].Descriptor.RegisterSpace = 0;
    RootParams[2].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RootSignature.ReleaseAndGetAddressOf())));

    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);
    auto CreateDenoiserPso = [&](const wchar_t* ShaderPath, const wchar_t* EntryPoint, Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipeline) -> bool
    {
        std::vector<uint8_t> CSByteCode;
        if (!Compiler.CompileFromFile(ShaderPath, EntryPoint, CSTarget, CSByteCode))
        {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
        PsoDesc.pRootSignature = RootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(OutPipeline.ReleaseAndGetAddressOf())));
        return true;
    };

    if (!CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSPreBlur", PreBlurPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSTemporalAccumulation", TemporalAccumulationPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGiMipGenSpd.hlsl", L"CSGenerateShMipsSpd", GenerateShMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGiLinearDepthMipGenSpd.hlsl", L"CSGenerateLinearDepthMipsSpd", GenerateLinearDepthMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSHistoryReconstruction", HistoryReconstructionPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSFinalBlur", FinalBlurPipeline))
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
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser Freeze Reset", [this, &Owner, HistorySHHandle, HistoryCountAHandle, HistoryCountBHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = ShouldResetHistoryForFreeze(Owner);
        if (!Data.bEnabled) { return; }
        Builder.WriteTexture(HistorySHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        const bool bInputsValid = (HistorySHUavBindlessIndex != UINT32_MAX)
            && (HistoryCountAUavBindlessIndex != UINT32_MAX)
            && (HistoryCountBUavBindlessIndex != UINT32_MAX)
            && (HistorySHTexture != nullptr)
            && (HistoryCountATexture != nullptr)
            && (HistoryCountBTexture != nullptr);
        if (!bInputsValid) { return; }

        FScopedPixEvent Event(LocalCommandList, L"Denoiser Freeze Reset");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t ClearUint4[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryShGpuHandle = Owner.GetBindlessGpuHandle(HistorySHUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryShCpuHandle = Owner.GetBindlessCpuClearHandle(HistorySHUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryShGpuHandle, HistoryShCpuHandle, HistorySHTexture.Get(), ClearUint4, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryCountAGpuHandle = Owner.GetBindlessGpuHandle(HistoryCountAUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryCountACpuHandle = Owner.GetBindlessCpuClearHandle(HistoryCountAUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryCountAGpuHandle, HistoryCountACpuHandle, HistoryCountATexture.Get(), ClearUint4, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryCountBGpuHandle = Owner.GetBindlessGpuHandle(HistoryCountBUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryCountBCpuHandle = Owner.GetBindlessCpuClearHandle(HistoryCountBUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryCountBGpuHandle, HistoryCountBCpuHandle, HistoryCountBTexture.Get(), ClearUint4, 0, nullptr);
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
        Data.bEnabled = Owner.IsRestirGIEnabled() && RootSignature && GenerateShMipsPipeline;
        if (!Data.bEnabled) { return; }
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
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }
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
        uint32_t SpdConstants[10] = { TemporalShSrvBindlessIndex, AtomicCounterUavBindlessIndex, ShMipUav0, ShMipUav1, ShMipUav2, ShMipUav3, NumWorkGroupsAndMips[1], NumWorkGroupsAndMips[0], WorkGroupOffset[0], WorkGroupOffset[1] };
        if (SpdConstants[0] == UINT32_MAX || SpdConstants[2] == UINT32_MAX || SpdConstants[3] == UINT32_MAX || SpdConstants[4] == UINT32_MAX || SpdConstants[5] == UINT32_MAX) { return; }
        FScopedPixEvent Event(LocalCommandList, L"Denoiser SH Mip SPD");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(GenerateShMipsPipeline.Get());
        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        ID3D12Resource* AtomicCounterResource = Graph.GetBufferResource(Data.AtomicCounterHandle);
        if (!AtomicCounterResource || AtomicCounterUavBindlessIndex == UINT32_MAX) { return; }
        const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = Owner.GetBindlessGpuHandle(AtomicCounterUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = Owner.GetBindlessCpuClearHandle(AtomicCounterUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, AtomicCounterResource, ClearValues, 0, nullptr);
        D3D12_RESOURCE_BARRIER CounterBarrier = {}; CounterBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; CounterBarrier.UAV.pResource = AtomicCounterResource; LocalCommandList->ResourceBarrier(1, &CounterBarrier);
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(SpdConstants), SpdConstants, 0);
        LocalCommandList->Dispatch(DispatchThreadGroupCountXY[0], DispatchThreadGroupCountXY[1], 1);
        ID3D12Resource* DestinationResource = Graph.GetTextureResource(Data.DestinationHandle);
        if (DestinationResource)
        {
            D3D12_RESOURCE_BARRIER MipBarrier = {}; MipBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; MipBarrier.UAV.pResource = DestinationResource; LocalCommandList->ResourceBarrier(1, &MipBarrier);
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
        Data.bEnabled = Owner.IsRestirGIEnabled() && RootSignature && GenerateLinearDepthMipsPipeline;
        if (!Data.bEnabled) { return; }
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
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }
        AU1 DispatchThreadGroupCountXY[2] = { 0u, 0u }; AU1 WorkGroupOffset[2] = { 0u, 0u }; AU1 NumWorkGroupsAndMips[2] = { 0u, 0u };
        AU1 RectInfo[4] = { 0u, 0u, static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Width)), static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Height)) };
        SpdSetup(DispatchThreadGroupCountXY, WorkGroupOffset, NumWorkGroupsAndMips, RectInfo, AU1_(4));
        const uint32_t AtomicCounterUavBindlessIndex = Graph.GetBufferUavBindlessIndex(Data.AtomicCounterHandle);
        const uint32_t DepthMipUav0 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 0u);
        const uint32_t DepthMipUav1 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 1u);
        const uint32_t DepthMipUav2 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 2u);
        const uint32_t DepthMipUav3 = Graph.GetTextureMipUavBindlessIndex(Data.DestinationHandle, 3u);
        uint32_t SpdConstants[10] = { Owner.LinearDepthBindlessIndex, AtomicCounterUavBindlessIndex, DepthMipUav0, DepthMipUav1, DepthMipUav2, DepthMipUav3, NumWorkGroupsAndMips[1], NumWorkGroupsAndMips[0], WorkGroupOffset[0], WorkGroupOffset[1] };
        if (SpdConstants[0] == UINT32_MAX || SpdConstants[2] == UINT32_MAX || SpdConstants[3] == UINT32_MAX || SpdConstants[4] == UINT32_MAX || SpdConstants[5] == UINT32_MAX) { return; }
        FScopedPixEvent Event(LocalCommandList, L"Denoiser Depth Mip SPD");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(GenerateLinearDepthMipsPipeline.Get());
        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        ID3D12Resource* AtomicCounterResource = Graph.GetBufferResource(Data.AtomicCounterHandle);
        if (!AtomicCounterResource || AtomicCounterUavBindlessIndex == UINT32_MAX) { return; }
        const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = Owner.GetBindlessGpuHandle(AtomicCounterUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = Owner.GetBindlessCpuClearHandle(AtomicCounterUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, AtomicCounterResource, ClearValues, 0, nullptr);
        D3D12_RESOURCE_BARRIER CounterBarrier = {}; CounterBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; CounterBarrier.UAV.pResource = AtomicCounterResource; LocalCommandList->ResourceBarrier(1, &CounterBarrier);
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(SpdConstants), SpdConstants, 0);
        LocalCommandList->Dispatch(DispatchThreadGroupCountXY[0], DispatchThreadGroupCountXY[1], 1);
        ID3D12Resource* DestinationResource = Graph.GetTextureResource(Data.DestinationHandle);
        if (DestinationResource)
        {
            D3D12_RESOURCE_BARRIER MipBarrier = {}; MipBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; MipBarrier.UAV.pResource = DestinationResource; LocalCommandList->ResourceBarrier(1, &MipBarrier);
        }
    });
}

void FRestirGIDenoiser::AddHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser HistoryBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, HistorySHHandle, HistoryCountHandle, TemporalSHHandle, ShMipHandle, DepthMipHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.IsRestirGIEnabled() && RootSignature && HistoryReconstructionPipeline;
        if (!Data.bEnabled) { return; }
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
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t TemporalShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(TemporalSHHandle);
        const uint32_t LinearDepthMipSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(DepthMipHandle);
        const uint32_t ShMipSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(ShMipHandle);

        const bool bInputsValid = (InputSHSrvBindlessIndex != UINT32_MAX)
            && (VarianceSrvBindlessIndex != UINT32_MAX)
            && (Owner.VelocityBindlessIndex != UINT32_MAX)
            && (Owner.LinearDepthBindlessIndex != UINT32_MAX)
            && (PrevLinearDepthSrvBindlessIndex != UINT32_MAX)
            && (Owner.GBufferBindlessIndices[0] != UINT32_MAX)
            && (PrevNormalSrvBindlessIndex != UINT32_MAX)
            && (HistorySHSrvBindlessIndex != UINT32_MAX)
            && (HistoryCountBSrvBindlessIndex != UINT32_MAX)
            && (TemporalShUavBindlessIndex != UINT32_MAX)
            && (HistoryIrradianceUavBindlessIndex != UINT32_MAX)
            && (HistorySHUavBindlessIndex != UINT32_MAX)
            && (HistoryCountBUavBindlessIndex != UINT32_MAX)
            && (LinearDepthMipSrvBindlessIndex != UINT32_MAX)
            && (PrevNormalUavBindlessIndex != UINT32_MAX)
            && (ShMipSrvBindlessIndex != UINT32_MAX);
        if (!bInputsValid) { return; }

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Owner.Viewport.Width), static_cast<uint32_t>(Owner.Viewport.Height), 0u, 4u, 1.03f, 0.9f, 1.0f, 0u, 0.0f, 0.0f };
        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        const uint32_t Bindless[16] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormalSrvBindlessIndex,
            HistorySHSrvBindlessIndex,
            HistoryCountBSrvBindlessIndex,
            TemporalShUavBindlessIndex,
            HistoryIrradianceUavBindlessIndex,
            HistorySHUavBindlessIndex,
            HistoryCountBUavBindlessIndex,
            LinearDepthMipSrvBindlessIndex,
            PrevNormalUavBindlessIndex,
            ShMipSrvBindlessIndex
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser HistoryBlur");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(HistoryReconstructionPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FRestirGIDenoiser::AddFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser FinalBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.IsRestirGIEnabled() && RootSignature && FinalBlurPipeline;
        if (!Data.bEnabled) { return; }
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
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t TemporalShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(TemporalSHHandle);

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Owner.Viewport.Width), static_cast<uint32_t>(Owner.Viewport.Height), 0u, 5u, 1.03f, 0.9f, 1.0f, 0u, 0.0f, 0.0f };

        const uint32_t Bindless[16] =
        {
            InputSHSrvBindlessIndex,
            VarianceSrvBindlessIndex,
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormalSrvBindlessIndex,
            HistorySHSrvBindlessIndex,
            HistoryCountBSrvBindlessIndex,
            TemporalShSrvBindlessIndex,
            HistoryIrradianceUavBindlessIndex,
            HistorySHUavBindlessIndex,
            HistoryCountBUavBindlessIndex,
            PrevLinearDepthUavBindlessIndex,
            PrevNormalUavBindlessIndex,
            UINT32_MAX
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser FinalBlur");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(FinalBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}


void FRestirGIDenoiser::AddPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle& PreBlurSHHandle) const
{
    struct FPassData { bool bEnabled = false; FRGResourceHandle PreBlurSHHandle{}; };
    Graph.AddPass<FPassData>("Denoiser PreBlur", [this, &Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, &PreBlurSHHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.IsRestirGIEnabled() && RootSignature && PreBlurPipeline;
        if (!Data.bEnabled) { return; }
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
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t PreBlurShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Data.PreBlurSHHandle);

        const bool bInputsValid = (InputSHSrvBindlessIndex != UINT32_MAX)
            && (VarianceSrvBindlessIndex != UINT32_MAX)
            && (Owner.LinearDepthBindlessIndex != UINT32_MAX)
            && (Owner.GBufferBindlessIndices[0] != UINT32_MAX)
            && (PreBlurShUavBindlessIndex != UINT32_MAX);
        if (!bInputsValid) { return; }

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
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormalSrvBindlessIndex,
            HistorySHSrvBindlessIndex,
            HistoryCountASrvBindlessIndex,
            UINT32_MAX,
            HistoryIrradianceUavBindlessIndex,
            HistorySHUavBindlessIndex,
            HistoryCountBUavBindlessIndex,
            PrevLinearDepthUavBindlessIndex,
            PrevNormalUavBindlessIndex,
            PreBlurShUavBindlessIndex
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser PreBlur");
        LocalCommandList->SetPipelineState(PreBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(PreBlurBindless), PreBlurBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FRestirGIDenoiser::AddTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle& TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const
{
    struct FPassData { bool bEnabled = false; FRGResourceHandle TemporalSHHandle{}; };
    Graph.AddPass<FPassData>("Denoiser TemporalAccum", [this, &Owner, GBufferHandles, DepthHandle, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle, HistorySHHandle, HistoryCountAHandle, &TemporalSHHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.IsRestirGIEnabled() && RootSignature && TemporalAccumulationPipeline;
        if (!Data.bEnabled) { return; }
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
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        const uint32_t DepthBindlessIndex = Owner.DepthBindlessIndices.empty()
            ? UINT32_MAX
            : Owner.DepthBindlessIndices[Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size())];
        const uint32_t InputSHSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(InputSHHandle);
        const uint32_t VarianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(VarianceHandle);
        const uint32_t PreBlurShSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(PreBlurSHHandle);
        const uint32_t TemporalShUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Data.TemporalSHHandle);

        const bool bInputsValid = (PreBlurShSrvBindlessIndex != UINT32_MAX)
            && (DepthBindlessIndex != UINT32_MAX)
            && (Owner.VelocityBindlessIndex != UINT32_MAX)
            && (Owner.LinearDepthBindlessIndex != UINT32_MAX)
            && (PrevLinearDepthSrvBindlessIndex != UINT32_MAX)
            && (Owner.GBufferBindlessIndices[0] != UINT32_MAX)
            && (PrevNormalSrvBindlessIndex != UINT32_MAX)
            && (HistorySHSrvBindlessIndex != UINT32_MAX)
            && (HistoryCountASrvBindlessIndex != UINT32_MAX)
            && (InputSHSrvBindlessIndex != UINT32_MAX)
            && (VarianceSrvBindlessIndex != UINT32_MAX)
            && (TemporalShUavBindlessIndex != UINT32_MAX)
            && (HistoryCountBUavBindlessIndex != UINT32_MAX)
            && (PrevLinearDepthUavBindlessIndex != UINT32_MAX)
            && (PrevNormalUavBindlessIndex != UINT32_MAX);
        if (!bInputsValid) { return; }

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
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            PrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            PrevNormalSrvBindlessIndex,
            HistorySHSrvBindlessIndex,
            HistoryCountASrvBindlessIndex,
            TemporalShUavBindlessIndex,
            DepthBindlessIndex,
            TemporalShUavBindlessIndex,
            HistoryCountBUavBindlessIndex,
            PrevLinearDepthUavBindlessIndex,
            PrevNormalUavBindlessIndex,
            PreBlurShSrvBindlessIndex
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser TemporalAccum");
        LocalCommandList->SetPipelineState(TemporalAccumulationPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(TemporalBindless), TemporalBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FRestirGIDenoiser::ImportPersistentResources(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FRestirGIDenoiserFrameResources& OutResources = Context.Resources.RestirGIDenoiser;

    OutResources.HistoryIrradianceHandle = Graph.ImportTexture(
        "ReSTIR GI Denoised Irradiance",
        HistoryIrradianceTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&HistoryIrradianceState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), HistoryIrradianceTexture->GetDesc().Format });

    OutResources.HistorySHHandle = Graph.ImportTexture(
        "ReSTIR GI History SH",
        HistorySHTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&HistorySHState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R32G32B32A32_UINT });

    OutResources.HistoryCountAHandle = Graph.ImportTexture(
        "ReSTIR GI History Count A",
        HistoryCountATexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&HistoryCountAState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R8_UINT });

    OutResources.HistoryCountBHandle = Graph.ImportTexture(
        "ReSTIR GI History Count B",
        HistoryCountBTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&HistoryCountBState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R8_UINT });

    OutResources.PrevLinearDepthHandle = Graph.ImportTexture(
        "ReSTIR GI Prev LinearDepth",
        PrevLinearDepthTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&PrevLinearDepthState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16_FLOAT });

    OutResources.PrevNormalHandle = Graph.ImportTexture(
        "ReSTIR GI Prev Normal",
        PrevNormalTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&PrevNormalState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), DXGI_FORMAT_R16G16B16A16_FLOAT });
}

bool FRestirGIDenoiser::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    if (!Device)
    {
        return false;
    }

    const DXGI_FORMAT IrradianceFormat = HistoryIrradianceTexture ? HistoryIrradianceTexture->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
    SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SrvDesc.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
    UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    UavDesc.Texture2D.MipSlice = 0;

    SrvDesc.Format = IrradianceFormat;
    UavDesc.Format = IrradianceFormat;
    HistoryIrradianceSrvBindlessIndex = Device->CreateBindlessSrv(HistoryIrradianceTexture.Get(), SrvDesc);
    HistoryIrradianceUavBindlessIndex = Device->CreateBindlessUav(HistoryIrradianceTexture.Get(), nullptr, UavDesc);

    SrvDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    UavDesc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    HistorySHSrvBindlessIndex = Device->CreateBindlessSrv(HistorySHTexture.Get(), SrvDesc);
    HistorySHUavBindlessIndex = Device->CreateBindlessUav(HistorySHTexture.Get(), nullptr, UavDesc);

    SrvDesc.Format = DXGI_FORMAT_R8_UINT;
    UavDesc.Format = DXGI_FORMAT_R8_UINT;
    HistoryCountASrvBindlessIndex = Device->CreateBindlessSrv(HistoryCountATexture.Get(), SrvDesc);
    HistoryCountAUavBindlessIndex = Device->CreateBindlessUav(HistoryCountATexture.Get(), nullptr, UavDesc);
    HistoryCountBSrvBindlessIndex = Device->CreateBindlessSrv(HistoryCountBTexture.Get(), SrvDesc);
    HistoryCountBUavBindlessIndex = Device->CreateBindlessUav(HistoryCountBTexture.Get(), nullptr, UavDesc);

    SrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    UavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    PrevLinearDepthSrvBindlessIndex = Device->CreateBindlessSrv(PrevLinearDepthTexture.Get(), SrvDesc);
    PrevLinearDepthUavBindlessIndex = Device->CreateBindlessUav(PrevLinearDepthTexture.Get(), nullptr, UavDesc);

    SrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    UavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    PrevNormalSrvBindlessIndex = Device->CreateBindlessSrv(PrevNormalTexture.Get(), SrvDesc);
    PrevNormalUavBindlessIndex = Device->CreateBindlessUav(PrevNormalTexture.Get(), nullptr, UavDesc);

    return true;
}

void FRestirGIDenoiser::FinalizeFrame(FDeferredRenderer& Owner)
{
    if (HistoryCountATexture && HistoryCountBTexture)
    {
        std::swap(HistoryCountATexture, HistoryCountBTexture);
        std::swap(HistoryCountAState, HistoryCountBState);
        std::swap(HistoryCountASrvBindlessIndex, HistoryCountBSrvBindlessIndex);
        std::swap(HistoryCountAUavBindlessIndex, HistoryCountBUavBindlessIndex);
    }

    bHistoryValid = Owner.IsRestirGIEnabled() && IsEnabled() && HistoryIrradianceTexture != nullptr;
}

void FRestirGIDenoiser::InvalidateHistory()
{
    bHistoryValid = false;

    if (HistoryCountASrvBindlessIndex != UINT32_MAX && HistoryCountBSrvBindlessIndex != UINT32_MAX)
    {
        if (HistoryCountASrvBindlessIndex > HistoryCountBSrvBindlessIndex)
        {
            std::swap(HistoryCountASrvBindlessIndex, HistoryCountBSrvBindlessIndex);
            std::swap(HistoryCountAUavBindlessIndex, HistoryCountBUavBindlessIndex);
            std::swap(HistoryCountATexture, HistoryCountBTexture);
        }
    }
}
