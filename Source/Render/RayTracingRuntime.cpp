#include "RayTracingRuntime.h"

#include "Renderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include "../RHI/RayTracing.h"
#include <d3dx12.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    constexpr uint64 GFnvOffsetBasis64 = 1469598103934665603ull;
    constexpr uint64 GFnvPrime64 = 1099511628211ull;

    inline void HashU64(uint64& Hash, uint64 Value)
    {
        Hash ^= Value;
        Hash *= GFnvPrime64;
    }

    inline void HashU32(uint64& Hash, uint32 Value)
    {
        HashU64(Hash, static_cast<uint64>(Value));
    }

    inline void HashF32Bits(uint64& Hash, float Value)
    {
        uint32 Bits = 0;
        static_assert(sizeof(Bits) == sizeof(Value), "float and uint32 size mismatch");
        std::memcpy(&Bits, &Value, sizeof(uint32));
        HashU32(Hash, Bits);
    }

    uint64 ComputeTlasInstanceHash(const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& Instances)
    {
        uint64 Hash = GFnvOffsetBasis64;

        for (const D3D12_RAYTRACING_INSTANCE_DESC& Instance : Instances)
        {
            HashU64(Hash, Instance.AccelerationStructure);
            HashU32(Hash, Instance.InstanceID);
            HashU32(Hash, Instance.InstanceMask);
            HashU32(Hash, Instance.InstanceContributionToHitGroupIndex);
            HashU32(Hash, Instance.Flags);

            for (uint32 Row = 0; Row < 3; ++Row)
            {
                for (uint32 Col = 0; Col < 4; ++Col)
                {
                    HashF32Bits(Hash, Instance.Transform[Row][Col]);
                }
            }
        }

        return Hash;
    }

    uint64_t AlignRayTracingBufferSize(uint64_t Size)
    {
        const uint64_t Alignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
        return (Size + Alignment - 1) & ~(Alignment - 1);
    }

    bool CreateRayTracingBuffer(
        FDX12Device* Device,
        uint64_t SizeInBytes,
        D3D12_RESOURCE_FLAGS Flags,
        D3D12_RESOURCE_STATES InitialState,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource,
        const wchar_t* Name)
    {
        if (!Device || SizeInBytes == 0)
        {
            return false;
        }

        const D3D12_HEAP_PROPERTIES HeapProps = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        const D3D12_RESOURCE_DESC Desc = CreateBufferResourceDesc(AlignRayTracingBufferSize(SizeInBytes), Flags);
 
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            InitialState,
            nullptr,
            IID_PPV_ARGS(OutResource.ReleaseAndGetAddressOf())));

        if (OutResource && Name)
        {
            OutResource->SetName(Name);
        }

        return OutResource != nullptr;
    }
}

