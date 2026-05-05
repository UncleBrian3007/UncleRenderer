#include "EnvironmentMap.h"

#include "Renderer.h"
#include "RendererUtils.h"
#include "ShaderCompiler.h"
#include "TextureLoader.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../Core/RendererConfig.h"
#include "../RHI/DX12CommandQueue.h"
#include "../RHI/DX12Device.h"
#include <d3dx12.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace
{
    D3D12_GPU_DESCRIPTOR_HANDLE GetBindlessGpuHandle(FDX12Device* Device, uint32_t Index)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE Handle{};
        if (!Device || !Device->GetBindlessDescriptorHeap())
        {
            return Handle;
        }

        const UINT Stride = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Handle = Device->GetBindlessDescriptorHeap()->GetGPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<UINT64>(Index) * Stride;
        return Handle;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetBindlessCpuClearHandle(FDX12Device* Device, uint32_t Index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE Handle{};
        if (!Device || !Device->GetBindlessCpuDescriptorHeap())
        {
            return Handle;
        }

        const UINT Stride = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        Handle = Device->GetBindlessCpuDescriptorHeap()->GetCPUDescriptorHandleForHeapStart();
        Handle.ptr += static_cast<SIZE_T>(Index) * Stride;
        return Handle;
    }
}

bool FEnvironmentMap::InitializePipelines(FRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    return CreateBuildPipelines(Device);
}

bool FEnvironmentMap::InitializeResources(FRenderer& Owner, FDX12Device* Device, const FRendererConfig& Config, const char* LogPrefix)
{
    const std::string Prefix = LogPrefix ? LogPrefix : "Renderer";
    if (!Owner.TextureLoader)
    {
        LogError(Prefix + " renderer initialization failed: texture loader is not initialized");
        return false;
    }

    if (!BuildFromEquirect(Owner, Device, *Owner.TextureLoader, Config))
    {
        LogError(Prefix + " renderer initialization failed: environment build requires R11G11B10 typed UAV support");
        return false;
    }

    if (!Owner.TextureLoader->LoadOrDefault(L"Assets/Textures/PreintegratedGF.dds", BrdfLutTexture))
    {
        LogError(Prefix + " renderer initialization failed: BRDF LUT texture loading failed");
        return false;
    }

    if (BrdfLutTexture)
    {
        BrdfLutTexture->SetName(L"BrdfLut");
    }

    return true;
}

bool FEnvironmentMap::CreateBuildPipelines(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(56, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc;
    VersionedRootDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(BuildRootSignature.ReleaseAndGetAddressOf())));

    FShaderCompiler Compiler;

    std::vector<uint8_t> EquirectBytecode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/EnvironmentMap/EnvEquirectToCubeCS.hlsl", EquirectBytecode))
    {
        LogError("Failed to compile equirect-to-cube compute shader");
        return false;
    }

    std::vector<uint8_t> MipGenBytecode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/EnvironmentMap/EnvCubeMipGenCS.hlsl", MipGenBytecode))
    {
        LogError("Failed to compile environment cube mipgen compute shader");
        return false;
    }

    std::vector<uint8_t> SpecularBytecode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/EnvironmentMap/EnvSpecularPrefilterCS.hlsl", SpecularBytecode))
    {
        LogError("Failed to compile environment specular prefilter compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = BuildRootSignature.Get();

    PsoDesc.CS = { EquirectBytecode.data(), EquirectBytecode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(EquirectToCubePipeline.ReleaseAndGetAddressOf())));

    PsoDesc.CS = { MipGenBytecode.data(), MipGenBytecode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(CubeMipGenPipeline.ReleaseAndGetAddressOf())));

    PsoDesc.CS = { SpecularBytecode.data(), SpecularBytecode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(SpecularPrefilterPipeline.ReleaseAndGetAddressOf())));

    return true;
}

DXGI_FORMAT FEnvironmentMap::ResolveBuildFormat(FDX12Device* Device) const
{
    const auto SupportsTypedUav = [&](DXGI_FORMAT Format) -> bool
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT FormatSupport = {};
        FormatSupport.Format = Format;
        if (FAILED(Device->GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &FormatSupport, sizeof(FormatSupport))))
        {
            return false;
        }

        return (FormatSupport.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
    };

    if (SupportsTypedUav(DXGI_FORMAT_R11G11B10_FLOAT))
    {
        LogInfo("Environment build format selected: DXGI_FORMAT_R11G11B10_FLOAT (typed UAV supported)");
        return DXGI_FORMAT_R11G11B10_FLOAT;
    }

    LogError("Environment build requires DXGI_FORMAT_R11G11B10_FLOAT typed UAV support. Renderer initialization will fail on this device.");
    return DXGI_FORMAT_UNKNOWN;
}

