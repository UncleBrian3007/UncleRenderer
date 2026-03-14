#include "DeferredRayTracingPasses.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include "../ShaderCompiler.h"
#include <d3dx12.h>
#define A_CPU
#include "../../../Shaders/ffx_a.h"
#include "../../../Shaders/ffx_spd.h"
#undef A_CPU

namespace
{
    bool ShouldResetDenoiserHistoryForFreeze(const FDeferredRenderer& Owner)
    {
        if (!Owner.IsRestirGIEnabled() || !Owner.IsRestirGIDenoiserEnabled() || !Owner.IsRestirGIFreezeFrame())
        {
            return false;
        }

        const uint32_t ResetPeriod = Owner.GetRestirGIFreezeDenoiserHistoryResetPeriod();
        if (ResetPeriod == 0u)
        {
            return false;
        }

        const uint64_t FreezeFrameIndex = Owner.GetFrameNumber() - Owner.GetRestirGIFreezeStartFrameNumber();
        return (FreezeFrameIndex % ResetPeriod) == 0u;
    }
}

bool FDeferredRenderer::CreateRestirGiDenoiserResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const DXGI_FORMAT RestirGiRadianceFormat = ResolveRestirGiRadianceFormat(Device);
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

    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, 1, L"ReSTIR_GI_InputSH", RestirGiInputSHTexture);
    CreateTexture(DXGI_FORMAT_R8_UNORM, Width, Height, 1, L"ReSTIR_GI_Variance", RestirGiVarianceTexture);
    CreateTexture(RestirGiRadianceFormat, Width, Height, 1, L"ReSTIR_GI_HistoryIrradiance", RestirGiHistoryIrradianceTexture);
    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, 1, L"ReSTIR_GI_PreBlurSH", RestirGiPreBlurSHTexture);
    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, 1, L"ReSTIR_GI_TemporalSH", RestirGiTemporalSHTexture);
    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, Width, Height, 1, L"ReSTIR_GI_HistorySH", RestirGiHistorySHTexture);
    CreateTexture(DXGI_FORMAT_R8_UINT, Width, Height, 1, L"ReSTIR_GI_HistoryCountA", RestirGiHistoryCountATexture);
    CreateTexture(DXGI_FORMAT_R8_UINT, Width, Height, 1, L"ReSTIR_GI_HistoryCountB", RestirGiHistoryCountBTexture);
    CreateTexture(DXGI_FORMAT_R16_FLOAT, Width, Height, 1, L"ReSTIR_GI_PrevLinearDepth", RestirGiPrevLinearDepthTexture);
    CreateTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, Width, Height, 1, L"ReSTIR_GI_PrevNormal", RestirGiPrevNormalTexture);
    CreateTexture(DXGI_FORMAT_R32G32B32A32_UINT, (Width + 1u) / 2u, (Height + 1u) / 2u, 4, L"ReSTIR_GI_SH_Mips", RestirGiShMipTexture);
    CreateTexture(DXGI_FORMAT_R16_FLOAT, (Width + 1u) / 2u, (Height + 1u) / 2u, 4, L"ReSTIR_GI_LinearDepth_Mips", RestirGiLinearDepthMipTexture);

    const CD3DX12_RESOURCE_DESC SpdCounterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t) * 4u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &SpdCounterDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGiSpdAtomicCounterBuffer.ReleaseAndGetAddressOf())));
    if (RestirGiSpdAtomicCounterBuffer)
    {
        RestirGiSpdAtomicCounterBuffer->SetName(L"ReSTIR_GI_SpdAtomicCounter");
    }

    RestirGiInputSHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiVarianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistoryIrradianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiPreBlurSHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiTemporalSHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistorySHState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistoryCountAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiHistoryCountBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiPrevLinearDepthState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiPrevNormalState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiShMipState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiLinearDepthMipState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGiSpdAtomicCounterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