bool FRayTracingRuntime::BuildSceneBlas(FDX12Device* Device, FWorld& World)
{
    using Microsoft::WRL::ComPtr;

    if (!Device || !Device->IsRayTracingSupported())
    {
        return true;
    }

    FRayTracingDevice RayTracingDevice;
    if (!Device->CreateRayTracingDevice(RayTracingDevice))
    {
        LogWarning("Ray tracing device unavailable; BLAS build skipped.");
        return true;
    }

    ComPtr<ID3D12CommandAllocator> Allocator;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(Allocator.ReleaseAndGetAddressOf())));

    ComPtr<ID3D12GraphicsCommandList> CommandList;
    HR_CHECK(Device->GetDevice()->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        Allocator.Get(),
        nullptr,
        IID_PPV_ARGS(CommandList.ReleaseAndGetAddressOf())));
    CommandList->SetName(L"RayTracingRuntime_BLASBuild_CL");

    ComPtr<ID3D12GraphicsCommandList4> CommandList4;
    if (FAILED(CommandList.As(&CommandList4)))
    {
        LogWarning("Ray tracing command list interface not available; BLAS build skipped.");
        return true;
    }

    for (const FDrawSectionView& DrawSection : World.GetDrawSectionViews())
    {
        FMeshSection& Section = *DrawSection.Section;
        if (!Section.Geometry.VertexBuffers[kMeshVertexStreamPosition] || !Section.Geometry.IndexBuffer)
        {
            continue;
        }

        const uint32_t VertexStride = Section.Geometry.VertexBufferViews[kMeshVertexStreamPosition].StrideInBytes;
        const uint32_t VertexCount = VertexStride > 0
            ? (Section.Geometry.VertexBufferViews[kMeshVertexStreamPosition].SizeInBytes / VertexStride)
            : 0;
        const uint32_t IndexCount = Section.Geometry.IndexCount;
        if (VertexCount == 0 || IndexCount == 0)
        {
            continue;
        }

        D3D12_RAYTRACING_GEOMETRY_DESC GeometryDesc = {};
        GeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        GeometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        GeometryDesc.Triangles.VertexBuffer.StartAddress = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition]->GetGPUVirtualAddress();
        GeometryDesc.Triangles.VertexBuffer.StrideInBytes = VertexStride;
        GeometryDesc.Triangles.VertexCount = VertexCount;
        GeometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        GeometryDesc.Triangles.IndexBuffer = Section.Geometry.IndexBuffer->GetGPUVirtualAddress();
        GeometryDesc.Triangles.IndexCount = IndexCount;
        GeometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
        Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        Inputs.NumDescs = 1;
        Inputs.pGeometryDescs = &GeometryDesc;
        Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (Section.bUseSkinning)
        {
            Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO PrebuildInfo = {};
        RayTracingDevice.GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &PrebuildInfo);

        if (PrebuildInfo.ResultDataMaxSizeInBytes == 0 || PrebuildInfo.ScratchDataSizeInBytes == 0)
        {
            continue;
        }

        if (!CreateRayTracingBuffer(
                Device,
                PrebuildInfo.ResultDataMaxSizeInBytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                Section.BlasResultBuffer,
                L"BLAS_Result")
            || !CreateRayTracingBuffer(
                Device,
                PrebuildInfo.ScratchDataSizeInBytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                Section.BlasScratchBuffer,
                L"BLAS_Scratch"))
        {
            LogWarning("Failed to allocate BLAS buffers for section: " + Section.Name);
            continue;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BuildDesc = {};
        BuildDesc.Inputs = Inputs;
        BuildDesc.DestAccelerationStructureData = Section.BlasResultBuffer->GetGPUVirtualAddress();
        BuildDesc.ScratchAccelerationStructureData = Section.BlasScratchBuffer->GetGPUVirtualAddress();

        CommandList4->BuildRaytracingAccelerationStructure(&BuildDesc, 0, nullptr);

        const auto Barrier = CD3DX12_RESOURCE_BARRIER::UAV(Section.BlasResultBuffer.Get());
        CommandList4->ResourceBarrier(1, &Barrier);

        Section.BlasGeometryDesc = GeometryDesc;
        Section.bHasRayTracingBlas = true;
    }

    HR_CHECK(CommandList->Close());

    ID3D12CommandList* Lists[] = { CommandList.Get() };
    FDX12CommandQueue* Queue = Device->GetGraphicsQueue();
    if (Queue)
    {
        Queue->ExecuteCommandLists(1, Lists);
        const uint64_t FenceValue = Queue->Signal();
        Queue->Wait(FenceValue);
    }

    return true;
}

uint32_t FRayTracingRuntime::UpdateDepthSrv(FRenderer& Owner, uint32_t FrameIndex, ID3D12Resource* DepthBuffer)
{
    if (!DepthBuffer || FrameIndex >= RayTracingDepthSrvBindlessIndices.size())
    {
        return UINT32_MAX;
    }

    const uint32_t DepthBindlessIndex = RayTracingDepthSrvBindlessIndices[FrameIndex];
    if (DepthBindlessIndex == UINT32_MAX)
    {
        return UINT32_MAX;
    }

    const auto DepthSrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);

    if (FrameIndex < RayTracingDepthResources.size() && RayTracingDepthResources[FrameIndex] != DepthBuffer)
    {
        Owner.Device->WriteBindlessSrv(DepthBindlessIndex, DepthBuffer, DepthSrvDesc);
        RayTracingDepthResources[FrameIndex] = DepthBuffer;
    }

    return DepthBindlessIndex;
}

uint32_t FRayTracingRuntime::UpdateGBufferSrv(FRenderer& Owner, EGBufferSlot Slot, ID3D12Resource* GBuffer)
{
    if (!GBuffer)
    {
        return UINT32_MAX;
    }

    uint32_t* BindlessIndex = nullptr;
    ID3D12Resource** CachedResource = nullptr;
    switch (Slot)
    {
    case EGBufferSlot::A:
        BindlessIndex = &RayTracingGBufferASrvBindlessIndex;
        CachedResource = &RayTracingGBufferAResource;
        break;
    case EGBufferSlot::B:
        BindlessIndex = &RayTracingGBufferBSrvBindlessIndex;
        CachedResource = &RayTracingGBufferBResource;
        break;
    case EGBufferSlot::C:
        BindlessIndex = &RayTracingGBufferCSrvBindlessIndex;
        CachedResource = &RayTracingGBufferCResource;
        break;
    default:
        return UINT32_MAX;
    }

    const auto SrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(GBuffer->GetDesc().Format, 1);

    if (*BindlessIndex == UINT32_MAX)
    {
        *BindlessIndex = Owner.Device->CreateBindlessSrv(GBuffer, SrvDesc);
    }
    else if (*CachedResource != GBuffer)
    {
        Owner.Device->WriteBindlessSrv(*BindlessIndex, GBuffer, SrvDesc);
    }
    *CachedResource = GBuffer;

    return *BindlessIndex;
}

