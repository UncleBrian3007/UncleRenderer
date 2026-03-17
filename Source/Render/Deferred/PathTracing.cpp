#include "PathTracing.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include "../ShaderCompiler.h"
#include <d3dx12.h>
#include <algorithm>
#include <string>

using Microsoft::WRL::ComPtr;

bool FPathTracing::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    return CreateAccumulationRootSignature(Device) && CreateAccumulationPipeline(Device);
}

bool FPathTracing::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
{
    (void)Owner;
    return CreateAccumulationResources(Device, Width, Height, FrameCount);
}

void FPathTracing::ImportPersistentResources(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FPathTracingFrameResources& OutResources = Context.Resources.PathTracing;

    OutResources.TempHandle = Graph.ImportTexture(
        "PathTracingTemp",
        PathTracingTempTexture.Get(),
        &PathTracingTempState,
        { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::PathTracingBufferFormat });

    OutResources.AccumulationHandles.reserve(PathTracingAccumulationTextures.size());
    for (size_t Index = 0; Index < PathTracingAccumulationTextures.size(); ++Index)
    {
        const std::string HandleName = "PathTracingAccumulation_" + std::to_string(Index);
        OutResources.AccumulationHandles.push_back(Graph.ImportTexture(
            HandleName,
            PathTracingAccumulationTextures[Index].Get(),
            &PathTracingAccumulationStates[Index],
            { static_cast<uint32>(Owner.Viewport.Width), static_cast<uint32>(Owner.Viewport.Height), FDeferredRenderer::PathTracingBufferFormat }));
    }
}

bool FPathTracing::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    if (!Device)
    {
        return false;
    }

    if (PathTracingTempTexture)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC PathTracingTempUavDesc = {};
        PathTracingTempUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        PathTracingTempUavDesc.Format = FDeferredRenderer::PathTracingBufferFormat;
        PathTracingTempUavDesc.Texture2D.MipSlice = 0;
        PathTracingTempBindlessIndex = Device->CreateBindlessUav(PathTracingTempTexture.Get(), nullptr, PathTracingTempUavDesc);
    }

    PathTracingAccumulationSrvBindlessIndices.clear();
    PathTracingAccumulationUavBindlessIndices.clear();
    PathTracingAccumulationSrvBindlessIndices.resize(PathTracingAccumulationTextures.size(), UINT32_MAX);
    PathTracingAccumulationUavBindlessIndices.resize(PathTracingAccumulationTextures.size(), UINT32_MAX);

    for (uint32_t Index = 0; Index < PathTracingAccumulationTextures.size(); ++Index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC AccumSrvDesc = {};
        AccumSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        AccumSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        AccumSrvDesc.Format = FDeferredRenderer::PathTracingBufferFormat;
        AccumSrvDesc.Texture2D.MipLevels = 1;
        PathTracingAccumulationSrvBindlessIndices[Index] = Device->CreateBindlessSrv(PathTracingAccumulationTextures[Index].Get(), AccumSrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC AccumUavDesc = {};
        AccumUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        AccumUavDesc.Format = FDeferredRenderer::PathTracingBufferFormat;
        AccumUavDesc.Texture2D.MipSlice = 0;
        AccumUavDesc.Texture2D.PlaneSlice = 0;
        PathTracingAccumulationUavBindlessIndices[Index] = Device->CreateBindlessUav(PathTracingAccumulationTextures[Index].Get(), nullptr, AccumUavDesc);
    }

    return true;
}

void FPathTracing::AddPasses(FDeferredPassContext& Context)
{
    AddPathTracingPass(Context);
    AddPathTracingAccumulationPass(Context);
}

void FPathTracing::PrepareFrameState(uint32_t FrameIndex, bool bCameraMoved, bool& bAccumulationActive, bool& bHistoryReady, uint32_t& ReadIndex, uint32_t& WriteIndex)
{
    bAccumulationActive = bPathTracingAccumulationEnabled
        && PathTracingAccumulationPipeline
        && PathTracingAccumulationRootSignature
        && !PathTracingAccumulationTextures.empty();

    ReadIndex = PathTracingAccumulationFrameCount > 0 ? (FrameIndex + PathTracingAccumulationFrameCount - 1u) % PathTracingAccumulationFrameCount : 0u;
    WriteIndex = PathTracingAccumulationFrameCount > 0 ? FrameIndex % PathTracingAccumulationFrameCount : 0u;

    if (bCameraMoved)
    {
        ResetAccumulation();
    }

    bHistoryReady = bAccumulationActive && ReadIndex < PathTracingAccumulationHistoryValid.size()
        ? PathTracingAccumulationHistoryValid[ReadIndex]
        : false;

    if (!bHistoryReady)
    {
        PathTracingAccumulatedFrames = 0;
    }
}