bool FDeferredRenderer::CreateRestirGiDenoiserPipelines(FDX12Device* Device)
{
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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RestirGiDenoiserRootSignature.ReleaseAndGetAddressOf())));

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
        PsoDesc.pRootSignature = RestirGiDenoiserRootSignature.Get();
        PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(OutPipeline.ReleaseAndGetAddressOf())));
        return true;
    };

    if (!CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSPreBlur", RestirGiPreBlurPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSTemporalAccumulation", RestirGiTemporalAccumulationPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGiMipGenSpd.hlsl", L"CSGenerateShMipsSpd", RestirGiGenerateShMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGiLinearDepthMipGenSpd.hlsl", L"CSGenerateLinearDepthMipsSpd", RestirGiGenerateLinearDepthMipsPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSHistoryReconstruction", RestirGiHistoryReconstructionPipeline)
        || !CreateDenoiserPso(L"Shaders/RestirGIDenoiser.hlsl", L"CSFinalBlur", RestirGiFinalBlurPipeline))
    {
        return false;
    }
    return true;
}

void FDeferredRayTracingPasses::AddRestirGiDenoiserPasses(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle InputSHHandle = Context.Resources.RestirGiInputSHHandle;
    const FRGResourceHandle VarianceHandle = Context.Resources.RestirGiVarianceHandle;
    const FRGResourceHandle PreBlurSHHandle = Context.Resources.RestirGiPreBlurSHHandle;
    const FRGResourceHandle TemporalSHHandle = Context.Resources.RestirGiTemporalSHHandle;
    const FRGResourceHandle HistorySHHandle = Context.Resources.RestirGiHistorySHHandle;
    const FRGResourceHandle HistoryIrradianceHandle = Context.Resources.RestirGiHistoryIrradianceHandle;
    const FRGResourceHandle HistoryCountAHandle = Context.Resources.RestirGiHistoryCountAHandle;
    const FRGResourceHandle HistoryCountBHandle = Context.Resources.RestirGiHistoryCountBHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGiPrevLinearDepthHandle;
    const FRGResourceHandle PrevNormalHandle = Context.Resources.RestirGiPrevNormalHandle;
    const FRGResourceHandle ShMipHandle = Context.Resources.RestirGiShMipHandle;
    const FRGResourceHandle LinearDepthMipHandle = Context.Resources.RestirGiLinearDepthMipHandle;

    if (!Owner.bRestirGIDenoiserEnabled)
    {
        return;
    }

    AddRestirGiDenoiserFreezeResetPass(Owner, Graph, HistorySHHandle, HistoryCountAHandle, HistoryCountBHandle);
    AddRestirGiDenoiserPreBlurPass(Owner, Graph, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle);
    AddRestirGiDenoiserTemporalAccumulationPass(Owner, Graph, FrameState, GBufferHandles, DepthHandle, VelocityHandle, LinearDepthHandle, PreBlurSHHandle, TemporalSHHandle, HistorySHHandle, HistoryCountAHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle);

    AddRestirGiShMipGenPass(Owner, Graph, TemporalSHHandle, ShMipHandle, Context.Resources.RestirGiSpdAtomicCounterHandle);
    AddRestirGiLinearDepthMipGenPass(Owner, Graph, LinearDepthHandle, LinearDepthMipHandle, Context.Resources.RestirGiSpdAtomicCounterHandle);

    AddRestirGiHistoryReconstructionPass(Owner, Graph, GBufferHandles, LinearDepthHandle, HistorySHHandle, HistoryCountBHandle, TemporalSHHandle, ShMipHandle, LinearDepthMipHandle);

    AddRestirGiFinalBlurPass(Owner, Graph, GBufferHandles, LinearDepthHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountBHandle);
}

