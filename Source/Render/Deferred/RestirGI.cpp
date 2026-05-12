#include "RestirGI.h"
#include "../DeferredRenderer.h"
#include "DeferredPassContext.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include <algorithm>
#define A_CPU
#include "../../../Shaders/ffx_a.h"
#include "../../../Shaders/ffx_spd.h"
#undef A_CPU
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

constexpr uint32_t kRestirGIConstantsDwordCount = 19;
constexpr uint32_t kRestirGIBindlessDwordCount  = 30;

bool FRestirGI::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    if (!Device || !Device->IsRayTracingSupported())
    {
        if (bEnabled)
        {
            LogWarning("Deferred renderer: ReSTIR GI disabled because DXR is not supported on this device.");
        }
        bEnabled = false;
        RestirGIRootSignature.Reset();
        for (ComPtr<ID3D12PipelineState>& Pipeline : RestirGIInitialPipelines)
        {
            Pipeline.Reset();
        }
        RestirGIReservoirBootstrapPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        RestirGISpatialPipeline.Reset();
        RestirGIResolvePipeline.Reset();
        return true;
    }

    if (!CreateRootSignature(Device) || !CreatePipeline(Device))
    {
        LogWarning("Deferred renderer: ReSTIR GI pipeline creation failed.");
        RestirGIRootSignature.Reset();
        for (ComPtr<ID3D12PipelineState>& Pipeline : RestirGIInitialPipelines)
        {
            Pipeline.Reset();
        }
        RestirGIReservoirBootstrapPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        RestirGISpatialPipeline.Reset();
        RestirGIResolvePipeline.Reset();
        bEnabled = false;
    }

    (void)Owner;
    return true;
}

bool FRestirGI::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
{
    (void)Owner;
    (void)FrameCount;
    return CreateResources(Device, Width, Height);
}

void FRestirGI::ImportPersistentResources(FDeferredPassContext& Context)
{
    FRenderGraph& Graph = Context.Graph;
    FRestirGIFrameResources& Resources = Context.Resources.RestirGI;

    Resources.RestirGIHandle = ImportBindlessTexture(Graph, "ReSTIR GI", RestirGI);
    Resources.RestirGIHistoryHandle = ImportBindlessTexture(Graph, "ReSTIR GI History", RestirGIHistory);
    Resources.RestirGIReservoirDepthNormalAHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir DepthNormal A", RestirGIReservoirDepthNormalA);
    Resources.RestirGIReservoirDepthNormalBHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir DepthNormal B", RestirGIReservoirDepthNormalB);
    Resources.RestirGIReservoirSampleRadianceAHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir SampleRadiance A", RestirGIReservoirSampleRadianceA);
    Resources.RestirGIReservoirSampleRadianceBHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir SampleRadiance B", RestirGIReservoirSampleRadianceB);
    Resources.RestirGIReservoirRayDirectionAHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir RayDirection A", RestirGIReservoirRayDirectionA);
    Resources.RestirGIReservoirRayDirectionBHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir RayDirection B", RestirGIReservoirRayDirectionB);
    Resources.RestirGIReservoirMWAHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir MW A", RestirGIReservoirMWA);
    Resources.RestirGIReservoirMWBHandle = ImportBindlessTexture(Graph, "ReSTIR GI Reservoir MW B", RestirGIReservoirMWB);
}

bool FRestirGI::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    RefreshPersistentInputValidation(Owner, Device);
    return bOutputPersistentDescriptorsValid
        && bReservoirAReadDescriptorsValid
        && bReservoirBReadDescriptorsValid
        && bReservoirAWriteDescriptorsValid
        && bReservoirBWriteDescriptorsValid;
}

