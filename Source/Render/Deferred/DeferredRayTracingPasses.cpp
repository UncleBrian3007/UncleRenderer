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

bool FDeferredRayTracingPasses::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device) const
{
    if (!Device->IsRayTracingSupported())
    {
        if (Owner.bRestirGIEnabled)
        {
            LogWarning("Deferred renderer: ReSTIR GI disabled because DXR is not supported on this device.");
        }
        Owner.bRestirGIEnabled = false;
    }
    else
    {
        if (!Owner.CreateRestirGIRootSignature(Device) || !Owner.CreateRestirGIPipeline(Device))
        {
            LogWarning("Deferred renderer: ReSTIR GI pipeline creation failed.");
            Owner.RestirGIRootSignature.Reset();
            Owner.RestirGIInitialPipeline.Reset();
            Owner.RestirGIReservoirBootstrapPipeline.Reset();
            Owner.RestirGITemporalPipeline.Reset();
            Owner.RestirGISpatialPipeline.Reset();
            Owner.RestirGIResolvePipeline.Reset();
        }

        if (!Owner.CreateRestirGiDenoiserPipelines(Device))
        {
            LogWarning("Deferred renderer: ReSTIR GI denoiser pipeline creation failed (passes will be skipped).");
        }
    }

    return Owner.CreatePathTracingAccumulationRootSignature(Device) && Owner.CreatePathTracingAccumulationPipeline(Device);
}

bool FDeferredRayTracingPasses::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount) const
{
    return Owner.CreateRestirGIResources(Device, Width, Height)
        && Owner.CreateRestirGiDenoiserResources(Device, Width, Height)
        && Owner.CreatePathTracingAccumulationResources(Device, Width, Height, FrameCount);
}

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

bool FDeferredRenderer::CreatePathTracingAccumulationRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    // RootParams[0]: PathTracing accumulation constants (output size, accumulation weight, history toggle)
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

    // RootParams[1]: PathTracing accumulation bindless indices (b1) - 4 indices now
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[1].Constants.Num32BitValues = 4;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.ShaderRegister = 1;

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

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(PathTracingAccumulationRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreatePathTracingAccumulationPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    std::vector<uint8_t> CSByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/PathTracingAccumulation.hlsl", L"CSMain", CSTarget, CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = PathTracingAccumulationRootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(PathTracingAccumulationPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreatePathTracingAccumulationResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t EffectiveFrameCount = (std::max)(1u, FrameCount);

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = Width;
    Desc.Height = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = FDeferredRenderer::PathTracingBufferFormat;
    Desc.SampleDesc.Count = 1;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Create temporary texture for path tracing output
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(PathTracingTempTexture.GetAddressOf())));

    if (PathTracingTempTexture)
    {
        PathTracingTempTexture->SetName(L"PathTracingTemp");
    }

    // Create accumulation history textures
    PathTracingAccumulationTextures.clear();
    PathTracingAccumulationTextures.resize(EffectiveFrameCount);
    for (uint32_t Index = 0; Index < EffectiveFrameCount; ++Index)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(PathTracingAccumulationTextures[Index].GetAddressOf())));

        if (PathTracingAccumulationTextures[Index])
        {
            const std::wstring ResourceName = L"PathTracingAccumulation_" + std::to_wstring(Index);
            PathTracingAccumulationTextures[Index]->SetName(ResourceName.c_str());
        }
    }

    PathTracingAccumulationFrameCount = EffectiveFrameCount;
    PathTracingAccumulationStates.assign(EffectiveFrameCount, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    PathTracingAccumulationHistoryValid.assign(EffectiveFrameCount, false);
    PathTracingTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

bool FDeferredRenderer::CreateRestirGIResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);

    CD3DX12_RESOURCE_DESC Desc = CD3DX12_RESOURCE_DESC::Tex2D(
        DXGI_FORMAT_R16G16B16A16_FLOAT,
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

    CreateRestirGITexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"ReSTIR_GI_InitialRadiance", RestirGIInitialRadianceTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32_UINT, L"ReSTIR_GI_InitialRayDirection", RestirGIInitialRayDirectionTexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalA", RestirGIReservoirDepthNormalATexture);
    CreateRestirGITexture(DXGI_FORMAT_R32G32_UINT, L"ReSTIR_GI_ReservoirDepthNormalB", RestirGIReservoirDepthNormalBTexture);
    CreateRestirGITexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"ReSTIR_GI_ReservoirSampleRadianceA", RestirGIReservoirSampleRadianceATexture);
    CreateRestirGITexture(DXGI_FORMAT_R16G16B16A16_FLOAT, L"ReSTIR_GI_ReservoirSampleRadianceB", RestirGIReservoirSampleRadianceBTexture);
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

bool FDeferredRenderer::CreateRestirGiDenoiserResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    if (Device == nullptr)
    {
        return false;
    }

    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_DEFAULT);
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
    CreateTexture(DXGI_FORMAT_R11G11B10_FLOAT, Width, Height, 1, L"ReSTIR_GI_HistoryIrradiance", RestirGiHistoryIrradianceTexture);
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

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
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