void FDeferredRayTracingPasses::AddRestirGiDenoiserFreezeResetPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser Freeze Reset", [&Owner, HistorySHHandle, HistoryCountAHandle, HistoryCountBHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = ShouldResetDenoiserHistoryForFreeze(Owner);
        if (!Data.bEnabled) { return; }
        Builder.WriteTexture(HistorySHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        const bool bInputsValid = (Owner.RestirGiHistorySHUavBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiHistoryCountAUavBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiHistoryCountBUavBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiHistorySHTexture != nullptr)
            && (Owner.RestirGiHistoryCountATexture != nullptr)
            && (Owner.RestirGiHistoryCountBTexture != nullptr);
        if (!bInputsValid) { return; }

        FScopedPixEvent Event(LocalCommandList, L"Denoiser Freeze Reset");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t ClearUint4[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryShGpuHandle = Owner.GetBindlessGpuHandle(Owner.RestirGiHistorySHUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryShCpuHandle = Owner.GetBindlessCpuClearHandle(Owner.RestirGiHistorySHUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryShGpuHandle, HistoryShCpuHandle, Owner.RestirGiHistorySHTexture.Get(), ClearUint4, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryCountAGpuHandle = Owner.GetBindlessGpuHandle(Owner.RestirGiHistoryCountAUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryCountACpuHandle = Owner.GetBindlessCpuClearHandle(Owner.RestirGiHistoryCountAUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryCountAGpuHandle, HistoryCountACpuHandle, Owner.RestirGiHistoryCountATexture.Get(), ClearUint4, 0, nullptr);

        const D3D12_GPU_DESCRIPTOR_HANDLE HistoryCountBGpuHandle = Owner.GetBindlessGpuHandle(Owner.RestirGiHistoryCountBUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE HistoryCountBCpuHandle = Owner.GetBindlessCpuClearHandle(Owner.RestirGiHistoryCountBUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(HistoryCountBGpuHandle, HistoryCountBCpuHandle, Owner.RestirGiHistoryCountBTexture.Get(), ClearUint4, 0, nullptr);
    });
}

void FDeferredRayTracingPasses::AddRestirGiShMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle DestinationHandle, FRGBufferHandle AtomicCounterHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser SH Mip SPD", [&Owner, SourceHandle, DestinationHandle, AtomicCounterHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGiDenoiserRootSignature && Owner.RestirGiGenerateShMipsPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(SourceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(DestinationHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(AtomicCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        if (!Owner.RestirGiSpdAtomicCounterBuffer || Owner.RestirGiSpdAtomicCounterUavBindlessIndex == UINT32_MAX) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList || Owner.RestirGiShMipUavBindlessIndices[0] == UINT32_MAX) { return; }
        AU1 DispatchThreadGroupCountXY[2] = { 0u, 0u };
        AU1 WorkGroupOffset[2] = { 0u, 0u };
        AU1 NumWorkGroupsAndMips[2] = { 0u, 0u };
        AU1 RectInfo[4] = { 0u, 0u, static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Width)), static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Height)) };
        SpdSetup(DispatchThreadGroupCountXY, WorkGroupOffset, NumWorkGroupsAndMips, RectInfo, AU1_(4));
        uint32_t SpdConstants[10] = { Owner.RestirGiTemporalSHSrvBindlessIndex, Owner.RestirGiSpdAtomicCounterUavBindlessIndex, Owner.RestirGiShMipUavBindlessIndices[0], Owner.RestirGiShMipUavBindlessIndices[1], Owner.RestirGiShMipUavBindlessIndices[2], Owner.RestirGiShMipUavBindlessIndices[3], NumWorkGroupsAndMips[1], NumWorkGroupsAndMips[0], WorkGroupOffset[0], WorkGroupOffset[1] };
        if (SpdConstants[0] == UINT32_MAX || SpdConstants[2] == UINT32_MAX || SpdConstants[3] == UINT32_MAX || SpdConstants[4] == UINT32_MAX || SpdConstants[5] == UINT32_MAX) { return; }
        FScopedPixEvent Event(LocalCommandList, L"Denoiser SH Mip SPD");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(Owner.RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(Owner.RestirGiGenerateShMipsPipeline.Get());
        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = Owner.GetBindlessGpuHandle(Owner.RestirGiSpdAtomicCounterUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = Owner.GetBindlessCpuClearHandle(Owner.RestirGiSpdAtomicCounterUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, Owner.RestirGiSpdAtomicCounterBuffer.Get(), ClearValues, 0, nullptr);
        D3D12_RESOURCE_BARRIER CounterBarrier = {}; CounterBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; CounterBarrier.UAV.pResource = Owner.RestirGiSpdAtomicCounterBuffer.Get(); LocalCommandList->ResourceBarrier(1, &CounterBarrier);
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(SpdConstants), SpdConstants, 0);
        LocalCommandList->Dispatch(DispatchThreadGroupCountXY[0], DispatchThreadGroupCountXY[1], 1);
        D3D12_RESOURCE_BARRIER MipBarrier = {}; MipBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; MipBarrier.UAV.pResource = Owner.RestirGiShMipTexture.Get(); LocalCommandList->ResourceBarrier(1, &MipBarrier);
    });
}

