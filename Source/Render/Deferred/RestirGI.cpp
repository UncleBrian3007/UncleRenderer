#include "RestirGI.h"
#include "../DeferredRenderer.h"
#include "DeferredPassContext.h"
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

using Microsoft::WRL::ComPtr;

bool FRestirGI::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    if (!Device || !Device->IsRayTracingSupported())
    {
        if (bEnabled_)
        {
            LogWarning("Deferred renderer: ReSTIR GI disabled because DXR is not supported on this device.");
        }
        bEnabled_ = false;
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
        bEnabled_ = false;
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

void FRestirGI::ImportPersistentResources(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FRestirGIFrameResources& Resources = Context.Resources.RestirGI;

    Resources.RestirGIHandle = Graph.ImportTexture(
        "ReSTIR GI",
        RestirGITexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), RestirGITexture->GetDesc().Format });

    Resources.RestirGIHistoryHandle = Graph.ImportTexture(
        "ReSTIR GI History",
        RestirGIHistoryTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIHistoryState),
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), RestirGIHistoryTexture->GetDesc().Format });

    const uint32_t HalfWidth = (static_cast<uint32>(Owner.Viewport.Width) + 1u) / 2u;
    const uint32_t HalfHeight = (static_cast<uint32>(Owner.Viewport.Height) + 1u) / 2u;

    Resources.RestirGIReservoirDepthNormalAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir DepthNormal A",
        RestirGIReservoirDepthNormalATexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirDepthNormalAState),
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT });

    Resources.RestirGIReservoirDepthNormalBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir DepthNormal B",
        RestirGIReservoirDepthNormalBTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirDepthNormalBState),
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32G32_UINT });

    Resources.RestirGIReservoirSampleRadianceAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir SampleRadiance A",
        RestirGIReservoirSampleRadianceATexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirSampleRadianceAState),
        { HalfWidth, HalfHeight, RestirGIReservoirSampleRadianceATexture->GetDesc().Format });

    Resources.RestirGIReservoirSampleRadianceBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir SampleRadiance B",
        RestirGIReservoirSampleRadianceBTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirSampleRadianceBState),
        { HalfWidth, HalfHeight, RestirGIReservoirSampleRadianceBTexture->GetDesc().Format });

    Resources.RestirGIReservoirRayDirectionAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir RayDirection A",
        RestirGIReservoirRayDirectionATexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirRayDirectionAState),
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    Resources.RestirGIReservoirRayDirectionBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir RayDirection B",
        RestirGIReservoirRayDirectionBTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirRayDirectionBState),
        { HalfWidth, HalfHeight, DXGI_FORMAT_R32_UINT });

    Resources.RestirGIReservoirMWAHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir MW A",
        RestirGIReservoirMWATexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirMWAState),
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16_FLOAT });

    Resources.RestirGIReservoirMWBHandle = Graph.ImportTexture(
        "ReSTIR GI Reservoir MW B",
        RestirGIReservoirMWBTexture.Get(),
        const_cast<D3D12_RESOURCE_STATES*>(&RestirGIReservoirMWBState),
        { HalfWidth, HalfHeight, DXGI_FORMAT_R16G16_FLOAT });
}