void FPathTracing::OnFrameFenceSignaled(uint32_t FrameIndex)
{
    if (!bPathTracingAccumulationEnabled || PathTracingAccumulationFrameCount == 0)
    {
        return;
    }

    const uint32_t AccumWriteIndex = FrameIndex % static_cast<uint32_t>(PathTracingAccumulationHistoryValid.size());
    if (AccumWriteIndex < PathTracingAccumulationHistoryValid.size())
    {
        PathTracingAccumulationHistoryValid[AccumWriteIndex] = true;
    }
}

void FPathTracing::ResetAccumulation()
{
    std::fill(PathTracingAccumulationHistoryValid.begin(), PathTracingAccumulationHistoryValid.end(), false);
    PathTracingAccumulatedFrames = 0;
}

void FPathTracing::SetAccumulationEnabled(bool bEnabled)
{
    bPathTracingAccumulationEnabled = bEnabled;
    if (PathTracingDebugMode == 0)
    {
        bPathTracingAccumulationUserPreference = bEnabled;
    }
    ResetAccumulation();
}

void FPathTracing::SetDebugMode(int Mode)
{
    if (PathTracingDebugMode == Mode)
    {
        return;
    }

    if (PathTracingDebugMode == 0 && Mode >= 1 && Mode <= 12)
    {
        bPathTracingAccumulationUserPreference = bPathTracingAccumulationEnabled;
    }

    PathTracingDebugMode = Mode;
    ResetAccumulation();

    if (Mode >= 1 && Mode <= 12)
    {
        bPathTracingAccumulationEnabled = false;
    }
    else if (Mode == 0)
    {
        bPathTracingAccumulationEnabled = bPathTracingAccumulationUserPreference;
    }
}

bool FPathTracing::CreateAccumulationRootSignature(FDX12Device* Device)
{
    D3D12_ROOT_PARAMETER1 RootParams[2] = {};

    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Constants.Num32BitValues = 4;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.ShaderRegister = 0;

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

bool FPathTracing::CreateAccumulationPipeline(FDX12Device* Device)
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

bool FPathTracing::CreateAccumulationResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
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
    PathTracingAccumulatedFrames = 0;
    return true;
}

void FPathTracing::AddPathTracingPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FDeferredRenderer* OwnerPtr = &Context.Owner;
    FRenderGraph* GraphPtr = &Context.Graph;
    const FCamera& Camera = Context.Camera;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle GBufferAHandle = Context.Resources.GBufferHandles[0];
    const FRGResourceHandle GBufferBHandle = Context.Resources.GBufferHandles[1];
    const FRGResourceHandle GBufferCHandle = Context.Resources.GBufferHandles[2];
    const FRGResourceHandle OutputHandle = Context.Resources.PathTracing.TempHandle;

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

    Graph.AddPass<FPathTracingPassData>("PathTracing", [&, DepthHandle, GBufferAHandle, GBufferBHandle, GBufferCHandle, OutputHandle, this](FPathTracingPassData& Data, FRGPassBuilder& Builder)
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
        Data.FrameIndex = PathTracingAccumulatedFrames;
        Builder.WriteTexture(Data.OutputHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.ReadTexture(Data.DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferAHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferBHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Data.GBufferCHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.KeepAlive();
    }, [OwnerPtr, GraphPtr, this](const FPathTracingPassData& Data, FDX12CommandContext& CmdContext)
    {
        FDeferredRenderer& Owner = *OwnerPtr;
        FRenderGraph& Graph = *GraphPtr;
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

        ID3D12PipelineState* PathTracingPipelineState = nullptr;
        if (PathTracingDebugMode > 0)
        {
            PathTracingPipelineState = Owner.bPathTracingUseVndf ? Owner.RayQueryPathDebugVndfPipeline.Get() : Owner.RayQueryPathDebugPipeline.Get();
        }
        else
        {
            PathTracingPipelineState = Owner.bPathTracingUseVndf ? Owner.RayQueryPathVndfPipeline.Get() : Owner.RayQueryPathPipeline.Get();
        }
        if (!PathTracingPipelineState)
        {
            return;
        }
        CommandList4->SetPipelineState(PathTracingPipelineState);
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
            PathTracingMaxBounces,
            Owner.Device->GetLinearClampSamplerIndex(),
            Owner.EnvironmentCubeBindlessIndex,
            static_cast<uint32_t>(PathTracingDebugMode)
        };
        CommandList4->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        CommandList4->Dispatch(GroupCountX, GroupCountY, 1);
    });
}

void FPathTracing::AddPathTracingAccumulationPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FDeferredRenderer::FDeferredFrameState& FrameState = Context.FrameState;
    const FRGResourceHandle PathTracingTempHandle = Context.Resources.PathTracing.TempHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;
    const std::vector<FRGResourceHandle> AccumulationHandles = Context.Resources.PathTracing.AccumulationHandles;

    struct FPathTracingAccumulationPassData
    {
        bool bEnabled = false;
        bool bAccumulationActive = false;
        DirectX::XMFLOAT2 OutputSize{};
        uint32_t FrameIndex = 0;
        uint32_t UseHistory = 0;
        uint32_t ReadIndex = 0;
        uint32_t WriteIndex = 0;
    };

    Graph.AddPass<FPathTracingAccumulationPassData>("PTAccumulation", [&, PathTracingTempHandle, LightingHandle, AccumulationHandles, this](FPathTracingAccumulationPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = PathTracingTempHandle && LightingHandle;
        if (Data.bEnabled)
        {
            Data.ReadIndex = FrameState.PathTracingAccumulationReadIndex;
            Data.WriteIndex = FrameState.PathTracingAccumulationWriteIndex;
            Data.bAccumulationActive = FrameState.bPathTracingAccumulationActive;
            Data.OutputSize = DirectX::XMFLOAT2(Owner.Viewport.Width, Owner.Viewport.Height);
            Data.FrameIndex = PathTracingAccumulatedFrames;
            Data.UseHistory = (FrameState.bPathTracingAccumulationActive && FrameState.bPathTracingAccumulationHistoryReady) ? 1u : 0u;
            Builder.ReadTexture(PathTracingTempHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (FrameState.bPathTracingAccumulationActive && !AccumulationHandles.empty())
            {
                Builder.ReadTexture(AccumulationHandles[Data.ReadIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Builder.WriteTexture(AccumulationHandles[Data.WriteIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [OwnerPtr = &Context.Owner, this](const FPathTracingAccumulationPassData& Data, FDX12CommandContext& Cmd)
    {
        FDeferredRenderer& Owner = *OwnerPtr;
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
        LocalCommandList->SetPipelineState(PathTracingAccumulationPipeline.Get());
        LocalCommandList->SetComputeRootSignature(PathTracingAccumulationRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);

        const bool bAccumulationActive = Data.bAccumulationActive && !PathTracingAccumulationSrvBindlessIndices.empty();
        const uint32_t ReadIdx = bAccumulationActive ? Data.ReadIndex : 0;
        const uint32_t WriteIdx = bAccumulationActive ? Data.WriteIndex : 0;
        const uint32_t HistorySrv = bAccumulationActive && ReadIdx < PathTracingAccumulationSrvBindlessIndices.size()
            ? PathTracingAccumulationSrvBindlessIndices[ReadIdx]
            : PathTracingTempBindlessIndex;
        const uint32_t HistoryUav = bAccumulationActive && WriteIdx < PathTracingAccumulationUavBindlessIndices.size()
            ? PathTracingAccumulationUavBindlessIndices[WriteIdx]
            : PathTracingTempBindlessIndex;

        const uint32_t AccumBindlessIndices[] =
        {
            PathTracingTempBindlessIndex,
            HistorySrv,
            HistoryUav,
            Owner.LightingBufferBindlessIndex
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(AccumBindlessIndices), AccumBindlessIndices, 0);

        const uint32_t GroupX = (static_cast<uint32_t>(Data.OutputSize.x) + 7u) / 8u;
        const uint32_t GroupY = (static_cast<uint32_t>(Data.OutputSize.y) + 7u) / 8u;
        LocalCommandList->Dispatch(GroupX, GroupY, 1);

        if (bAccumulationActive)
        {
            ++PathTracingAccumulatedFrames;
        }
    });
}