void FDeferredRayTracingPasses::AddRayTracingShadowPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FCamera& Camera = Context.Camera;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GBufferHandle = Context.Resources.GBufferHandles[0];
    FRGResourceHandle& ShadowMaskHandle = Context.Resources.ShadowMaskHandle;

    struct FRayTracingShadowPassData
    {
        FRGResourceHandle ShadowMaskHandle{};
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle GBufferHandle{};
        const FCamera* Camera = nullptr;
    };

    const FRGTextureDesc ShadowMaskDesc =
    {
        static_cast<uint32>(Owner.Viewport.Width),
        static_cast<uint32>(Owner.Viewport.Height),
        DXGI_FORMAT_R8_UNORM
    };

    Graph.AddPass<FRayTracingShadowPassData>("RTShadowMask", [&, ShadowMaskDesc, DepthHandle, GBufferHandle](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        if (!Owner.bRayTracedShadowsEnabled || !Owner.bRayTracingPipelineReady || !GBufferHandle)
        {
            return;
        }

        Data.ShadowMaskHandle = Builder.CreateTexture("ShadowMask", ShadowMaskDesc);
        Data.DepthHandle = DepthHandle;
        Data.GBufferHandle = GBufferHandle;
        Data.Camera = &Camera;
        Builder.WriteTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadTexture(Data.DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.KeepAlive();
        ShadowMaskHandle = Data.ShadowMaskHandle;
    }, [&Owner, &Graph](const FRayTracingShadowPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Owner.bRayTracingPipelineReady || !Owner.RayQueryShadowPipeline || !Owner.RayQueryRootSignature || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (Owner.SceneModels.empty() || Data.Camera == nullptr)
        {
            return;
        }

        ID3D12Resource* ShadowMask = Graph.GetTextureResource(Data.ShadowMaskHandle);
        if (!ShadowMask)
        {
            return;
        }

        ID3D12Resource* DepthBuffer = Graph.GetTextureResource(Data.DepthHandle);
        if (!DepthBuffer)
        {
            return;
        }

        ID3D12Resource* GBufferA = Graph.GetTextureResource(Data.GBufferHandle);
        if (!GBufferA)
        {
            return;
        }

        const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
        if (FrameIndex >= Owner.TlasResultBuffers.size() || !Owner.TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FScopedPixEvent RayTracingEvent(CommandList4, L"RT Shadow Mask Pass");

        if (FrameIndex >= Owner.RayTracingDepthSrvBindlessIndices.size())
        {
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC DepthSrvDesc = {};
        DepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        DepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DepthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        DepthSrvDesc.Texture2D.MipLevels = 1;
        DepthSrvDesc.Texture2D.MostDetailedMip = 0;
        DepthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        const uint32_t DepthBindlessIndex = Owner.RayTracingDepthSrvBindlessIndices[FrameIndex];
        if (DepthBindlessIndex == UINT32_MAX)
        {
            return;
        }
        if (FrameIndex < Owner.RayTracingDepthResources.size() && Owner.RayTracingDepthResources[FrameIndex] != DepthBuffer)
        {
            Owner.WriteBindlessSrv(DepthBindlessIndex, DepthBuffer, DepthSrvDesc);
            Owner.RayTracingDepthResources[FrameIndex] = DepthBuffer;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferSrvDesc = {};
        GBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferSrvDesc.Format = GBufferA->GetDesc().Format;
        GBufferSrvDesc.Texture2D.MipLevels = 1;
        GBufferSrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (Owner.RayTracingGBufferASrvBindlessIndex == UINT32_MAX)
        {
            Owner.RayTracingGBufferASrvBindlessIndex = Owner.Device->CreateBindlessSrv(GBufferA, GBufferSrvDesc);
        }
        else if (Owner.RayTracingGBufferAResource != GBufferA)
        {
            Owner.WriteBindlessSrv(Owner.RayTracingGBufferASrvBindlessIndex, GBufferA, GBufferSrvDesc);
        }
        Owner.RayTracingGBufferAResource = GBufferA;

        D3D12_UNORDERED_ACCESS_VIEW_DESC ShadowMaskUavDesc = {};
        ShadowMaskUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ShadowMaskUavDesc.Format = DXGI_FORMAT_R8_UNORM;
        ShadowMaskUavDesc.Texture2D.MipSlice = 0;
        if (Owner.RayTracingShadowMaskUavBindlessIndex == UINT32_MAX)
        {
            Owner.RayTracingShadowMaskUavBindlessIndex = Owner.Device->CreateBindlessUav(ShadowMask, nullptr, ShadowMaskUavDesc);
        }
        else if (Owner.RayTracingShadowMaskUavResource != ShadowMask)
        {
            Owner.WriteBindlessUav(Owner.RayTracingShadowMaskUavBindlessIndex, ShadowMask, nullptr, ShadowMaskUavDesc);
        }
        Owner.RayTracingShadowMaskUavResource = ShadowMask;

        if (Owner.ShadowMaskBindlessIndex == UINT32_MAX || Owner.ShadowMaskResource != ShadowMask)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
            SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SrvDesc.Format = DXGI_FORMAT_R8_UNORM;
            SrvDesc.Texture2D.MipLevels = 1;
            if (Owner.ShadowMaskBindlessIndex == UINT32_MAX)
            {
                Owner.ShadowMaskBindlessIndex = Owner.Device->CreateBindlessSrv(ShadowMask, SrvDesc);
            }
            else
            {
                Owner.WriteBindlessSrv(Owner.ShadowMaskBindlessIndex, ShadowMask, SrvDesc);
            }
            Owner.ShadowMaskResource = ShadowMask;
        }

        if (Owner.RayTracingGBufferASrvBindlessIndex == UINT32_MAX || Owner.RayTracingShadowMaskUavBindlessIndex == UINT32_MAX || Owner.ShadowMaskBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        FScopedPixEvent SsrHwTraceEvent(CommandList4, L"SSR HW Trace");
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t DispatchWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t DispatchHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        if (DispatchWidth == 0 || DispatchHeight == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 8;
        const uint32_t GroupCountX = (DispatchWidth + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;
        const uint32_t GroupCountY = (DispatchHeight + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        CommandList4->SetPipelineState(Owner.RayQueryShadowPipeline.Get());
        CommandList4->SetComputeRootSignature(Owner.RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, Owner.TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (FrameIndex >= Owner.PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = Owner.PathTracingInstanceDataBindlessIndices[FrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t BindlessIndices[] =
        {
            DepthBindlessIndex,
            Owner.RayTracingGBufferASrvBindlessIndex,
            Owner.RayTracingShadowMaskUavBindlessIndex,
            0u,
            DispatchWidth,
            DispatchHeight
        };
        CommandList4->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        CommandList4->Dispatch(GroupCountX, GroupCountY, 1);
    });

    Graph.AddPass<FRayTracingShadowPassData>("ShadowMaskSRV", [&, ShadowMaskDesc](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        Data.ShadowMaskHandle = ShadowMaskHandle;
        if (Data.ShadowMaskHandle)
        {
            Builder.ReadTexture(Data.ShadowMaskHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Builder.KeepAlive();
        }
    }, [](const FRayTracingShadowPassData&, FDX12CommandContext&)
    {
    });
}

void FDeferredRayTracingPasses::AddPathTracingPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FCamera& Camera = Context.Camera;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GBufferAHandle = Context.Resources.GBufferHandles[0];
    const FRGResourceHandle GBufferBHandle = Context.Resources.GBufferHandles[1];
    const FRGResourceHandle GBufferCHandle = Context.Resources.GBufferHandles[2];
    const FRGResourceHandle OutputHandle = Context.Resources.PathTracingTempHandle;

    struct FPathTracingPassData
    {
        FRGResourceHandle OutputHandle{};
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle GBufferAHandle{};
        FRGResourceHandle GBufferBHandle{};
        FRGResourceHandle GBufferCHandle{};
        const FCamera* Camera = nullptr;
        uint32_t FrameIndex = 0;
    };

    Graph.AddPass<FPathTracingPassData>("PathTracing", [&, DepthHandle, GBufferAHandle, GBufferBHandle, GBufferCHandle, OutputHandle](FPathTracingPassData& Data, FRGPassBuilder& Builder)
    {
        if (!Owner.bPathTracingEnabled || !Owner.bRayTracingPipelineReady || !DepthHandle || !GBufferAHandle || !GBufferBHandle || !GBufferCHandle || !OutputHandle)
        {
            return;
        }

        Data.OutputHandle = OutputHandle;
        Data.DepthHandle = DepthHandle;
        Data.GBufferAHandle = GBufferAHandle;
        Data.GBufferBHandle = GBufferBHandle;
        Data.GBufferCHandle = GBufferCHandle;
        Data.Camera = &Camera;
        Data.FrameIndex = Owner.PathTracingAccumulatedFrames;
        Builder.WriteTexture(Data.OutputHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadTexture(Data.DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferCHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.KeepAlive();
    }, [&Owner, &Graph](const FPathTracingPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!Owner.bRayTracingPipelineReady || !Owner.RayQueryRootSignature || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (Owner.SceneModels.empty() || Data.Camera == nullptr)
        {
            return;
        }

        ID3D12Resource* OutputTarget = Graph.GetTextureResource(Data.OutputHandle);
        if (!OutputTarget)
        {
            return;
        }

        ID3D12Resource* DepthBuffer = Graph.GetTextureResource(Data.DepthHandle);
        if (!DepthBuffer)
        {
            return;
        }

        ID3D12Resource* GBufferA = Graph.GetTextureResource(Data.GBufferAHandle);
        ID3D12Resource* GBufferB = Graph.GetTextureResource(Data.GBufferBHandle);
        ID3D12Resource* GBufferC = Graph.GetTextureResource(Data.GBufferCHandle);
        if (!GBufferA || !GBufferB || !GBufferC)
        {
            return;
        }

        const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
        if (FrameIndex >= Owner.TlasResultBuffers.size() || !Owner.TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FScopedPixEvent PathTracingEvent(CommandList4, L"Path Tracing Pass");

        if (FrameIndex >= Owner.RayTracingDepthSrvBindlessIndices.size())
        {
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC DepthSrvDesc = {};
        DepthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        DepthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        DepthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        DepthSrvDesc.Texture2D.MipLevels = 1;
        DepthSrvDesc.Texture2D.MostDetailedMip = 0;
        DepthSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        const uint32_t DepthBindlessIndex = Owner.RayTracingDepthSrvBindlessIndices[FrameIndex];
        if (DepthBindlessIndex == UINT32_MAX)
        {
            return;
        }
        if (FrameIndex < Owner.RayTracingDepthResources.size() && Owner.RayTracingDepthResources[FrameIndex] != DepthBuffer)
        {
            Owner.WriteBindlessSrv(DepthBindlessIndex, DepthBuffer, DepthSrvDesc);
            Owner.RayTracingDepthResources[FrameIndex] = DepthBuffer;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferASrvDesc = {};
        GBufferASrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferASrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferASrvDesc.Format = GBufferA->GetDesc().Format;
        GBufferASrvDesc.Texture2D.MipLevels = 1;
        GBufferASrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferASrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (Owner.RayTracingGBufferASrvBindlessIndex == UINT32_MAX)
        {
            Owner.RayTracingGBufferASrvBindlessIndex = Owner.Device->CreateBindlessSrv(GBufferA, GBufferASrvDesc);
        }
        else if (Owner.RayTracingGBufferAResource != GBufferA)
        {
            Owner.WriteBindlessSrv(Owner.RayTracingGBufferASrvBindlessIndex, GBufferA, GBufferASrvDesc);
        }
        Owner.RayTracingGBufferAResource = GBufferA;

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferBSrvDesc = {};
        GBufferBSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferBSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferBSrvDesc.Format = GBufferB->GetDesc().Format;
        GBufferBSrvDesc.Texture2D.MipLevels = 1;
        GBufferBSrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferBSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (Owner.RayTracingGBufferBSrvBindlessIndex == UINT32_MAX)
        {
            Owner.RayTracingGBufferBSrvBindlessIndex = Owner.Device->CreateBindlessSrv(GBufferB, GBufferBSrvDesc);
        }
        else if (Owner.RayTracingGBufferBResource != GBufferB)
        {
            Owner.WriteBindlessSrv(Owner.RayTracingGBufferBSrvBindlessIndex, GBufferB, GBufferBSrvDesc);
        }
        Owner.RayTracingGBufferBResource = GBufferB;

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferCSrvDesc = {};
        GBufferCSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferCSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferCSrvDesc.Format = GBufferC->GetDesc().Format;
        GBufferCSrvDesc.Texture2D.MipLevels = 1;
        GBufferCSrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferCSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (Owner.RayTracingGBufferCSrvBindlessIndex == UINT32_MAX)
        {
            Owner.RayTracingGBufferCSrvBindlessIndex = Owner.Device->CreateBindlessSrv(GBufferC, GBufferCSrvDesc);
        }
        else if (Owner.RayTracingGBufferCResource != GBufferC)
        {
            Owner.WriteBindlessSrv(Owner.RayTracingGBufferCSrvBindlessIndex, GBufferC, GBufferCSrvDesc);
        }
        Owner.RayTracingGBufferCResource = GBufferC;

        D3D12_UNORDERED_ACCESS_VIEW_DESC OutputUavDesc = {};
        OutputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        OutputUavDesc.Format = OutputTarget->GetDesc().Format;
        OutputUavDesc.Texture2D.MipSlice = 0;
        if (Owner.RayTracingLightingUavBindlessIndex == UINT32_MAX)
        {
            Owner.RayTracingLightingUavBindlessIndex = Owner.Device->CreateBindlessUav(OutputTarget, nullptr, OutputUavDesc);
        }
        else if (Owner.RayTracingLightingResource != OutputTarget)
        {
            Owner.WriteBindlessUav(Owner.RayTracingLightingUavBindlessIndex, OutputTarget, nullptr, OutputUavDesc);
        }
        Owner.RayTracingLightingResource = OutputTarget;

        if (Owner.RayTracingGBufferASrvBindlessIndex == UINT32_MAX
            || Owner.RayTracingGBufferBSrvBindlessIndex == UINT32_MAX
            || Owner.RayTracingGBufferCSrvBindlessIndex == UINT32_MAX
            || Owner.RayTracingLightingUavBindlessIndex == UINT32_MAX
            || Owner.EnvironmentCubeBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t DispatchWidth = static_cast<uint32_t>(Owner.Viewport.Width);
        const uint32_t DispatchHeight = static_cast<uint32_t>(Owner.Viewport.Height);
        if (DispatchWidth == 0 || DispatchHeight == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 8;
        const uint32_t GroupCountX = (DispatchWidth + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;
        const uint32_t GroupCountY = (DispatchHeight + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        ID3D12PipelineState* PathTracingPipeline = nullptr;
        if (Owner.PathTracingDebugMode > 0)
        {
            PathTracingPipeline = Owner.bPathTracingUseVndf ? Owner.RayQueryPathDebugVndfPipeline.Get() : Owner.RayQueryPathDebugPipeline.Get();
        }
        else
        {
            PathTracingPipeline = Owner.bPathTracingUseVndf ? Owner.RayQueryPathVndfPipeline.Get() : Owner.RayQueryPathPipeline.Get();
        }
        if (!PathTracingPipeline)
        {
            return;
        }
        CommandList4->SetPipelineState(PathTracingPipeline);
        CommandList4->SetComputeRootSignature(Owner.RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, Owner.TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (FrameIndex >= Owner.PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = Owner.PathTracingInstanceDataBindlessIndices[FrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t BindlessIndices[] =
        {
            DepthBindlessIndex,
            Owner.RayTracingGBufferASrvBindlessIndex,
            Owner.RayTracingGBufferBSrvBindlessIndex,
            Owner.RayTracingGBufferCSrvBindlessIndex,
            Owner.RayTracingLightingUavBindlessIndex,
            DispatchWidth,
            DispatchHeight,
            Data.FrameIndex,
            PathTracingInstanceDataBindlessIndex,
            Owner.PathTracingMaxBounces,
            Owner.Device->GetLinearClampSamplerIndex(),
            Owner.EnvironmentCubeBindlessIndex,
            static_cast<uint32_t>(Owner.PathTracingDebugMode)
        };
        CommandList4->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        CommandList4->Dispatch(GroupCountX, GroupCountY, 1);
    });
}

void FDeferredRayTracingPasses::AddPathTracingAccumulationPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FRGResourceHandle PathTracingTempHandle = Context.Resources.PathTracingTempHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;
    const std::vector<FRGResourceHandle>& AccumulationHandles = Context.Resources.PathTracingAccumulationHandles;

    struct FPathTracingAccumulationPassData
    {
        bool bEnabled = false;
        DirectX::XMFLOAT2 OutputSize{};
        uint32_t FrameIndex = 0;
        uint32_t UseHistory = 0;
        uint32_t ReadIndex = 0;
        uint32_t WriteIndex = 0;
    };

    Graph.AddPass<FPathTracingAccumulationPassData>("PTAccumulation", [&](FPathTracingAccumulationPassData& Data, FRGPassBuilder& Builder)
    {
        // Always enable if we have PathTracingTemp and LightingHandle, even if accumulation is disabled
        // When disabled, we'll just copy temp to lighting without accumulation
        Data.bEnabled = PathTracingTempHandle && LightingHandle;
        if (Data.bEnabled)
        {
            Data.ReadIndex = FrameState.PathTracingAccumulationReadIndex;
            Data.WriteIndex = FrameState.PathTracingAccumulationWriteIndex;
            Data.OutputSize = DirectX::XMFLOAT2(Owner.Viewport.Width, Owner.Viewport.Height);
            Data.FrameIndex = Owner.PathTracingAccumulatedFrames;
            Data.UseHistory = (FrameState.bPathTracingAccumulationActive && FrameState.bPathTracingAccumulationHistoryReady) ? 1u : 0u;
            Builder.ReadTexture(PathTracingTempHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (FrameState.bPathTracingAccumulationActive && !AccumulationHandles.empty())
            {
                Builder.ReadTexture(AccumulationHandles[Data.ReadIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Builder.WriteTexture(AccumulationHandles[Data.WriteIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [&Owner, &FrameState, &AccumulationHandles](const FPathTracingAccumulationPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

        FScopedPixEvent AccumulationEvent(LocalCommandList, L"PathTracingAccumulation");

        struct FPathTracingAccumulationConstants
        {
            uint32_t OutputWidth;
            uint32_t OutputHeight;
            uint32_t FrameIndex;
            uint32_t UseHistory;
        };

        const FPathTracingAccumulationConstants Constants =
        {
            static_cast<uint32_t>(Data.OutputSize.x),
            static_cast<uint32_t>(Data.OutputSize.y),
            Data.FrameIndex,
            Data.UseHistory
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetPipelineState(Owner.PathTracingAccumulationPipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.PathTracingAccumulationRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);

        // When accumulation is disabled, use index 0 for both read/write (doesn't matter since UseHistory=0)
        const bool bAccumulationActive = FrameState.bPathTracingAccumulationActive && !AccumulationHandles.empty();
        const uint32_t ReadIdx = bAccumulationActive ? Data.ReadIndex : 0;
        const uint32_t WriteIdx = bAccumulationActive ? Data.WriteIndex : 0;
        const uint32_t HistorySrv = bAccumulationActive && ReadIdx < Owner.PathTracingAccumulationSrvBindlessIndices.size()
            ? Owner.PathTracingAccumulationSrvBindlessIndices[ReadIdx]
            : Owner.PathTracingTempBindlessIndex; // Use temp as dummy when disabled
        const uint32_t HistoryUav = bAccumulationActive && WriteIdx < Owner.PathTracingAccumulationUavBindlessIndices.size()
            ? Owner.PathTracingAccumulationUavBindlessIndices[WriteIdx]
            : Owner.PathTracingTempBindlessIndex; // Use temp as dummy when disabled

        const uint32_t AccumBindlessIndices[] =
        {
            Owner.PathTracingTempBindlessIndex,
            HistorySrv,
            HistoryUav,
            Owner.LightingBufferBindlessIndex
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(AccumBindlessIndices), AccumBindlessIndices, 0);

        const uint32_t GroupX = (static_cast<uint32_t>(Data.OutputSize.x) + 7u) / 8u;
        const uint32_t GroupY = (static_cast<uint32_t>(Data.OutputSize.y) + 7u) / 8u;
        LocalCommandList->Dispatch(GroupX, GroupY, 1);

        // Increment accumulated frame count after dispatch (only when accumulation is active)
        if (bAccumulationActive)
        {
            Owner.PathTracingAccumulatedFrames++;
        }
    });
}

void FDeferredRayTracingPasses::AddRestirGIPass(FDeferredPassContext& Context) const
{
    AddRestirGIPassImpl(Context);
}

void FDeferredRayTracingPasses::AddRestirGiDenoiserPasses(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle InputSHHandle = Context.Resources.RestirGiInputSHHandle;
    const FRGResourceHandle VarianceHandle = Context.Resources.RestirGiVarianceHandle;
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

    AddRestirGiDenoiserPreTemporalPass(Owner, Graph, FrameState, GBufferHandles, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistorySHHandle, HistoryIrradianceHandle, HistoryCountAHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle);

    AddRestirGiShMipGenPass(Owner, Graph, TemporalSHHandle, ShMipHandle, Context.Resources.RestirGiSpdAtomicCounterHandle);
    AddRestirGiLinearDepthMipGenPass(Owner, Graph, LinearDepthHandle, LinearDepthMipHandle, Context.Resources.RestirGiSpdAtomicCounterHandle);

    AddRestirGiHistoryReconstructionPass(Owner, Graph, GBufferHandles, LinearDepthHandle, HistorySHHandle, HistoryCountBHandle, TemporalSHHandle, ShMipHandle, LinearDepthMipHandle);

    AddRestirGiFinalBlurPass(Owner, Graph, GBufferHandles, LinearDepthHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountBHandle);
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
        LocalCommandList->SetPipelineState(Owner.RestirGiFinalBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(Bindless), Bindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}

void FDeferredRayTracingPasses::AddRestirGIPassImpl(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const std::array<FRGResourceHandle, 4>& GBufferHandles = Context.Resources.GBufferHandles;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle VelocityHandle = Context.Resources.VelocityHandle;
    const FRGResourceHandle LinearDepthHandle = Context.Resources.LinearDepthHandle;
    const FRGResourceHandle PrevLinearDepthHandle = Context.Resources.RestirGiPrevLinearDepthHandle;
    const FRGResourceHandle RestirGIHandle = Context.Resources.RestirGIHandle;
    const FRGResourceHandle RestirGIHistoryHandle = Context.Resources.RestirGIHistoryHandle;
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
    const FRGResourceHandle RestirGiInputSHHandle = Context.Resources.RestirGiInputSHHandle;
    const FRGResourceHandle RestirGiVarianceHandle = Context.Resources.RestirGiVarianceHandle;

    auto& Device = Owner.Device;
    auto& PathTracingInstanceDataBindlessIndices = Owner.PathTracingInstanceDataBindlessIndices;
    auto& TlasResultBuffers = Owner.TlasResultBuffers;
    auto& RestirGIRootSignature = Owner.RestirGIRootSignature;
    auto& Viewport = Owner.Viewport;
    auto& RestirGIMaxHistoryFrames = Owner.RestirGIMaxHistoryFrames;
    auto& bRestirGIFreezeFrame = Owner.bRestirGIFreezeFrame;
    auto& RestirGIFrozenSequenceFrame = Owner.RestirGIFrozenSequenceFrame;
    auto& bRestirGIEnabled = Owner.bRestirGIEnabled;
    auto& bRestirGIReservoirHistoryValid = Owner.bRestirGIReservoirHistoryValid;
    auto& RestirGIReservoirHistoryFrameCount = Owner.RestirGIReservoirHistoryFrameCount;
    auto& RestirGIIntensity = Owner.RestirGIIntensity;
    auto& RestirGIRayLength = Owner.RestirGIRayLength;
    auto& RestirGIClamp = Owner.RestirGIClamp;
    auto& bRestirGITemporalReuse = Owner.bRestirGITemporalReuse;
    auto& bRestirGISpatialReuse = Owner.bRestirGISpatialReuse;
    auto& bRestirGIUseVisibility = Owner.bRestirGIUseVisibility;
    auto& bRestirGIUseBrdf = Owner.bRestirGIUseBrdf;
    auto& bRestirGIUseHistoryIndirect = Owner.bRestirGIUseHistoryIndirect;
    auto& bRestirGIDebugRayEnabled = Owner.bRestirGIDebugRayEnabled;
    auto& RestirGIDebugPixelX = Owner.RestirGIDebugPixelX;
    auto& RestirGIDebugPixelY = Owner.RestirGIDebugPixelY;
    auto& RestirGIInitialPipeline = Owner.RestirGIInitialPipeline;
    auto& DepthBindlessIndices = Owner.DepthBindlessIndices;
    auto& VelocityBindlessIndex = Owner.VelocityBindlessIndex;
    auto& GBufferBindlessIndices = Owner.GBufferBindlessIndices;
    auto& EnvironmentCubeBindlessIndex = Owner.EnvironmentCubeBindlessIndex;
    auto& RestirGIInitialRadianceUavBindlessIndex = Owner.RestirGIInitialRadianceUavBindlessIndex;
    auto& RestirGIInitialRayDirectionUavBindlessIndex = Owner.RestirGIInitialRayDirectionUavBindlessIndex;
    auto& RestirGIHistorySrvBindlessIndex = Owner.RestirGIHistorySrvBindlessIndex;
    auto& LinearDepthBindlessIndex = Owner.LinearDepthBindlessIndex;
    auto& RestirGiPrevLinearDepthSrvBindlessIndex = Owner.RestirGiPrevLinearDepthSrvBindlessIndex;
    auto& GpuDebugLineBufferUavBindlessIndex = Owner.GpuDebugLineBufferUavBindlessIndex;
    auto& RestirGITemporalPipeline = Owner.RestirGITemporalPipeline;
    auto& RestirGIReservoirBootstrapPipeline = Owner.RestirGIReservoirBootstrapPipeline;
    auto& RestirGIInitialRadianceSrvBindlessIndex = Owner.RestirGIInitialRadianceSrvBindlessIndex;
    auto& RestirGIInitialRayDirectionSrvBindlessIndex = Owner.RestirGIInitialRayDirectionSrvBindlessIndex;
    auto& RestirGIReservoirDepthNormalASrvBindlessIndex = Owner.RestirGIReservoirDepthNormalASrvBindlessIndex;
    auto& RestirGIReservoirSampleRadianceASrvBindlessIndex = Owner.RestirGIReservoirSampleRadianceASrvBindlessIndex;
    auto& RestirGIReservoirRayDirectionASrvBindlessIndex = Owner.RestirGIReservoirRayDirectionASrvBindlessIndex;
    auto& RestirGIReservoirMWASrvBindlessIndex = Owner.RestirGIReservoirMWASrvBindlessIndex;
    auto& RestirGIReservoirDepthNormalBUavBindlessIndex = Owner.RestirGIReservoirDepthNormalBUavBindlessIndex;
    auto& RestirGIReservoirSampleRadianceBUavBindlessIndex = Owner.RestirGIReservoirSampleRadianceBUavBindlessIndex;
    auto& RestirGIReservoirRayDirectionBUavBindlessIndex = Owner.RestirGIReservoirRayDirectionBUavBindlessIndex;
    auto& RestirGIReservoirMWBUavBindlessIndex = Owner.RestirGIReservoirMWBUavBindlessIndex;
    auto& RestirGISpatialPipeline = Owner.RestirGISpatialPipeline;
    auto& RestirGIReservoirDepthNormalBSrvBindlessIndex = Owner.RestirGIReservoirDepthNormalBSrvBindlessIndex;
    auto& RestirGIReservoirSampleRadianceBSrvBindlessIndex = Owner.RestirGIReservoirSampleRadianceBSrvBindlessIndex;
    auto& RestirGIReservoirRayDirectionBSrvBindlessIndex = Owner.RestirGIReservoirRayDirectionBSrvBindlessIndex;
    auto& RestirGIReservoirMWBSrvBindlessIndex = Owner.RestirGIReservoirMWBSrvBindlessIndex;
    auto& RestirGIReservoirDepthNormalAUavBindlessIndex = Owner.RestirGIReservoirDepthNormalAUavBindlessIndex;
    auto& RestirGIReservoirSampleRadianceAUavBindlessIndex = Owner.RestirGIReservoirSampleRadianceAUavBindlessIndex;
    auto& RestirGIReservoirRayDirectionAUavBindlessIndex = Owner.RestirGIReservoirRayDirectionAUavBindlessIndex;
    auto& RestirGIReservoirMWAUavBindlessIndex = Owner.RestirGIReservoirMWAUavBindlessIndex;
    auto& RestirGIResolvePipeline = Owner.RestirGIResolvePipeline;
    auto& RestirGIUavBindlessIndex = Owner.RestirGIUavBindlessIndex;
    auto& RestirGiInputSHUavBindlessIndex = Owner.RestirGiInputSHUavBindlessIndex;
    auto& RestirGiVarianceUavBindlessIndex = Owner.RestirGiVarianceUavBindlessIndex;

    struct FRestirGIPassData
    {
        bool bEnabled = false;
    };

    auto DispatchNewPass = [&](FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, const wchar_t* EventName, uint32_t SpatialPassIndex, const uint32_t BindlessIndices[28], uint32_t DispatchWidth, uint32_t DispatchHeight, bool bEnabled)
    {
        if (!bEnabled || !Device || !Device->GetBindlessDescriptorHeap() || !PipelineState || !RestirGIRootSignature)
        {
            return;
        }

        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        if (FrameIndex >= PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        ID3D12Resource* TlasResource = (FrameIndex < TlasResultBuffers.size()) ? TlasResultBuffers[FrameIndex].Get() : nullptr;
        if (!TlasResource)
        {
            for (const auto& TlasBuffer : TlasResultBuffers)
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

        const uint32_t FullWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Viewport.Height);
        const uint32_t HalfWidth = (FullWidth + 1u) / 2u;
        const uint32_t HalfHeight = (FullHeight + 1u) / 2u;
        const uint32_t MaxHistoryFrames = (std::max)(1u, RestirGIMaxHistoryFrames);
        const uint32_t SequenceFrame = bRestirGIFreezeFrame ? RestirGIFrozenSequenceFrame : (FrameState.bTaaActive ? FrameState.TaaFrameIndex : Owner.GetFrameIndex());

        const FRestirGIConstants Constants =
        {
            FullWidth,
            FullHeight,
            HalfWidth,
            HalfHeight,
            FrameState.bTaaActive ? FrameState.TaaFrameIndex : Owner.GetFrameIndex(),
            bRestirGIEnabled ? 1u : 0u,
            (bRestirGIReservoirHistoryValid && RestirGIReservoirHistoryFrameCount >= MaxHistoryFrames) ? 1u : 0u,
            SpatialPassIndex,
            (std::max)(0.0f, RestirGIIntensity),
            RestirGIRayLength,
            RestirGIClamp,
            bRestirGIUseVisibility ? 1u : 0u,
            bRestirGIUseBrdf ? 1u : 0u,
            bRestirGIUseHistoryIndirect ? 1u : 0u,
            SequenceFrame,
            bRestirGIDebugRayEnabled ? 1u : 0u,
            RestirGIDebugPixelX,
            RestirGIDebugPixelY
        };

        FScopedPixEvent RestirEvent(CommandList4, EventName);
        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList4->SetComputeRootSignature(RestirGIRootSignature.Get());
        CommandList4->SetPipelineState(PipelineState);
        CommandList4->SetComputeRootShaderResourceView(0, TlasResource->GetGPUVirtualAddress());
        CommandList4->SetComputeRootConstantBufferView(1, Owner.GetSceneConstantBufferAddress());
        CommandList4->SetComputeRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &Constants, 0);
        CommandList4->SetComputeRoot32BitConstants(3, 28, BindlessIndices, 0);

        const uint32_t GroupSize = 8;
        const uint32_t DispatchX = (DispatchWidth + GroupSize - 1) / GroupSize;
        const uint32_t DispatchY = (DispatchHeight + GroupSize - 1) / GroupSize;
        CommandList4->Dispatch(DispatchX, DispatchY, 1);
    };

    auto AddInitialSamplingPass = [&]()
    {
        Graph.AddPass<FRestirGIPassData>("InitialSampling", [&, DepthHandle, VelocityHandle, LinearDepthHandle, PrevLinearDepthHandle, GBufferHandles, RestirGIHistoryHandle, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle](FRestirGIPassData& Data, FRGPassBuilder& Builder)
        {
            Builder.SetPixGroup("RestirGI");
            Data.bEnabled = bRestirGIEnabled
                && RestirGIRootSignature
                && RestirGIInitialPipeline;
            if (!Data.bEnabled)
            {
                return;
            }

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
        }, [&, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
        {
            const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
            const uint32_t DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthArrayIndex];
            const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
            const uint32_t InstanceDataBindlessIndex = (FrameIndex < PathTracingInstanceDataBindlessIndices.size()) ? PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
            const uint32_t LinearClampSamplerIndex = Device ? Device->GetLinearClampSamplerIndex() : UINT32_MAX;

            const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX)
                && (VelocityBindlessIndex != UINT32_MAX)
                && (GBufferBindlessIndices[0] != UINT32_MAX)
                && (GBufferBindlessIndices[1] != UINT32_MAX)
                && (GBufferBindlessIndices[2] != UINT32_MAX)
                && (InstanceDataBindlessIndex != UINT32_MAX)
                && (EnvironmentCubeBindlessIndex != UINT32_MAX)
                && (LinearClampSamplerIndex != UINT32_MAX)
                && (RestirGIInitialRadianceUavBindlessIndex != UINT32_MAX)
                && (RestirGIInitialRayDirectionUavBindlessIndex != UINT32_MAX)
                && (RestirGIHistorySrvBindlessIndex != UINT32_MAX)
                && (LinearDepthBindlessIndex != UINT32_MAX)
                && (RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);

            const uint32_t BindlessIndices[28] =
            {
                RestirGIInitialRadianceUavBindlessIndex,
                DepthBindlessIndex,
                VelocityBindlessIndex,
                GBufferBindlessIndices[0],
                GBufferBindlessIndices[1],
                GBufferBindlessIndices[2],
                InstanceDataBindlessIndex,
                EnvironmentCubeBindlessIndex,
                LinearClampSamplerIndex,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                RestirGIInitialRadianceUavBindlessIndex,
                RestirGIInitialRayDirectionUavBindlessIndex,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                UINT32_MAX,
                RestirGIHistorySrvBindlessIndex,
                RestirGiPrevLinearDepthSrvBindlessIndex,
                GpuDebugLineBufferUavBindlessIndex
            };

            const uint32_t FullWidth = static_cast<uint32_t>(Viewport.Width);
            const uint32_t FullHeight = static_cast<uint32_t>(Viewport.Height);
            DispatchNewPass(Cmd, RestirGIInitialPipeline.Get(), L"InitialSampling", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
        });
    };
    AddInitialSamplingPass();

    auto AddTemporalResamplingPass = [&]()
    {
        Graph.AddPass<FRestirGIPassData>("TemporalResampling", [&, DepthHandle, VelocityHandle, PrevLinearDepthHandle, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGIPassData& Data, FRGPassBuilder& Builder)
        {
            Builder.SetPixGroup("RestirGI");
            Data.bEnabled = bRestirGIEnabled
                && RestirGIRootSignature
                && RestirGITemporalPipeline;
            if (!Data.bEnabled)
            {
                return;
            }

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
        }, [&, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
        {
        const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthArrayIndex];
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < PathTracingInstanceDataBindlessIndices.size()) ? PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Device ? Device->GetLinearClampSamplerIndex() : UINT32_MAX;

        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX)
            && (VelocityBindlessIndex != UINT32_MAX)
            && (GBufferBindlessIndices[0] != UINT32_MAX)
            && (GBufferBindlessIndices[1] != UINT32_MAX)
            && (GBufferBindlessIndices[2] != UINT32_MAX)
            && (InstanceDataBindlessIndex != UINT32_MAX)
            && (EnvironmentCubeBindlessIndex != UINT32_MAX)
            && (LinearClampSamplerIndex != UINT32_MAX)
            && (RestirGIInitialRadianceSrvBindlessIndex != UINT32_MAX)
            && (RestirGIInitialRayDirectionSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirDepthNormalASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirSampleRadianceASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirRayDirectionASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirMWASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX)
            && (RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);

        const uint32_t BindlessIndices[28] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            VelocityBindlessIndex,
            GBufferBindlessIndices[0],
            GBufferBindlessIndices[1],
            GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            EnvironmentCubeBindlessIndex,
            LinearClampSamplerIndex,
            RestirGIInitialRadianceSrvBindlessIndex,
            RestirGIInitialRayDirectionSrvBindlessIndex,
            RestirGIReservoirDepthNormalASrvBindlessIndex,
            RestirGIReservoirSampleRadianceASrvBindlessIndex,
            RestirGIReservoirRayDirectionASrvBindlessIndex,
            RestirGIReservoirMWASrvBindlessIndex,
            RestirGIReservoirDepthNormalBUavBindlessIndex,
            RestirGIReservoirSampleRadianceBUavBindlessIndex,
            RestirGIReservoirRayDirectionBUavBindlessIndex,
            RestirGIReservoirMWBUavBindlessIndex,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            UINT32_MAX,
            RestirGIHistorySrvBindlessIndex,
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GpuDebugLineBufferUavBindlessIndex
        };

        const uint32_t FullWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Viewport.Height);
            DispatchNewPass(Cmd, RestirGITemporalPipeline.Get(), L"TemporalResampling", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
        });
    };
    if (bRestirGITemporalReuse)
    {
        AddTemporalResamplingPass();
    }
    else
    {
        Graph.AddPass<FRestirGIPassData>("ReservoirBootstrap", [&, DepthHandle, GBufferHandles, RestirGIInitialRadianceHandle, RestirGIInitialRayDirectionHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWBHandle](FRestirGIPassData& Data, FRGPassBuilder& Builder)
        {
            Builder.SetPixGroup("RestirGI");
            Data.bEnabled = bRestirGIEnabled && RestirGIRootSignature && RestirGIReservoirBootstrapPipeline;
            if (!Data.bEnabled) { return; }
            Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadTexture(RestirGIInitialRadianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadTexture(RestirGIInitialRayDirectionHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteTexture(RestirGIReservoirDepthNormalBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteTexture(RestirGIReservoirSampleRadianceBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteTexture(RestirGIReservoirRayDirectionBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteTexture(RestirGIReservoirMWBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }, [&, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
        {
            const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
            const uint32_t DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthArrayIndex];
            const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
            const uint32_t InstanceDataBindlessIndex = (FrameIndex < PathTracingInstanceDataBindlessIndices.size()) ? PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
            const uint32_t LinearClampSamplerIndex = Device ? Device->GetLinearClampSamplerIndex() : UINT32_MAX;
            const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX) && (GBufferBindlessIndices[0] != UINT32_MAX) && (GBufferBindlessIndices[1] != UINT32_MAX) && (GBufferBindlessIndices[2] != UINT32_MAX) && (InstanceDataBindlessIndex != UINT32_MAX) && (EnvironmentCubeBindlessIndex != UINT32_MAX) && (LinearClampSamplerIndex != UINT32_MAX) && (RestirGIInitialRadianceSrvBindlessIndex != UINT32_MAX) && (RestirGIInitialRayDirectionSrvBindlessIndex != UINT32_MAX) && (RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX) && (RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX) && (RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX) && (RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX);
            const uint32_t BindlessIndices[28] = { UINT32_MAX, DepthBindlessIndex, VelocityBindlessIndex, GBufferBindlessIndices[0], GBufferBindlessIndices[1], GBufferBindlessIndices[2], InstanceDataBindlessIndex, EnvironmentCubeBindlessIndex, LinearClampSamplerIndex, RestirGIInitialRadianceSrvBindlessIndex, RestirGIInitialRayDirectionSrvBindlessIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, RestirGIReservoirDepthNormalBUavBindlessIndex, RestirGIReservoirSampleRadianceBUavBindlessIndex, RestirGIReservoirRayDirectionBUavBindlessIndex, RestirGIReservoirMWBUavBindlessIndex, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, RestirGIHistorySrvBindlessIndex, RestirGiPrevLinearDepthSrvBindlessIndex, GpuDebugLineBufferUavBindlessIndex };
            const uint32_t FullWidth = static_cast<uint32_t>(Viewport.Width);
            const uint32_t FullHeight = static_cast<uint32_t>(Viewport.Height);
            DispatchNewPass(Cmd, RestirGIReservoirBootstrapPipeline.Get(), L"ReservoirBootstrap", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
        });
    }

    auto AddSpatialResampling0Pass = [&]()
    {
        Graph.AddPass<FRestirGIPassData>("SpatialResampling0", [&, PrevLinearDepthHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGIPassData& Data, FRGPassBuilder& Builder)
        {
            Builder.SetPixGroup("RestirGI");
            Data.bEnabled = bRestirGIEnabled
                && RestirGIRootSignature
                && RestirGISpatialPipeline;
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
        }, [&, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
        {
        const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthArrayIndex];
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < PathTracingInstanceDataBindlessIndices.size()) ? PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Device ? Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX)
            && (GBufferBindlessIndices[0] != UINT32_MAX)
            && (GBufferBindlessIndices[1] != UINT32_MAX)
            && (GBufferBindlessIndices[2] != UINT32_MAX)
            && (InstanceDataBindlessIndex != UINT32_MAX)
            && (EnvironmentCubeBindlessIndex != UINT32_MAX)
            && (LinearClampSamplerIndex != UINT32_MAX)
            && (RestirGIReservoirDepthNormalBSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirSampleRadianceBSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirRayDirectionBSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirMWBSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirDepthNormalAUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirSampleRadianceAUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirRayDirectionAUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirMWAUavBindlessIndex != UINT32_MAX)
            && (RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);

        const uint32_t BindlessIndices[28] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            VelocityBindlessIndex,
            GBufferBindlessIndices[0],
            GBufferBindlessIndices[1],
            GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            EnvironmentCubeBindlessIndex,
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
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GpuDebugLineBufferUavBindlessIndex
        };

        const uint32_t FullWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Viewport.Height);
            DispatchNewPass(Cmd, RestirGISpatialPipeline.Get(), L"SpatialResampling0", 0u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
        });
    };
    if (bRestirGISpatialReuse)
    {
        AddSpatialResampling0Pass();
    }

    auto AddSpatialResampling1Pass = [&]()
    {
        Graph.AddPass<FRestirGIPassData>("SpatialResampling1", [&, PrevLinearDepthHandle, RestirGIReservoirDepthNormalAHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceAHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionAHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWAHandle, RestirGIReservoirMWBHandle](FRestirGIPassData& Data, FRGPassBuilder& Builder)
        {
            Builder.SetPixGroup("RestirGI");
            Data.bEnabled = bRestirGIEnabled
                && RestirGIRootSignature
                && RestirGISpatialPipeline;
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
        }, [&, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
        {
        const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthArrayIndex];
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < PathTracingInstanceDataBindlessIndices.size()) ? PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Device ? Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX)
            && (GBufferBindlessIndices[0] != UINT32_MAX)
            && (GBufferBindlessIndices[1] != UINT32_MAX)
            && (GBufferBindlessIndices[2] != UINT32_MAX)
            && (InstanceDataBindlessIndex != UINT32_MAX)
            && (EnvironmentCubeBindlessIndex != UINT32_MAX)
            && (LinearClampSamplerIndex != UINT32_MAX)
            && (RestirGIReservoirDepthNormalASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirSampleRadianceASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirRayDirectionASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirMWASrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirDepthNormalBUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirSampleRadianceBUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirRayDirectionBUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirMWBUavBindlessIndex != UINT32_MAX)
            && (RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);

        const uint32_t BindlessIndices[28] =
        {
            UINT32_MAX,
            DepthBindlessIndex,
            VelocityBindlessIndex,
            GBufferBindlessIndices[0],
            GBufferBindlessIndices[1],
            GBufferBindlessIndices[2],
            InstanceDataBindlessIndex,
            EnvironmentCubeBindlessIndex,
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
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GpuDebugLineBufferUavBindlessIndex
        };

        const uint32_t FullWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Viewport.Height);
            DispatchNewPass(Cmd, RestirGISpatialPipeline.Get(), L"SpatialResampling1", 1u, BindlessIndices, (FullWidth + 1u) / 2u, (FullHeight + 1u) / 2u, Data.bEnabled && bInputsValid);
        });
    };
    if (bRestirGISpatialReuse)
    {
        AddSpatialResampling1Pass();
    }

    auto AddResolvePass = [&]()
    {
        Graph.AddPass<FRestirGIPassData>("RestirGIResolve", [&, DepthHandle, PrevLinearDepthHandle, GBufferHandles, RestirGIHandle, RestirGIReservoirDepthNormalBHandle, RestirGIReservoirSampleRadianceBHandle, RestirGIReservoirRayDirectionBHandle, RestirGIReservoirMWBHandle, RestirGiInputSHHandle, RestirGiVarianceHandle](FRestirGIPassData& Data, FRGPassBuilder& Builder)
        {
            Builder.SetPixGroup("RestirGI");
            Data.bEnabled = bRestirGIEnabled
                && RestirGIRootSignature
                && RestirGIResolvePipeline;
            if (!Data.bEnabled)
            {
                return;
            }

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
        }, [&, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
        {
        const uint32_t DepthArrayIndex = Owner.GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
        const uint32_t DepthBindlessIndex = DepthBindlessIndices.empty() ? UINT32_MAX : DepthBindlessIndices[DepthArrayIndex];
        const uint32_t FrameIndex = Cmd.GetCurrentFrameIndex();
        const uint32_t InstanceDataBindlessIndex = (FrameIndex < PathTracingInstanceDataBindlessIndices.size()) ? PathTracingInstanceDataBindlessIndices[FrameIndex] : UINT32_MAX;
        const uint32_t LinearClampSamplerIndex = Device ? Device->GetLinearClampSamplerIndex() : UINT32_MAX;
        const bool bInputsValid = (DepthBindlessIndex != UINT32_MAX)
            && (GBufferBindlessIndices[0] != UINT32_MAX)
            && (GBufferBindlessIndices[1] != UINT32_MAX)
            && (GBufferBindlessIndices[2] != UINT32_MAX)
            && (InstanceDataBindlessIndex != UINT32_MAX)
            && (EnvironmentCubeBindlessIndex != UINT32_MAX)
            && (LinearClampSamplerIndex != UINT32_MAX)
            && (RestirGIUavBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirDepthNormalBSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirSampleRadianceBSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirRayDirectionBSrvBindlessIndex != UINT32_MAX)
            && (RestirGIReservoirMWBSrvBindlessIndex != UINT32_MAX)
            && (RestirGiInputSHUavBindlessIndex != UINT32_MAX)
            && (RestirGiVarianceUavBindlessIndex != UINT32_MAX)
            && (RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX);

        // RestirGI resolve bindless slots contract: output UAVs must be b2[23]/b2[24].
        const uint32_t BindlessIndices[28] =
        {
            RestirGIUavBindlessIndex,                      // b2[0]
            DepthBindlessIndex,                           // b2[1]
            VelocityBindlessIndex,                        // b2[2]
            GBufferBindlessIndices[0],                    // b2[3]
            GBufferBindlessIndices[1],                    // b2[4]
            GBufferBindlessIndices[2],                    // b2[5]
            InstanceDataBindlessIndex,                    // b2[6]
            EnvironmentCubeBindlessIndex,                 // b2[7]
            LinearClampSamplerIndex,                      // b2[8]
            UINT32_MAX,                                   // b2[9]  initial radiance SRV (unused in resolve)
            UINT32_MAX,                                   // b2[10] initial ray direction SRV (unused in resolve)
            UINT32_MAX,                                   // b2[11]
            UINT32_MAX,                                   // b2[12]
            UINT32_MAX,                                   // b2[13]
            UINT32_MAX,                                   // b2[14]
            UINT32_MAX,                                   // b2[15]
            UINT32_MAX,                                   // b2[16]
            UINT32_MAX,                                   // b2[17]
            UINT32_MAX,                                   // b2[18]
            RestirGIReservoirDepthNormalBSrvBindlessIndex,// b2[19]
            RestirGIReservoirSampleRadianceBSrvBindlessIndex, // b2[20]
            RestirGIReservoirRayDirectionBSrvBindlessIndex,   // b2[21]
            RestirGIReservoirMWBSrvBindlessIndex,         // b2[22]
            RestirGiInputSHUavBindlessIndex,              // b2[23] resolve InputSH UAV output
            RestirGiVarianceUavBindlessIndex,             // b2[24] resolve Variance UAV output
            RestirGIHistorySrvBindlessIndex,              // b2[25]
            RestirGiPrevLinearDepthSrvBindlessIndex,      // b2[26] previous linear depth SRV
            GpuDebugLineBufferUavBindlessIndex            // b2[27]
        };

        const uint32_t FullWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t FullHeight = static_cast<uint32_t>(Viewport.Height);
            DispatchNewPass(Cmd, RestirGIResolvePipeline.Get(), L"Resolve", 0u, BindlessIndices, FullWidth, FullHeight, Data.bEnabled && bInputsValid);
        });
    };
    AddResolvePass();
}

void FDeferredRayTracingPasses::AddRestirGiDenoiserPreTemporalPass(FDeferredRenderer& Owner, FRenderGraph& Graph, const FDeferredRenderer::FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle) const
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("Denoiser PreTemporal", [&Owner, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistorySHHandle, HistoryIrradianceHandle, HistoryCountAHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle, GBufferHandles](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("RestirGI Denoiser");
        Data.bEnabled = Owner.bRestirGIEnabled && Owner.RestirGiDenoiserRootSignature && Owner.RestirGiPreBlurPipeline && Owner.RestirGiTemporalAccumulationPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VelocityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(InputSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(VarianceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistorySHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(PrevNormalHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryIrradianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistoryCountBHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevLinearDepthHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(PrevNormalHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&Owner, &FrameState](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Owner.Device || !Owner.Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        const bool bInputsValid = (Owner.RestirGiInputSHSrvBindlessIndex != UINT32_MAX)
            && (Owner.RestirGiVarianceSrvBindlessIndex != UINT32_MAX)
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
        Constants.HistoryValid = (Owner.bRestirGIDenoiserHistoryValid && !FrameState.bCameraMoved) ? 1u : 0u;

        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        auto DispatchDenoiserPass = [&](ID3D12PipelineState* Pipeline, uint32_t PassIndex, const uint32_t* BindlessIndices, uint32_t BindlessCount)
        {
            Constants.PassIndex = PassIndex;
            Constants.MipLevel = 0u;
            LocalCommandList->SetPipelineState(Pipeline);
            LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
            LocalCommandList->SetComputeRoot32BitConstants(1, BindlessCount, BindlessIndices, 0);
            LocalCommandList->Dispatch(DispatchX, DispatchY, 1);

            D3D12_RESOURCE_BARRIER UavBarrier = {};
            UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UavBarrier.UAV.pResource = nullptr;
            LocalCommandList->ResourceBarrier(1, &UavBarrier);
        };

        FScopedPixEvent Event(LocalCommandList, L"Denoiser PreTemporal");
        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(Owner.RestirGiDenoiserRootSignature.Get());

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
            UINT32_MAX
        };

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
            Owner.RestirGiHistoryIrradianceUavBindlessIndex,
            Owner.RestirGiTemporalSHUavBindlessIndex,
            Owner.RestirGiHistoryCountBUavBindlessIndex,
            Owner.RestirGiPrevLinearDepthUavBindlessIndex,
            Owner.RestirGiPrevNormalUavBindlessIndex,
            UINT32_MAX
        };

        DispatchDenoiserPass(Owner.RestirGiPreBlurPipeline.Get(), 0u, PreBlurBindless, _countof(PreBlurBindless));
        DispatchDenoiserPass(Owner.RestirGiTemporalAccumulationPipeline.Get(), 1u, TemporalBindless, _countof(TemporalBindless));
    });
}