bool FEnvironmentMap::BuildFromEquirect(FRenderer& Owner, FDX12Device* Device, FTextureLoader& TextureLoader, const FRendererConfig& Config)
{
    Microsoft::WRL::ComPtr<ID3D12Resource> EquirectTexture;
    if (!TextureLoader.LoadHdrTexture(Config.EnvironmentEquirectPath, EquirectTexture))
    {
        LogWarning("Failed to load HDR equirect texture. Falling back to DDS PMREM.");
        return false;
    }

    const DXGI_FORMAT EnvironmentBuildFormat = ResolveBuildFormat(Device);
    if (EnvironmentBuildFormat == DXGI_FORMAT_UNKNOWN)
    {
        LogError("Environment build aborted: required format support is unavailable.");
        return false;
    }

    const uint32_t CubeResolution = (std::max)(16u, Config.EnvironmentCubeResolution);
    const uint16_t MipLevels = static_cast<uint16_t>((std::max)(1u, static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(CubeResolution)))) + 1u));
    const uint32_t GeneratedMipCount = (MipLevels > 0u) ? (static_cast<uint32_t>(MipLevels) - 1u) : 0u;
    if (GeneratedMipCount > 12u)
    {
        LogWarning("Environment cube one-pass SPD mip generation supports up to 12 generated mips. Falling back to DDS PMREM.");
        return false;
    }

    auto CreateCubeResource = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource, const wchar_t* Name) -> bool
    {
        D3D12_RESOURCE_DESC Desc = {};
        Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        Desc.Width = CubeResolution;
        Desc.Height = CubeResolution;
        Desc.DepthOrArraySize = 6;
        Desc.MipLevels = MipLevels;
        Desc.Format = EnvironmentBuildFormat;
        Desc.SampleDesc.Count = 1;
        Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_HEAP_PROPERTIES HeapProps = {};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        HeapProps.CreationNodeMask = 1;
        HeapProps.VisibleNodeMask = 1;

        const HRESULT Hr = Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(OutResource.ReleaseAndGetAddressOf()));
        if (FAILED(Hr))
        {
            return false;
        }

        OutResource->SetName(Name);
        return true;
    };

    Microsoft::WRL::ComPtr<ID3D12Resource> RawCube;
    Microsoft::WRL::ComPtr<ID3D12Resource> PrefilteredCube;
    if (!CreateCubeResource(RawCube, L"EnvironmentRawCube") || !CreateCubeResource(PrefilteredCube, L"EnvironmentSpecularCube"))
    {
        LogWarning("Failed to create environment cube resources with selected build format. Falling back to DDS PMREM.");
        return false;
    }

    const D3D12_SHADER_RESOURCE_VIEW_DESC EquirectSrvDesc = []()
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC Desc = {};
        Desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        Desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        Desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        Desc.Texture2D.MipLevels = 1;
        Desc.Texture2D.ResourceMinLODClamp = 0.0f;
        return Desc;
    }();

    D3D12_SHADER_RESOURCE_VIEW_DESC CubeSrvDesc = {};
    CubeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    CubeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    CubeSrvDesc.Format = EnvironmentBuildFormat;
    CubeSrvDesc.TextureCube.MipLevels = MipLevels;
    CubeSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    std::vector<D3D12_UNORDERED_ACCESS_VIEW_DESC> RawUavs(MipLevels);
    std::vector<D3D12_UNORDERED_ACCESS_VIEW_DESC> PrefilteredUavs(MipLevels);
    for (uint16_t Mip = 0; Mip < MipLevels; ++Mip)
    {
        RawUavs[Mip].Format = EnvironmentBuildFormat;
        RawUavs[Mip].ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        RawUavs[Mip].Texture2DArray.MipSlice = Mip;
        RawUavs[Mip].Texture2DArray.ArraySize = 6;

        PrefilteredUavs[Mip] = RawUavs[Mip];
    }

    const uint32_t EquirectSrvIndex = Device->CreateBindlessSrv(EquirectTexture.Get(), EquirectSrvDesc);
    const uint32_t RawCubeSrvIndex = Device->CreateBindlessSrv(RawCube.Get(), CubeSrvDesc);
    if (EquirectSrvIndex == UINT32_MAX || RawCubeSrvIndex == UINT32_MAX)
    {
        LogWarning("Failed to allocate bindless SRV for environment map generation.");
        return false;
    }

    std::vector<uint32_t> RawUavIndices;
    std::vector<uint32_t> PrefilteredUavIndices;
    RawUavIndices.reserve(MipLevels);
    PrefilteredUavIndices.reserve(MipLevels);
    for (uint16_t Mip = 0; Mip < MipLevels; ++Mip)
    {
        const uint32_t RawUav = Device->CreateBindlessUav(RawCube.Get(), nullptr, RawUavs[Mip]);
        const uint32_t PrefilteredUav = Device->CreateBindlessUav(PrefilteredCube.Get(), nullptr, PrefilteredUavs[Mip]);
        if (RawUav == UINT32_MAX || PrefilteredUav == UINT32_MAX)
        {
            LogWarning("Failed to allocate bindless UAV for environment map generation.");
            return false;
        }
        RawUavIndices.push_back(RawUav);
        PrefilteredUavIndices.push_back(PrefilteredUav);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> SpdAtomicCounterBuffer;
    {
        const D3D12_RESOURCE_DESC CounterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(uint32_t) * 6u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_HEAP_PROPERTIES HeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &CounterDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(SpdAtomicCounterBuffer.ReleaseAndGetAddressOf()))))
        {
            LogWarning("Failed to create SPD atomic counter buffer for environment mip generation.");
            return false;
        }
        SpdAtomicCounterBuffer->SetName(L"EnvironmentSpdAtomicCounter");
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC SpdCounterUavDesc = {};
    SpdCounterUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    SpdCounterUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    SpdCounterUavDesc.Buffer.NumElements = 6;
    SpdCounterUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    const uint32_t SpdCounterUavIndex = Device->CreateBindlessUav(SpdAtomicCounterBuffer.Get(), nullptr, SpdCounterUavDesc);
    if (SpdCounterUavIndex == UINT32_MAX)
    {
        LogWarning("Failed to allocate bindless UAV for SPD atomic counter.");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> Cmd;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(Allocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Allocator.Get(), nullptr, IID_PPV_ARGS(Cmd.GetAddressOf())));
    Cmd->SetName(L"EnvironmentBuild_CL");

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> Cmd7;
    if (Device->SupportsEnhancedBarriers())
    {
        Cmd->QueryInterface(IID_PPV_ARGS(Cmd7.ReleaseAndGetAddressOf()));
    }

    const auto EmitTransitionBarrier = [&](ID3D12Resource* Resource, D3D12_RESOURCE_STATES Before, D3D12_RESOURCE_STATES After)
    {
        if (!Resource || Before == After)
        {
            return;
        }

        if (Cmd7)
        {
            D3D12_TEXTURE_BARRIER TextureBarrier = {};
            TextureBarrier.SyncBefore = DX12MapStateToBarrierSync(Before);
            TextureBarrier.SyncAfter = DX12MapStateToBarrierSync(After);
            TextureBarrier.AccessBefore = DX12MapStateToBarrierAccess(Before);
            TextureBarrier.AccessAfter = DX12MapStateToBarrierAccess(After);
            TextureBarrier.LayoutBefore = DX12MapStateToTextureLayout(Before);
            TextureBarrier.LayoutAfter = DX12MapStateToTextureLayout(After);
            TextureBarrier.pResource = Resource;
            TextureBarrier.Subresources = DX12MakeTextureBarrierRange(Resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
            TextureBarrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

            D3D12_BARRIER_GROUP Group = {};
            Group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            Group.NumBarriers = 1;
            Group.pTextureBarriers = &TextureBarrier;
            Cmd7->Barrier(1, &Group);
            return;
        }

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = Resource;
        Barrier.Transition.StateBefore = Before;
        Barrier.Transition.StateAfter = After;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Cmd->ResourceBarrier(1, &Barrier);
    };

    const auto EmitUavBarrier = [&](ID3D12Resource* Resource)
    {
        if (!Resource)
        {
            return;
        }

        if (Cmd7)
        {
            D3D12_TEXTURE_BARRIER TextureBarrier = {};
            TextureBarrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
            TextureBarrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
            TextureBarrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            TextureBarrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            TextureBarrier.LayoutBefore = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
            TextureBarrier.LayoutAfter = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
            TextureBarrier.pResource = Resource;
            TextureBarrier.Subresources = DX12MakeTextureBarrierRange(Resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
            TextureBarrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

            D3D12_BARRIER_GROUP Group = {};
            Group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            Group.NumBarriers = 1;
            Group.pTextureBarriers = &TextureBarrier;
            Cmd7->Barrier(1, &Group);
            return;
        }

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Barrier.UAV.pResource = Resource;
        Cmd->ResourceBarrier(1, &Barrier);
    };

    std::array<uint32_t, 56> RootConstants = {};
    {
        FScopedPixEvent SpdMipEvent(Cmd.Get(), L"EnvEquirectToCube");

        EmitTransitionBarrier(RawCube.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        EmitTransitionBarrier(PrefilteredCube.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        Cmd->SetDescriptorHeaps(2, Heaps);
        Cmd->SetComputeRootSignature(BuildRootSignature.Get());

        Cmd->SetPipelineState(EquirectToCubePipeline.Get());
        RootConstants[0] = EquirectSrvIndex;
        RootConstants[1] = RawUavIndices[0];
        RootConstants[2] = CubeResolution;
        RootConstants[3] = CubeResolution;
        RootConstants[4] = 0;
        RootConstants[5] = 6;
        RootConstants[6] = Device->GetLinearWrapSamplerIndex();
        RootConstants[7] = 0;
        Cmd->SetComputeRoot32BitConstants(0, 8, RootConstants.data(), 0);
        const uint32_t GroupCount = (CubeResolution + 7u) / 8u;
        Cmd->Dispatch(GroupCount, GroupCount, 6);

        EmitUavBarrier(RawCube.Get());
    }

    if (GeneratedMipCount > 0u)
    {
        Cmd->SetPipelineState(CubeMipGenPipeline.Get());
        FScopedPixEvent SpdMipEvent(Cmd.Get(), L"EnvCubeMipGenSPD");
        RootConstants.fill(0u);
        RootConstants[0] = RawCubeSrvIndex;
        RootConstants[1] = SpdCounterUavIndex;
        RootConstants[2] = CubeResolution;
        RootConstants[3] = CubeResolution;
        RootConstants[4] = GeneratedMipCount;
        const uint32_t SpdDispatchX = (CubeResolution + 63u) / 64u;
        const uint32_t SpdDispatchY = (CubeResolution + 63u) / 64u;
        RootConstants[5] = SpdDispatchX * SpdDispatchY;
        RootConstants[6] = 6u;
        RootConstants[7] = Device->GetLinearClampSamplerIndex();
        const uint32_t FallbackRawUav = RawUavIndices.back();
        for (uint32_t Mip = 0; Mip < 12; ++Mip)
        {
            const size_t RawMipIndex = static_cast<size_t>(Mip) + 1u;
            RootConstants[8 + Mip * 4] = (RawMipIndex < RawUavIndices.size()) ? RawUavIndices[RawMipIndex] : FallbackRawUav;
        }

        const uint32_t ClearValues[4] = { 0u, 0u, 0u, 0u };
        const D3D12_GPU_DESCRIPTOR_HANDLE CounterGpuHandle = GetBindlessGpuHandle(Device, SpdCounterUavIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE CounterCpuHandle = GetBindlessCpuClearHandle(Device, SpdCounterUavIndex);
        Cmd->ClearUnorderedAccessViewUint(CounterGpuHandle, CounterCpuHandle, SpdAtomicCounterBuffer.Get(), ClearValues, 0, nullptr);

        D3D12_RESOURCE_BARRIER CounterBarrier = {};
        CounterBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        CounterBarrier.UAV.pResource = SpdAtomicCounterBuffer.Get();
        Cmd->ResourceBarrier(1, &CounterBarrier);

        Cmd->SetComputeRoot32BitConstants(0, 56, RootConstants.data(), 0);
        Cmd->Dispatch(SpdDispatchX, SpdDispatchY, 6);
        EmitUavBarrier(RawCube.Get());
    }

    {
        FScopedPixEvent SpdMipEvent(Cmd.Get(), L"EnvSpecularPrefilter");
        Cmd->SetPipelineState(SpecularPrefilterPipeline.Get());
        for (uint16_t Mip = 0; Mip < MipLevels; ++Mip)
        {
            const uint32_t MipResolution = (std::max)(1u, CubeResolution >> Mip);
            RootConstants[0] = RawCubeSrvIndex;
            RootConstants[1] = PrefilteredUavIndices[Mip];
            RootConstants[2] = MipResolution;
            RootConstants[3] = MipResolution;
            RootConstants[4] = Mip;
            RootConstants[5] = MipLevels;
            RootConstants[6] = (std::max)(1u, Config.EnvironmentSpecularSampleCount);
            RootConstants[7] = Device->GetLinearClampSamplerIndex();
            Cmd->SetComputeRoot32BitConstants(0, 8, RootConstants.data(), 0);
            const uint32_t MipGroupCount = (MipResolution + 7u) / 8u;
            Cmd->Dispatch(MipGroupCount, MipGroupCount, 6);

            EmitUavBarrier(PrefilteredCube.Get());
        }
    }

    EmitTransitionBarrier(RawCube.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    EmitTransitionBarrier(PrefilteredCube.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    HR_CHECK(Cmd->Close());
    ID3D12CommandList* Lists[] = { Cmd.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    EnvironmentCubeTexture = PrefilteredCube;
    EnvironmentMipCount = static_cast<float>(MipLevels);
    if (EnvironmentCubeTexture)
    {
        EnvironmentCubeTexture->SetName(L"EnvironmentCube");
    }

    return true;
}