void FRestirGI::RefreshPersistentInputValidation(const FDeferredRenderer& Owner, FDX12Device* Device)
{
    CachedLinearClampSamplerIndex = Device ? Device->GetLinearClampSamplerIndex() : UINT32_MAX;
    CachedPrevLinearDepthSrvBindlessIndex = Owner.RestirGIDenoiser ? Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex() : UINT32_MAX;

    bOutputPersistentDescriptorsValid =
        RestirGI.HasSrv() &&
        RestirGI.HasUav() &&
        RestirGIHistory.HasSrv() &&
        RestirGIHistory.HasUav();

    bReservoirAReadDescriptorsValid =
        RestirGIReservoirDepthNormalA.HasSrv() &&
        RestirGIReservoirSampleRadianceA.HasSrv() &&
        RestirGIReservoirRayDirectionA.HasSrv() &&
        RestirGIReservoirMWA.HasSrv();

    bReservoirBReadDescriptorsValid =
        RestirGIReservoirDepthNormalB.HasSrv() &&
        RestirGIReservoirSampleRadianceB.HasSrv() &&
        RestirGIReservoirRayDirectionB.HasSrv() &&
        RestirGIReservoirMWB.HasSrv();

    bReservoirAWriteDescriptorsValid =
        RestirGIReservoirDepthNormalA.HasUav() &&
        RestirGIReservoirSampleRadianceA.HasUav() &&
        RestirGIReservoirRayDirectionA.HasUav() &&
        RestirGIReservoirMWA.HasUav();

    bReservoirBWriteDescriptorsValid =
        RestirGIReservoirDepthNormalB.HasUav() &&
        RestirGIReservoirSampleRadianceB.HasUav() &&
        RestirGIReservoirRayDirectionB.HasUav() &&
        RestirGIReservoirMWB.HasUav();

    bCommonPersistentInputsValid =
        AreAllBindlessIndicesValid(
            CachedLinearClampSamplerIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            CachedPrevLinearDepthSrvBindlessIndex) &&
        bOutputPersistentDescriptorsValid;

    bBlueNoisePersistentInputsValid = AreAllBindlessIndicesValid(
        Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
        Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex);
}

void FRestirGI::FinalizeFrame(FDeferredRenderer& Owner)
{
    if (RestirGI && RestirGIHistory)
    {
        std::swap(RestirGI, RestirGIHistory);
    }

    if (RestirGIReservoirDepthNormalA && RestirGIReservoirDepthNormalB)
    {
        std::swap(RestirGIReservoirDepthNormalA, RestirGIReservoirDepthNormalB);
    }

    if (RestirGIReservoirSampleRadianceA && RestirGIReservoirSampleRadianceB)
    {
        std::swap(RestirGIReservoirSampleRadianceA, RestirGIReservoirSampleRadianceB);
    }

    if (RestirGIReservoirRayDirectionA && RestirGIReservoirRayDirectionB)
    {
        std::swap(RestirGIReservoirRayDirectionA, RestirGIReservoirRayDirectionB);
    }

    if (RestirGIReservoirMWA && RestirGIReservoirMWB)
    {
        std::swap(RestirGIReservoirMWA, RestirGIReservoirMWB);
    }

    if (bEnabled && RestirGIHistory.IsValid())
    {
        const uint32_t MaxFrames = (std::max)(1u, MaxHistoryFrames);
        ReservoirHistoryFrameCount = (std::min)(ReservoirHistoryFrameCount + 1u, MaxFrames);
        bReservoirHistoryValid = ReservoirHistoryFrameCount > 0u;
    }
    else
    {
        ReservoirHistoryFrameCount = 0u;
        bReservoirHistoryValid = false;
    }

    (void)Owner;
}

void FRestirGI::InvalidateReservoirHistory()
{
    bReservoirHistoryValid = false;
    ReservoirHistoryFrameCount = 0u;
}

void FRestirGI::SetFreezeFrame(bool bInEnabled, uint64_t FrameNumber)
{
    if (bInEnabled && !bFreezeFrame)
    {
        FrozenSequenceFrame = 0;
        FreezeStartFrameNumber = FrameNumber;
    }

    bFreezeFrame = bInEnabled;
}

bool FRestirGI::CreateRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[4] = {};
    // RootParams[0]: TLAS SRV (t0), used in Shaders/RestirGI.hlsl
    RootParams[0].InitAsShaderResourceView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[1]: Scene constants CBV (b0), used in Shaders/SceneConstants.hlsl
    RootParams[1].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[2]: ReSTIR GI constants (b1)
    RootParams[2].InitAsConstants(kRestirGIConstantsDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);
    // RootParams[3]: ReSTIR GI bindless indices (b2)
    RootParams[3].InitAsConstants(kRestirGIBindlessDwordCount, 2, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RestirGIRootSignature.GetAddressOf())));
    return true;
}

