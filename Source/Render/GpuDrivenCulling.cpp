#include "Renderer.h"
#include "DeferredRenderer.h"
#include "Deferred/DeferredPassContext.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include "../Scene/Camera.h"

#include <d3dx12.h>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t kGpuCullingBindlessDwordCount   = 32;
    constexpr uint32_t kMeshletRunBindlessDwordCount   = 8;
    constexpr uint32_t kVisibilityListBindlessDwordCount = 8;

    struct FClearCountsConstants
    {
        uint32_t Count0Index;
        uint32_t Count1Index;
    };
    static_assert(sizeof(FClearCountsConstants) / sizeof(uint32_t) <= kVisibilityListBindlessDwordCount);

    bool ArePerFrameMappedUploadBuffersValid(const std::vector<FMappedUploadBuffer>& Buffers, size_t ExpectedFrameCount)
    {
        if (Buffers.size() != ExpectedFrameCount)
        {
            return false;
        }

        for (const FMappedUploadBuffer& Buffer : Buffers)
        {
            if (!Buffer)
            {
                return false;
            }
        }

        return true;
    }

    bool ArePerFrameBindlessBuffersValid(const std::vector<FBindlessBuffer>& Buffers, size_t ExpectedFrameCount, bool bRequireSrv, bool bRequireUav)
    {
        if (Buffers.size() != ExpectedFrameCount)
        {
            return false;
        }

        for (const FBindlessBuffer& Buffer : Buffers)
        {
            if (!Buffer)
            {
                return false;
            }

            if (bRequireSrv && !Buffer.HasSrv())
            {
                return false;
            }

            if (bRequireUav && !Buffer.HasUav())
            {
                return false;
            }
        }

        return true;
    }

    const FBindlessBuffer* GetPerFrameBuffer(const std::vector<FBindlessBuffer>& Buffers, uint32_t FrameIndex)
    {
        return FrameIndex < Buffers.size() ? &Buffers[FrameIndex] : nullptr;
    }

    FBindlessBuffer* GetPerFrameBuffer(std::vector<FBindlessBuffer>& Buffers, uint32_t FrameIndex)
    {
        return FrameIndex < Buffers.size() ? &Buffers[FrameIndex] : nullptr;
    }

    uint32_t GetPerFrameSrvIndex(const std::vector<FBindlessBuffer>& Buffers, uint32_t FrameIndex)
    {
        const FBindlessBuffer* Buffer = GetPerFrameBuffer(Buffers, FrameIndex);
        return Buffer ? Buffer->SrvBindlessIndex : UINT32_MAX;
    }

    uint32_t GetPerFrameUavIndex(const std::vector<FBindlessBuffer>& Buffers, uint32_t FrameIndex)
    {
        const FBindlessBuffer* Buffer = GetPerFrameBuffer(Buffers, FrameIndex);
        return Buffer ? Buffer->UavBindlessIndex : UINT32_MAX;
    }

    void ResetBindlessBufferVector(std::vector<FBindlessBuffer>& Buffers, uint32_t FramesInFlight)
    {
        Buffers.clear();
        Buffers.resize(FramesInFlight);
    }

    void ResetBindlessBufferStates(std::vector<FBindlessBuffer>& Buffers, D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON)
    {
        for (FBindlessBuffer& Buffer : Buffers)
        {
            Buffer.State = State;
        }
    }
}

void FGpuDrivenCulling::ConfigureHZBOcclusion(bool bEnabled, uint32_t HZBBindlessIndex, uint32_t Width, uint32_t Height, uint32_t MipCount)
{
    HZBCullingBindlessIndex = HZBBindlessIndex;
    HZBCullingWidth = Width;
    HZBCullingHeight = Height;
    HZBCullingMipCount = MipCount;
    bHZBOcclusionEnabled = bEnabled && IsValidBindlessIndex(HZBCullingBindlessIndex)
        && HZBCullingWidth > 0 && HZBCullingHeight > 0 && HZBCullingMipCount > 0;
}

ID3D12Resource* FGpuDrivenCulling::GetCullingConstantBuffer(uint32_t FrameIndex, bool bLatePass) const
{
    const auto& Buffers = bLatePass ? CullingConstantBuffersLate : CullingConstantBuffers;
    return FrameIndex < Buffers.size() ? Buffers[FrameIndex].Get() : nullptr;
}

D3D12_GPU_VIRTUAL_ADDRESS FGpuDrivenCulling::GetCullingConstantBufferAddress(uint32_t FrameIndex, bool bLatePass) const
{
    const auto& Buffers = bLatePass ? CullingConstantBuffersLate : CullingConstantBuffers;
    return FrameIndex < Buffers.size() ? Buffers[FrameIndex].GetGPUVirtualAddress() : 0;
}

uint8_t* FGpuDrivenCulling::GetCullingConstantBufferMapped(uint32_t FrameIndex, bool bLatePass) const
{
    const auto& Buffers = bLatePass ? CullingConstantBuffersLate : CullingConstantBuffers;
    return FrameIndex < Buffers.size() ? Buffers[FrameIndex].MappedData : nullptr;
}

ID3D12Resource* FGpuDrivenCulling::GetIndirectCommandBuffer(uint32_t FrameIndex) const
{
    const FBindlessBuffer* Buffer = GetPerFrameBuffer(IndirectCommandBuffers, FrameIndex);
    return Buffer ? Buffer->Get() : nullptr;
}

ID3D12Resource* FGpuDrivenCulling::GetIndirectCommandTemplateBuffer(uint32_t FrameIndex) const
{
    const FBindlessBuffer* Buffer = GetPerFrameBuffer(IndirectCommandTemplateBuffers, FrameIndex);
    return Buffer ? Buffer->Get() : nullptr;
}

ID3D12Resource* FGpuDrivenCulling::GetMeshletRunCountBuffer(uint32_t FrameIndex) const
{
    const FBindlessBuffer* Buffer = GetPerFrameBuffer(MeshletRunCountBuffers, FrameIndex);
    return Buffer ? Buffer->Get() : nullptr;
}

D3D12_RESOURCE_STATES* FGpuDrivenCulling::GetIndirectCommandState(uint32_t FrameIndex)
{
    FBindlessBuffer* Buffer = GetPerFrameBuffer(IndirectCommandBuffers, FrameIndex);
    return Buffer ? &Buffer->State : nullptr;
}

D3D12_RESOURCE_STATES* FGpuDrivenCulling::GetMeshletRunCountState(uint32_t FrameIndex)
{
    FBindlessBuffer* Buffer = GetPerFrameBuffer(MeshletRunCountBuffers, FrameIndex);
    return Buffer ? &Buffer->State : nullptr;
}

FGpuDrivenCulling::FGpuCullingDispatchFrameData FGpuDrivenCulling::GetDispatchFrameData(uint32_t FrameIndex)
{
    FGpuCullingDispatchFrameData Data;
    Data.IndirectBuffer = GetIndirectCommandBuffer(FrameIndex);
    Data.RunCountBuffer = GetMeshletRunCountBuffer(FrameIndex);
    Data.IndirectState = GetIndirectCommandState(FrameIndex);
    Data.RunCountState = GetMeshletRunCountState(FrameIndex);
    Data.IndirectUavBindlessIndex = GetPerFrameUavIndex(IndirectCommandBuffers, FrameIndex);
    Data.TemplateSrvBindlessIndex = GetPerFrameSrvIndex(IndirectCommandTemplateBuffers, FrameIndex);
    Data.RunCountUavBindlessIndex = GetPerFrameUavIndex(MeshletRunCountBuffers, FrameIndex);
    return Data;
}

void FGpuDrivenCulling::RefreshCullingPersistentValidation(uint32_t FramesInFlight)
{
    const size_t ExpectedFrameCount = static_cast<size_t>(FramesInFlight);
    bCullingDispatchPersistentInputsValid =
        CullingRootSignature &&
        CullingPipeline &&
        CullingListPipeline &&
        MeshletRunRootSignature &&
        MeshletRunClearPipeline &&
        MeshletRunAppendPipeline &&
        ArePerFrameMappedUploadBuffersValid(CullingConstantBuffers, ExpectedFrameCount) &&
        ArePerFrameMappedUploadBuffersValid(CullingConstantBuffersLate, ExpectedFrameCount) &&
        ArePerFrameBindlessBuffersValid(IndirectCommandBuffers, ExpectedFrameCount, false, true) &&
        ArePerFrameBindlessBuffersValid(IndirectCommandTemplateBuffers, ExpectedFrameCount, true, false) &&
        ArePerFrameBindlessBuffersValid(MeshletRunCountBuffers, ExpectedFrameCount, false, true);

    bSharedInputPersistentInputsValid =
        ModelBoundsBuffer &&
        MeshletDrawDataBuffer &&
        MeshletRangeOffsetBuffer &&
        MeshletConeAxisBuffer &&
        MeshletConeApexBuffer &&
        ModelBoundsBuffer.HasSrv() &&
        MeshletDrawDataBuffer.HasSrv() &&
        MeshletRangeOffsetBuffer.HasSrv() &&
        MeshletConeAxisBuffer.HasSrv() &&
        MeshletConeApexBuffer.HasSrv();
}