uint32_t FRayTracingRuntime::UpdateLightingUav(FRenderer& Owner, ID3D12Resource* OutputTarget)
{
    if (!OutputTarget)
    {
        return UINT32_MAX;
    }

    const auto UavDesc = CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(OutputTarget->GetDesc().Format);

    if (RayTracingLightingUavBindlessIndex == UINT32_MAX)
    {
        RayTracingLightingUavBindlessIndex = Owner.Device->CreateBindlessUav(OutputTarget, nullptr, UavDesc);
    }
    else if (RayTracingLightingResource != OutputTarget)
    {
        Owner.Device->WriteBindlessUav(RayTracingLightingUavBindlessIndex, OutputTarget, nullptr, UavDesc);
    }
    RayTracingLightingResource = OutputTarget;

    return RayTracingLightingUavBindlessIndex;
}

uint32_t FRayTracingRuntime::UpdateShadowMaskUav(FRenderer& Owner, ID3D12Resource* ShadowMask)
{
    if (!ShadowMask)
    {
        return UINT32_MAX;
    }

    const auto UavDesc = CD3DX12_UNORDERED_ACCESS_VIEW_DESC::Tex2D(DXGI_FORMAT_R8_UNORM);

    if (RayTracingShadowMaskUavBindlessIndex == UINT32_MAX)
    {
        RayTracingShadowMaskUavBindlessIndex = Owner.Device->CreateBindlessUav(ShadowMask, nullptr, UavDesc);
    }
    else if (RayTracingShadowMaskUavResource != ShadowMask)
    {
        Owner.Device->WriteBindlessUav(RayTracingShadowMaskUavBindlessIndex, ShadowMask, nullptr, UavDesc);
    }
    RayTracingShadowMaskUavResource = ShadowMask;

    return RayTracingShadowMaskUavBindlessIndex;
}

uint32_t FRayTracingRuntime::UpdateShadowMaskSrv(FRenderer& Owner, ID3D12Resource* ShadowMask)
{
    if (!ShadowMask)
    {
        return UINT32_MAX;
    }

    const auto SrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R8_UNORM, 1);

    if (Owner.ShadowMaskBindlessIndex == UINT32_MAX)
    {
        Owner.ShadowMaskBindlessIndex = Owner.Device->CreateBindlessSrv(ShadowMask, SrvDesc);
    }
    else if (Owner.ShadowMaskResource != ShadowMask)
    {
        Owner.Device->WriteBindlessSrv(Owner.ShadowMaskBindlessIndex, ShadowMask, SrvDesc);
    }
    Owner.ShadowMaskResource = ShadowMask;

    return Owner.ShadowMaskBindlessIndex;
}