bool FRestirGI::CreatePipeline(FDX12Device* Device)
{
    if (!Device->IsRayTracingSupported())
    {
        LogWarning("Skipping ReSTIR GI pipeline creation: DXR is not supported.");
        return false;
    }

    FShaderCompiler Compiler;

    std::array<std::vector<uint8_t>, 2> InitialByteCodes;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/RestirGI/RestirGI.hlsl", L"CSInitialSampling", InitialByteCodes[0], { L"RESTIR_GI_RANDOM_MODE_HASH=1" }))
    {
        return false;
    }
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/RestirGI/RestirGI.hlsl", L"CSInitialSampling", InitialByteCodes[1], { L"RESTIR_GI_RANDOM_MODE_BLUE_NOISE_SOBOL=1" }))
    {
        return false;
    }

    std::vector<uint8_t> TemporalByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/RestirGI/RestirGI.hlsl", L"CSTemporalResampling", TemporalByteCode))
    {
        return false;
    }

    std::vector<uint8_t> BootstrapByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/RestirGI/RestirGI.hlsl", L"CSReservoirBootstrap", BootstrapByteCode))
    {
        return false;
    }

    std::vector<uint8_t> SpatialByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/RestirGI/RestirGI.hlsl", L"CSSpatialResampling", SpatialByteCode))
    {
        return false;
    }

    std::vector<uint8_t> ResolveByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/RestirGI/RestirGI.hlsl", L"CSResolve", ResolveByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RestirGIRootSignature.Get();

    auto CreateComputePso = [Device](const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipeline, const char* PipelineName)
    {
        const HRESULT Hr = Device->GetDevice()->CreateComputePipelineState(&Desc, IID_PPV_ARGS(OutPipeline.GetAddressOf()));
        if (FAILED(Hr))
        {
            std::ostringstream Oss;
            Oss << "ReSTIR GI pipeline creation failed for " << PipelineName << ", hr=0x" << std::hex << static_cast<uint32_t>(Hr);
            LogWarning(Oss.str());
            return false;
        }
        return true;
    };

    static const char* InitialPipelineNames[2] = { "CSInitialSamplingHash", "CSInitialSamplingBlueNoiseSobol" };
    for (uint32_t Index = 0; Index < static_cast<uint32_t>(InitialByteCodes.size()); ++Index)
    {
        PsoDesc.CS = { InitialByteCodes[Index].data(), InitialByteCodes[Index].size() };
        if (!CreateComputePso(PsoDesc, RestirGIInitialPipelines[Index], InitialPipelineNames[Index]))
        {
            return false;
        }
    }

    PsoDesc.CS = { TemporalByteCode.data(), TemporalByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGITemporalPipeline, "CSTemporalResampling"))
    {
        return false;
    }

    PsoDesc.CS = { BootstrapByteCode.data(), BootstrapByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIReservoirBootstrapPipeline, "CSReservoirBootstrap"))
    {
        return false;
    }

    PsoDesc.CS = { SpatialByteCode.data(), SpatialByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGISpatialPipeline, "CSSpatialResampling"))
    {
        return false;
    }

    PsoDesc.CS = { ResolveByteCode.data(), ResolveByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIResolvePipeline, "CSResolve"))
    {
        return false;
    }

    return true;
}