bool FRestirGI::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    if (!Device)
    {
        return false;
    }

    const DXGI_FORMAT RestirGiRadianceFormat = RestirGITexture ? RestirGITexture->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_SHADER_RESOURCE_VIEW_DESC RestirSrvDesc = {};
    RestirSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    RestirSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    RestirSrvDesc.Format = RestirGiRadianceFormat;
    RestirSrvDesc.Texture2D.MipLevels = 1;
    RestirGIBindlessIndex = Device->CreateBindlessSrv(RestirGITexture.Get(), RestirSrvDesc);

    D3D12_UNORDERED_ACCESS_VIEW_DESC RestirUavDesc = {};
    RestirUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    RestirUavDesc.Format = RestirGiRadianceFormat;
    RestirUavDesc.Texture2D.MipSlice = 0;
    RestirGIUavBindlessIndex = Device->CreateBindlessUav(RestirGITexture.Get(), nullptr, RestirUavDesc);

    RestirGIHistorySrvBindlessIndex = Device->CreateBindlessSrv(RestirGIHistoryTexture.Get(), RestirSrvDesc);
    RestirGIHistoryUavBindlessIndex = Device->CreateBindlessUav(RestirGIHistoryTexture.Get(), nullptr, RestirUavDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC ReservoirSrvDesc = {};
    ReservoirSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ReservoirSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ReservoirSrvDesc.Texture2D.MipLevels = 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC ReservoirUavDesc = {};
    ReservoirUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ReservoirUavDesc.Texture2D.MipSlice = 0;

    ReservoirSrvDesc.Format = RestirGiRadianceFormat;
    ReservoirUavDesc.Format = RestirGiRadianceFormat;
    RestirGIReservoirSampleRadianceASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirSampleRadianceATexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirSampleRadianceAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirSampleRadianceATexture.Get(), nullptr, ReservoirUavDesc);
    RestirGIReservoirSampleRadianceBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirSampleRadianceBTexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirSampleRadianceBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirSampleRadianceBTexture.Get(), nullptr, ReservoirUavDesc);

    ReservoirSrvDesc.Format = DXGI_FORMAT_R32_UINT;
    ReservoirUavDesc.Format = DXGI_FORMAT_R32_UINT;
    RestirGIReservoirRayDirectionASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirRayDirectionATexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirRayDirectionAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirRayDirectionATexture.Get(), nullptr, ReservoirUavDesc);
    RestirGIReservoirRayDirectionBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirRayDirectionBTexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirRayDirectionBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirRayDirectionBTexture.Get(), nullptr, ReservoirUavDesc);

    ReservoirSrvDesc.Format = DXGI_FORMAT_R32G32_UINT;
    ReservoirUavDesc.Format = DXGI_FORMAT_R32G32_UINT;
    RestirGIReservoirDepthNormalASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirDepthNormalATexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirDepthNormalAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirDepthNormalATexture.Get(), nullptr, ReservoirUavDesc);
    RestirGIReservoirDepthNormalBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirDepthNormalBTexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirDepthNormalBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirDepthNormalBTexture.Get(), nullptr, ReservoirUavDesc);

    ReservoirSrvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    ReservoirUavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
    RestirGIReservoirMWASrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirMWATexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirMWAUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirMWATexture.Get(), nullptr, ReservoirUavDesc);
    RestirGIReservoirMWBSrvBindlessIndex = Device->CreateBindlessSrv(RestirGIReservoirMWBTexture.Get(), ReservoirSrvDesc);
    RestirGIReservoirMWBUavBindlessIndex = Device->CreateBindlessUav(RestirGIReservoirMWBTexture.Get(), nullptr, ReservoirUavDesc);

    return true;
}