bool FRayTracingRuntime::CreatePipeline(FRenderer& Owner, FDX12Device* Device)
{
    bRayTracingPipelineReady = false;

    if (!Device->IsRayTracingSupported())
    {
        LogWarning("Ray tracing pipeline skipped: DXR is not supported. Ray-traced features disabled.");
        Owner.SetRayTracedShadowsEnabled(false);
        Owner.ForceDisablePathTracing();
        return true;
    }

    if (!RayTracingDevice.IsValid() && !Device->CreateRayTracingDevice(RayTracingDevice))
    {
        return false;
    }

    CD3DX12_ROOT_PARAMETER RootParameters[3] = {};
    RootParameters[0].InitAsShaderResourceView(0);
    RootParameters[1].InitAsConstantBufferView(0);
    RootParameters[2].InitAsConstants(FRayTracingRuntime::RayQueryRootConstantDwordCount, 1);

    D3D12_ROOT_SIGNATURE_DESC GlobalRootDesc = {};
    GlobalRootDesc.NumParameters = _countof(RootParameters);
    GlobalRootDesc.pParameters = RootParameters;
    GlobalRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
        | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> GlobalSignatureBlob;
    ComPtr<ID3DBlob> GlobalErrorBlob;
    HR_CHECK(D3D12SerializeRootSignature(&GlobalRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, GlobalSignatureBlob.GetAddressOf(), GlobalErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(
        0,
        GlobalSignatureBlob->GetBufferPointer(),
        GlobalSignatureBlob->GetBufferSize(),
        IID_PPV_ARGS(RayQueryRootSignature.ReleaseAndGetAddressOf())));

    FShaderCompiler Compiler;
    RayQueryShadowPipeline.Reset();
    RayQuerySsrFallbackPipeline.Reset();
    RayQuerySsrHwPipeline.Reset();
    RayQueryPathPipeline.Reset();
    RayQueryPathDebugPipeline.Reset();
    RayQueryPathVndfPipeline.Reset();
    RayQueryPathDebugVndfPipeline.Reset();

    std::vector<uint8_t> ShadowBytecode;
    if (!RendererUtils::CompileComputeShader(Compiler, L"cs_6_6", L"Shaders/ShadowRays.hlsl", ShadowBytecode))
    {
        LogError("Failed to compile ray query shadow shader.");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ShadowPsoDesc = {};
    ShadowPsoDesc.pRootSignature = RayQueryRootSignature.Get();
    ShadowPsoDesc.CS.pShaderBytecode = ShadowBytecode.data();
    ShadowPsoDesc.CS.BytecodeLength = ShadowBytecode.size();
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&ShadowPsoDesc, IID_PPV_ARGS(RayQueryShadowPipeline.ReleaseAndGetAddressOf())));
    if (!RayQueryShadowPipeline)
    {
        LogError("Ray query shadow pipeline state creation failed.");
        return false;
    }
    RayQueryShadowPipeline->SetName(L"RayQueryShadowPipeline");

    std::vector<uint8_t> SsrFallbackBytecode;
    if (!RendererUtils::CompileComputeShader(Compiler, L"cs_6_6", L"Shaders/Ssr/SsrRayFallback.hlsl", SsrFallbackBytecode))
    {
        LogError("Failed to compile SSR ray tracing fallback shader.");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC SsrFallbackPsoDesc = {};
    SsrFallbackPsoDesc.pRootSignature = RayQueryRootSignature.Get();
    SsrFallbackPsoDesc.CS.pShaderBytecode = SsrFallbackBytecode.data();
    SsrFallbackPsoDesc.CS.BytecodeLength = SsrFallbackBytecode.size();
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&SsrFallbackPsoDesc, IID_PPV_ARGS(RayQuerySsrFallbackPipeline.ReleaseAndGetAddressOf())));
    if (!RayQuerySsrFallbackPipeline)
    {
        LogError("Ray query SSR fallback pipeline state creation failed.");
        return false;
    }
    RayQuerySsrFallbackPipeline->SetName(L"RayQuerySsrFallbackPipeline");

    std::vector<uint8_t> SsrHwBytecode;
    if (!RendererUtils::CompileComputeShader(Compiler, L"cs_6_6", L"Shaders/Ssr/SsrHWTraceCS.hlsl", SsrHwBytecode))
    {
        LogError("Failed to compile SSR HW trace shader.");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC SsrHwPsoDesc = {};
    SsrHwPsoDesc.pRootSignature = RayQueryRootSignature.Get();
    SsrHwPsoDesc.CS.pShaderBytecode = SsrHwBytecode.data();
    SsrHwPsoDesc.CS.BytecodeLength = SsrHwBytecode.size();
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&SsrHwPsoDesc, IID_PPV_ARGS(RayQuerySsrHwPipeline.ReleaseAndGetAddressOf())));
    if (!RayQuerySsrHwPipeline)
    {
        LogError("Ray query SSR HW trace pipeline state creation failed.");
        return false;
    }
    RayQuerySsrHwPipeline->SetName(L"RayQuerySsrHwPipeline");

    std::vector<uint8_t> PathBytecode;
    const std::vector<std::wstring> PathDefines = { L"PATH_TRACING_USE_VNDF=0" };
    if (!RendererUtils::CompileComputeShader(Compiler, L"cs_6_6", L"Shaders/PathTracing/PathTracing.hlsl", PathBytecode, PathDefines))
    {
        LogError("Failed to compile ray query path tracing shader.");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PathPsoDesc = {};
    PathPsoDesc.pRootSignature = RayQueryRootSignature.Get();
    PathPsoDesc.CS.pShaderBytecode = PathBytecode.data();
    PathPsoDesc.CS.BytecodeLength = PathBytecode.size();
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PathPsoDesc, IID_PPV_ARGS(RayQueryPathPipeline.ReleaseAndGetAddressOf())));
    if (!RayQueryPathPipeline)
    {
        LogError("Ray query path tracing pipeline state creation failed.");
        return false;
    }
    RayQueryPathPipeline->SetName(L"RayQueryPathPipeline");

    std::vector<uint8_t> PathVndfBytecode;
    const std::vector<std::wstring> PathVndfDefines = { L"PATH_TRACING_USE_VNDF=1" };
    if (!RendererUtils::CompileComputeShader(Compiler, L"cs_6_6", L"Shaders/PathTracing/PathTracing.hlsl", PathVndfBytecode, PathVndfDefines))
    {
        LogError("Failed to compile ray query path tracing VNDF shader.");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PathVndfPsoDesc = PathPsoDesc;
    PathVndfPsoDesc.CS.pShaderBytecode = PathVndfBytecode.data();
    PathVndfPsoDesc.CS.BytecodeLength = PathVndfBytecode.size();
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PathVndfPsoDesc, IID_PPV_ARGS(RayQueryPathVndfPipeline.ReleaseAndGetAddressOf())));
    if (!RayQueryPathVndfPipeline)
    {
        LogError("Ray query path tracing VNDF pipeline state creation failed.");
        return false;
    }
    RayQueryPathVndfPipeline->SetName(L"RayQueryPathVndfPipeline");

    std::vector<uint8_t> PathDebugBytecode;
    const std::vector<std::wstring> PathDebugDefines = { L"PATH_TRACING_DEBUG=1", L"PATH_TRACING_USE_VNDF=0" };
    if (!RendererUtils::CompileComputeShader(Compiler, L"cs_6_6", L"Shaders/PathTracing/PathTracing.hlsl", PathDebugBytecode, PathDebugDefines))
    {
        LogError("Failed to compile ray query path tracing debug shader.");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PathDebugPsoDesc = {};
    PathDebugPsoDesc.pRootSignature = RayQueryRootSignature.Get();
    PathDebugPsoDesc.CS.pShaderBytecode = PathDebugBytecode.data();
    PathDebugPsoDesc.CS.BytecodeLength = PathDebugBytecode.size();
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PathDebugPsoDesc, IID_PPV_ARGS(RayQueryPathDebugPipeline.ReleaseAndGetAddressOf())));
    if (!RayQueryPathDebugPipeline)
    {
        LogError("Ray query path tracing debug pipeline state creation failed.");
        return false;
    }
    RayQueryPathDebugPipeline->SetName(L"RayQueryPathDebugPipeline");

    std::vector<uint8_t> PathDebugVndfBytecode;
    const std::vector<std::wstring> PathDebugVndfDefines = { L"PATH_TRACING_DEBUG=1", L"PATH_TRACING_USE_VNDF=1" };
    if (!RendererUtils::CompileComputeShader(Compiler, L"cs_6_6", L"Shaders/PathTracing/PathTracing.hlsl", PathDebugVndfBytecode, PathDebugVndfDefines))
    {
        LogError("Failed to compile ray query path tracing debug VNDF shader.");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PathDebugVndfPsoDesc = PathDebugPsoDesc;
    PathDebugVndfPsoDesc.CS.pShaderBytecode = PathDebugVndfBytecode.data();
    PathDebugVndfPsoDesc.CS.BytecodeLength = PathDebugVndfBytecode.size();
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PathDebugVndfPsoDesc, IID_PPV_ARGS(RayQueryPathDebugVndfPipeline.ReleaseAndGetAddressOf())));
    if (!RayQueryPathDebugVndfPipeline)
    {
        LogError("Ray query path tracing debug VNDF pipeline state creation failed.");
        return false;
    }
    RayQueryPathDebugVndfPipeline->SetName(L"RayQueryPathDebugVndfPipeline");

    RayTracingDepthSrvBindlessIndices.assign(Owner.FramesInFlight, UINT32_MAX);
    RayTracingDepthResources.assign(Owner.FramesInFlight, nullptr);
    RayTracingGBufferASrvBindlessIndex = UINT32_MAX;
    RayTracingGBufferCSrvBindlessIndex = UINT32_MAX;
    RayTracingLightingUavBindlessIndex = UINT32_MAX;
    RayTracingShadowMaskUavBindlessIndex = UINT32_MAX;
    RayTracingGBufferAResource = nullptr;
    RayTracingGBufferCResource = nullptr;
    RayTracingLightingResource = nullptr;
    RayTracingShadowMaskUavResource = nullptr;

    const auto DepthSrvDesc = CD3DX12_SHADER_RESOURCE_VIEW_DESC::Tex2D(DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1);

    for (uint32_t FrameIndex = 0; FrameIndex < Owner.FramesInFlight; ++FrameIndex)
    {
        RayTracingDepthSrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(nullptr, DepthSrvDesc);
        if (RayTracingDepthSrvBindlessIndices[FrameIndex] == UINT32_MAX)
        {
            LogError("Failed to allocate bindless depth SRV for ray tracing.");
            return false;
        }
    }

    bRayTracingPipelineReady = true;
    return true;
}