DXGI_FORMAT FRestirGI::ResolveRadianceFormat(FDX12Device* Device) const
{
    constexpr D3D12_FORMAT_SUPPORT1 RequiredFlags = static_cast<D3D12_FORMAT_SUPPORT1>(
        D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
    if (CheckFormatSupport(Device, DXGI_FORMAT_R11G11B10_FLOAT, RequiredFlags))
    {
        return DXGI_FORMAT_R11G11B10_FLOAT;
    }
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool FRestirGI::CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    const DXGI_FORMAT RestirGiRadianceFormat = ResolveRadianceFormat(Device);
    const uint32_t HalfWidth = (Width + 1u) / 2u;
    const uint32_t HalfHeight = (Height + 1u) / 2u;

    CreateBindlessTexture(Device, L"ReSTIR_GI", { Width, Height, RestirGiRadianceFormat }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGI, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_History", { Width, Height, RestirGiRadianceFormat }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIHistory, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirDepthNormalA", { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirDepthNormalA, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirDepthNormalB", { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirDepthNormalB, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirSampleRadianceA", { HalfWidth, HalfHeight, RestirGiRadianceFormat }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirSampleRadianceA, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirSampleRadianceB", { HalfWidth, HalfHeight, RestirGiRadianceFormat }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirSampleRadianceB, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirRayDirectionA", { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirRayDirectionA, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirRayDirectionB", { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirRayDirectionB, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirMWA", { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16_FLOAT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirMWA, true, true);
    CreateBindlessTexture(Device, L"ReSTIR_GI_ReservoirMWB", { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16_FLOAT }, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, RestirGIReservoirMWB, true, true);

    bReservoirHistoryValid = false;
    return true;
}

void FRestirGI::AddPasses(FDeferredPassContext& Context) const
{
    Context.Resources.RestirGI.RestirGIInitialRadianceHandle = {};
    Context.Resources.RestirGI.RestirGIInitialRayDirectionHandle = {};
    AddInitialSamplingPass(Context);
    if (bTemporalReuse)
    {
        AddTemporalResamplingPass(Context);
    }
    else
    {
        AddReservoirBootstrapPass(Context);
    }
    if (bSpatialReuse)
    {
        AddSpatialResampling0Pass(Context);
        AddSpatialResampling1Pass(Context);
    }
    AddResolvePass(Context);
}

namespace
{
    struct FRestirGiSplitPassData
    {
        bool bEnabled = false;
    };
}

uint32_t FRestirGI::GetDepthBindlessIndexForRestir(FDeferredRenderer& Owner) const
{
    if (Owner.DepthBindlessIndices.empty())
    {
        return UINT32_MAX;
    }
    const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
    return Owner.DepthBindlessIndices[DepthArrayIndex];
}

void FRestirGI::DispatchRestirPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, uint32_t SpatialPassIndex, const uint32_t BindlessIndices[30], uint32_t DispatchWidth, uint32_t DispatchHeight, bool bPassEnabled) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FDX12Device* Device = Owner.Device;
    if (!bPassEnabled)
    {
        return;
    }

    const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
    if (FrameIndex >= Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size())
    {
        return;
    }

    ID3D12Resource* TlasResource = (FrameIndex < Owner.GetRayTracingRuntime().TlasResultBuffers.size()) ? Owner.GetRayTracingRuntime().TlasResultBuffers[FrameIndex].Get() : nullptr;
    if (!TlasResource)
    {
        for (const auto& TlasBuffer : Owner.GetRayTracingRuntime().TlasResultBuffers)
        {
            if (TlasBuffer)
            {
                TlasResource = TlasBuffer.Get();
                break;
            }
        }
    }
    if (!TlasResource)
    {
        return;
    }

    ID3D12GraphicsCommandList4* CommandList4 = Cmd.GetCommandList4();
    if (!CommandList4)
    {
        return;
    }

    struct FRestirGIConstants
    {
        uint32_t FullWidth = 0;
        uint32_t FullHeight = 0;
        uint32_t HalfWidth = 0;
        uint32_t HalfHeight = 0;
        uint32_t FrameIndex = 0;
        uint32_t Enabled = 0;
        uint32_t HistoryValid = 0;
        uint32_t SpatialPassIndex = 0;
        float Intensity = 0.0f;
        float RayLength = 0.0f;
        float ClampThreshold = 0.0f;
        uint32_t UseVisibility = 0;
        uint32_t UseBrdf = 0;
        uint32_t UseHistoryIndirect = 0;
        uint32_t SequenceFrame = 0;
        uint32_t DebugRayEnabled = 0;
        uint32_t DebugPixelX = 0;
        uint32_t DebugPixelY = 0;
    };

    const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
    const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
    const uint32_t HalfWidth = (FullWidth + 1u) / 2u;
    const uint32_t HalfHeight = (FullHeight + 1u) / 2u;
    const uint32_t HistoryFrameLimit = (std::max)(1u, MaxHistoryFrames);
    const uint32_t GlobalFrameNumber = static_cast<uint32_t>(Owner.GetFrameNumber());
    const uint32_t SequenceFrame = bFreezeFrame ? FrozenSequenceFrame : GlobalFrameNumber;

    const FRestirGIConstants Constants =
    {
        FullWidth,
        FullHeight,
        HalfWidth,
        HalfHeight,
        GlobalFrameNumber,
        bEnabled ? 1u : 0u,
        (bReservoirHistoryValid && ReservoirHistoryFrameCount >= HistoryFrameLimit) ? 1u : 0u,
        SpatialPassIndex,
        (std::max)(0.0f, Intensity_),
        RayLength,
        ClampThreshold,
        bUseVisibility ? 1u : 0u,
        bUseBrdf ? 1u : 0u,
        bUseHistoryIndirect ? 1u : 0u,
        SequenceFrame,
        bDebugRayEnabled ? 1u : 0u,
        DebugPixelX,
        DebugPixelY
    };

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
    CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList4->SetComputeRootSignature(RestirGIRootSignature.Get());
    CommandList4->SetPipelineState(PipelineState);
    CommandList4->SetComputeRootShaderResourceView(0, TlasResource->GetGPUVirtualAddress());
    CommandList4->SetComputeRootConstantBufferView(1, Owner.GetSceneConstantBufferAddress());
    static_assert(sizeof(FRestirGIConstants) / sizeof(uint32_t) <= kRestirGIConstantsDwordCount);
    CommandList4->SetComputeRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &Constants, 0);
    CommandList4->SetComputeRoot32BitConstants(3, kRestirGIBindlessDwordCount, BindlessIndices, 0);

    const uint32_t GroupSize = 8u;
    CommandList4->Dispatch((DispatchWidth + GroupSize - 1u) / GroupSize, (DispatchHeight + GroupSize - 1u) / GroupSize, 1u);
}

void FRestirGI::AddInitialSamplingPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const auto& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle RestirGIHalfDepthNormalHandle = Context.Resources.RestirGI.RestirGIHalfDepthNormalHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGIDenoiser.PrevLinearDepthHandle;
    const FRGResourceHandle RestirGIHistoryHandle = Context.Resources.RestirGI.RestirGIHistoryHandle;

    const uint32_t InitialPipelineIndex = (GetRandomMode() == ERestirGIRandomMode::BlueNoiseSobol) ? 1u : 0u;
    ID3D12PipelineState* InitialPipeline = RestirGIInitialPipelines[InitialPipelineIndex].Get();
    const bool bUseBlueNoiseSobol = InitialPipelineIndex == 1u;

    Graph.AddPass<FRestirGiSplitPassData>("InitialSampling", [&, DepthHandle, VelocityHandle, RestirGIHalfDepthNormalHandle, PrevLinearDepthHandle, GBufferHandles, RestirGIHistoryHandle, InitialPipeline, bUseBlueNoiseSobol](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = bEnabled && RestirGIRootSignature && InitialPipeline;
        if (!Data.bEnabled)
        {
            return;
        }
        const uint32_t HalfWidth = (static_cast<uint32_t>(Owner.Viewport.Width) + 1u) / 2u;
        const uint32_t HalfHeight = (static_cast<uint32_t>(Owner.Viewport.Height) + 1u) / 2u;
        const DXGI_FORMAT RadianceFormat = RestirGI.IsValid() ? RestirGI.Desc.Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        Context.Resources.RestirGI.RestirGIInitialRadianceHandle = Builder.CreateTexture("ReSTIR GI Initial Radiance", { HalfWidth, HalfHeight, RadianceFormat });
        Context.Resources.RestirGI.RestirGIInitialRayDirectionHandle = Builder.CreateTexture("ReSTIR GI Initial RayDir", { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIHalfDepthNormalHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIHistoryHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Context.Resources.RestirGI.RestirGIInitialRadianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(Context.Resources.RestirGI.RestirGIInitialRayDirectionHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, InitialPipeline, bUseBlueNoiseSobol, RestirGIHalfDepthNormalHandle](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size()) ? Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = CachedLinearClampSamplerIndex;
        const uint32_t InitialRadianceUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGIInitialRadianceHandle);
        const uint32_t InitialRayDirectionUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGIInitialRayDirectionHandle);
        const uint32_t HalfDepthNormalSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIHalfDepthNormalHandle);
        const bool bInputsValid =
            bCommonPersistentInputsValid &&
            (!bUseBlueNoiseSobol || bBlueNoisePersistentInputsValid) &&
            AreAllBindlessIndicesValid(
                InstanceDataBindlessIndex,
                InitialRadianceUavBindlessIndex,
                InitialRayDirectionUavBindlessIndex,
                HalfDepthNormalSrvBindlessIndex);
        const uint32_t BindlessIndices[30] =
        {
            InitialRadianceUavBindlessIndex,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            InitialRadianceUavBindlessIndex,
            InitialRayDirectionUavBindlessIndex,
            UINT32_MAX,
            HalfDepthNormalSrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistory.SrvBindlessIndex,
            CachedPrevLinearDepthSrvBindlessIndex,
            Owner.GpuDebugState.GetLineBufferUavBindlessIndex(),
            Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex
        };
        static_assert(_countof(BindlessIndices) <= kRestirGIBindlessDwordCount);
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, InitialPipeline, 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FRestirGI::AddTemporalResamplingPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle RestirGIHalfDepthNormalHandle = Context.Resources.RestirGI.RestirGIHalfDepthNormalHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGIDenoiser.PrevLinearDepthHandle;
    const FRGResourceHandle RestirGIInitialRadianceHandle = Context.Resources.RestirGI.RestirGIInitialRadianceHandle;
    const FRGResourceHandle RestirGIInitialRayDirectionHandle = Context.Resources.RestirGI.RestirGIInitialRayDirectionHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalAHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalAHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceAHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceAHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionAHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionAHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWAHandle = Context.Resources.RestirGI.RestirGIReservoirMWAHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGI.RestirGIReservoirMWBHandle;
    Graph.AddPass<FRestirGiSplitPassData>("TemporalResampling", [&, DepthHandle, VelocityHandle, RestirGIHalfDepthNormalHandle, PrevLinearDepthHandle, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = bEnabled && RestirGIRootSignature && RestirGITemporalPipeline;
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIHalfDepthNormalHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIInitialRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIInitialRayDirectionHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirDepthNormalAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirSampleRadianceAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirRayDirectionAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirMWAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, RestirGIHalfDepthNormalHandle, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size()) ? Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = CachedLinearClampSamplerIndex;
        const uint32_t InitialRadianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRadianceHandle);
        const uint32_t InitialRayDirectionSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRayDirectionHandle);
        const uint32_t HalfDepthNormalSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIHalfDepthNormalHandle);
        const bool bInputsValid =
            bCommonPersistentInputsValid &&
            AreAllBindlessIndicesValid(
                InitialRadianceSrvBindlessIndex,
                InitialRayDirectionSrvBindlessIndex,
                InstanceDataBindlessIndex,
                HalfDepthNormalSrvBindlessIndex) &&
            bReservoirAReadDescriptorsValid &&
            bReservoirBWriteDescriptorsValid;
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            InitialRadianceSrvBindlessIndex,
            InitialRayDirectionSrvBindlessIndex,
            RestirGIReservoirDepthNormalA.SrvBindlessIndex,
            RestirGIReservoirSampleRadianceA.SrvBindlessIndex,
            RestirGIReservoirRayDirectionA.SrvBindlessIndex,
            RestirGIReservoirMWA.SrvBindlessIndex,
            RestirGIReservoirDepthNormalB.UavBindlessIndex,
            RestirGIReservoirSampleRadianceB.UavBindlessIndex,
            RestirGIReservoirRayDirectionB.UavBindlessIndex,
            RestirGIReservoirMWB.UavBindlessIndex,
            HalfDepthNormalSrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistory.SrvBindlessIndex,
            CachedPrevLinearDepthSrvBindlessIndex,
            Owner.GpuDebugState.GetLineBufferUavBindlessIndex(),
            Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGITemporalPipeline.Get(), 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FRestirGI::AddReservoirBootstrapPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const auto& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle RestirGIHalfDepthNormalHandle = Context.Resources.RestirGI.RestirGIHalfDepthNormalHandle;
    const FRGResourceHandle RestirGIInitialRadianceHandle = Context.Resources.RestirGI.RestirGIInitialRadianceHandle;
    const FRGResourceHandle RestirGIInitialRayDirectionHandle = Context.Resources.RestirGI.RestirGIInitialRayDirectionHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGI.RestirGIReservoirMWBHandle;
    Graph.AddPass<FRestirGiSplitPassData>("ReservoirBootstrap", [&, DepthHandle, RestirGIHalfDepthNormalHandle, GBufferHandles, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = bEnabled && RestirGIRootSignature && RestirGIReservoirBootstrapPipeline;
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIHalfDepthNormalHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIInitialRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIInitialRayDirectionHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, RestirGIHalfDepthNormalHandle, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size()) ? Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = CachedLinearClampSamplerIndex;
        const uint32_t InitialRadianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRadianceHandle);
        const uint32_t InitialRayDirectionSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRayDirectionHandle);
        const uint32_t HalfDepthNormalSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIHalfDepthNormalHandle);
        const bool bInputsValid =
            bCommonPersistentInputsValid &&
            AreAllBindlessIndicesValid(
                InitialRadianceSrvBindlessIndex,
                InitialRayDirectionSrvBindlessIndex,
                InstanceDataBindlessIndex,
                HalfDepthNormalSrvBindlessIndex) &&
            bReservoirBWriteDescriptorsValid;
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            InitialRadianceSrvBindlessIndex,
            InitialRayDirectionSrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIReservoirDepthNormalB.UavBindlessIndex,
            RestirGIReservoirSampleRadianceB.UavBindlessIndex,
            RestirGIReservoirRayDirectionB.UavBindlessIndex,
            RestirGIReservoirMWB.UavBindlessIndex,
            HalfDepthNormalSrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistory.SrvBindlessIndex,
            CachedPrevLinearDepthSrvBindlessIndex,
            Owner.GpuDebugState.GetLineBufferUavBindlessIndex(),
            Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGIReservoirBootstrapPipeline.Get(), 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FRestirGI::AddSpatialResampling0Pass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGIDenoiser.PrevLinearDepthHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalAHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalAHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceAHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceAHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionAHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionAHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWAHandle = Context.Resources.RestirGI.RestirGIReservoirMWAHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGI.RestirGIReservoirMWBHandle;
    Graph.AddPass<FRestirGiSplitPassData>("SpatialResampling0", [&, PrevLinearDepthHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = bEnabled && RestirGIRootSignature && RestirGISpatialPipeline;
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIReservoirDepthNormalAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirSampleRadianceAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirRayDirectionAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirMWAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, PrevLinearDepthHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size()) ? Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = CachedLinearClampSamplerIndex;
        const bool bInputsValid =
            bCommonPersistentInputsValid &&
            IsValidBindlessIndex(InstanceDataBindlessIndex) &&
            bReservoirBReadDescriptorsValid &&
            bReservoirAWriteDescriptorsValid;
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIReservoirDepthNormalA.UavBindlessIndex,
            RestirGIReservoirSampleRadianceA.UavBindlessIndex,
            RestirGIReservoirRayDirectionA.UavBindlessIndex,
            RestirGIReservoirMWA.UavBindlessIndex,
            RestirGIReservoirDepthNormalB.SrvBindlessIndex,
            RestirGIReservoirSampleRadianceB.SrvBindlessIndex,
            RestirGIReservoirRayDirectionB.SrvBindlessIndex,
            RestirGIReservoirMWB.SrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,+
            RestirGIHistory.SrvBindlessIndex,
            CachedPrevLinearDepthSrvBindlessIndex,
            Owner.GpuDebugState.GetLineBufferUavBindlessIndex(),
            Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGISpatialPipeline.Get(), 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FRestirGI::AddSpatialResampling1Pass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle RestirGIReservoirDepthNormalAHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalAHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceAHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceAHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionAHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionAHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWAHandle = Context.Resources.RestirGI.RestirGIReservoirMWAHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGI.RestirGIReservoirMWBHandle;
    Graph.AddPass<FRestirGiSplitPassData>("SpatialResampling1", [&, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = bEnabled && RestirGIRootSignature && RestirGISpatialPipeline;
        if (!Data.bEnabled)
        {
            return;
        }
        Builder.ReadTexture(RestirGIReservoirDepthNormalAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirSampleRadianceAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirRayDirectionAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirMWAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size()) ? Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = CachedLinearClampSamplerIndex;
        const bool bInputsValid =
            bCommonPersistentInputsValid &&
            IsValidBindlessIndex(InstanceDataBindlessIndex) &&
            bReservoirAReadDescriptorsValid &&
            bReservoirBWriteDescriptorsValid;
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIReservoirDepthNormalB.UavBindlessIndex,
            RestirGIReservoirSampleRadianceB.UavBindlessIndex,
            RestirGIReservoirRayDirectionB.UavBindlessIndex,
            RestirGIReservoirMWB.UavBindlessIndex,
            RestirGIReservoirDepthNormalA.SrvBindlessIndex,
            RestirGIReservoirSampleRadianceA.SrvBindlessIndex,
            RestirGIReservoirRayDirectionA.SrvBindlessIndex,
            RestirGIReservoirMWA.SrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistory.SrvBindlessIndex,
            CachedPrevLinearDepthSrvBindlessIndex,
            Owner.GpuDebugState.GetLineBufferUavBindlessIndex(),
            Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGISpatialPipeline.Get(), 1u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FRestirGI::AddResolvePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const auto& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGIDenoiser.PrevLinearDepthHandle;
    const FRGResourceHandle RestirGIHandle = Context.Resources.RestirGI.RestirGIHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGI.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGI.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGI.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGI.RestirGIReservoirMWBHandle;
    Graph.AddPass<FRestirGiSplitPassData>("RestirGIResolve", [&, DepthHandle, PrevLinearDepthHandle, GBufferHandles, RestirGIHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = bEnabled && RestirGIRootSignature && RestirGIResolvePipeline;
        if (!Data.bEnabled)
        {
            return;
        }
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        Context.Resources.RestirGI.RestirGiInputSHHandle = Builder.CreateTexture("ReSTIR GI Input SH", { FullWidth, FullHeight, DXGI_FORMAT_R32G32B32A32_UINT });
        Context.Resources.RestirGI.RestirGiVarianceHandle = Builder.CreateTexture("ReSTIR GI Variance", { FullWidth, FullHeight, DXGI_FORMAT_R8_UNORM });
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(Context.Resources.RestirGI.RestirGiInputSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(Context.Resources.RestirGI.RestirGiVarianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, DepthHandle, PrevLinearDepthHandle, RestirGIHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWBHandle](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size()) ? Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = CachedLinearClampSamplerIndex;
        const uint32_t InputSHUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGiInputSHHandle);
        const uint32_t VarianceUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGiVarianceHandle);
        const bool bInputsValid =
            bCommonPersistentInputsValid &&
            AreAllBindlessIndicesValid(
                DepthBindlessIndex,
                InstanceDataBindlessIndex,
                RestirGI.UavBindlessIndex,
                InputSHUavBindlessIndex,
                VarianceUavBindlessIndex) &&
            bReservoirBReadDescriptorsValid;
        const uint32_t BindlessIndices[30] =
        {
            RestirGI.UavBindlessIndex,
            DepthBindlessIndex,
            Owner.VelocityTexture.SrvBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIReservoirDepthNormalB.SrvBindlessIndex,
            RestirGIReservoirSampleRadianceB.SrvBindlessIndex,
            RestirGIReservoirRayDirectionB.SrvBindlessIndex,
            RestirGIReservoirMWB.SrvBindlessIndex,
            InputSHUavBindlessIndex,
            VarianceUavBindlessIndex,
            RestirGIHistory.SrvBindlessIndex,
            CachedPrevLinearDepthSrvBindlessIndex,
            Owner.GpuDebugState.GetLineBufferUavBindlessIndex(),
            Owner.BlueNoiseSobolTexture.SrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPTexture.SrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGIResolvePipeline.Get(), 0u, BindlessIndices, FullWidth, FullHeight, Data.bEnabled && bInputsValid);
    });
}
