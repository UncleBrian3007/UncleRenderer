#include "DeferredRayTracingPasses.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12Device.h"
#include "../ShaderCompiler.h"
#include <d3dx12.h>
#include <algorithm>
#define A_CPU
#include "../../../Shaders/ffx_a.h"
#include "../../../Shaders/ffx_spd.h"
#undef A_CPU
#include <sstream>
#include <string>

bool FDeferredRenderer::CreateRestirGIRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[4] = {};

    // RootParams[0]: TLAS SRV (t0), used in Shaders/RestirGI.hlsl
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[1]: Scene constants CBV (b0), used in Shaders/SceneConstants.hlsl
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Descriptor.ShaderRegister = 0;
    RootParams[1].Descriptor.RegisterSpace = 0;
    RootParams[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    // RootParams[2]: ReSTIR GI constants (b1)
    RootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[2].Constants.Num32BitValues = 19;
    RootParams[2].Constants.RegisterSpace = 0;
    RootParams[2].Constants.ShaderRegister = 1;

    // RootParams[3]: ReSTIR GI bindless indices (b2)
    RootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[3].Constants.Num32BitValues = 28;
    RootParams[3].Constants.RegisterSpace = 0;
    RootParams[3].Constants.ShaderRegister = 2;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 0;
    RootSigDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

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

bool FDeferredRenderer::CreateRestirGIPipeline(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    if (!Device->IsRayTracingSupported())
    {
        LogWarning("Skipping ReSTIR GI pipeline creation: DXR is not supported.");
        return false;
    }

    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> InitialByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSInitialSampling", CSTarget, InitialByteCode))
    {
        return false;
    }

    std::vector<uint8_t> TemporalByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSTemporalResampling", CSTarget, TemporalByteCode))
    {
        return false;
    }

    std::vector<uint8_t> BootstrapByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSReservoirBootstrap", CSTarget, BootstrapByteCode))
    {
        return false;
    }

    std::vector<uint8_t> SpatialByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSSpatialResampling", CSTarget, SpatialByteCode))
    {
        return false;
    }

    std::vector<uint8_t> ResolveByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSResolve", CSTarget, ResolveByteCode))
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

    PsoDesc.CS = { InitialByteCode.data(), InitialByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIInitialPipeline, "CSInitialSampling"))
    {
        return false;
    }

    PsoDesc.CS = { TemporalByteCode.data(), TemporalByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGITemporalPipeline, "CSTemporalResampling"))
    {
        RestirGIInitialPipeline.Reset();
        return false;
    }

    PsoDesc.CS = { BootstrapByteCode.data(), BootstrapByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIReservoirBootstrapPipeline, "CSReservoirBootstrap"))
    {
        RestirGIInitialPipeline.Reset();
        RestirGIReservoirBootstrapPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        return false;
    }

    PsoDesc.CS = { SpatialByteCode.data(), SpatialByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGISpatialPipeline, "CSSpatialResampling"))
    {
        RestirGIInitialPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        return false;
    }

    PsoDesc.CS = { ResolveByteCode.data(), ResolveByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIResolvePipeline, "CSResolve"))
    {
        RestirGIInitialPipeline.Reset();
        RestirGIReservoirBootstrapPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        RestirGISpatialPipeline.Reset();
        return false;
    }

    return true;
}