void FRestirGI::FinalizeFrame(FDeferredRenderer& Owner)
{
    if (RestirGITexture && RestirGIHistoryTexture)
    {
        std::swap(RestirGITexture, RestirGIHistoryTexture);
        std::swap(RestirGIState, RestirGIHistoryState);

        const uint32_t CurrentSrv = RestirGIBindlessIndex;
        const uint32_t CurrentUav = RestirGIUavBindlessIndex;
        RestirGIBindlessIndex = RestirGIHistorySrvBindlessIndex;
        RestirGIUavBindlessIndex = RestirGIHistoryUavBindlessIndex;
        RestirGIHistorySrvBindlessIndex = CurrentSrv;
        RestirGIHistoryUavBindlessIndex = CurrentUav;
    }

    if (RestirGIReservoirDepthNormalATexture && RestirGIReservoirDepthNormalBTexture)
    {
        std::swap(RestirGIReservoirDepthNormalATexture, RestirGIReservoirDepthNormalBTexture);
        std::swap(RestirGIReservoirDepthNormalAState, RestirGIReservoirDepthNormalBState);
        std::swap(RestirGIReservoirDepthNormalASrvBindlessIndex, RestirGIReservoirDepthNormalBSrvBindlessIndex);
        std::swap(RestirGIReservoirDepthNormalAUavBindlessIndex, RestirGIReservoirDepthNormalBUavBindlessIndex);
    }

    if (RestirGIReservoirSampleRadianceATexture && RestirGIReservoirSampleRadianceBTexture)
    {
        std::swap(RestirGIReservoirSampleRadianceATexture, RestirGIReservoirSampleRadianceBTexture);
        std::swap(RestirGIReservoirSampleRadianceAState, RestirGIReservoirSampleRadianceBState);
        std::swap(RestirGIReservoirSampleRadianceASrvBindlessIndex, RestirGIReservoirSampleRadianceBSrvBindlessIndex);
        std::swap(RestirGIReservoirSampleRadianceAUavBindlessIndex, RestirGIReservoirSampleRadianceBUavBindlessIndex);
    }

    if (RestirGIReservoirRayDirectionATexture && RestirGIReservoirRayDirectionBTexture)
    {
        std::swap(RestirGIReservoirRayDirectionATexture, RestirGIReservoirRayDirectionBTexture);
        std::swap(RestirGIReservoirRayDirectionAState, RestirGIReservoirRayDirectionBState);
        std::swap(RestirGIReservoirRayDirectionASrvBindlessIndex, RestirGIReservoirRayDirectionBSrvBindlessIndex);
        std::swap(RestirGIReservoirRayDirectionAUavBindlessIndex, RestirGIReservoirRayDirectionBUavBindlessIndex);
    }

    if (RestirGIReservoirMWATexture && RestirGIReservoirMWBTexture)
    {
        std::swap(RestirGIReservoirMWATexture, RestirGIReservoirMWBTexture);
        std::swap(RestirGIReservoirMWAState, RestirGIReservoirMWBState);
        std::swap(RestirGIReservoirMWASrvBindlessIndex, RestirGIReservoirMWBSrvBindlessIndex);
        std::swap(RestirGIReservoirMWAUavBindlessIndex, RestirGIReservoirMWBUavBindlessIndex);
    }

    if (bEnabled_ && RestirGIHistoryTexture != nullptr)
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

void FRestirGI::SetFreezeFrame(bool bEnabled, uint64_t FrameNumber)
{
    if (bEnabled && !bFreezeFrame)
    {
        FrozenSequenceFrame = 0;
        FreezeStartFrameNumber = FrameNumber;
    }

    bFreezeFrame = bEnabled;
}

bool FRestirGI::CreateRootSignature(FDX12Device* Device)
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
    RootParams[3].Constants.Num32BitValues = 30;
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

bool FRestirGI::CreatePipeline(FDX12Device* Device)
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

    std::array<std::vector<uint8_t>, 2> InitialByteCodes;
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSInitialSampling", CSTarget, InitialByteCodes[0], { L"RESTIR_GI_RANDOM_MODE_HASH=1" }))
    {
        return false;
    }
    if (!Compiler.CompileFromFile(L"Shaders/RestirGI.hlsl", L"CSInitialSampling", CSTarget, InitialByteCodes[1], { L"RESTIR_GI_RANDOM_MODE_BLUE_NOISE_SOBOL=1" }))
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
        for (Microsoft::WRL::ComPtr<ID3D12PipelineState>& Pipeline : RestirGIInitialPipelines)
        {
            Pipeline.Reset();
        }
        return false;
    }

    PsoDesc.CS = { BootstrapByteCode.data(), BootstrapByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIReservoirBootstrapPipeline, "CSReservoirBootstrap"))
    {
        for (Microsoft::WRL::ComPtr<ID3D12PipelineState>& Pipeline : RestirGIInitialPipelines)
        {
            Pipeline.Reset();
        }
        RestirGITemporalPipeline.Reset();
        return false;
    }

    PsoDesc.CS = { SpatialByteCode.data(), SpatialByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGISpatialPipeline, "CSSpatialResampling"))
    {
        for (Microsoft::WRL::ComPtr<ID3D12PipelineState>& Pipeline : RestirGIInitialPipelines)
        {
            Pipeline.Reset();
        }
        RestirGITemporalPipeline.Reset();
        RestirGIReservoirBootstrapPipeline.Reset();
        return false;
    }

    PsoDesc.CS = { ResolveByteCode.data(), ResolveByteCode.size() };
    if (!CreateComputePso(PsoDesc, RestirGIResolvePipeline, "CSResolve"))
    {
        for (Microsoft::WRL::ComPtr<ID3D12PipelineState>& Pipeline : RestirGIInitialPipelines)
        {
            Pipeline.Reset();
        }
        RestirGIReservoirBootstrapPipeline.Reset();
        RestirGITemporalPipeline.Reset();
        RestirGISpatialPipeline.Reset();
        return false;
    }

    return true;
}

DXGI_FORMAT FRestirGI::ResolveRadianceFormat(FDX12Device* Device) const
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