void FDeferredRayTracingPasses::AddRestirGiLinearDepthMipGenPass(FDeferredRenderer& Owner, FRenderGraph& Graph, FRGResourceHandle SourceHandle, FRGResourceHandle DestinationHandle, FRGBufferHandle AtomicCounterHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser Depth Mip SPD", [&Owner, SourceHandle, DestinationHandle, AtomicCounterHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGiDenoiserRootSignature && Owner.RestirGiGenerateLinearDepthMipsPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(SourceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(DestinationHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteBuffer(AtomicCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        if (!Owner.RestirGiSpdAtomicCounterBuffer || Owner.RestirGiSpdAtomicCounterUavBindlessIndex == UINT32_MAX) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList || Owner.RestirGiLinearDepthMipUavBindlessIndices[0] == UINT32_MAX) { return; }
        AU1 DispatchThreadGroupCountXY[2] = { 0u, 0u }; AU1 WorkGroupOffset[2] = { 0u, 0u }; AU1 NumWorkGroupsAndMips[2] = { 0u, 0u };
        AU1 RectInfo[4] = { 0u, 0u, static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Width)), static_cast<AU1>(static_cast<uint32_t>(Owner.Viewport.Height)) };
        SpdSetup(DispatchThreadGroupCountXY, WorkGroupOffset, NumWorkGroupsAndMips, RectInfo, AU1_(4));
        uint32_t SpdConstants[10] = { Owner.LinearDepthBindlessIndex, Owner.RestirGiSpdAtomicCounterUavBindlessIndex, Owner.RestirGiLinearDepthMipUavBindlessIndices[0], Owner.RestirGiLinearDepthMipUavBindlessIndices[1], Owner.RestirGiLinearDepthMipUavBindlessIndices[2], Owner.RestirGiLinearDepthMipUavBindlessIndices[3], NumWorkGroupsAndMips[1], NumWorkGroupsAndMips[0], WorkGroupOffset[0], WorkGroupOffset[1] };
        if (SpdConstants[0] == UINT32_MAX || SpdConstants[2] == UINT32_MAX || SpdConstants[3] == UINT32_MAX || SpdConstants[4] == UINT32_MAX || SpdConstants[5] == UINT32_MAX) { return; }
        FScopedPixEvent Event(LocalCommandList, L"Denoiser Depth Mip SPD");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(Owner.RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(Owner.RestirGiGenerateLinearDepthMipsPipeline.Get());
        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = Owner.GetBindlessGpuHandle(Owner.RestirGiSpdAtomicCounterUavBindlessIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = Owner.GetBindlessCpuClearHandle(Owner.RestirGiSpdAtomicCounterUavBindlessIndex);
        LocalCommandList->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, Owner.RestirGiSpdAtomicCounterBuffer.Get(), ClearValues, 0, nullptr);
        D3D12_RESOURCE_BARRIER CounterBarrier = {}; CounterBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; CounterBarrier.UAV.pResource = Owner.RestirGiSpdAtomicCounterBuffer.Get(); LocalCommandList->ResourceBarrier(1, &CounterBarrier);
        LocalCommandList->SetComputeRoot32BitConstants(0, _countof(SpdConstants), SpdConstants, 0);
        LocalCommandList->Dispatch(DispatchThreadGroupCountXY[0], DispatchThreadGroupCountXY[1], 1);
        D3D12_RESOURCE_BARRIER MipBarrier = {}; MipBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; MipBarrier.UAV.pResource = Owner.RestirGiLinearDepthMipTexture.Get(); LocalCommandList->ResourceBarrier(1, &MipBarrier);
    });
}

