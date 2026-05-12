#include "PathTracing.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/RendererConfig.h"
#include "../RendererUtils.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include "../ShaderCompiler.h"
#include <d3dx12.h>
#include <algorithm>
#include <cassert>
#include <string>

using Microsoft::WRL::ComPtr;

constexpr uint32_t kPathTracingConstantsDwordCount = 4;
constexpr uint32_t kPathTracingBindlessDwordCount  = 4;

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
    FRenderGraph& Graph = Context.Graph;
    FPathTracingFrameResources& OutResources = Context.Resources.PathTracing;

    OutResources.TempHandle = ImportBindlessTexture(Graph, "PathTracingTemp", PathTracingTempTexture);

    OutResources.AccumulationHandles.reserve(PathTracingAccumulationTextures.size());
    for (size_t Index = 0; Index < PathTracingAccumulationTextures.size(); ++Index)
    {
        const std::string HandleName = "PathTracingAccumulation_" + std::to_string(Index);
        OutResources.AccumulationHandles.push_back(ImportBindlessTexture(Graph, HandleName, PathTracingAccumulationTextures[Index]));
    }
}

bool FPathTracing::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    if (PathTracingTempTexture)
    {
        CreateBindlessTextureViews(Device, PathTracingTempTexture, true, true);
    }

    for (FBindlessTexture& AccumulationTexture : PathTracingAccumulationTextures)
    {
        CreateBindlessTextureViews(Device, AccumulationTexture, true, true);
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

void FPathTracing::ApplyConfig(const FRendererConfig& Config)
{
    if (bEnabled != Config.bEnablePathTracing)
    {
        bEnabled = Config.bEnablePathTracing;
        ResetAccumulation();
    }
    if (bUseVndf != Config.bEnablePathTracingVndf)
    {
        bUseVndf = Config.bEnablePathTracingVndf;
        ResetAccumulation();
    }
    SetAccumulationEnabled(Config.bEnablePathTracingAccumulation);
    SetMaxBounces(Config.PathTracingMaxBounces);
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

void FPathTracing::SetMaxBounces(uint32_t MaxBounces)
{
    if (PathTracingMaxBounces == MaxBounces)
    {
        return;
    }

    PathTracingMaxBounces = MaxBounces;
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
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(kPathTracingConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kPathTracingBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

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

    std::vector<uint8_t> CSByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/PathTracing/PathTracingAccumulation.hlsl", CSByteCode))
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
    const uint32_t EffectiveFrameCount = (std::max)(1u, FrameCount);
    const FRGTextureDesc TextureDesc = { Width, Height, FDeferredRenderer::PathTracingBufferFormat };

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    const D3D12_RESOURCE_DESC Desc = CreateTexture2DResourceDesc(TextureDesc, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(PathTracingTempTexture.ReleaseAndGetAddressOf())));
    InitializeBindlessTexture(PathTracingTempTexture, TextureDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

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
            IID_PPV_ARGS(PathTracingAccumulationTextures[Index].ReleaseAndGetAddressOf())));
        InitializeBindlessTexture(PathTracingAccumulationTextures[Index], TextureDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (PathTracingAccumulationTextures[Index])
        {
            const std::wstring ResourceName = L"PathTracingAccumulation_" + std::to_wstring(Index);
            PathTracingAccumulationTextures[Index]->SetName(ResourceName.c_str());
        }
    }

    PathTracingAccumulationFrameCount = EffectiveFrameCount;
    PathTracingAccumulationHistoryValid.assign(EffectiveFrameCount, false);
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
        if (!bEnabled || !Owner.GetRayTracingRuntime().bRayTracingPipelineReady || !DepthHandle || !GBufferAHandle || !GBufferBHandle || !GBufferCHandle || !OutputHandle)
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
        if (!Owner.GetRayTracingRuntime().bRayTracingPipelineReady || !Owner.GetRayTracingRuntime().RayQueryRootSignature)
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
        if (FrameIndex >= Owner.GetRayTracingRuntime().TlasResultBuffers.size() || !Owner.GetRayTracingRuntime().TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }


        FRayTracingRuntime& RayTracing = Owner.GetRayTracingRuntime();
        const uint32_t DepthBindlessIndex = RayTracing.UpdateDepthSrv(Owner, FrameIndex, DepthBuffer);
        const uint32_t GBufferABindlessIndex = RayTracing.UpdateGBufferSrv(Owner, FRayTracingRuntime::EGBufferSlot::A, GBufferA);
        const uint32_t GBufferBBindlessIndex = RayTracing.UpdateGBufferSrv(Owner, FRayTracingRuntime::EGBufferSlot::B, GBufferB);
        const uint32_t GBufferCBindlessIndex = RayTracing.UpdateGBufferSrv(Owner, FRayTracingRuntime::EGBufferSlot::C, GBufferC);
        const uint32_t LightingUavBindlessIndex = RayTracing.UpdateLightingUav(Owner, OutputTarget);

        if (DepthBindlessIndex == UINT32_MAX
            || GBufferABindlessIndex == UINT32_MAX
            || GBufferBBindlessIndex == UINT32_MAX
            || GBufferCBindlessIndex == UINT32_MAX
            || LightingUavBindlessIndex == UINT32_MAX
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
            PathTracingPipelineState = bUseVndf ? Owner.GetRayTracingRuntime().RayQueryPathDebugVndfPipeline.Get() : Owner.GetRayTracingRuntime().RayQueryPathDebugPipeline.Get();
        }
        else
        {
            PathTracingPipelineState = bUseVndf ? Owner.GetRayTracingRuntime().RayQueryPathVndfPipeline.Get() : Owner.GetRayTracingRuntime().RayQueryPathPipeline.Get();
        }
        if (!PathTracingPipelineState)
        {
            return;
        }
        CommandList4->SetPipelineState(PathTracingPipelineState);
        CommandList4->SetComputeRootSignature(Owner.GetRayTracingRuntime().RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, Owner.GetRayTracingRuntime().TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        Owner.UpdateSceneConstants(*Data.Camera, Owner.SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = Owner.GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (FrameIndex >= Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = Owner.GetRayTracingRuntime().PathTracingInstanceDataBindlessIndices[FrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t BindlessIndices[] =
        {
            DepthBindlessIndex,
            GBufferABindlessIndex,
            GBufferBBindlessIndex,
            GBufferCBindlessIndex,
            LightingUavBindlessIndex,
            DispatchWidth,
            DispatchHeight,
            Data.FrameIndex,
            PathTracingInstanceDataBindlessIndex,
            PathTracingMaxBounces,
            Owner.Device->GetLinearClampSamplerIndex(),
            Owner.EnvironmentCubeBindlessIndex,
            static_cast<uint32_t>(PathTracingDebugMode)
        };
        static_assert(_countof(BindlessIndices) <= FRayTracingRuntime::RayQueryRootConstantDwordCount, "Ray query root constants exceed root signature capacity.");
        assert(_countof(BindlessIndices) <= FRayTracingRuntime::RayQueryRootConstantDwordCount);
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
        static_assert(sizeof(FPathTracingAccumulationConstants) / sizeof(uint32_t) <= kPathTracingConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);

        const bool bAccumulationActive = Data.bAccumulationActive && !PathTracingAccumulationTextures.empty();
        const uint32_t ReadIdx = bAccumulationActive ? Data.ReadIndex : 0;
        const uint32_t WriteIdx = bAccumulationActive ? Data.WriteIndex : 0;
        const uint32_t HistorySrv = bAccumulationActive && ReadIdx < PathTracingAccumulationTextures.size()
            ? PathTracingAccumulationTextures[ReadIdx].SrvBindlessIndex
            : PathTracingTempTexture.SrvBindlessIndex;
        const uint32_t HistoryUav = bAccumulationActive && WriteIdx < PathTracingAccumulationTextures.size()
            ? PathTracingAccumulationTextures[WriteIdx].UavBindlessIndex
            : PathTracingTempTexture.UavBindlessIndex;

        const uint32_t AccumBindlessIndices[] =
        {
            PathTracingTempTexture.SrvBindlessIndex,
            HistorySrv,
            HistoryUav,
            Owner.LightingBuffer.SrvBindlessIndex
        };
        static_assert(_countof(AccumBindlessIndices) <= kPathTracingBindlessDwordCount);
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