DXGI_FORMAT FDeferredRenderer::ResolveRestirGiRadianceFormat(FDX12Device* Device) const
{
    if (!Device || !Device->GetDevice())
    {
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport = {};
    FormatSupport.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    if (SUCCEEDED(Device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &FormatSupport, sizeof(FormatSupport)))
        && (FormatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0
        && (FormatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0)
    {
        return DXGI_FORMAT_R11G11B10_FLOAT;
    }

    return DXGI_FORMAT_R16G16B16A16_FLOAT;
}

bool FDeferredRenderer::CreateRestirGIResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const DXGI_FORMAT RestirGiRadianceFormat = ResolveRestirGiRadianceFormat(Device);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        RestirGiRadianceFormat,
        Width,
        Height,
        1,
        1,
        1,
        0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGITexture.GetAddressOf())));

    if (RestirGITexture)
    {
        RestirGITexture->SetName(L"ReSTIR_GI");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGIHistoryTexture.GetAddressOf())));

    if (RestirGIHistoryTexture)
    {
        RestirGIHistoryTexture->SetName(L"ReSTIR_GI_History");
    }

    const uint64_t ReservoirStride = sizeof(float) * 8u;
    const uint64_t ReservoirElementCount = static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height);
    const uint64_t ReservoirBufferSize = ReservoirStride * ReservoirElementCount;
    CD3DX12_RESOURCE_DESC ReservoirDesc = CD3DX12_RESOURCE_DESC::Buffer(ReservoirBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ReservoirDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGITemporalReservoirBuffer.GetAddressOf())));

    if (RestirGITemporalReservoirBuffer)
    {
        RestirGITemporalReservoirBuffer->SetName(L"ReSTIR_GI_TemporalReservoir");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ReservoirDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(RestirGISpatialReservoirBuffer.GetAddressOf())));

    if (RestirGISpatialReservoirBuffer)
    {
        RestirGISpatialReservoirBuffer->SetName(L"ReSTIR_GI_SpatialReservoir");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &ReservoirDesc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(RestirGIReservoirHistoryBuffer.GetAddressOf())));

    if (RestirGIReservoirHistoryBuffer)
    {
        RestirGIReservoirHistoryBuffer->SetName(L"ReSTIR_GI_Reservoir_History");
    }

    const uint32_t HalfWidth = (Width + 1u) / 2u;
    const uint32_t HalfHeight = (Height + 1u) / 2u;

    auto CreateRestirGITexture = [&](DXGI_FORMAT Format, const wchar_t* Name, Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource)
    {
        CD3DX12_RESOURCE_DESC TextureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            Format,
            HalfWidth,
            HalfHeight,
            1,
            1,
            1,
            0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &TextureDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(OutResource.ReleaseAndGetAddressOf())));

        if (OutResource)
        {
            OutResource->SetName(Name);
        }
    };

    CreateRestirGITexture(RestirGiRadianceFormat, L"ReSTIR_GI_InitialRadiance", RestirGIInitialRadianceTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_InitialRayDirection", RestirGIInitialRayDirectionTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalA", RestirGIReservoirDepthNormalATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalB", RestirGIReservoirDepthNormalBTexture);
    CreateRestirGITexture(RestirGiRadianceFormat, L"ReSTIR_GI_ReservoirSampleRadianceA", RestirGIReservoirSampleRadianceATexture);
    CreateRestirGITexture(RestirGiRadianceFormat, L"ReSTIR_GI_ReservoirSampleRadianceB", RestirGIReservoirSampleRadianceBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_ReservoirRayDirectionA", RestirGIReservoirRayDirectionATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_ReservoirRayDirectionB", RestirGIReservoirRayDirectionBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_FLOAT, L"ReSTIR_GI_ReservoirMWA", RestirGIReservoirMWATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_FLOAT, L"ReSTIR_GI_ReservoirMWB", RestirGIReservoirMWBTexture);

    RestirGIState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIHistoryState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGITemporalReservoirState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGISpatialReservoirState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirHistoryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    RestirGIInitialRadianceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIInitialRayDirectionState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirDepthNormalAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirDepthNormalBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirSampleRadianceAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirSampleRadianceBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirRayDirectionAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirRayDirectionBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirMWAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirMWBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bRestirGIReservoirHistoryValid = false;
    bRestirGIDenoiserHistoryValid = false;
    return true;
}

void FDeferredRayTracingPasses::AddRestirGIPass(FDeferredPassContext& Context) const
{
    AddRestirGIPassImpl(Context);
}