void FRayTracingRuntime::UpdateBlasRefit(FRenderer& Owner, FDX12CommandContext& CmdContext)
{
    if (!Owner.IsRayTracedShadowsEnabled() && !Owner.IsPathTracingPreferred())
    {
        return;
    }

    if (!Owner.Device->IsRayTracingSupported() || !bRayTracingPipelineReady)
    {
        return;
    }

    if (!RayTracingDevice.IsValid() && !Owner.Device->CreateRayTracingDevice(RayTracingDevice))
    {
        return;
    }

    ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
    if (!CommandList4)
    {
        return;
    }
	FScopedPixEvent BlasRefitEvent(CommandList4, L"UpdateRayTracingBlasRefit");

    bool bHasUpdates = false;
    const auto SkinningBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
    CommandList4->ResourceBarrier(1, &SkinningBarrier);

    const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
    auto DrawSections = Owner.GetWorld().BuildSectionList();

    for (size_t DrawSectionIndex = 0; DrawSectionIndex < DrawSections.size(); ++DrawSectionIndex)
    {
        FMeshSection& Section = DrawSections[DrawSectionIndex];
        if (!Section.bHasRayTracingBlas || !Section.BlasResultBuffer || !Section.BlasScratchBuffer)
        {
            continue;
        }

        const bool bUseSkinning = IsValidBindlessIndex(Section.BoneMatrixBuffer.SrvBindlessIndex) && Section.BoneMatrixCount > 0
            && FrameIndex < Section.SkinnedPositionBuffers.size();
        if (!bUseSkinning)
        {
            continue;
        }

        if (!Section.bSkinningUpdatedThisFrame)
        {
            continue;
        }
        if (!Section.bSkinningVisible)
        {
            continue;
        }

        ID3D12Resource* SkinnedBuffer = Section.SkinnedPositionBuffers[FrameIndex].Get();
        if (!SkinnedBuffer)
        {
            continue;
        }

        Section.BlasGeometryDesc.Triangles.VertexBuffer.StartAddress = SkinnedBuffer->GetGPUVirtualAddress();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
        Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        Inputs.NumDescs = 1;
        Inputs.pGeometryDescs = &Section.BlasGeometryDesc;
        Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BuildDesc = {};
        BuildDesc.Inputs = Inputs;
        BuildDesc.DestAccelerationStructureData = Section.BlasResultBuffer->GetGPUVirtualAddress();
        BuildDesc.SourceAccelerationStructureData = Section.BlasResultBuffer->GetGPUVirtualAddress();
        BuildDesc.ScratchAccelerationStructureData = Section.BlasScratchBuffer->GetGPUVirtualAddress();

        CommandList4->BuildRaytracingAccelerationStructure(&BuildDesc, 0, nullptr);

        const auto Barrier = CD3DX12_RESOURCE_BARRIER::UAV(Section.BlasResultBuffer.Get());
        CommandList4->ResourceBarrier(1, &Barrier);
        bHasUpdates = true;
    }

    if (bHasUpdates)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        CommandList4->ResourceBarrier(1, &Barrier);
    }
}