bool FRestirGI::CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    const DXGI_FORMAT RestirGiRadianceFormat = ResolveRadianceFormat(Device);

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

    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalA", RestirGIReservoirDepthNormalATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalB", RestirGIReservoirDepthNormalBTexture);
    CreateRestirGITexture(RestirGiRadianceFormat, L"ReSTIR_GI_ReservoirSampleRadianceA", RestirGIReservoirSampleRadianceATexture);
    CreateRestirGITexture(RestirGiRadianceFormat, L"ReSTIR_GI_ReservoirSampleRadianceB", RestirGIReservoirSampleRadianceBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_ReservoirRayDirectionA", RestirGIReservoirRayDirectionATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_ReservoirRayDirectionB", RestirGIReservoirRayDirectionBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R16G16_FLOAT, L"ReSTIR_GI_ReservoirMWA", RestirGIReservoirMWATexture);
    CreateRestirGITexture(DXGI_FORMAT_R16G16_FLOAT, L"ReSTIR_GI_ReservoirMWB", RestirGIReservoirMWBTexture);

    RestirGIState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIHistoryState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirDepthNormalAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirDepthNormalBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirSampleRadianceAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirSampleRadianceBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirRayDirectionAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirRayDirectionBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirMWAState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    RestirGIReservoirMWBState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
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