void FDeferredRayTracingPasses::AddRestirGIPassImpl(FDeferredPassContext& Context) const
{
    AddInitialSamplingPass(Context);
    if (Context.Owner.bRestirGITemporalReuse)
    {
        AddTemporalResamplingPass(Context);
    }
    else
    {
        AddReservoirBootstrapPass(Context);
    }
    if (Context.Owner.bRestirGISpatialReuse)
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

uint32_t FDeferredRayTracingPasses::GetDepthBindlessIndexForRestir(FDeferredRenderer& Owner) const
{
    if (Owner.DepthBindlessIndices.empty())
    {
        return UINT32_MAX;
    }
    const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(Owner.DepthBindlessIndices.size());
    return Owner.DepthBindlessIndices[DepthArrayIndex];
}

void FDeferredRayTracingPasses::DispatchRestirPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, const wchar_t* EventName, uint32_t SpatialPassIndex, const uint32_t BindlessIndices[28], uint32_t DispatchWidth, uint32_t DispatchHeight, bool bEnabled) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FDX12Device* Device = Owner.Device;
    if (!bEnabled || !Device || !Device->GetBindlessDescriptorHeap() || !PipelineState || !Owner.RestirGIRootSignature)
    {
        return;
    }

    const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
    if (FrameIndex >= Owner.PathTracingInstanceDataBindlessIndices.size())
    {
        return;
    }

    ID3D12Resource* TlasResource = (FrameIndex < Owner.TlasResultBuffers.size()) ? Owner.TlasResultBuffers[FrameIndex].Get() : nullptr;
    if (!TlasResource)
    {
        for (const auto& TlasBuffer : Owner.TlasResultBuffers)
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
    const uint32_t MaxHistoryFrames = (std::max)(1u, Owner.RestirGIMaxHistoryFrames);
    const uint32_t GlobalFrameNumber = static_cast<uint32_t>(Owner.GetFrameNumber());
    const uint32_t SequenceFrame = Owner.bRestirGIFreezeFrame ? Owner.RestirGIFrozenSequenceFrame : GlobalFrameNumber;

    const FRestirGIConstants Constants =
    {
        FullWidth,
        FullHeight,
        HalfWidth,
        HalfHeight,
        GlobalFrameNumber,
        Owner.bRestirGIEnabled ? 1u : 0u,
        (Owner.bRestirGIReservoirHistoryValid && Owner.RestirGIReservoirHistoryFrameCount >= MaxHistoryFrames) ? 1u : 0u,
        SpatialPassIndex,
        (std::max)(0.0f, Owner.RestirGIIntensity),
        Owner.RestirGIRayLength,
        Owner.RestirGIClamp,
        Owner.bRestirGIUseVisibility ? 1u : 0u,
        Owner.bRestirGIUseBrdf ? 1u : 0u,
        Owner.bRestirGIUseHistoryIndirect ? 1u : 0u,
        SequenceFrame,
        Owner.bRestirGIDebugRayEnabled ? 1u : 0u,
        Owner.RestirGIDebugPixelX,
        Owner.RestirGIDebugPixelY
    };

    FScopedPixEvent RestirEvent(CommandList4, EventName);
    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
    CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList4->SetComputeRootSignature(Owner.RestirGIRootSignature.Get());
    CommandList4->SetPipelineState(PipelineState);
    CommandList4->SetComputeRootShaderResourceView(0, TlasResource->GetGPUVirtualAddress());
    CommandList4->SetComputeRootConstantBufferView(1, Owner.GetSceneConstantBufferAddress());
    CommandList4->SetComputeRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &Constants, 0);
    CommandList4->SetComputeRoot32BitConstants(3, 28, BindlessIndices, 0);

    const uint32_t GroupSize = 8u;
    CommandList4->Dispatch((DispatchWidth + GroupSize - 1u) / GroupSize, (DispatchHeight + GroupSize - 1u) / GroupSize, 1u);
}