bool FGpuDrivenCulling::HasCullingPipelines() const
{
    return CullingRootSignature
        && CullingPipeline
        && CullingListPipeline
        && MeshletRunRootSignature
        && MeshletRunClearPipeline
        && MeshletRunAppendPipeline;
}

void FGpuDrivenCulling::FillDispatchSharedIndices(FGpuCullingDispatchIndices& Indices) const
{
    Indices.MeshletDrawDataIndex = MeshletDrawDataBuffer.SrvBindlessIndex;
    Indices.RangeOffsetsIndex = MeshletRangeOffsetBuffer.SrvBindlessIndex;
    Indices.ModelBoundsIndex = ModelBoundsBuffer.SrvBindlessIndex;
    Indices.MeshletConeAxisIndex = MeshletConeAxisBuffer.SrvBindlessIndex;
    Indices.MeshletConeApexIndex = MeshletConeApexBuffer.SrvBindlessIndex;
}

FGpuDrivenCulling::FMeshletVisibilityFrameData FGpuDrivenCulling::GetMeshletVisibilityFrameData(uint32_t FrameIndex, bool bLatePass) const
{
    const auto& Buffers = bLatePass ? MeshletVisibilityLateBuffers : MeshletVisibilityBuffers;

    FMeshletVisibilityFrameData Data;
    if (const FBindlessBuffer* Buffer = GetPerFrameBuffer(Buffers, FrameIndex))
    {
        Data.Buffer = Buffer->Get();
        Data.UavBindlessIndex = Buffer->UavBindlessIndex;
        Data.SrvBindlessIndex = Buffer->SrvBindlessIndex;
    }
    return Data;
}

D3D12_RESOURCE_STATES* FGpuDrivenCulling::GetMeshletVisibilityState(uint32_t FrameIndex, bool bLatePass)
{
    auto& Buffers = bLatePass ? MeshletVisibilityLateBuffers : MeshletVisibilityBuffers;
    FBindlessBuffer* Buffer = GetPerFrameBuffer(Buffers, FrameIndex);
    return Buffer ? &Buffer->State : nullptr;
}

FGpuDrivenCulling::FVisibilityListBuildIndices FGpuDrivenCulling::GetVisibilityListBuildIndices(uint32_t FrameIndex) const
{
    FVisibilityListBuildIndices Indices;
    Indices.PrevVisibleListUav = GetPerFrameUavIndex(PrevVisibleListBuffers, FrameIndex);
    Indices.PrevInvisibleListUav = GetPerFrameUavIndex(PrevInvisibleListBuffers, FrameIndex);
    Indices.PrevVisibleCountUav = GetPerFrameUavIndex(PrevVisibleCountBuffers, FrameIndex);
    Indices.PrevInvisibleCountUav = GetPerFrameUavIndex(PrevInvisibleCountBuffers, FrameIndex);
    return Indices;
}

FGpuDrivenCulling::FEarlyRejectListIndices FGpuDrivenCulling::GetEarlyRejectListIndices(uint32_t FrameIndex) const
{
    FEarlyRejectListIndices Indices;
    Indices.RejectListUav = GetPerFrameUavIndex(EarlyRejectListBuffers, FrameIndex);
    Indices.RejectCountUav = GetPerFrameUavIndex(EarlyRejectCountBuffers, FrameIndex);
    return Indices;
}

FGpuDrivenCulling::FLateMergeVisibilityListIndices FGpuDrivenCulling::GetLateMergeVisibilityListIndices(uint32_t FrameIndex) const
{
    FLateMergeVisibilityListIndices Indices;
    Indices.PrevInvisibleListSrv = GetPerFrameSrvIndex(PrevInvisibleListBuffers, FrameIndex);
    Indices.EarlyRejectListSrv = GetPerFrameSrvIndex(EarlyRejectListBuffers, FrameIndex);
    Indices.PrevInvisibleCountSrv = GetPerFrameSrvIndex(PrevInvisibleCountBuffers, FrameIndex);
    Indices.EarlyRejectCountSrv = GetPerFrameSrvIndex(EarlyRejectCountBuffers, FrameIndex);
    Indices.LateListUav = GetPerFrameUavIndex(LateListBuffers, FrameIndex);
    Indices.LateListCountUav = GetPerFrameUavIndex(LateListCountBuffers, FrameIndex);
    Indices.LateListFlagUav = GetPerFrameUavIndex(LateListFlagBuffers, FrameIndex);
    return Indices;
}

FGpuDrivenCulling::FVisibilityListFrameSrvIndices FGpuDrivenCulling::GetVisibilityListFrameSrvIndices(uint32_t FrameIndex) const
{
    FVisibilityListFrameSrvIndices Indices;
    Indices.PrevVisibleListSrv = GetPerFrameSrvIndex(PrevVisibleListBuffers, FrameIndex);
    Indices.PrevVisibleCountSrv = GetPerFrameSrvIndex(PrevVisibleCountBuffers, FrameIndex);
    Indices.LateListSrv = GetPerFrameSrvIndex(LateListBuffers, FrameIndex);
    Indices.LateListCountSrv = GetPerFrameSrvIndex(LateListCountBuffers, FrameIndex);
    return Indices;
}