void FDeferredRayTracingPasses::AddRestirGiHistoryReconstructionPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser HistoryBlur", [&Owner, GBufferHandles, LinearDepthHandle, HistorySHHandle, HistoryCountHandle, TemporalSHHandle, ShMipHandle, DepthMipHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGiDenoiserRootSignature && Owner.RestirGiHistoryReconstructionPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistorySHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(ShMipHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthMipHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Owner.Viewport.Width), static_cast<uint32_t>(Owner.Viewport.Height), 0u, 4u, 1.03f, 0.9f, 1.0f, 0u, 0.0f, 0.0f };
        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        const uint32_t Bindless[16] =
        {
            Owner.RestirGiInputSHSrvBindlessIndex,
            Owner.RestirGiVarianceSrvBindlessIndex,
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            Owner.RestirGiPrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.RestirGiPrevNormalSrvBindlessIndex,
            Owner.RestirGiHistorySHSrvBindlessIndex,
            Owner.RestirGiHistoryCountBSrvBindlessIndex,
            Owner.RestirGiTemporalSHUavBindlessIndex,
            Owner.RestirGiHistoryIrradianceUavBindlessIndex,
            Owner.RestirGiHistorySHUavBindlessIndex,
            Owner.RestirGiHistoryCountBUavBindlessIndex,
            Owner.RestirGiLinearDepthMipSrvBindlessIndex,
            Owner.RestirGiPrevNormalUavBindlessIndex,
            Owner.RestirGiShMipSrvBindlessIndex
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser HistoryBlur");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(Owner.RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(Owner.RestirGiHistoryReconstructionPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FDeferredRayTracingPasses::AddRestirGiFinalBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser FinalBlur", [&Owner, GBufferHandles, LinearDepthHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGiDenoiserRootSignature && Owner.RestirGiFinalBlurPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(HistoryIrradianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistorySHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Owner.Viewport.Width), static_cast<uint32_t>(Owner.Viewport.Height), 0u, 5u, 1.03f, 0.9f, 1.0f, 0u, 0.0f, 0.0f };

        const uint32_t Bindless[16] =
        {
            Owner.RestirGiInputSHSrvBindlessIndex,
            Owner.RestirGiVarianceSrvBindlessIndex,
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            Owner.RestirGiPrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.RestirGiPrevNormalSrvBindlessIndex,
            Owner.RestirGiHistorySHSrvBindlessIndex,
            Owner.RestirGiHistoryCountBSrvBindlessIndex,
            Owner.RestirGiTemporalSHSrvBindlessIndex,
            Owner.RestirGiHistoryIrradianceUavBindlessIndex,
            Owner.RestirGiHistorySHUavBindlessIndex,
            Owner.RestirGiHistoryCountBUavBindlessIndex,
            Owner.RestirGiPrevLinearDepthUavBindlessIndex,
            Owner.RestirGiPrevNormalUavBindlessIndex,
            UINT32_MAX
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser FinalBlur");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(Owner.RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());
        LocalCommandList->SetPipelineState(Owner.RestirGiFinalBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}


void FDeferredRayTracingPasses::AddRestirGiDenoiserPreBlurPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle PreBlurSHHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser PreBlur", [&Owner, GBufferHandles, LinearDepthHandle, InputSHHandle, VarianceHandle, PreBlurSHHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGiDenoiserRootSignature && Owner.RestirGiPreBlurPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(InputSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VarianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(PreBlurSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        const bool bInputsValid = (Owner.RestirGiInputSHSrvBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiVarianceSrvBindlessIndex != UINT32_MAX)
            && (Owner.LinearDepthBindlessIndex != UINT32_MAX)
            && (Owner.GBufferBindlessIndices[0] != UINT32_MAX)
            && (Owner.RestirGiPreBlurSHUavBindlessIndex != UINT32_MAX);
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
        LocalCommandList->SetComputeRootSignature(Owner.RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());

        const uint32_t PreBlurBindless[16] =
        {
            Owner.RestirGiInputSHSrvBindlessIndex,
            Owner.RestirGiVarianceSrvBindlessIndex,
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            Owner.RestirGiPrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.RestirGiPrevNormalSrvBindlessIndex,
            Owner.RestirGiHistorySHSrvBindlessIndex,
            Owner.RestirGiHistoryCountASrvBindlessIndex,
            Owner.RestirGiTemporalSHUavBindlessIndex,
            Owner.RestirGiHistoryIrradianceUavBindlessIndex,
            Owner.RestirGiHistorySHUavBindlessIndex,
            Owner.RestirGiHistoryCountBUavBindlessIndex,
            Owner.RestirGiPrevLinearDepthUavBindlessIndex,
            Owner.RestirGiPrevNormalUavBindlessIndex,
            Owner.RestirGiPreBlurSHUavBindlessIndex
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser PreBlur");
        LocalCommandList->SetPipelineState(Owner.RestirGiPreBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(PreBlurBindless), PreBlurBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FDeferredRayTracingPasses::AddRestirGiDenoiserTemporalAccumulationPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredRenderer::FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle PreBlurSHHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser TemporalAccumulation", [&Owner, GBufferHandles, DepthHandle, VelocityHandle, LinearDepthHandle, PreBlurSHHandle, HistorySHHandle, HistoryCountAHandle, TemporalSHHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGiDenoiserRootSignature && Owner.RestirGiTemporalAccumulationPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PreBlurSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistorySHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevNormalHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevNormalHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, &Owner, &FrameState](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);

        const bool bInputsValid = (Owner.RestirGiPreBlurSHSrvBindlessIndex != UINT32_MAX)
            && (DepthBindlessIndex != UINT32_MAX)
            && (Owner.VelocityBindlessIndex != UINT32_MAX)
            && (Owner.LinearDepthBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX)
            && (Owner.GBufferBindlessIndices[0] != UINT32_MAX)
            && (Owner.RestirGiPrevNormalSrvBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiHistorySHSrvBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiHistoryCountASrvBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiTemporalSHUavBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiHistoryCountBUavBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiPrevLinearDepthUavBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiPrevNormalUavBindlessIndex != UINT32_MAX);
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
        const bool bResetHistoryThisFrame = ShouldResetDenoiserHistoryForFreeze(Owner);
        Constants.HistoryValid = (Owner.bRestirGIDenoiserHistoryValid && !bResetHistoryThisFrame) ? 1u : 0u;
        Constants.PassIndex = 1u;
        Constants.MipLevel = 0u;

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(Owner.RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetComputeRootConstantBufferView(2, Owner.GetSceneConstantBufferAddress());

        const uint32_t TemporalBindless[16] =
        {
            Owner.RestirGiInputSHSrvBindlessIndex,
            Owner.RestirGiVarianceSrvBindlessIndex,
            Owner.VelocityBindlessIndex,
            Owner.LinearDepthBindlessIndex,
            Owner.RestirGiPrevLinearDepthSrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.RestirGiPrevNormalSrvBindlessIndex,
            Owner.RestirGiHistorySHSrvBindlessIndex,
            Owner.RestirGiHistoryCountASrvBindlessIndex,
            Owner.RestirGiTemporalSHUavBindlessIndex,
            DepthBindlessIndex,
            Owner.RestirGiTemporalSHUavBindlessIndex,
            Owner.RestirGiHistoryCountBUavBindlessIndex,
            Owner.RestirGiPrevLinearDepthUavBindlessIndex,
            Owner.RestirGiPrevNormalUavBindlessIndex,
            Owner.RestirGiPreBlurSHSrvBindlessIndex
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser TemporalAccum");
        LocalCommandList->SetPipelineState(Owner.RestirGiTemporalAccumulationPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(TemporalBindless), TemporalBindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}