void FRestirGI::DispatchRestirPass(FDeferredPassContext& Context, FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, const wchar_t* EventName, uint32_t SpatialPassIndex, const uint32_t BindlessIndices[30], uint32_t DispatchWidth, uint32_t DispatchHeight, bool bEnabled) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FDX12Device* Device = Owner.Device;
    if (!bEnabled || !Device || !Device->GetBindlessDescriptorHeap() || !PipelineState || !RestirGIRootSignature)
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
        bEnabled_ ? 1u : 0u,
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

    FScopedPixEvent RestirEvent(CommandList4, EventName);
    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
    CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList4->SetComputeRootSignature(RestirGIRootSignature.Get());
    CommandList4->SetPipelineState(PipelineState);
    CommandList4->SetComputeRootShaderResourceView(0, TlasResource->GetGPUVirtualAddress());
    CommandList4->SetComputeRootConstantBufferView(1, Owner.GetSceneConstantBufferAddress());
    CommandList4->SetComputeRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &Constants, 0);
    CommandList4->SetComputeRoot32BitConstants(3, 30, BindlessIndices, 0);

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
        Data.bEnabled = bEnabled_ && RestirGIRootSignature && InitialPipeline;
        if (!Data.bEnabled) { return; }
        const uint32_t HalfWidth = (static_cast<uint32_t>(Owner.Viewport.Width) + 1u) / 2u;
        const uint32_t HalfHeight = (static_cast<uint32_t>(Owner.Viewport.Height) + 1u) / 2u;
        const DXGI_FORMAT RadianceFormat = RestirGITexture ? RestirGITexture->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
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
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const uint32_t InitialRadianceUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGIInitialRadianceHandle);
        const uint32_t InitialRayDirectionUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGIInitialRayDirectionHandle);
        const uint32_t HalfDepthNormalSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIHalfDepthNormalHandle);
        const bool bInputsValid =
            (DepthBindlessIndex != UINT32_MAX) &&
            (Owner.VelocityBindlessIndex != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[0] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[1] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[2] != UINT32_MAX) &&
            (InstanceDataBindlessIndex != UINT32_MAX) &&
            (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) &&
            (LinearClampSamplerIndex != UINT32_MAX) &&
            (InitialRadianceUavBindlessIndex != UINT32_MAX) &&
            (InitialRayDirectionUavBindlessIndex != UINT32_MAX) &&
            (RestirGIHistorySrvBindlessIndex != UINT32_MAX) &&
            ((Owner.RestirGIDenoiser != nullptr) && (Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex() != UINT32_MAX)) &&
            (HalfDepthNormalSrvBindlessIndex != UINT32_MAX) &&
            (!bUseBlueNoiseSobol || (Owner.BlueNoiseSobolSrvBindlessIndex != UINT32_MAX)) &&
            (!bUseBlueNoiseSobol || (Owner.BlueNoiseScramblingRanking1SPPSrvBindlessIndex != UINT32_MAX));
        const uint32_t BindlessIndices[30] =
        {
            InitialRadianceUavBindlessIndex,
            DepthBindlessIndex,
            Owner.VelocityBindlessIndex,
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
            RestirGIHistorySrvBindlessIndex,
            Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex(),
            Owner.GpuDebugLineBufferUavBindlessIndex,
            Owner.BlueNoiseSobolSrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPSrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, InitialPipeline, L"InitialSampling", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
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
        Data.bEnabled = bEnabled_ && RestirGIRootSignature && RestirGITemporalPipeline;
        if (!Data.bEnabled) { return; }
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
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const uint32_t InitialRadianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRadianceHandle);
        const uint32_t InitialRayDirectionSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRayDirectionHandle);
        const uint32_t HalfDepthNormalSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIHalfDepthNormalHandle);
        const bool bInputsValid =
            (DepthBindlessIndex != UINT32_MAX) &&
            (Owner.VelocityBindlessIndex != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[0] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[1] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[2] != UINT32_MAX) &&
            (InstanceDataBindlessIndex != UINT32_MAX) &&
            (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) &&
            (LinearClampSamplerIndex != UINT32_MAX) &&
            (InitialRadianceSrvBindlessIndex != UINT32_MAX) &&
            (InitialRayDirectionSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX) &&
            ((Owner.RestirGIDenoiser != nullptr) && (Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex() != UINT32_MAX)) &&
            (HalfDepthNormalSrvBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityBindlessIndex,
            Owner.GBufferBindlessIndices[0],
            Owner.GBufferBindlessIndices[1],
            Owner.GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            Owner.EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            InitialRadianceSrvBindlessIndex,
            InitialRayDirectionSrvBindlessIndex,
            RestirGIReservoirDepthNormalASrvBindlessIndex,
            RestirGIReservoirSampleRadianceASrvBindlessIndex,
            RestirGIReservoirRayDirectionASrvBindlessIndex,
            RestirGIReservoirMWASrvBindlessIndex,
            RestirGIReservoirDepthNormalBUavBindlessIndex,
            RestirGIReservoirSampleRadianceBUavBindlessIndex,
            RestirGIReservoirRayDirectionBUavBindlessIndex,
            RestirGIReservoirMWBUavBindlessIndex,
            HalfDepthNormalSrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistorySrvBindlessIndex,
            Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex(),
            Owner.GpuDebugLineBufferUavBindlessIndex,
            Owner.BlueNoiseSobolSrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPSrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGITemporalPipeline.Get(), L"TemporalResampling", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
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
        Data.bEnabled = bEnabled_ && RestirGIRootSignature && RestirGIReservoirBootstrapPipeline;
        if (!Data.bEnabled) { return; }
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
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const uint32_t InitialRadianceSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRadianceHandle);
        const uint32_t InitialRayDirectionSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIInitialRayDirectionHandle);
        const uint32_t HalfDepthNormalSrvBindlessIndex = Graph.GetTextureSrvBindlessIndex(RestirGIHalfDepthNormalHandle);
        const bool bInputsValid =
            (DepthBindlessIndex != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[0] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[1] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[2] != UINT32_MAX) &&
            (InstanceDataBindlessIndex != UINT32_MAX) &&
            (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) &&
            (LinearClampSamplerIndex != UINT32_MAX) &&
            (InitialRadianceSrvBindlessIndex != UINT32_MAX) &&
            (InitialRayDirectionSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX) &&
            (HalfDepthNormalSrvBindlessIndex != UINT32_MAX);
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityBindlessIndex,
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
            RestirGIReservoirDepthNormalBUavBindlessIndex,
            RestirGIReservoirSampleRadianceBUavBindlessIndex,
            RestirGIReservoirRayDirectionBUavBindlessIndex,
            RestirGIReservoirMWBUavBindlessIndex,
            HalfDepthNormalSrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistorySrvBindlessIndex,
            Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex(),
            Owner.GpuDebugLineBufferUavBindlessIndex,
            Owner.BlueNoiseSobolSrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPSrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGIReservoirBootstrapPipeline.Get(), L"ReservoirBootstrap", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
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
        Data.bEnabled = bEnabled_ && RestirGIRootSignature && RestirGISpatialPipeline;
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
    }, [&, PrevLinearDepthHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](const FRestirGiSplitPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthBindlessIndex = GetDepthBindlessIndexForRestir(Owner);
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid =
            (DepthBindlessIndex != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[0] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[1] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[2] != UINT32_MAX) &&
            (InstanceDataBindlessIndex != UINT32_MAX) &&
            (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) &&
            (LinearClampSamplerIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalBSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceBSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionBSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWBSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalAUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceAUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionAUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWAUavBindlessIndex != UINT32_MAX) &&
            ((Owner.RestirGIDenoiser != nullptr) && (Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex() != UINT32_MAX));
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityBindlessIndex,
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
            RestirGIReservoirDepthNormalAUavBindlessIndex,
            RestirGIReservoirSampleRadianceAUavBindlessIndex,
            RestirGIReservoirRayDirectionAUavBindlessIndex,
            RestirGIReservoirMWAUavBindlessIndex,
            RestirGIReservoirDepthNormalBSrvBindlessIndex,
            RestirGIReservoirSampleRadianceBSrvBindlessIndex,
            RestirGIReservoirRayDirectionBSrvBindlessIndex,
            RestirGIReservoirMWBSrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistorySrvBindlessIndex,
            Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex(),
            Owner.GpuDebugLineBufferUavBindlessIndex,
            Owner.BlueNoiseSobolSrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPSrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGISpatialPipeline.Get(), L"SpatialResampling0", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
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
        Data.bEnabled = bEnabled_ && RestirGIRootSignature && RestirGISpatialPipeline;
        if (!Data.bEnabled) { return; }
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
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid =
            (DepthBindlessIndex != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[0] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[1] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[2] != UINT32_MAX) &&
            (InstanceDataBindlessIndex != UINT32_MAX) &&
            (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) &&
            (LinearClampSamplerIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWASrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX) &&
            ((Owner.RestirGIDenoiser != nullptr) && (Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex() != UINT32_MAX));
        const uint32_t BindlessIndices[30] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            Owner.VelocityBindlessIndex,
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
            RestirGIReservoirDepthNormalBUavBindlessIndex,
            RestirGIReservoirSampleRadianceBUavBindlessIndex,
            RestirGIReservoirRayDirectionBUavBindlessIndex,
            RestirGIReservoirMWBUavBindlessIndex,
            RestirGIReservoirDepthNormalASrvBindlessIndex,
            RestirGIReservoirSampleRadianceASrvBindlessIndex,
            RestirGIReservoirRayDirectionASrvBindlessIndex,
            RestirGIReservoirMWASrvBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistorySrvBindlessIndex,
            Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex(),
            Owner.GpuDebugLineBufferUavBindlessIndex,
            Owner.BlueNoiseSobolSrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPSrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGISpatialPipeline.Get(), L"SpatialResampling1", 1u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
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
        Data.bEnabled = bEnabled_ && RestirGIRootSignature && RestirGIResolvePipeline;
        if (!Data.bEnabled) { return; }
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
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < Owner.PathTracingInstanceDataBindlessIndices.size()) ? Owner.PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Owner.Device ? Owner.Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const uint32_t InputSHUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGiInputSHHandle);
        const uint32_t VarianceUavBindlessIndex = Graph.GetTextureUavBindlessIndex(Context.Resources.RestirGI.RestirGiVarianceHandle);
        const bool bInputsValid =
            (DepthBindlessIndex != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[0] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[1] != UINT32_MAX) &&
            (Owner.GBufferBindlessIndices[2] != UINT32_MAX) &&
            (InstanceDataBindlessIndex != UINT32_MAX) &&
            (Owner.EnvironmentCubeBindlessIndex != UINT32_MAX) &&
            (LinearClampSamplerIndex != UINT32_MAX) &&
            (RestirGIUavBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirDepthNormalBSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirSampleRadianceBSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirRayDirectionBSrvBindlessIndex != UINT32_MAX) &&
            (RestirGIReservoirMWBSrvBindlessIndex != UINT32_MAX) &&
            (InputSHUavBindlessIndex != UINT32_MAX) &&
            (VarianceUavBindlessIndex != UINT32_MAX) &&
            ((Owner.RestirGIDenoiser != nullptr) && (Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex() != UINT32_MAX));
        const uint32_t BindlessIndices[30] =
        {
            RestirGIUavBindlessIndex,
            DepthBindlessIndex,
            Owner.VelocityBindlessIndex,
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
            RestirGIReservoirDepthNormalBSrvBindlessIndex,
            RestirGIReservoirSampleRadianceBSrvBindlessIndex,
            RestirGIReservoirRayDirectionBSrvBindlessIndex,
            RestirGIReservoirMWBSrvBindlessIndex,
            InputSHUavBindlessIndex,
            VarianceUavBindlessIndex,
            RestirGIHistorySrvBindlessIndex,
            Owner.RestirGIDenoiser->GetPrevLinearDepthSrvBindlessIndex(),
            Owner.GpuDebugLineBufferUavBindlessIndex,
            Owner.BlueNoiseSobolSrvBindlessIndex,
            Owner.BlueNoiseScramblingRanking1SPPSrvBindlessIndex
        };
        const uint32_t FullWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        DispatchRestirPass(Context, Cmd, RestirGIResolvePipeline.Get(), L"Resolve", 0u, BindlessIndices, FullWidth, FullHeight, Data.bEnabled && bInputsValid);
    });
}