void FDeferredRayTracingPasses::AddInitialSamplingPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const auto& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGiPrevLinearDepthHandle;
    const FRGResourceHandle RestirGIHistoryHandle = Context.Resources.RestirGIHistoryHandle;
    const FRGResourceHandle RestirGIInitialRadianceHandle = Context.Resources.RestirGIInitialRadianceHandle;
    const FRGResourceHandle RestirGIInitialRayDirectionHandle = Context.Resources.RestirGIInitialRayDirectionHandle;

    Graph.AddPass<FRestirGiSplitPassData>("InitialSampling", [&, DepthHandle, VelocityHandle, LinearDepthHandle, PrevLinearDepthHandle, GBufferHandles, RestirGIHistoryHandle, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGIRootSignature && Owner.RestirGIInitialPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIHistoryHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[1], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[2], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIInitialRadianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIInitialRayDirectionHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX) && (Owner.VelocityBindlessIndex != UINT32_MAX) && (Owner.GBufferBindlessIndices[0] != UINT32_MAX) && (Owner.GBufferBindlessIndices[1] != UINT32_MAX) && (Owner.GBufferBindlessIndices[2] != UINT32_MAX) && (InstanceDataBindlessIndex != UINT32_MAX) && (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) && (LinearClampSamplerIndex != UINT32_MAX) && (Owner.RestirGIInitialRadianceUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIInitialRayDirectionUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIHistorySrvBindlessIndex != UINT32_MAX) && (Owner.LinearDepthBindlessIndex != UINT32_MAX) && (Owner.RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[28] = { Owner.RestirGIInitialRadianceUavBindlessIndex, DepthBindlessIndex, Owner.VelocityBindlessIndex, Owner.GBufferBindlessIndices[0], Owner.GBufferBindlessIndices[1], Owner.GBufferBindlessIndices[2], InstanceDataBindlessIndex, Owner.EnvironmentCubeBindlessIndex, LinearClampSamplerIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIInitialRadianceUavBindlessIndex, Owner.RestirGIInitialRayDirectionUavBindlessIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIHistorySrvBindlessIndex, Owner.RestirGiPrevLinearDepthSrvBindlessIndex, Owner.GpuDebugLineBufferUavBindlessIndex };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, Owner.RestirGIInitialPipeline.Get(), L"InitialSampling", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FDeferredRayTracingPasses::AddTemporalResamplingPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGiPrevLinearDepthHandle;
    const FRGResourceHandle RestirGIInitialRadianceHandle = Context.Resources.RestirGIInitialRadianceHandle;
    const FRGResourceHandle RestirGIInitialRayDirectionHandle = Context.Resources.RestirGIInitialRayDirectionHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalAHandle = Context.Resources.RestirGIReservoirDepthNormalAHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceAHandle = Context.Resources.RestirGIReservoirSampleRadianceAHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionAHandle = Context.Resources.RestirGIReservoirRayDirectionAHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWAHandle = Context.Resources.RestirGIReservoirMWAHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGIReservoirMWBHandle;

    Graph.AddPass<FRestirGiSplitPassData>("TemporalResampling", [&, DepthHandle, VelocityHandle, PrevLinearDepthHandle, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGIRootSignature && Owner.RestirGITemporalPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
    }, [&](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX) && (Owner.VelocityBindlessIndex != UINT32_MAX) && (Owner.GBufferBindlessIndices[0] != UINT32_MAX) && (Owner.GBufferBindlessIndices[1] != UINT32_MAX) && (Owner.GBufferBindlessIndices[2] != UINT32_MAX) && (InstanceDataBindlessIndex != UINT32_MAX) && (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) && (LinearClampSamplerIndex != UINT32_MAX) && (Owner.RestirGIInitialRadianceSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIInitialRayDirectionSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[28] = { UINT32_MAX, DepthBindlessIndex, Owner.VelocityBindlessIndex, Owner.GBufferBindlessIndices[0], Owner.GBufferBindlessIndices[1], Owner.GBufferBindlessIndices[2], InstanceDataBindlessIndex, Owner.EnvironmentCubeBindlessIndex, LinearClampSamplerIndex, Owner.RestirGIInitialRadianceSrvBindlessIndex, Owner.RestirGIInitialRayDirectionSrvBindlessIndex, Owner.RestirGIReservoirDepthNormalASrvBindlessIndex, Owner.RestirGIReservoirSampleRadianceASrvBindlessIndex, Owner.RestirGIReservoirRayDirectionASrvBindlessIndex, Owner.RestirGIReservoirMWASrvBindlessIndex, Owner.RestirGIReservoirDepthNormalBUavBindlessIndex, Owner.RestirGIReservoirSampleRadianceBUavBindlessIndex, Owner.RestirGIReservoirRayDirectionBUavBindlessIndex, Owner.RestirGIReservoirMWBUavBindlessIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIHistorySrvBindlessIndex, Owner.RestirGiPrevLinearDepthSrvBindlessIndex, Owner.GpuDebugLineBufferUavBindlessIndex };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, Owner.RestirGITemporalPipeline.Get(), L"TemporalResampling", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FDeferredRayTracingPasses::AddReservoirBootstrapPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const auto& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle RestirGIInitialRadianceHandle = Context.Resources.RestirGIInitialRadianceHandle;
    const FRGResourceHandle RestirGIInitialRayDirectionHandle = Context.Resources.RestirGIInitialRayDirectionHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGIReservoirMWBHandle;

    Graph.AddPass<FRestirGiSplitPassData>("ReservoirBootstrap", [&, DepthHandle, GBufferHandles, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGIRootSignature && Owner.RestirGIReservoirBootstrapPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIInitialRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIInitialRayDirectionHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX) && (Owner.GBufferBindlessIndices[0] != UINT32_MAX) && (Owner.GBufferBindlessIndices[1] != UINT32_MAX) && (Owner.GBufferBindlessIndices[2] != UINT32_MAX) && (InstanceDataBindlessIndex != UINT32_MAX) && (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) && (LinearClampSamplerIndex != UINT32_MAX) && (Owner.RestirGIInitialRadianceSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIInitialRayDirectionSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[28] = { UINT32_MAX, DepthBindlessIndex, Owner.VelocityBindlessIndex, Owner.GBufferBindlessIndices[0], Owner.GBufferBindlessIndices[1], Owner.GBufferBindlessIndices[2], InstanceDataBindlessIndex, Owner.EnvironmentCubeBindlessIndex, LinearClampSamplerIndex, Owner.RestirGIInitialRadianceSrvBindlessIndex, Owner.RestirGIInitialRayDirectionSrvBindlessIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIReservoirDepthNormalBUavBindlessIndex, Owner.RestirGIReservoirSampleRadianceBUavBindlessIndex, Owner.RestirGIReservoirRayDirectionBUavBindlessIndex, Owner.RestirGIReservoirMWBUavBindlessIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIHistorySrvBindlessIndex, Owner.RestirGiPrevLinearDepthSrvBindlessIndex, Owner.GpuDebugLineBufferUavBindlessIndex };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, Owner.RestirGIReservoirBootstrapPipeline.Get(), L"ReservoirBootstrap", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FDeferredRayTracingPasses::AddSpatialResampling0Pass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGiPrevLinearDepthHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalAHandle = Context.Resources.RestirGIReservoirDepthNormalAHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceAHandle = Context.Resources.RestirGIReservoirSampleRadianceAHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionAHandle = Context.Resources.RestirGIReservoirRayDirectionAHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWAHandle = Context.Resources.RestirGIReservoirMWAHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGIReservoirMWBHandle;

    Graph.AddPass<FRestirGiSplitPassData>("SpatialResampling0", [&, PrevLinearDepthHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGIRootSignature && Owner.RestirGISpatialPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIReservoirDepthNormalAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirSampleRadianceAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirRayDirectionAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirMWAHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX) && (Owner.GBufferBindlessIndices[0] != UINT32_MAX) && (Owner.GBufferBindlessIndices[1] != UINT32_MAX) && (Owner.GBufferBindlessIndices[2] != UINT32_MAX) && (InstanceDataBindlessIndex != UINT32_MAX) && (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) && (LinearClampSamplerIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalAUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceAUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionAUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWAUavBindlessIndex != UINT32_MAX) && (Owner.RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[28] = { UINT32_MAX, DepthBindlessIndex, Owner.VelocityBindlessIndex, Owner.GBufferBindlessIndices[0], Owner.GBufferBindlessIndices[1], Owner.GBufferBindlessIndices[2], InstanceDataBindlessIndex, Owner.EnvironmentCubeBindlessIndex, LinearClampSamplerIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIReservoirDepthNormalAUavBindlessIndex, Owner.RestirGIReservoirSampleRadianceAUavBindlessIndex, Owner.RestirGIReservoirRayDirectionAUavBindlessIndex, Owner.RestirGIReservoirMWAUavBindlessIndex, Owner.RestirGIReservoirDepthNormalBSrvBindlessIndex, Owner.RestirGIReservoirSampleRadianceBSrvBindlessIndex, Owner.RestirGIReservoirRayDirectionBSrvBindlessIndex, Owner.RestirGIReservoirMWBSrvBindlessIndex, UINT32_MAX, UINT32_MAX, Owner.RestirGIHistorySrvBindlessIndex, Owner.RestirGiPrevLinearDepthSrvBindlessIndex, Owner.GpuDebugLineBufferUavBindlessIndex };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, Owner.RestirGISpatialPipeline.Get(), L"SpatialResampling0", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FDeferredRayTracingPasses::AddSpatialResampling1Pass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FRGResourceHandle RestirGIReservoirDepthNormalAHandle = Context.Resources.RestirGIReservoirDepthNormalAHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceAHandle = Context.Resources.RestirGIReservoirSampleRadianceAHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionAHandle = Context.Resources.RestirGIReservoirRayDirectionAHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWAHandle = Context.Resources.RestirGIReservoirMWAHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGIReservoirMWBHandle;

    Graph.AddPass<FRestirGiSplitPassData>("SpatialResampling1", [&, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGIRootSignature && Owner.RestirGISpatialPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(RestirGIReservoirDepthNormalAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirSampleRadianceAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirRayDirectionAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(RestirGIReservoirMWAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX) && (Owner.GBufferBindlessIndices[0] != UINT32_MAX) && (Owner.GBufferBindlessIndices[1] != UINT32_MAX) && (Owner.GBufferBindlessIndices[2] != UINT32_MAX) && (InstanceDataBindlessIndex != UINT32_MAX) && (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) && (LinearClampSamplerIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWASrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX) && (Owner.RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[28] = { UINT32_MAX, DepthBindlessIndex, Owner.VelocityBindlessIndex, Owner.GBufferBindlessIndices[0], Owner.GBufferBindlessIndices[1], Owner.GBufferBindlessIndices[2], InstanceDataBindlessIndex, Owner.EnvironmentCubeBindlessIndex, LinearClampSamplerIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIReservoirDepthNormalBUavBindlessIndex, Owner.RestirGIReservoirSampleRadianceBUavBindlessIndex, Owner.RestirGIReservoirRayDirectionBUavBindlessIndex, Owner.RestirGIReservoirMWBUavBindlessIndex, Owner.RestirGIReservoirDepthNormalASrvBindlessIndex, Owner.RestirGIReservoirSampleRadianceASrvBindlessIndex, Owner.RestirGIReservoirRayDirectionASrvBindlessIndex, Owner.RestirGIReservoirMWASrvBindlessIndex, UINT32_MAX, UINT32_MAX, Owner.RestirGIHistorySrvBindlessIndex, Owner.RestirGiPrevLinearDepthSrvBindlessIndex, Owner.GpuDebugLineBufferUavBindlessIndex };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, Owner.RestirGISpatialPipeline.Get(), L"SpatialResampling1", 1u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
    });
}

void FDeferredRayTracingPasses::AddResolvePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const auto& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGiPrevLinearDepthHandle;
    const FRGResourceHandle RestirGIHandle = Context.Resources.RestirGIHandle;
    const FRGResourceHandle RestirGIReservoirDepthNormalBHandle = Context.Resources.RestirGIReservoirDepthNormalBHandle;
    const FRGResourceHandle RestirGIReservoirSampleRadianceBHandle = Context.Resources.RestirGIReservoirSampleRadianceBHandle;
    const FRGResourceHandle RestirGIReservoirRayDirectionBHandle = Context.Resources.RestirGIReservoirRayDirectionBHandle;
    const FRGResourceHandle RestirGIReservoirMWBHandle = Context.Resources.RestirGIReservoirMWBHandle;
    const FRGResourceHandle RestirGiInputSHHandle = Context.Resources.RestirGiInputSHHandle;
    const FRGResourceHandle RestirGiVarianceHandle = Context.Resources.RestirGiVarianceHandle;

    Graph.AddPass<FRestirGiSplitPassData>("RestirGIResolve", [&, DepthHandle, PrevLinearDepthHandle, GBufferHandles, RestirGIHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWBHandle, RestirGiInputSHHandle, RestirGiVarianceHandle](FRestirGiSplitPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGIRootSignature && Owner.RestirGIResolvePipeline;
        if (!Data.bEnabled) { return; }
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
        Builder.WriteTexture(RestirGiInputSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(RestirGiVarianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX) && (Owner.GBufferBindlessIndices[0] != UINT32_MAX) && (Owner.GBufferBindlessIndices[1] != UINT32_MAX) && (Owner.GBufferBindlessIndices[2] != UINT32_MAX) && (InstanceDataBindlessIndex != UINT32_MAX) && (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) && (LinearClampSamplerIndex != UINT32_MAX) && (Owner.RestirGIUavBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirDepthNormalBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirSampleRadianceBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirRayDirectionBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGIReservoirMWBSrvBindlessIndex != UINT32_MAX) && (Owner.RestirGiInputSHUavBindlessIndex != UINT32_MAX) && (Owner.RestirGiVarianceUavBindlessIndex != UINT32_MAX) && (Owner.RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[28] = { Owner.RestirGIUavBindlessIndex, DepthBindlessIndex, Owner.VelocityBindlessIndex, Owner.GBufferBindlessIndices[0], Owner.GBufferBindlessIndices[1], Owner.GBufferBindlessIndices[2], InstanceDataBindlessIndex, Owner.EnvironmentCubeBindlessIndex, LinearClampSamplerIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, Owner.RestirGIReservoirDepthNormalBSrvBindlessIndex, Owner.RestirGIReservoirSampleRadianceBSrvBindlessIndex, Owner.RestirGIReservoirRayDirectionBSrvBindlessIndex, Owner.RestirGIReservoirMWBSrvBindlessIndex, Owner.RestirGiInputSHUavBindlessIndex, Owner.RestirGiVarianceUavBindlessIndex, Owner.RestirGIHistorySrvBindlessIndex, Owner.RestirGiPrevLinearDepthSrvBindlessIndex, Owner.GpuDebugLineBufferUavBindlessIndex };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, Owner.RestirGIResolvePipeline.Get(), L"Resolve", 0u, BindlessIndices, FullWidth, FullHeight, Data.bEnabled && bInputsValid);
    });
}