void FGpuDrivenCulling::RefreshVisibilityPersistentValidation(uint32_t FramesInFlight)
{
    const size_t ExpectedFrameCount = static_cast<size_t>(FramesInFlight);
    bMeshletVisibilityPersistentInputsValid =
        ArePerFrameBindlessBuffersValid(MeshletVisibilityBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(MeshletVisibilityLateBuffers, ExpectedFrameCount, true, true);

    bVisibilityListPersistentInputsValid =
        VisibilityListRootSignature &&
        BuildVisibilityListsPipeline &&
        BuildEarlyRejectListPipeline &&
        MergeVisibilityListsPipeline &&
        ClearVisibilityCountsPipeline &&
        ClearVisibilityFlagsPipeline &&
        ArePerFrameBindlessBuffersValid(PrevVisibleListBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(PrevInvisibleListBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(EarlyRejectListBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(LateListBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(LateListFlagBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(PrevVisibleCountBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(PrevInvisibleCountBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(EarlyRejectCountBuffers, ExpectedFrameCount, true, true) &&
        ArePerFrameBindlessBuffersValid(LateListCountBuffers, ExpectedFrameCount, true, true);
}

void FGpuDrivenCulling::ResetVisibilityResources(uint32_t FramesInFlight)
{
    bCullingDispatchPersistentInputsValid = false;
    bMeshletVisibilityPersistentInputsValid = false;
    bVisibilityListPersistentInputsValid = false;

    ResetBindlessBufferVector(MeshletVisibilityBuffers, FramesInFlight);
    ResetBindlessBufferVector(MeshletVisibilityLateBuffers, FramesInFlight);
    ResetBindlessBufferVector(PrevVisibleListBuffers, FramesInFlight);
    ResetBindlessBufferVector(PrevInvisibleListBuffers, FramesInFlight);
    ResetBindlessBufferVector(EarlyRejectListBuffers, FramesInFlight);
    ResetBindlessBufferVector(LateListBuffers, FramesInFlight);
    ResetBindlessBufferVector(LateListFlagBuffers, FramesInFlight);
    ResetBindlessBufferVector(PrevVisibleCountBuffers, FramesInFlight);
    ResetBindlessBufferVector(PrevInvisibleCountBuffers, FramesInFlight);
    ResetBindlessBufferVector(EarlyRejectCountBuffers, FramesInFlight);
    ResetBindlessBufferVector(LateListCountBuffers, FramesInFlight);
}

bool FGpuDrivenCulling::CreateCullingConstantBuffers(FDX12Device* Device, uint32_t FramesInFlight)
{
    bCullingDispatchPersistentInputsValid = false;

    CullingConstantBuffers.clear();
    CullingConstantBuffersLate.clear();
    CullingConstantBuffers.resize(FramesInFlight);
    CullingConstantBuffersLate.resize(FramesInFlight);

    constexpr uint64_t CullingConstantSize = sizeof(FGpuCullingConstants);

    for (uint32_t Index = 0; Index < FramesInFlight; ++Index)
    {
        const std::wstring Name = L"CullingConstantBuffer_Frame" + std::to_wstring(Index);
        if (!CreateMappedUploadBuffer(Device, Name, CullingConstantSize, CullingConstantBuffers[Index]))
        {
            return false;
        }

        const std::wstring LateName = L"CullingConstantLateBuffer_Frame" + std::to_wstring(Index);
        if (!CreateMappedUploadBuffer(Device, LateName, CullingConstantSize, CullingConstantBuffersLate[Index]))
        {
            return false;
        }
    }

    RefreshCullingPersistentValidation(FramesInFlight);
    return true;
}

bool FGpuDrivenCulling::CreatePerFrameCullingResources(FDX12Device* Device, uint32_t FramesInFlight, uint64_t CommandBufferSize, uint32_t IndirectCommandCount, uint32_t RangeCount)
{
    bCullingDispatchPersistentInputsValid = false;

    ResetBindlessBufferVector(IndirectCommandBuffers, FramesInFlight);
    ResetBindlessBufferVector(IndirectCommandTemplateBuffers, FramesInFlight);
    ResetBindlessBufferVector(MeshletRunCountBuffers, FramesInFlight);

    const uint64_t SafeCommandBufferSize = (std::max)(CommandBufferSize, 4ull);
    const FRGBufferDesc BufferDesc = CreateRawBufferDesc(SafeCommandBufferSize, static_cast<uint32_t>(CommandBufferSize / 4), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const FRGBufferDesc TemplateDesc = CreateRawBufferDesc(SafeCommandBufferSize, static_cast<uint32_t>(CommandBufferSize / 4));
    const FRGBufferDesc RunCountDesc = CreateRWStructuredBufferDesc(
        (std::max)(static_cast<uint64_t>(sizeof(uint32_t)) * RangeCount, 4ull),
        RangeCount,
        static_cast<uint32_t>(sizeof(uint32_t)));

    for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
    {
        CreateBindlessBuffer(
            Device,
            L"IndirectCommandBuffer_Frame" + std::to_wstring(FrameIndex),
            BufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            IndirectCommandBuffers[FrameIndex],
            false,
            true);
        CreateBindlessBuffer(
            Device,
            L"IndirectCommandTemplateBuffer_Frame" + std::to_wstring(FrameIndex),
            TemplateDesc,
            D3D12_RESOURCE_STATE_COMMON,
            IndirectCommandTemplateBuffers[FrameIndex],
            true,
            false);
        CreateBindlessBuffer(
            Device,
            L"MeshletRunCountBuffer_Frame" + std::to_wstring(FrameIndex),
            RunCountDesc,
            D3D12_RESOURCE_STATE_COMMON,
            MeshletRunCountBuffers[FrameIndex],
            false,
            true);
    }

    RefreshCullingPersistentValidation(FramesInFlight);
    return true;
}

void FGpuDrivenCulling::ResetCullingStatesToCommon(uint32_t FramesInFlight)
{
    ResetBindlessBufferStates(IndirectCommandBuffers);
    ResetBindlessBufferStates(MeshletRunCountBuffers);
}

bool FGpuDrivenCulling::CreateCullingPipelines(FDX12Device* Device)
{
    bCullingDispatchPersistentInputsValid = false;

    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kGpuCullingBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc;
    VersionedRootDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(CullingRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/CullMeshletVisibility.hlsl", CsByteCode))
    {
        LogError("Failed to compile culling compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC CsDesc = {};
    CsDesc.pRootSignature = CullingRootSignature.Get();
    CsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&CsDesc, IID_PPV_ARGS(CullingPipeline.GetAddressOf())));

    CsByteCode.clear();
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/CullMeshletVisibilityList.hlsl", CsByteCode))
    {
        LogError("Failed to compile list culling compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ListDesc = {};
    ListDesc.pRootSignature = CullingRootSignature.Get();
    ListDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&ListDesc, IID_PPV_ARGS(CullingListPipeline.GetAddressOf())));

    CD3DX12_ROOT_PARAMETER1 RunRootParams[2] = {};
    RunRootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RunRootParams[1].InitAsConstants(kMeshletRunBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RunVersionedDesc;
    RunVersionedDesc.Init_1_1(_countof(RunRootParams), RunRootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> RunSerializedSig;
    ComPtr<ID3DBlob> RunErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RunVersionedDesc, RunSerializedSig.GetAddressOf(), RunErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, RunSerializedSig->GetBufferPointer(), RunSerializedSig->GetBufferSize(), IID_PPV_ARGS(MeshletRunRootSignature.GetAddressOf())));

    std::vector<uint8_t> ClearByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/ClearMeshletRunCounts.hlsl", ClearByteCode))
    {
        LogError("Failed to compile meshlet run clear compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ClearDesc = {};
    ClearDesc.pRootSignature = MeshletRunRootSignature.Get();
    ClearDesc.CS = { ClearByteCode.data(), ClearByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&ClearDesc, IID_PPV_ARGS(MeshletRunClearPipeline.GetAddressOf())));

    std::vector<uint8_t> RunByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/BuildMeshletRunsAppend.hlsl", RunByteCode))
    {
        LogError("Failed to compile meshlet run append compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC RunDesc = {};
    RunDesc.pRootSignature = MeshletRunRootSignature.Get();
    RunDesc.CS = { RunByteCode.data(), RunByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&RunDesc, IID_PPV_ARGS(MeshletRunAppendPipeline.GetAddressOf())));

    return true;
}

void FGpuDrivenCulling::ResetSharedInputResources()
{
    bSharedInputPersistentInputsValid = false;

    MeshletDrawDataBuffer = {};
    MeshletDrawDataUpload = {};
    MeshletRangeOffsetBuffer = {};
    MeshletRangeOffsetUpload = {};
    ModelBoundsBuffer = {};
    ModelBoundsUpload = {};
    MeshletConeAxisBuffer = {};
    MeshletConeAxisUpload = {};
    MeshletConeApexBuffer = {};
    MeshletConeApexUpload = {};

    BoundsSizeInBytes = 0;
    MeshletDrawDataSizeInBytes = 0;
    RangeOffsetsSizeInBytes = 0;
    ConeAxisSizeInBytes = 0;
    ConeApexSizeInBytes = 0;
}

bool FGpuDrivenCulling::CreateSharedInputResources(FDX12Device* Device, const FGpuCullingSharedInputData& Data)
{
    ResetSharedInputResources();

    const bool bHasSharedData =
        Data.BoundsData && Data.BoundsSizeInBytes != 0 && Data.BoundsElementCount != 0 &&
        Data.MeshletDrawData && Data.MeshletDrawDataSizeInBytes != 0 && Data.MeshletDrawDataElementCount != 0 &&
        Data.RangeOffsetsData && Data.RangeOffsetsSizeInBytes != 0 && Data.RangeOffsetsElementCount != 0 &&
        Data.ConeAxisData && Data.ConeAxisSizeInBytes != 0 && Data.ConeAxisElementCount != 0 &&
        Data.ConeApexData && Data.ConeApexSizeInBytes != 0 && Data.ConeApexElementCount != 0;

    if (!bHasSharedData)
    {
        RefreshCullingPersistentValidation(static_cast<uint32_t>(CullingConstantBuffers.size()));
        return true;
    }

    const auto CreateStructuredBuffer = [&](const wchar_t* BufferName,
        const void* SourceData,
        uint64_t SizeInBytes,
        uint32_t ElementCount,
        uint32_t ElementStride,
        FBindlessBuffer& Buffer,
        FUploadBuffer& Upload)
    {
        CreateBindlessBufferWithUpload(
            Device,
            BufferName,
            CreateStructuredBufferDesc((std::max)(SizeInBytes, 4ull), ElementCount, ElementStride),
            D3D12_RESOURCE_STATE_COMMON,
            Buffer,
            Upload,
            SourceData,
            true,
            false);
    };

    CreateStructuredBuffer(
        L"ModelBoundsBuffer",
        Data.BoundsData,
        Data.BoundsSizeInBytes,
        Data.BoundsElementCount,
        sizeof(uint32_t) * 4,
        ModelBoundsBuffer,
        ModelBoundsUpload);
    CreateStructuredBuffer(
        L"MeshletDrawDataBuffer",
        Data.MeshletDrawData,
        Data.MeshletDrawDataSizeInBytes,
        Data.MeshletDrawDataElementCount,
        static_cast<uint32_t>(Data.MeshletDrawDataSizeInBytes / Data.MeshletDrawDataElementCount),
        MeshletDrawDataBuffer,
        MeshletDrawDataUpload);
    CreateStructuredBuffer(
        L"MeshletRangeOffsetBuffer",
        Data.RangeOffsetsData,
        Data.RangeOffsetsSizeInBytes,
        Data.RangeOffsetsElementCount,
        sizeof(uint32_t),
        MeshletRangeOffsetBuffer,
        MeshletRangeOffsetUpload);
    CreateStructuredBuffer(
        L"MeshletConeAxisBuffer",
        Data.ConeAxisData,
        Data.ConeAxisSizeInBytes,
        Data.ConeAxisElementCount,
        sizeof(uint32_t) * 4,
        MeshletConeAxisBuffer,
        MeshletConeAxisUpload);
    CreateStructuredBuffer(
        L"MeshletConeApexBuffer",
        Data.ConeApexData,
        Data.ConeApexSizeInBytes,
        Data.ConeApexElementCount,
        sizeof(uint32_t) * 4,
        MeshletConeApexBuffer,
        MeshletConeApexUpload);

    BoundsSizeInBytes = Data.BoundsSizeInBytes;
    MeshletDrawDataSizeInBytes = Data.MeshletDrawDataSizeInBytes;
    RangeOffsetsSizeInBytes = Data.RangeOffsetsSizeInBytes;
    ConeAxisSizeInBytes = Data.ConeAxisSizeInBytes;
    ConeApexSizeInBytes = Data.ConeApexSizeInBytes;

    RefreshCullingPersistentValidation(static_cast<uint32_t>(CullingConstantBuffers.size()));
    return true;
}

void FGpuDrivenCulling::AddSharedInputUploadPreCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const
{
    const auto AddBarrier = [&](ID3D12Resource* Resource)
    {
        if (!Resource)
        {
            return;
        }

        Barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
    };

    AddBarrier(MeshletDrawDataBuffer.Get());
    AddBarrier(MeshletRangeOffsetBuffer.Get());
    AddBarrier(ModelBoundsBuffer.Get());
    AddBarrier(MeshletConeAxisBuffer.Get());
    AddBarrier(MeshletConeApexBuffer.Get());
}

void FGpuDrivenCulling::CopySharedInputData(ID3D12GraphicsCommandList* CommandList) const
{
    if (!CommandList)
    {
        return;
    }

    if (ModelBoundsBuffer && ModelBoundsUpload && BoundsSizeInBytes != 0)
    {
        CommandList->CopyBufferRegion(ModelBoundsBuffer.Get(), 0, ModelBoundsUpload.Get(), 0, BoundsSizeInBytes);
    }
    if (MeshletDrawDataBuffer && MeshletDrawDataUpload && MeshletDrawDataSizeInBytes != 0)
    {
        CommandList->CopyBufferRegion(MeshletDrawDataBuffer.Get(), 0, MeshletDrawDataUpload.Get(), 0, MeshletDrawDataSizeInBytes);
    }
    if (MeshletRangeOffsetBuffer && MeshletRangeOffsetUpload && RangeOffsetsSizeInBytes != 0)
    {
        CommandList->CopyBufferRegion(MeshletRangeOffsetBuffer.Get(), 0, MeshletRangeOffsetUpload.Get(), 0, RangeOffsetsSizeInBytes);
    }
    if (MeshletConeAxisBuffer && MeshletConeAxisUpload && ConeAxisSizeInBytes != 0)
    {
        CommandList->CopyBufferRegion(MeshletConeAxisBuffer.Get(), 0, MeshletConeAxisUpload.Get(), 0, ConeAxisSizeInBytes);
    }
    if (MeshletConeApexBuffer && MeshletConeApexUpload && ConeApexSizeInBytes != 0)
    {
        CommandList->CopyBufferRegion(MeshletConeApexBuffer.Get(), 0, MeshletConeApexUpload.Get(), 0, ConeApexSizeInBytes);
    }
}

void FGpuDrivenCulling::AddSharedInputUploadPostCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers) const
{
    const auto AddBarrier = [&](ID3D12Resource* Resource)
    {
        if (!Resource)
        {
            return;
        }

        Barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    };

    AddBarrier(ModelBoundsBuffer.Get());
    AddBarrier(MeshletDrawDataBuffer.Get());
    AddBarrier(MeshletRangeOffsetBuffer.Get());
    AddBarrier(MeshletConeAxisBuffer.Get());
    AddBarrier(MeshletConeApexBuffer.Get());
}

void FGpuDrivenCulling::AddInitialUploadPreCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers, uint32_t FramesInFlight) const
{
    for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
    {
        ID3D12Resource* TemplateBuffer = GetIndirectCommandTemplateBuffer(FrameIndex);
        if (TemplateBuffer)
        {
            Barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                TemplateBuffer,
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
        }
    }
    if (HasSharedInputs())
    {
        AddSharedInputUploadPreCopyBarriers(Barriers);
    }
}

void FGpuDrivenCulling::CopyInitialData(
    ID3D12GraphicsCommandList* CommandList,
    const std::vector<FUploadBuffer>& IndirectCommandUploads,
    uint64_t CommandBufferSize) const
{
    if (!CommandList)
    {
        return;
    }
    if (!IndirectCommandUploads.empty() && CommandBufferSize > 0)
    {
        for (uint32_t FrameIndex = 0; FrameIndex < static_cast<uint32_t>(IndirectCommandUploads.size()); ++FrameIndex)
        {
            ID3D12Resource* TemplateBuffer = GetIndirectCommandTemplateBuffer(FrameIndex);
            if (TemplateBuffer && IndirectCommandUploads[FrameIndex].Get())
            {
                CommandList->CopyBufferRegion(TemplateBuffer, 0, IndirectCommandUploads[FrameIndex].Get(), 0, CommandBufferSize);
            }
        }
    }
    if (HasSharedInputs())
    {
        CopySharedInputData(CommandList);
    }
}

void FGpuDrivenCulling::AddInitialUploadPostCopyBarriers(std::vector<D3D12_RESOURCE_BARRIER>& Barriers, uint32_t FramesInFlight) const
{
    for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
    {
        ID3D12Resource* TemplateBuffer = GetIndirectCommandTemplateBuffer(FrameIndex);
        if (TemplateBuffer)
        {
            Barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                TemplateBuffer,
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
    }
    if (HasSharedInputs())
    {
        AddSharedInputUploadPostCopyBarriers(Barriers);
    }
}

void FGpuDrivenCulling::ResetInitialUploadStates(uint32_t FramesInFlight)
{
    ResetCullingStatesToCommon(FramesInFlight);
    ResetVisibilityStatesToCommon(FramesInFlight);
}

bool FGpuDrivenCulling::CreateVisibilityResources(FDX12Device* Device, uint32_t FramesInFlight, uint32_t IndirectCommandCount)
{
    ResetVisibilityResources(FramesInFlight);

    const FRGBufferDesc VisibilityDesc = CreateRWStructuredBufferDesc(
        static_cast<uint64_t>(sizeof(uint32_t)) * IndirectCommandCount,
        IndirectCommandCount,
        static_cast<uint32_t>(sizeof(uint32_t)));
    const FRGBufferDesc CountDesc = CreateRawBufferDesc(sizeof(uint32_t), 1u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    const auto CreateBufferWithViews = [&](uint32_t FrameIndex,
        const std::wstring& BaseName,
        const FRGBufferDesc& Desc,
        std::vector<FBindlessBuffer>& Buffers)
    {
        CreateBindlessBuffer(
            Device,
            BaseName + L"_Frame" + std::to_wstring(FrameIndex),
            Desc,
            D3D12_RESOURCE_STATE_COMMON,
            Buffers[FrameIndex],
            true,
            true);
    };

    for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
    {
        CreateBufferWithViews(FrameIndex, L"MeshletVisibilityBuffer", VisibilityDesc, MeshletVisibilityBuffers);
        CreateBufferWithViews(FrameIndex, L"MeshletVisibilityLateBuffer", VisibilityDesc, MeshletVisibilityLateBuffers);
        CreateBufferWithViews(FrameIndex, L"PrevVisibleList", VisibilityDesc, PrevVisibleListBuffers);
        CreateBufferWithViews(FrameIndex, L"PrevInvisibleList", VisibilityDesc, PrevInvisibleListBuffers);
        CreateBufferWithViews(FrameIndex, L"EarlyRejectList", VisibilityDesc, EarlyRejectListBuffers);
        CreateBufferWithViews(FrameIndex, L"LateList", VisibilityDesc, LateListBuffers);
        CreateBufferWithViews(FrameIndex, L"LateListFlags", VisibilityDesc, LateListFlagBuffers);

        CreateBufferWithViews(FrameIndex, L"PrevVisibleCount", CountDesc, PrevVisibleCountBuffers);
        CreateBufferWithViews(FrameIndex, L"PrevInvisibleCount", CountDesc, PrevInvisibleCountBuffers);
        CreateBufferWithViews(FrameIndex, L"EarlyRejectCount", CountDesc, EarlyRejectCountBuffers);
        CreateBufferWithViews(FrameIndex, L"LateListCount", CountDesc, LateListCountBuffers);
    }

    return true;
}

void FGpuDrivenCulling::ResetVisibilityStatesToCommon(uint32_t FramesInFlight)
{
    ResetBindlessBufferStates(MeshletVisibilityBuffers);
    ResetBindlessBufferStates(MeshletVisibilityLateBuffers);
    ResetBindlessBufferStates(PrevVisibleListBuffers);
    ResetBindlessBufferStates(PrevInvisibleListBuffers);
    ResetBindlessBufferStates(EarlyRejectListBuffers);
    ResetBindlessBufferStates(LateListBuffers);
    ResetBindlessBufferStates(LateListFlagBuffers);
    ResetBindlessBufferStates(PrevVisibleCountBuffers);
    ResetBindlessBufferStates(PrevInvisibleCountBuffers);
    ResetBindlessBufferStates(EarlyRejectCountBuffers);
    ResetBindlessBufferStates(LateListCountBuffers);
}

bool FGpuDrivenCulling::CreateVisibilityListPipelines(FDX12Device* Device)
{
    bVisibilityListPersistentInputsValid = false;

    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kVisibilityListBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedDesc;
    VersionedDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(VisibilityListRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;

    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/ClearVisibilityCounts.hlsl", CsByteCode))
    {
        LogError("Failed to compile visibility count clear shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ClearDesc = {};
    ClearDesc.pRootSignature = VisibilityListRootSignature.Get();
    ClearDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&ClearDesc, IID_PPV_ARGS(ClearVisibilityCountsPipeline.GetAddressOf())));

    CsByteCode.clear();
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/ClearVisibilityFlags.hlsl", CsByteCode))
    {
        LogError("Failed to compile visibility flags clear shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ClearFlagsDesc = {};
    ClearFlagsDesc.pRootSignature = VisibilityListRootSignature.Get();
    ClearFlagsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&ClearFlagsDesc, IID_PPV_ARGS(ClearVisibilityFlagsPipeline.GetAddressOf())));

    CsByteCode.clear();
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/BuildVisibilityLists.hlsl", CsByteCode))
    {
        LogError("Failed to compile visibility list build shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC BuildDesc = {};
    BuildDesc.pRootSignature = VisibilityListRootSignature.Get();
    BuildDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&BuildDesc, IID_PPV_ARGS(BuildVisibilityListsPipeline.GetAddressOf())));

    CsByteCode.clear();
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/BuildEarlyRejectList.hlsl", CsByteCode))
    {
        LogError("Failed to compile early reject list shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC RejectDesc = {};
    RejectDesc.pRootSignature = VisibilityListRootSignature.Get();
    RejectDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&RejectDesc, IID_PPV_ARGS(BuildEarlyRejectListPipeline.GetAddressOf())));

    CsByteCode.clear();
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/GpuDrivenCulling/MergeVisibilityLists.hlsl", CsByteCode))
    {
        LogError("Failed to compile visibility list merge shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC MergeDesc = {};
    MergeDesc.pRootSignature = VisibilityListRootSignature.Get();
    MergeDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&MergeDesc, IID_PPV_ARGS(MergeVisibilityListsPipeline.GetAddressOf())));

    return true;
}

void FGpuDrivenCulling::DispatchBuildVisibilityLists(
    FDX12Device* Device,
    D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress,
    uint32_t IndirectCommandCount,
    FDX12CommandContext& CmdContext,
    uint32_t VisibilityIndex,
    uint32_t VisibleListIndex,
    uint32_t InvisibleListIndex,
    uint32_t VisibleCountIndex,
    uint32_t InvisibleCountIndex,
    uint32_t VisibilityFrameIndex,
    uint32_t FrameIndex)
{
    if (!bVisibilityListPersistentInputsValid)
    {
        return;
    }

    if (FrameIndex >= PrevVisibleListBuffers.size() || FrameIndex >= PrevInvisibleListBuffers.size())
    {
        return;
    }

    ID3D12Resource* VisibleListBuffer = PrevVisibleListBuffers[FrameIndex].Get();
    ID3D12Resource* InvisibleListBuffer = PrevInvisibleListBuffers[FrameIndex].Get();
    ID3D12Resource* VisibleCountBuffer = PrevVisibleCountBuffers[FrameIndex].Get();
    ID3D12Resource* InvisibleCountBuffer = PrevInvisibleCountBuffers[FrameIndex].Get();

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent VisibilityListEvent(CommandList, L"BuildVisibilityLists");

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(VisibilityListRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, CullingConstantBufferAddress);

    const FClearCountsConstants ClearConstants = { VisibleCountIndex, InvisibleCountIndex };
    CommandList->SetPipelineState(ClearVisibilityCountsPipeline.Get());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(ClearConstants) / sizeof(uint32_t), &ClearConstants, 0);
    CommandList->Dispatch(1, 1, 1);

    const auto VisibleBarrier = CD3DX12_RESOURCE_BARRIER::UAV(VisibleCountBuffer);
    CommandList->ResourceBarrier(1, &VisibleBarrier);
    const auto InvisibleBarrier = CD3DX12_RESOURCE_BARRIER::UAV(InvisibleCountBuffer);
    CommandList->ResourceBarrier(1, &InvisibleBarrier);

    struct FBuildListsConstants
    {
        uint32_t VisibilityIndex;
        uint32_t VisibleListIndex;
        uint32_t InvisibleListIndex;
        uint32_t VisibleCountIndex;
        uint32_t InvisibleCountIndex;
        uint32_t IndirectCommandCount;
    };

    static_assert(sizeof(FBuildListsConstants) / sizeof(uint32_t) <= kVisibilityListBindlessDwordCount);
    const FBuildListsConstants BuildConstants = { VisibilityIndex, VisibleListIndex, InvisibleListIndex, VisibleCountIndex, InvisibleCountIndex, IndirectCommandCount };
    CommandList->SetPipelineState(BuildVisibilityListsPipeline.Get());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(BuildConstants) / sizeof(uint32_t), &BuildConstants, 0);
    const uint32_t DispatchCount = (IndirectCommandCount + 63) / 64;
    CommandList->Dispatch(DispatchCount, 1, 1);
}

void FGpuDrivenCulling::DispatchBuildEarlyRejectList(
    FDX12Device* Device,
    D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress,
    uint32_t IndirectCommandCount,
    FDX12CommandContext& CmdContext,
    uint32_t VisibilityIndex,
    uint32_t RejectListIndex,
    uint32_t RejectCountIndex,
    uint32_t FrameIndex)
{
    if (!bVisibilityListPersistentInputsValid)
    {
        return;
    }

    if (FrameIndex >= EarlyRejectListBuffers.size())
    {
        return;
    }

    ID3D12Resource* RejectListBuffer = EarlyRejectListBuffers[FrameIndex].Get();
    ID3D12Resource* RejectCountBuffer = EarlyRejectCountBuffers[FrameIndex].Get();

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent EarlyRejectEvent(CommandList, L"BuildEarlyRejectList");

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(VisibilityListRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, CullingConstantBufferAddress);

    const FClearCountsConstants ClearConstants = { RejectCountIndex, RejectCountIndex };
    CommandList->SetPipelineState(ClearVisibilityCountsPipeline.Get());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(ClearConstants) / sizeof(uint32_t), &ClearConstants, 0);
    CommandList->Dispatch(1, 1, 1);

    const auto CountBarrier = CD3DX12_RESOURCE_BARRIER::UAV(RejectCountBuffer);
    CommandList->ResourceBarrier(1, &CountBarrier);

    struct FEarlyRejectConstants
    {
        uint32_t VisibilityIndex;
        uint32_t RejectListIndex;
        uint32_t RejectCountIndex;
        uint32_t IndirectCommandCount;
    };

    static_assert(sizeof(FEarlyRejectConstants) / sizeof(uint32_t) <= kVisibilityListBindlessDwordCount);
    const FEarlyRejectConstants RejectConstants = { VisibilityIndex, RejectListIndex, RejectCountIndex, IndirectCommandCount };
    CommandList->SetPipelineState(BuildEarlyRejectListPipeline.Get());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(RejectConstants) / sizeof(uint32_t), &RejectConstants, 0);
    const uint32_t DispatchCount = (IndirectCommandCount + 63) / 64;
    CommandList->Dispatch(DispatchCount, 1, 1);
}

void FGpuDrivenCulling::DispatchMergeVisibilityLists(
    FDX12Device* Device,
    D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress,
    uint32_t IndirectCommandCount,
    FDX12CommandContext& CmdContext,
    uint32_t ListAIndex,
    uint32_t ListBIndex,
    uint32_t CountAIndex,
    uint32_t CountBIndex,
    uint32_t OutputListIndex,
    uint32_t OutputCountIndex,
    uint32_t FlagsIndex,
    uint32_t FrameIndex)
{
    if (!bVisibilityListPersistentInputsValid)
    {
        return;
    }

    if (FrameIndex >= LateListBuffers.size() || FrameIndex >= LateListFlagBuffers.size())
    {
        return;
    }

    ID3D12Resource* OutputListBuffer = LateListBuffers[FrameIndex].Get();
    ID3D12Resource* OutputCountBuffer = LateListCountBuffers[FrameIndex].Get();
    ID3D12Resource* FlagsBuffer = LateListFlagBuffers[FrameIndex].Get();
    if (FlagsIndex == UINT32_MAX)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent MergeListEvent(CommandList, L"MergeVisibilityLists");

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(VisibilityListRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, CullingConstantBufferAddress);

    struct FClearFlagsConstants
    {
        uint32_t FlagsIndex;
        uint32_t IndirectCommandCount;
    };

    static_assert(sizeof(FClearFlagsConstants) / sizeof(uint32_t) <= kVisibilityListBindlessDwordCount);
    const FClearFlagsConstants ClearFlagsConstants = { FlagsIndex, IndirectCommandCount };
    CommandList->SetPipelineState(ClearVisibilityFlagsPipeline.Get());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(ClearFlagsConstants) / sizeof(uint32_t), &ClearFlagsConstants, 0);
    const uint32_t FlagDispatchCount = (IndirectCommandCount + 63) / 64;
    CommandList->Dispatch(FlagDispatchCount, 1, 1);

    const auto FlagsBarrier = CD3DX12_RESOURCE_BARRIER::UAV(FlagsBuffer);
    CommandList->ResourceBarrier(1, &FlagsBarrier);

    struct FMergeConstants
    {
        uint32_t ListAIndex;
        uint32_t ListBIndex;
        uint32_t CountAIndex;
        uint32_t CountBIndex;
        uint32_t OutputListIndex;
        uint32_t OutputCountIndex;
        uint32_t FlagsIndex;
    };

    static_assert(sizeof(FMergeConstants) / sizeof(uint32_t) <= kVisibilityListBindlessDwordCount);
    const FMergeConstants MergeConstants = { ListAIndex, ListBIndex, CountAIndex, CountBIndex, OutputListIndex, OutputCountIndex, FlagsIndex };
    const FClearCountsConstants ClearConstants = { OutputCountIndex, OutputCountIndex };
    CommandList->SetPipelineState(ClearVisibilityCountsPipeline.Get());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(ClearConstants) / sizeof(uint32_t), &ClearConstants, 0);
    CommandList->Dispatch(1, 1, 1);

    const auto CountBarrier = CD3DX12_RESOURCE_BARRIER::UAV(OutputCountBuffer);
    CommandList->ResourceBarrier(1, &CountBarrier);

    CommandList->SetPipelineState(MergeVisibilityListsPipeline.Get());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(MergeConstants) / sizeof(uint32_t), &MergeConstants, 0);

    const uint32_t DispatchCount = ((IndirectCommandCount * 2) + 63) / 64;
    CommandList->Dispatch(DispatchCount, 1, 1);
}

void FGpuDrivenCulling::DispatchGpuCulling(
    const FGpuCullingDispatchConfig& Config,
    const FGpuCullingDispatchFrameData& FrameData,
    FGpuCullingDispatchIndices Indices,
    FDX12CommandContext& CmdContext,
    const FCamera& Camera,
    uint32_t FrameIndex,
    bool bLatePass,
    uint32_t VisibilityInputFrameIndex)
{
    if (!bCullingDispatchPersistentInputsValid
        || !Config.BindlessDescriptorHeap
        || Config.IndirectCommandCount == 0
        || Config.RangeCount == 0
        || !FrameData.IndirectBuffer
        || !FrameData.RunCountBuffer
        || !FrameData.IndirectState
        || !FrameData.RunCountState)
    {
        return;
    }

    const D3D12_GPU_VIRTUAL_ADDRESS CullingConstantBufferAddress = GetCullingConstantBufferAddress(FrameIndex, bLatePass);
    uint8_t* const CullingConstantBufferMapped = GetCullingConstantBufferMapped(FrameIndex, bLatePass);
    if (CullingConstantBufferAddress == 0)
    {
        return;
    }

    uint32_t ResolvedMode = Config.Mode;
    if (ResolvedMode == 1u && (Indices.VisibilityInputIndex == UINT32_MAX
        || !GetMeshletVisibilityFrameData(VisibilityInputFrameIndex).IsValid()))
    {
        ResolvedMode = 0u;
    }

    const bool bUseVisibilityInput = ResolvedMode == 1u;
    const FMeshletVisibilityFrameData VisibilityFrame = GetMeshletVisibilityFrameData(FrameIndex, bLatePass);
    D3D12_RESOURCE_STATES* VisibilityState = GetMeshletVisibilityState(FrameIndex, bLatePass);
    if (!VisibilityState)
    {
        return;
    }

    DirectX::XMVECTOR Planes[6] = {};
    RendererUtils::BuildCameraFrustumPlanes(Camera, Planes);

    FGpuCullingConstants Constants = {};
    for (uint32_t PlaneIndex = 0; PlaneIndex < 6; ++PlaneIndex)
    {
        DirectX::XMFLOAT4 Plane{};
        DirectX::XMStoreFloat4(&Plane, Planes[PlaneIndex]);
        std::memcpy(Constants.FrustumPlanes[PlaneIndex], &Plane, sizeof(DirectX::XMFLOAT4));
    }

    const DirectX::XMMATRIX ViewProjection = Camera.GetViewMatrix() * Camera.GetProjectionMatrix();
    DirectX::XMFLOAT4X4 ViewProjectionMatrix = {};
    DirectX::XMStoreFloat4x4(&ViewProjectionMatrix, ViewProjection);

    Constants.DebugPrintEnabled                 = Config.bGpuDebugPrintEnabled ? 1u : 0u;
    const DirectX::XMFLOAT3 CameraPosition = Camera.GetPosition();
    std::memcpy(Constants.CameraPosition, &CameraPosition, sizeof(DirectX::XMFLOAT3));

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent CullingEvent(CommandList, L"GpuCulling");

    D3D12_RESOURCE_STATES& IndirectState = *FrameData.IndirectState;
    if (IndirectState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(FrameData.IndirectBuffer, IndirectState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CommandList->ResourceBarrier(1, &Barrier);
        IndirectState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    if (*VisibilityState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(VisibilityFrame.Buffer, *VisibilityState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CommandList->ResourceBarrier(1, &Barrier);
        *VisibilityState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    if (bUseVisibilityInput)
    {
        const FMeshletVisibilityFrameData InputVisibilityFrame = GetMeshletVisibilityFrameData(VisibilityInputFrameIndex);
        D3D12_RESOURCE_STATES* InputState = GetMeshletVisibilityState(VisibilityInputFrameIndex);
        if (InputVisibilityFrame.Buffer && InputState && *InputState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        {
            const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(InputVisibilityFrame.Buffer, *InputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CommandList->ResourceBarrier(1, &Barrier);
            *InputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_RESOURCE_STATES& RunCountState = *FrameData.RunCountState;
    if (RunCountState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(FrameData.RunCountBuffer, RunCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        CommandList->ResourceBarrier(1, &Barrier);
        RunCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    if (CullingConstantBufferMapped)
    {
        std::memcpy(CullingConstantBufferMapped, &Constants, sizeof(Constants));
    }

    ID3D12DescriptorHeap* Heaps[] = { Config.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

    struct FMeshletRunBindlessConstants
    {
        uint32_t VisibleMeshletsIndex;
        uint32_t MeshletDrawDataIndex;
        uint32_t RangeOffsetsIndex;
        uint32_t CommandTemplatesIndex;
        uint32_t OutputCommandsIndex;
        uint32_t RunCountsIndex;
        uint32_t IndirectCommandCount;
        uint32_t RangeCount;
    };

    static_assert(sizeof(FMeshletRunBindlessConstants) / sizeof(uint32_t) <= kMeshletRunBindlessDwordCount);
    const FMeshletRunBindlessConstants RunBindlessConstants =
    {
        VisibilityFrame.SrvBindlessIndex,
        Indices.MeshletDrawDataIndex,
        Indices.RangeOffsetsIndex,
        FrameData.TemplateSrvBindlessIndex,
        FrameData.IndirectUavBindlessIndex,
        FrameData.RunCountUavBindlessIndex,
        Config.IndirectCommandCount,
        Config.RangeCount
    };

    CommandList->SetPipelineState(MeshletRunClearPipeline.Get());
    CommandList->SetComputeRootSignature(MeshletRunRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, CullingConstantBufferAddress);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(RunBindlessConstants) / sizeof(uint32_t), &RunBindlessConstants, 0);
    CommandList->Dispatch((Config.RangeCount + 63u) / 64u, 1, 1);

    const auto RunCountBarrier = CD3DX12_RESOURCE_BARRIER::UAV(FrameData.RunCountBuffer);
    CommandList->ResourceBarrier(1, &RunCountBarrier);

    struct FGpuCullingBindlessConstants
    {
        DirectX::XMFLOAT4X4 ViewProjection;
        uint32_t ModelBoundsIndex;
        uint32_t HZBTextureIndex;
        uint32_t MeshletConeAxisIndex;
        uint32_t MeshletConeApexIndex;
        uint32_t VisibleMeshletsIndex;
        uint32_t VisibilityInputIndex;
        uint32_t CullingListIndex;
        uint32_t CullingListCountIndex;
        uint32_t DebugPrintBufferIndex;
        uint32_t DebugPrintStatsIndex;
        uint32_t IndirectCommandCount;
        uint32_t HZBEnabled;
        uint32_t HZBMipCount;
        uint32_t HZBWidth;
        uint32_t HZBHeight;
        uint32_t CullingMode;
    };

    const uint32_t ResolvedVisibilityInputIndex = bUseVisibilityInput
        ? Indices.VisibilityInputIndex
        : VisibilityFrame.SrvBindlessIndex;
    const uint32_t ResolvedListIndex = Indices.bUseCullingList ? Indices.CullingListIndex : VisibilityFrame.SrvBindlessIndex;
    const uint32_t ResolvedListCountIndex = Indices.bUseCullingList ? Indices.CullingListCountIndex : UINT32_MAX;

    static_assert(sizeof(FGpuCullingBindlessConstants) / sizeof(uint32_t) <= kGpuCullingBindlessDwordCount);
    const FGpuCullingBindlessConstants CullingBindlessConstants =
    {
        ViewProjectionMatrix,
        Indices.ModelBoundsIndex,
        HZBCullingBindlessIndex,
        Indices.MeshletConeAxisIndex,
        Indices.MeshletConeApexIndex,
        VisibilityFrame.UavBindlessIndex,
        ResolvedVisibilityInputIndex,
        ResolvedListIndex,
        ResolvedListCountIndex,
        Indices.DebugPrintBufferIndex,
        Indices.DebugPrintStatsIndex,
        Config.IndirectCommandCount,
        bHZBOcclusionEnabled ? 1u : 0u,
        HZBCullingMipCount,
        HZBCullingWidth,
        HZBCullingHeight,
        ResolvedMode
    };

    ID3D12PipelineState* SelectedPipeline = Indices.bUseCullingList ? CullingListPipeline.Get() : CullingPipeline.Get();
    CommandList->SetPipelineState(SelectedPipeline);
    CommandList->SetComputeRootSignature(CullingRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, CullingConstantBufferAddress);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(CullingBindlessConstants) / sizeof(uint32_t), &CullingBindlessConstants, 0);

    const uint32_t DispatchCount = (Config.IndirectCommandCount + 63u) / 64u;
    CommandList->Dispatch(DispatchCount, 1, 1);

    if (*VisibilityState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(VisibilityFrame.Buffer, *VisibilityState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CommandList->ResourceBarrier(1, &Barrier);
        *VisibilityState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    CommandList->SetPipelineState(MeshletRunAppendPipeline.Get());
    CommandList->SetComputeRootSignature(MeshletRunRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, CullingConstantBufferAddress);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(RunBindlessConstants) / sizeof(uint32_t), &RunBindlessConstants, 0);
    CommandList->Dispatch(DispatchCount, 1, 1);

    const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(FrameData.IndirectBuffer, IndirectState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    CommandList->ResourceBarrier(1, &Barrier);
    IndirectState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;

    if (RunCountState != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
    {
        const auto CountBarrier = CD3DX12_RESOURCE_BARRIER::Transition(FrameData.RunCountBuffer, RunCountState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        CommandList->ResourceBarrier(1, &CountBarrier);
        RunCountState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
}

FRenderer::FGpuDrivenCullingProvider FRenderer::GetGpuDrivenCullingProvider(bool bLatePass) const
{
    FGpuDrivenCullingProvider Provider;
    Provider.BindlessDescriptorHeap = Device ? Device->GetBindlessDescriptorHeap() : nullptr;
    Provider.BindlessCpuClearDescriptorHeap = Device ? Device->GetBindlessCpuDescriptorHeap() : nullptr;
    Provider.BindlessDescriptorStride = Device ? Device->GetBindlessDescriptorStride() : 0;
    Provider.CullingRootSignature = GpuDrivenCullingState.GetCullingRootSignature();
    Provider.MeshletRunRootSignature = GpuDrivenCullingState.GetMeshletRunRootSignature();
    Provider.CullingConstantBufferAddress = GpuDrivenCullingState.GetCullingConstantBufferAddress(CurrentFrameIndex, bLatePass);
    Provider.CullingConstantBufferMapped = GpuDrivenCullingState.GetCullingConstantBufferMapped(CurrentFrameIndex, bLatePass);
    Provider.bClusterDagDebugEnabled = IsClusterDagDebugEnabled();
    Provider.bClusterDagFastShaderEnabled = IsClusterDagFastShaderEnabled();
    Provider.bClusterDagGpuDebugEnabled = IsClusterDagDebugEnabled() && GpuDebugState.IsPrintEnabled();
    Provider.ViewportHeightPixels = Viewport.Height;
    Provider.GpuDebugPrintStatsUavBindlessIndex = Provider.bClusterDagGpuDebugEnabled ? GpuDebugState.GetPrintStatsUavBindlessIndex() : UINT32_MAX;
    Provider.GpuDebugLineBufferUavBindlessIndex = Provider.bClusterDagGpuDebugEnabled ? GpuDebugState.GetLineBufferUavBindlessIndex() : UINT32_MAX;
    return Provider;
}

void FGpuDrivenCulling::AddVisibilityListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex, uint32_t VisibilityFrameIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FVisibilityListPassData
    {
        bool bEnabled = false;
        uint32_t VisibilityIndex = UINT32_MAX;
        uint32_t VisibilityFrameIndex = UINT32_MAX;
        uint32_t FrameIndex = 0;
    };

    Context.Graph.AddPass<FVisibilityListPassData>("Build Prev Visibility Lists", [this, &Owner, &Context, VisibilityIndex, VisibilityFrameIndex](FVisibilityListPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bUseHzbTwoPass && Owner.GetGpuDrivenCullingState().HasVisibilityListBuildPipeline();
        Data.VisibilityIndex = VisibilityIndex;
        Data.VisibilityFrameIndex = VisibilityFrameIndex;
        Data.FrameIndex = Context.FrameIndex;
        if (Data.bEnabled)
        {
            FGpuDrivenCulling& MutableThis = *const_cast<FGpuDrivenCulling*>(this);
            const FRGBufferHandle PrevVisibleListHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_PrevVisibleList", MutableThis.PrevVisibleListBuffers[Data.FrameIndex]);
            const FRGBufferHandle PrevInvisibleListHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_PrevInvisibleList", MutableThis.PrevInvisibleListBuffers[Data.FrameIndex]);
            const FRGBufferHandle PrevVisibleCountHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_PrevVisibleCount", MutableThis.PrevVisibleCountBuffers[Data.FrameIndex]);
            const FRGBufferHandle PrevInvisibleCountHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_PrevInvisibleCount", MutableThis.PrevInvisibleCountBuffers[Data.FrameIndex]);

            Builder.WriteBuffer(PrevVisibleListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(PrevInvisibleListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(PrevVisibleCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(PrevInvisibleCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (Data.VisibilityFrameIndex < MutableThis.MeshletVisibilityBuffers.size())
            {
                const FRGBufferHandle VisibilityHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_MeshletVisibilityRead", MutableThis.MeshletVisibilityBuffers[Data.VisibilityFrameIndex]);
                Builder.ReadBuffer(VisibilityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            Builder.KeepAlive();
        }
    }, [&Owner](const FVisibilityListPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (Data.VisibilityIndex == UINT32_MAX)
        {
            return;
        }

        const FGpuDrivenCulling::FVisibilityListBuildIndices Indices = Owner.GetGpuDrivenCullingState().GetVisibilityListBuildIndices(Data.FrameIndex);
        if (!Indices.IsValid())
        {
            return;
        }

        FGpuDrivenCulling& GpuDrivenCulling = Owner.GetGpuDrivenCullingState();
        GpuDrivenCulling.DispatchBuildVisibilityLists(
            Owner.GetDevice(),
            GpuDrivenCulling.GetCullingConstantBufferAddress(Owner.GetFrameIndex()),
            Owner.GetIndirectCommandCount(),
            Cmd,
            Data.VisibilityIndex,
            Indices.PrevVisibleListUav,
            Indices.PrevInvisibleListUav,
            Indices.PrevVisibleCountUav,
            Indices.PrevInvisibleCountUav,
            Data.VisibilityFrameIndex,
            Data.FrameIndex);
    });
}

void FGpuDrivenCulling::AddGpuCullingPass(
    FDeferredPassContext& Context,
    ECullingMode Mode,
    uint32_t VisibilityInputIndex,
    uint32_t VisibilityInputFrameIndex,
    uint32_t CullingListIndex,
    uint32_t CullingListCountIndex,
    const char* PassName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FGpuCullingPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
        FGpuDrivenCulling::ECullingMode Mode = FGpuDrivenCulling::ECullingMode::All;
        uint32_t VisibilityInputIndex = UINT32_MAX;
        uint32_t VisibilityInputFrameIndex = UINT32_MAX;
        uint32_t CullingListIndex = UINT32_MAX;
        uint32_t CullingListCountIndex = UINT32_MAX;
    };

    Context.Graph.AddPass<FGpuCullingPassData>(PassName, [&Owner, &Context, Mode, VisibilityInputIndex, VisibilityInputFrameIndex, CullingListIndex, CullingListCountIndex](FGpuCullingPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.CanDispatchGpuCulling();
        Data.Camera = &Context.Camera;
        Data.Mode = Mode;
        Data.VisibilityInputIndex = VisibilityInputIndex;
        Data.VisibilityInputFrameIndex = VisibilityInputFrameIndex;
        Data.CullingListIndex = CullingListIndex;
        Data.CullingListCountIndex = CullingListCountIndex;
        if (Data.bEnabled)
        {
            if (Context.FrameState.bUseHZBOcclusion)
            {
                Builder.ReadTexture(Context.Resources.Hzb.HzbHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            Builder.KeepAlive();
        }
    }, [&Owner](const FGpuCullingPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        Owner.DispatchGpuCulling(
            Cmd,
            *Data.Camera,
            Data.Mode,
            Data.VisibilityInputIndex,
            Data.VisibilityInputFrameIndex,
            Data.CullingListIndex,
            Data.CullingListCountIndex,
            Data.Mode == FGpuDrivenCulling::ECullingMode::LateAfterEarly);
    });
}

void FGpuDrivenCulling::AddEarlyRejectListPass(FDeferredPassContext& Context, uint32_t VisibilityIndex) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FEarlyRejectPassData
    {
        bool bEnabled = false;
        uint32_t VisibilityIndex = UINT32_MAX;
        uint32_t FrameIndex = 0;
    };

    Context.Graph.AddPass<FEarlyRejectPassData>("Build Early Reject List", [this, &Owner, &Context, VisibilityIndex](FEarlyRejectPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bUseHzbTwoPass && Owner.GetGpuDrivenCullingState().HasEarlyRejectListPipeline();
        Data.VisibilityIndex = VisibilityIndex;
        Data.FrameIndex = Context.FrameIndex;
        if (Data.bEnabled)
        {
            FGpuDrivenCulling& MutableThis = *const_cast<FGpuDrivenCulling*>(this);
            const FRGBufferHandle EarlyRejectListHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_EarlyRejectList", MutableThis.EarlyRejectListBuffers[Data.FrameIndex]);
            const FRGBufferHandle EarlyRejectCountHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_EarlyRejectCount", MutableThis.EarlyRejectCountBuffers[Data.FrameIndex]);
            Builder.WriteBuffer(EarlyRejectListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(EarlyRejectCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            if (Data.FrameIndex < MutableThis.MeshletVisibilityBuffers.size())
            {
                const FRGBufferHandle VisibilityHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_MeshletVisibilityCurrent", MutableThis.MeshletVisibilityBuffers[Data.FrameIndex]);
                Builder.ReadBuffer(VisibilityHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }

            Builder.KeepAlive();
        }
    }, [&Owner](const FEarlyRejectPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        if (Data.VisibilityIndex == UINT32_MAX)
        {
            return;
        }

        const FGpuDrivenCulling::FEarlyRejectListIndices Indices = Owner.GetGpuDrivenCullingState().GetEarlyRejectListIndices(Data.FrameIndex);
        if (!Indices.IsValid())
        {
            return;
        }

        FGpuDrivenCulling& GpuDrivenCulling = Owner.GetGpuDrivenCullingState();
        GpuDrivenCulling.DispatchBuildEarlyRejectList(
            Owner.GetDevice(),
            GpuDrivenCulling.GetCullingConstantBufferAddress(Owner.GetFrameIndex()),
            Owner.GetIndirectCommandCount(),
            Cmd,
            Data.VisibilityIndex,
            Indices.RejectListUav,
            Indices.RejectCountUav,
            Data.FrameIndex);
    });
}

void FGpuDrivenCulling::AddLateListMergePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FMergeListPassData
    {
        bool bEnabled = false;
        uint32_t FrameIndex = 0;
    };

    Context.Graph.AddPass<FMergeListPassData>("Merge Late Visibility Lists", [this, &Owner, &Context](FMergeListPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bUseHzbTwoPass && Owner.GetGpuDrivenCullingState().HasLateMergeVisibilityPipeline();
        Data.FrameIndex = Context.FrameIndex;
        if (Data.bEnabled)
        {
            FGpuDrivenCulling& MutableThis = *const_cast<FGpuDrivenCulling*>(this);
            const FRGBufferHandle PrevInvisibleListHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_PrevInvisibleListRead", MutableThis.PrevInvisibleListBuffers[Data.FrameIndex]);
            const FRGBufferHandle EarlyRejectListHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_EarlyRejectListRead", MutableThis.EarlyRejectListBuffers[Data.FrameIndex]);
            const FRGBufferHandle PrevInvisibleCountHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_PrevInvisibleCountRead", MutableThis.PrevInvisibleCountBuffers[Data.FrameIndex]);
            const FRGBufferHandle EarlyRejectCountHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_EarlyRejectCountRead", MutableThis.EarlyRejectCountBuffers[Data.FrameIndex]);
            const FRGBufferHandle LateListHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_LateList", MutableThis.LateListBuffers[Data.FrameIndex]);
            const FRGBufferHandle LateListCountHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_LateListCount", MutableThis.LateListCountBuffers[Data.FrameIndex]);
            const FRGBufferHandle LateListFlagHandle = ImportBindlessBuffer(Context.Graph, "GpuCulling_LateListFlag", MutableThis.LateListFlagBuffers[Data.FrameIndex]);

            Builder.ReadBuffer(PrevInvisibleListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadBuffer(EarlyRejectListHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadBuffer(PrevInvisibleCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadBuffer(EarlyRejectCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteBuffer(LateListHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(LateListCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(LateListFlagHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            Builder.KeepAlive();
        }
    }, [&Owner](const FMergeListPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        const FGpuDrivenCulling::FLateMergeVisibilityListIndices Indices = Owner.GetGpuDrivenCullingState().GetLateMergeVisibilityListIndices(Data.FrameIndex);
        if (!Indices.IsValid())
        {
            return;
        }

        FGpuDrivenCulling& GpuDrivenCulling = Owner.GetGpuDrivenCullingState();
        GpuDrivenCulling.DispatchMergeVisibilityLists(
            Owner.GetDevice(),
            GpuDrivenCulling.GetCullingConstantBufferAddress(Owner.GetFrameIndex()),
            Owner.GetIndirectCommandCount(),
            Cmd,
            Indices.PrevInvisibleListSrv,
            Indices.EarlyRejectListSrv,
            Indices.PrevInvisibleCountSrv,
            Indices.EarlyRejectCountSrv,
            Indices.LateListUav,
            Indices.LateListCountUav,
            Indices.LateListFlagUav,
            Data.FrameIndex);
    });
}