void FRayTracingRuntime::BuildTlas(FRenderer& Owner, FDX12CommandContext& CmdContext)
{
    if (!Owner.IsRayTracedShadowsEnabled() && !Owner.IsPathTracingPreferred())
    {
        return;
    }

    if (!Owner.Device->IsRayTracingSupported() || !bRayTracingPipelineReady)
    {
        return;
    }

    if (!RayTracingDevice.IsValid() && !Owner.Device->CreateRayTracingDevice(RayTracingDevice))
    {
        return;
    }

    ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
    if (!CommandList4)
    {
        return;
    }

    auto DrawSections = Owner.GetWorld().BuildSectionList();

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
    Instances.reserve(DrawSections.size());

    // Build instance data buffer for path tracing
    struct FPathTracingInstanceData
    {
        uint32_t PositionBufferIndex;
        uint32_t NormalBufferIndex;
        uint32_t UVBufferIndex;
        uint32_t IndexBufferIndex;
        uint32_t TangentBufferIndex;
        uint32_t BaseColorTextureIndex;
        uint32_t NormalTextureIndex;
        uint32_t MetallicRoughnessTextureIndex;
        uint32_t Flags;
        uint32_t EmissiveTextureIndex;
        uint32_t Padding0;
        uint32_t Padding1;
        DirectX::XMFLOAT4 EmissiveFactor;
        DirectX::XMFLOAT4 BaseColorFactorAndAlpha;
        DirectX::XMFLOAT4 MetallicRoughnessAlphaCutoff;
        DirectX::XMFLOAT4X4 WorldInverseTranspose;
    };

    static_assert((sizeof(FPathTracingInstanceData) % 16u) == 0u, "FPathTracingInstanceData must be 16-byte aligned in size");

    static_assert((offsetof(FPathTracingInstanceData, EmissiveTextureIndex) % 4u) == 0u, "Invalid EmissiveTextureIndex alignment");
    static_assert((offsetof(FPathTracingInstanceData, EmissiveFactor) % 16u) == 0u, "Invalid EmissiveFactor alignment");
    static_assert((offsetof(FPathTracingInstanceData, BaseColorFactorAndAlpha) % 16u) == 0u, "Invalid BaseColorFactorAndAlpha alignment");

    std::vector<FPathTracingInstanceData> InstanceDataArray;
    InstanceDataArray.reserve(DrawSections.size());

    uint32_t InstanceId = 0;
    for (size_t DrawSectionIndex = 0; DrawSectionIndex < DrawSections.size(); ++DrawSectionIndex)
    {
        const FMeshSection& Section = DrawSections[DrawSectionIndex];
        if (!Section.bHasRayTracingBlas || !Section.BlasResultBuffer)
        {
            continue;
        }
        if (!Section.bVisible)
        {
            continue;
        }

        D3D12_RAYTRACING_INSTANCE_DESC InstanceDesc = {};
        InstanceDesc.InstanceID = InstanceId++;
        InstanceDesc.InstanceMask = 0xFF;
        InstanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        InstanceDesc.AccelerationStructure = Section.BlasResultBuffer->GetGPUVirtualAddress();

        DirectX::XMFLOAT4X4 World = {};
        const DirectX::XMFLOAT4X4& SectionObjectWorld = DrawSections.GetView(DrawSectionIndex).Object->GetWorldMatrix();
        DirectX::XMStoreFloat4x4(&World, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&SectionObjectWorld)));
        InstanceDesc.Transform[0][0] = World._11;
        InstanceDesc.Transform[0][1] = World._12;
        InstanceDesc.Transform[0][2] = World._13;
        InstanceDesc.Transform[0][3] = World._14;
        InstanceDesc.Transform[1][0] = World._21;
        InstanceDesc.Transform[1][1] = World._22;
        InstanceDesc.Transform[1][2] = World._23;
        InstanceDesc.Transform[1][3] = World._24;
        InstanceDesc.Transform[2][0] = World._31;
        InstanceDesc.Transform[2][1] = World._32;
        InstanceDesc.Transform[2][2] = World._33;
        InstanceDesc.Transform[2][3] = World._34;

        Instances.push_back(InstanceDesc);

        // Build instance data for path tracing (same order as TLAS instances)
        FPathTracingInstanceData InstData = {};
        InstData.PositionBufferIndex = Section.Geometry.VertexBuffers[kMeshVertexStreamPosition].SrvBindlessIndex;
        InstData.NormalBufferIndex = Section.Geometry.VertexBuffers[kMeshVertexStreamNormal].SrvBindlessIndex;
        InstData.UVBufferIndex = Section.Geometry.VertexBuffers[kMeshVertexStreamUv].SrvBindlessIndex;
        InstData.IndexBufferIndex = Section.Geometry.IndexBuffer.SrvBindlessIndex;
        InstData.TangentBufferIndex = Section.Geometry.VertexBuffers[kMeshVertexStreamTangent].SrvBindlessIndex;
        InstData.BaseColorTextureIndex = Section.Material.BaseColor.SrvBindlessIndex;
        InstData.NormalTextureIndex = Section.Material.Normal.SrvBindlessIndex;
        InstData.MetallicRoughnessTextureIndex = Section.Material.MetallicRoughness.SrvBindlessIndex;
        InstData.Flags = Section.Material.bDoubleSided ? 1u : 0u;
        InstData.EmissiveTextureIndex = Section.Material.Emissive.SrvBindlessIndex;
        InstData.Padding0 = 0u;
        InstData.Padding1 = 0u;
        InstData.EmissiveFactor = DirectX::XMFLOAT4(Section.Material.EmissiveFactor.x, Section.Material.EmissiveFactor.y, Section.Material.EmissiveFactor.z, 0.0f);
        InstData.BaseColorFactorAndAlpha = DirectX::XMFLOAT4(
            Section.Material.BaseColorFactor.x,
            Section.Material.BaseColorFactor.y,
            Section.Material.BaseColorFactor.z,
            Section.Material.BaseColorAlpha);
        InstData.MetallicRoughnessAlphaCutoff = DirectX::XMFLOAT4(
            Section.Material.MetallicFactor,
            Section.Material.RoughnessFactor,
            Section.Material.AlphaCutoff,
            0.0f);
        DirectX::XMMATRIX WorldMatrix = DirectX::XMLoadFloat4x4(&SectionObjectWorld);
        DirectX::XMMATRIX WorldInverseTranspose = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, WorldMatrix));
        DirectX::XMStoreFloat4x4(&InstData.WorldInverseTranspose, WorldInverseTranspose);

        InstanceDataArray.push_back(InstData);
    }

    if (Instances.empty())
    {
        return;
    }

    const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();

    if (PathTracingInstanceDataBuffers.size() != Owner.GetFramesInFlight())
    {
        PathTracingInstanceDataBuffers.resize(Owner.GetFramesInFlight());
    }

    // Create or update the per-frame instance data buffer
    const uint64_t InstanceDataBufferSize = sizeof(FPathTracingInstanceData) * InstanceDataArray.size();
    if (!PathTracingInstanceDataBuffers[FrameIndex] || PathTracingInstanceDataBuffers[FrameIndex].Size < InstanceDataBufferSize)
    {
        CreateMappedUploadBuffer(Owner.Device,
            L"PathTracingInstanceDataBuffer_Frame_" + std::to_wstring(FrameIndex),
            InstanceDataBufferSize,
            PathTracingInstanceDataBuffers[FrameIndex]);

        CD3DX12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
        SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        SrvDesc.Buffer.NumElements = static_cast<UINT>(InstanceDataArray.size());
        SrvDesc.Buffer.StructureByteStride = sizeof(FPathTracingInstanceData);
        SrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        PathTracingInstanceDataBuffers[FrameIndex].SrvBindlessIndex =
            Owner.Device->CreateBindlessSrv(PathTracingInstanceDataBuffers[FrameIndex].Get(), SrvDesc);
    }

    // Update buffer data
    if (PathTracingInstanceDataBuffers[FrameIndex].MappedData)
    {
        std::memcpy(PathTracingInstanceDataBuffers[FrameIndex].MappedData,
            InstanceDataArray.data(), sizeof(FPathTracingInstanceData) * InstanceDataArray.size());
    }

    if (TlasScratchBuffers.size() != Owner.GetFramesInFlight())
    {
        TlasScratchBuffers.resize(Owner.GetFramesInFlight());
        TlasResultBuffers.resize(Owner.GetFramesInFlight());
        TlasInstanceBuffers.resize(Owner.GetFramesInFlight());
        TlasBuilt.assign(Owner.GetFramesInFlight(), false);
        TlasPrevInstanceCount.assign(Owner.GetFramesInFlight(), 0);
        TlasPrevInstanceHash.assign(Owner.GetFramesInFlight(), 0);
        TlasInstanceCapacity = 0;
    }

    const uint32_t InstanceCount = static_cast<uint32_t>(Instances.size());
    if (InstanceCount > TlasInstanceCapacity)
    {
        TlasInstanceCapacity = InstanceCount;
    }


    if (TlasPrevInstanceCount.size() != Owner.GetFramesInFlight())
    {
        TlasPrevInstanceCount.assign(Owner.GetFramesInFlight(), 0);
    }
    if (TlasPrevInstanceHash.size() != Owner.GetFramesInFlight())
    {
        TlasPrevInstanceHash.assign(Owner.GetFramesInFlight(), 0);
    }

    const uint64_t CurrentInstanceHash = ComputeTlasInstanceHash(Instances);

    const bool bInstanceCountChanged = TlasPrevInstanceCount[FrameIndex] != InstanceCount;
    const bool bInstanceCompositionChanged = TlasPrevInstanceHash[FrameIndex] != CurrentInstanceHash;
    if (TlasBuilt[FrameIndex] && (bInstanceCountChanged || bInstanceCompositionChanged))
    {
        TlasBuilt[FrameIndex] = false;
    }
    TlasPrevInstanceCount[FrameIndex] = InstanceCount;
    TlasPrevInstanceHash[FrameIndex] = CurrentInstanceHash;

    const uint64_t InstanceBufferSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * TlasInstanceCapacity;
    if (!TlasInstanceBuffers[FrameIndex] || TlasInstanceBuffers[FrameIndex].Size < InstanceBufferSize)
    {
        CreateMappedUploadBuffer(Owner.Device, L"TLAS_InstanceBuffer", InstanceBufferSize, TlasInstanceBuffers[FrameIndex]);
    }

    std::memcpy(TlasInstanceBuffers[FrameIndex].MappedData, Instances.data(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * Instances.size());

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
    Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    Inputs.InstanceDescs = TlasInstanceBuffers[FrameIndex].GetGPUVirtualAddress();
    Inputs.NumDescs = InstanceCount;
    Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO PrebuildInfo = {};
    RayTracingDevice.GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &PrebuildInfo);

    const uint64_t MaxScratchSize = (std::max)(PrebuildInfo.ScratchDataSizeInBytes, PrebuildInfo.UpdateScratchDataSizeInBytes);
    const uint64_t ScratchSize = (MaxScratchSize + 255) & ~255ull;
    const uint64_t ResultSize = (PrebuildInfo.ResultDataMaxSizeInBytes + 255) & ~255ull;

    const auto EnsureBuffer = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& Buffer, uint64_t Size, D3D12_RESOURCE_STATES State, const wchar_t* Name)
    {
        if (Buffer && Buffer->GetDesc().Width >= Size)
        {
            return;
        }
        CreateRayTracingBuffer(Owner.Device, Size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, State, Buffer, Name);
    };

    EnsureBuffer(TlasScratchBuffers[FrameIndex], ScratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"TLAS_Scratch");
    EnsureBuffer(TlasResultBuffers[FrameIndex], ResultSize, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"TLAS_Result");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BuildDesc = {};
    BuildDesc.Inputs = Inputs;
    BuildDesc.DestAccelerationStructureData = TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress();
    BuildDesc.ScratchAccelerationStructureData = TlasScratchBuffers[FrameIndex]->GetGPUVirtualAddress();

    if (TlasBuilt[FrameIndex])
    {
        BuildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        BuildDesc.SourceAccelerationStructureData = TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress();
    }

	FScopedPixEvent TlasEvent(CommandList4, L"Build TLAS");
    CommandList4->BuildRaytracingAccelerationStructure(&BuildDesc, 0, nullptr);

    const auto Barrier = CD3DX12_RESOURCE_BARRIER::UAV(TlasResultBuffers[FrameIndex].Get());
    CommandList4->ResourceBarrier(1, &Barrier);

    TlasBuilt[FrameIndex] = true;
}
