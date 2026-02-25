#include "DeferredRayTracingPasses.h"
#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include <d3dx12.h>

void FDeferredRayTracingPasses::AddRayTracingShadowPass(FDeferredPassContext& Context) const
{
    Context.Owner.AddRayTracingShadowPass(Context.Graph, Context.Camera, Context.Resources.DepthHandle, Context.Resources.GBufferHandles[0], Context.Resources.ShadowMaskHandle);
}

void FDeferredRayTracingPasses::AddPathTracingPass(FDeferredPassContext& Context) const
{
    Context.Owner.AddPathTracingPass(
        Context.Graph,
        Context.Camera,
        Context.Resources.DepthHandle,
        Context.Resources.GBufferHandles[0],
        Context.Resources.GBufferHandles[1],
        Context.Resources.GBufferHandles[2],
        Context.Resources.PathTracingTempHandle);
}

void FDeferredRayTracingPasses::AddPathTracingAccumulationPass(FDeferredPassContext& Context) const
{
    Context.Owner.AddPathTracingAccumulationPass(Context.Graph, Context.FrameState, Context.Resources.PathTracingTempHandle, Context.Resources.LightingHandle, Context.Resources.PathTracingAccumulationHandles);
}

void FDeferredRayTracingPasses::AddRestirGIPass(FDeferredPassContext& Context) const
{
    Context.Owner.AddRestirGIPass(
        Context.Graph,
        Context.FrameState,
        Context.Resources.GBufferHandles,
        Context.Resources.DepthHandle,
        Context.Resources.VelocityHandle,
        Context.Resources.LinearDepthHandle,
        Context.Resources.RestirGiPrevLinearDepthHandle,
        Context.Resources.RestirGIHandle,
        Context.Resources.RestirGIHistoryHandle,
        Context.Resources.RestirGIInitialRadianceHandle,
        Context.Resources.RestirGIInitialRayDirectionHandle,
        Context.Resources.RestirGIReservoirDepthNormalAHandle,
        Context.Resources.RestirGIReservoirDepthNormalBHandle,
        Context.Resources.RestirGIReservoirSampleRadianceAHandle,
        Context.Resources.RestirGIReservoirSampleRadianceBHandle,
        Context.Resources.RestirGIReservoirRayDirectionAHandle,
        Context.Resources.RestirGIReservoirRayDirectionBHandle,
        Context.Resources.RestirGIReservoirMWAHandle,
        Context.Resources.RestirGIReservoirMWBHandle,
        Context.Resources.RestirGiInputSHHandle,
        Context.Resources.RestirGiVarianceHandle);
}

void FDeferredRayTracingPasses::AddRestirGiDenoiserPasses(FDeferredPassContext& Context) const
{
    Context.Owner.AddRestirGiDenoiserPasses(
        Context.Graph,
        Context.FrameState,
        Context.Resources.GBufferHandles,
        Context.Resources.VelocityHandle,
        Context.Resources.LinearDepthHandle,
        Context.Resources.RestirGiInputSHHandle,
        Context.Resources.RestirGiVarianceHandle,
        Context.Resources.RestirGiTemporalSHHandle,
        Context.Resources.RestirGiHistorySHHandle,
        Context.Resources.RestirGiHistoryIrradianceHandle,
        Context.Resources.RestirGiHistoryCountAHandle,
        Context.Resources.RestirGiHistoryCountBHandle,
        Context.Resources.RestirGiPrevLinearDepthHandle,
        Context.Resources.RestirGiPrevNormalHandle,
        Context.Resources.RestirGiShMipHandles,
        Context.Resources.RestirGiLinearDepthMipHandles);
}

void FDeferredRenderer::AddRayTracingShadowPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferHandle, FRGResourceHandle& ShadowMaskHandle)
{
    struct FRayTracingShadowPassData
    {
        FRGResourceHandle ShadowMaskHandle{};
        FRGResourceHandle DepthHandle{};
        FRGResourceHandle GBufferHandle{};
        const FCamera* Camera = nullptr;
    };

    const FRGTextureDesc ShadowMaskDesc =
    {
        static_cast<uint32>(Viewport.Width),
        static_cast<uint32>(Viewport.Height),
        DXGI_FORMAT_R8_UNORM
    };

    Graph.AddPass<FRayTracingShadowPassData>("RTShadowMask", [&, ShadowMaskDesc, DepthHandle, GBufferHandle](FRayTracingShadowPassData& Data, FRGPassBuilder& Builder)
    {
        if (!bRayTracedShadowsEnabled || !bRayTracingPipelineReady || !GBufferHandle)
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
    }, [this, &Graph](const FRayTracingShadowPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!bRayTracingPipelineReady || !RayQueryShadowPipeline || !RayQueryRootSignature || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (SceneModels.empty() || Data.Camera == nullptr)
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
        if (FrameIndex >= TlasResultBuffers.size() || !TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

		FScopedPixEvent RayTracingEvent(CommandList4, L"Ray Tracing Shadow Mask Pass");

        if (FrameIndex >= RayTracingDepthSrvBindlessIndices.size())
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
        const uint32_t DepthBindlessIndex = RayTracingDepthSrvBindlessIndices[FrameIndex];
        if (DepthBindlessIndex == UINT32_MAX)
        {
            return;
        }
        if (FrameIndex < RayTracingDepthResources.size() && RayTracingDepthResources[FrameIndex] != DepthBuffer)
        {
            WriteBindlessSrv(DepthBindlessIndex, DepthBuffer, DepthSrvDesc);
            RayTracingDepthResources[FrameIndex] = DepthBuffer;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferSrvDesc = {};
        GBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferSrvDesc.Format = GBufferA->GetDesc().Format;
        GBufferSrvDesc.Texture2D.MipLevels = 1;
        GBufferSrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (RayTracingGBufferASrvBindlessIndex == UINT32_MAX)
        {
            RayTracingGBufferASrvBindlessIndex = Device->CreateBindlessSrv(GBufferA, GBufferSrvDesc);
        }
        else if (RayTracingGBufferAResource != GBufferA)
        {
            WriteBindlessSrv(RayTracingGBufferASrvBindlessIndex, GBufferA, GBufferSrvDesc);
        }
        RayTracingGBufferAResource = GBufferA;

        D3D12_UNORDERED_ACCESS_VIEW_DESC ShadowMaskUavDesc = {};
        ShadowMaskUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ShadowMaskUavDesc.Format = DXGI_FORMAT_R8_UNORM;
        ShadowMaskUavDesc.Texture2D.MipSlice = 0;
        if (RayTracingShadowMaskUavBindlessIndex == UINT32_MAX)
        {
            RayTracingShadowMaskUavBindlessIndex = Device->CreateBindlessUav(ShadowMask, nullptr, ShadowMaskUavDesc);
        }
        else if (RayTracingShadowMaskUavResource != ShadowMask)
        {
            WriteBindlessUav(RayTracingShadowMaskUavBindlessIndex, ShadowMask, nullptr, ShadowMaskUavDesc);
        }
        RayTracingShadowMaskUavResource = ShadowMask;

        if (ShadowMaskBindlessIndex == UINT32_MAX || ShadowMaskResource != ShadowMask)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
            SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SrvDesc.Format = DXGI_FORMAT_R8_UNORM;
            SrvDesc.Texture2D.MipLevels = 1;
            if (ShadowMaskBindlessIndex == UINT32_MAX)
            {
                ShadowMaskBindlessIndex = Device->CreateBindlessSrv(ShadowMask, SrvDesc);
            }
            else
            {
                WriteBindlessSrv(ShadowMaskBindlessIndex, ShadowMask, SrvDesc);
            }
            ShadowMaskResource = ShadowMask;
        }

        if (RayTracingGBufferASrvBindlessIndex == UINT32_MAX || RayTracingShadowMaskUavBindlessIndex == UINT32_MAX || ShadowMaskBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        FScopedPixEvent SsrHwTraceEvent(CommandList4, L"SSR HW Trace");
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t DispatchWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t DispatchHeight = static_cast<uint32_t>(Viewport.Height);
        if (DispatchWidth == 0 || DispatchHeight == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 8;
        const uint32_t GroupCountX = (DispatchWidth + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;
        const uint32_t GroupCountY = (DispatchHeight + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        CommandList4->SetPipelineState(RayQueryShadowPipeline.Get());
        CommandList4->SetComputeRootSignature(RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        UpdateSceneConstants(*Data.Camera, SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (FrameIndex >= PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = PathTracingInstanceDataBindlessIndices[FrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t BindlessIndices[] =
        {
            DepthBindlessIndex,
            RayTracingGBufferASrvBindlessIndex,
            RayTracingShadowMaskUavBindlessIndex,
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

void FDeferredRenderer::AddPathTracingPass(FRenderGraph& Graph, const FCamera& Camera, FRGResourceHandle DepthHandle, FRGResourceHandle GBufferAHandle, FRGResourceHandle GBufferBHandle, FRGResourceHandle GBufferCHandle, FRGResourceHandle OutputHandle)
{
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
        if (!bPathTracingEnabled || !bRayTracingPipelineReady || !DepthHandle || !GBufferAHandle || !GBufferBHandle || !GBufferCHandle || !OutputHandle)
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
    }, [this, &Graph](const FPathTracingPassData& Data, FDX12CommandContext& CmdContext)
    {
        if (!bRayTracingPipelineReady || !RayQueryRootSignature || !Device || !Device->GetBindlessDescriptorHeap())
        {
            return;
        }

        if (SceneModels.empty() || Data.Camera == nullptr)
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
        if (FrameIndex >= TlasResultBuffers.size() || !TlasResultBuffers[FrameIndex])
        {
            return;
        }

        ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
        if (!CommandList4)
        {
            return;
        }

        FScopedPixEvent PathTracingEvent(CommandList4, L"Path Tracing Pass");

        if (FrameIndex >= RayTracingDepthSrvBindlessIndices.size())
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
        const uint32_t DepthBindlessIndex = RayTracingDepthSrvBindlessIndices[FrameIndex];
        if (DepthBindlessIndex == UINT32_MAX)
        {
            return;
        }
        if (FrameIndex < RayTracingDepthResources.size() && RayTracingDepthResources[FrameIndex] != DepthBuffer)
        {
            WriteBindlessSrv(DepthBindlessIndex, DepthBuffer, DepthSrvDesc);
            RayTracingDepthResources[FrameIndex] = DepthBuffer;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferASrvDesc = {};
        GBufferASrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferASrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferASrvDesc.Format = GBufferA->GetDesc().Format;
        GBufferASrvDesc.Texture2D.MipLevels = 1;
        GBufferASrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferASrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (RayTracingGBufferASrvBindlessIndex == UINT32_MAX)
        {
            RayTracingGBufferASrvBindlessIndex = Device->CreateBindlessSrv(GBufferA, GBufferASrvDesc);
        }
        else if (RayTracingGBufferAResource != GBufferA)
        {
            WriteBindlessSrv(RayTracingGBufferASrvBindlessIndex, GBufferA, GBufferASrvDesc);
        }
        RayTracingGBufferAResource = GBufferA;

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferBSrvDesc = {};
        GBufferBSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferBSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferBSrvDesc.Format = GBufferB->GetDesc().Format;
        GBufferBSrvDesc.Texture2D.MipLevels = 1;
        GBufferBSrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferBSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (RayTracingGBufferBSrvBindlessIndex == UINT32_MAX)
        {
            RayTracingGBufferBSrvBindlessIndex = Device->CreateBindlessSrv(GBufferB, GBufferBSrvDesc);
        }
        else if (RayTracingGBufferBResource != GBufferB)
        {
            WriteBindlessSrv(RayTracingGBufferBSrvBindlessIndex, GBufferB, GBufferBSrvDesc);
        }
        RayTracingGBufferBResource = GBufferB;

        D3D12_SHADER_RESOURCE_VIEW_DESC GBufferCSrvDesc = {};
        GBufferCSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        GBufferCSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        GBufferCSrvDesc.Format = GBufferC->GetDesc().Format;
        GBufferCSrvDesc.Texture2D.MipLevels = 1;
        GBufferCSrvDesc.Texture2D.MostDetailedMip = 0;
        GBufferCSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
        if (RayTracingGBufferCSrvBindlessIndex == UINT32_MAX)
        {
            RayTracingGBufferCSrvBindlessIndex = Device->CreateBindlessSrv(GBufferC, GBufferCSrvDesc);
        }
        else if (RayTracingGBufferCResource != GBufferC)
        {
            WriteBindlessSrv(RayTracingGBufferCSrvBindlessIndex, GBufferC, GBufferCSrvDesc);
        }
        RayTracingGBufferCResource = GBufferC;

        D3D12_UNORDERED_ACCESS_VIEW_DESC OutputUavDesc = {};
        OutputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        OutputUavDesc.Format = OutputTarget->GetDesc().Format;
        OutputUavDesc.Texture2D.MipSlice = 0;
        if (RayTracingLightingUavBindlessIndex == UINT32_MAX)
        {
            RayTracingLightingUavBindlessIndex = Device->CreateBindlessUav(OutputTarget, nullptr, OutputUavDesc);
        }
        else if (RayTracingLightingResource != OutputTarget)
        {
            WriteBindlessUav(RayTracingLightingUavBindlessIndex, OutputTarget, nullptr, OutputUavDesc);
        }
        RayTracingLightingResource = OutputTarget;

        if (RayTracingGBufferASrvBindlessIndex == UINT32_MAX
            || RayTracingGBufferBSrvBindlessIndex == UINT32_MAX
            || RayTracingGBufferCSrvBindlessIndex == UINT32_MAX
            || RayTracingLightingUavBindlessIndex == UINT32_MAX
            || EnvironmentCubeBindlessIndex == UINT32_MAX)
        {
            return;
        }

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        CommandList4->SetDescriptorHeaps(_countof(Heaps), Heaps);

        const uint32_t DispatchWidth = static_cast<uint32_t>(Viewport.Width);
        const uint32_t DispatchHeight = static_cast<uint32_t>(Viewport.Height);
        if (DispatchWidth == 0 || DispatchHeight == 0)
        {
            return;
        }

        constexpr uint32_t RayQueryThreadGroupSize = 8;
        const uint32_t GroupCountX = (DispatchWidth + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;
        const uint32_t GroupCountY = (DispatchHeight + RayQueryThreadGroupSize - 1u) / RayQueryThreadGroupSize;

        ID3D12PipelineState* PathTracingPipeline = nullptr;
        if (PathTracingDebugMode > 0)
        {
            PathTracingPipeline = bPathTracingUseVndf ? RayQueryPathDebugVndfPipeline.Get() : RayQueryPathDebugPipeline.Get();
        }
        else
        {
            PathTracingPipeline = bPathTracingUseVndf ? RayQueryPathVndfPipeline.Get() : RayQueryPathPipeline.Get();
        }
        if (!PathTracingPipeline)
        {
            return;
        }
        CommandList4->SetPipelineState(PathTracingPipeline);
        CommandList4->SetComputeRootSignature(RayQueryRootSignature.Get());
        CommandList4->SetComputeRootShaderResourceView(0, TlasResultBuffers[FrameIndex]->GetGPUVirtualAddress());
        const uint64_t ConstantBufferOffset = 0;
        UpdateSceneConstants(*Data.Camera, SceneModels.front(), 0u, ConstantBufferOffset);
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = GetSceneConstantBufferAddress();
        CommandList4->SetComputeRootConstantBufferView(1, ConstantBufferAddress + ConstantBufferOffset);

        if (FrameIndex >= PathTracingInstanceDataBindlessIndices.size())
        {
            return;
        }

        const uint32_t PathTracingInstanceDataBindlessIndex = PathTracingInstanceDataBindlessIndices[FrameIndex];
        if (PathTracingInstanceDataBindlessIndex == UINT32_MAX)
        {
            return;
        }

        const uint32_t BindlessIndices[] =
        {
            DepthBindlessIndex,
            RayTracingGBufferASrvBindlessIndex,
            RayTracingGBufferBSrvBindlessIndex,
            RayTracingGBufferCSrvBindlessIndex,
            RayTracingLightingUavBindlessIndex,
            DispatchWidth,
            DispatchHeight,
            Data.FrameIndex,
            PathTracingInstanceDataBindlessIndex,
            PathTracingMaxBounces,
            Device->GetLinearClampSamplerIndex(),
            EnvironmentCubeBindlessIndex,
            static_cast<uint32_t>(PathTracingDebugMode)
        };
        CommandList4->SetComputeRoot32BitConstants(2, _countof(BindlessIndices), BindlessIndices, 0);

        CommandList4->Dispatch(GroupCountX, GroupCountY, 1);
    });
}

void FDeferredRenderer::AddPathTracingAccumulationPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, FRGResourceHandle PathTracingTempHandle, FRGResourceHandle LightingHandle, const std::vector<FRGResourceHandle>& AccumulationHandles)
{
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
            Data.OutputSize = DirectX::XMFLOAT2(Viewport.Width, Viewport.Height);
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
    }, [this, &FrameState, &AccumulationHandles](const FPathTracingAccumulationPassData& Data, FDX12CommandContext& Cmd)
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

        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetPipelineState(PathTracingAccumulationPipeline.Get());
        LocalCommandList->SetComputeRootSignature(PathTracingAccumulationRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
        
        // When accumulation is disabled, use index 0 for both read/write (doesn't matter since UseHistory=0)
        const bool bAccumulationActive = FrameState.bPathTracingAccumulationActive && !AccumulationHandles.empty();
        const uint32_t ReadIdx = bAccumulationActive ? Data.ReadIndex : 0;
        const uint32_t WriteIdx = bAccumulationActive ? Data.WriteIndex : 0;
        const uint32_t HistorySrv = bAccumulationActive && ReadIdx < PathTracingAccumulationSrvBindlessIndices.size() 
            ? PathTracingAccumulationSrvBindlessIndices[ReadIdx] 
            : PathTracingTempBindlessIndex; // Use temp as dummy when disabled
        const uint32_t HistoryUav = bAccumulationActive && WriteIdx < PathTracingAccumulationUavBindlessIndices.size()
            ? PathTracingAccumulationUavBindlessIndices[WriteIdx]
            : PathTracingTempBindlessIndex; // Use temp as dummy when disabled
        
        const uint32_t AccumBindlessIndices[] =
        {
            PathTracingTempBindlessIndex,
            HistorySrv,
            HistoryUav,
            LightingBufferBindlessIndex
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(AccumBindlessIndices), AccumBindlessIndices, 0);

        const uint32_t GroupX = (static_cast<uint32_t>(Data.OutputSize.x) + 7u) / 8u;
        const uint32_t GroupY = (static_cast<uint32_t>(Data.OutputSize.y) + 7u) / 8u;
        LocalCommandList->Dispatch(GroupX, GroupY, 1);

        // Increment accumulated frame count after dispatch (only when accumulation is active)
        if (bAccumulationActive)
        {
            PathTracingAccumulatedFrames++;
        }
    });
}

void FDeferredRenderer::AddRestirGIPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle DepthHandle, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle RestirGIHandle, FRGResourceHandle RestirGIHistoryHandle, FRGResourceHandle RestirGIInitialRadianceHandle, FRGResourceHandle RestirGIInitialRayDirectionHandle, FRGResourceHandle RestirGIReservoirDepthNormalAHandle, FRGResourceHandle RestirGIReservoirDepthNormalBHandle, FRGResourceHandle RestirGIReservoirSampleRadianceAHandle, FRGResourceHandle RestirGIReservoirSampleRadianceBHandle, FRGResourceHandle RestirGIReservoirRayDirectionAHandle, FRGResourceHandle RestirGIReservoirRayDirectionBHandle, FRGResourceHandle RestirGIReservoirMWAHandle, FRGResourceHandle RestirGIReservoirMWBHandle, FRGResourceHandle RestirGiInputSHHandle, FRGResourceHandle RestirGiVarianceHandle)
{
    struct FRestirGIPassData
    {
        bool bEnabled = false;
    };

    auto DispatchNewPass = [this, &FrameState](FDX12CommandContext& Cmd, ID3D12PipelineState* PipelineState, const wchar_t* EventName, uint32_t SpatialPassIndex, const uint32_t BindlessIndices[28], uint32_t DispatchWidth, uint32_t DispatchHeight, bool bEnabled)
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
            uint32_t TemporalReuseEnabled = 0;
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
        const uint32_t SequenceFrame = bRestirGIFreezeFrame ? RestirGIFrozenSequenceFrame : (FrameState.bTaaActive ? FrameState.TaaFrameIndex : GetFrameIndex());

        const FRestirGIConstants Constants =
        {
            FullWidth,
            FullHeight,
            HalfWidth,
            HalfHeight,
            FrameState.bTaaActive ? FrameState.TaaFrameIndex : GetFrameIndex(),
            bRestirGIEnabled ? 1u : 0u,
            (bRestirGIHistoryValid && RestirGIHistoryFrameCount >= MaxHistoryFrames) ? 1u : 0u,
            SpatialPassIndex,
            (std::max)(0.0f, RestirGIIntensity),
            RestirGIRayLength,
            RestirGIClamp,
            bRestirGITemporalReuse ? 1u : 0u,
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
        CommandList4->SetComputeRootConstantBufferView(1, GetSceneConstantBufferAddress());
        CommandList4->SetComputeRoot32BitConstants(2, sizeof(FRestirGIConstants) / sizeof(uint32_t), &Constants, 0);
        CommandList4->SetComputeRoot32BitConstants(3, 28, BindlessIndices, 0);

        const uint32_t GroupSize = 8;
        const uint32_t DispatchX = (DispatchWidth + GroupSize - 1) / GroupSize;
        const uint32_t DispatchY = (DispatchHeight + GroupSize - 1) / GroupSize;
        CommandList4->Dispatch(DispatchX, DispatchY, 1);
    };

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
    }, [this, &FrameState, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthArrayIndex = GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
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
    }, [this, &FrameState, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthArrayIndex = GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
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
    }, [this, &FrameState, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthArrayIndex = GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
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
    }, [this, &FrameState, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthArrayIndex = GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
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
    }, [this, &FrameState, DispatchNewPass](const FRestirGIPassData& Data, FDX12CommandContext& Cmd)
    {
        const uint32_t DepthArrayIndex = GetFrameIndex() % static_cast<uint32_t>(DepthBindlessIndices.size());
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
}

void FDeferredRenderer::AddRestirGiDenoiserPasses(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle, const std::array<FRGResourceHandle, 4>& ShMipHandles, const std::array<FRGResourceHandle, 4>& LinearDepthMipHandles)
{
    AddRestirGiDenoiserPreTemporalPass(Graph, FrameState, GBufferHandles, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistorySHHandle, HistoryIrradianceHandle, HistoryCountAHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle);

    for (uint32_t MipLevel = 0; MipLevel < 4u; ++MipLevel)
    {
        const FRGResourceHandle ShSourceHandle = (MipLevel == 0u) ? TemporalSHHandle : ShMipHandles[MipLevel - 1u];
        const FRGResourceHandle DepthSourceHandle = (MipLevel == 0u) ? LinearDepthHandle : LinearDepthMipHandles[MipLevel - 1u];
        AddRestirGiShMipGenPass(Graph, MipLevel, ShSourceHandle, ShMipHandles[MipLevel]);
        AddRestirGiLinearDepthMipGenPass(Graph, MipLevel, DepthSourceHandle, LinearDepthMipHandles[MipLevel]);
    }

    for (uint32_t DispatchMip = 0; DispatchMip < 4u; ++DispatchMip)
    {
        AddRestirGiHistoryReconstructionPass(Graph, DispatchMip, GBufferHandles, LinearDepthHandle, HistorySHHandle, HistoryCountBHandle, TemporalSHHandle, ShMipHandles[DispatchMip], LinearDepthMipHandles[DispatchMip]);
    }

    AddRestirGiFinalBlurPass(Graph, GBufferHandles, LinearDepthHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountBHandle);
}

void FDeferredRenderer::AddRestirGiDenoiserPreTemporalPass(FRenderGraph& Graph, const FDeferredFrameState& FrameState, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle VelocityHandle, FRGResourceHandle LinearDepthHandle, FRGResourceHandle InputSHHandle, FRGResourceHandle VarianceHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistoryCountAHandle, FRGResourceHandle HistoryCountBHandle, FRGResourceHandle PrevLinearDepthHandle, FRGResourceHandle PrevNormalHandle)
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("RestirGI Denoiser PreTemporal", [this, VelocityHandle, LinearDepthHandle, InputSHHandle, VarianceHandle, TemporalSHHandle, HistorySHHandle, HistoryIrradianceHandle, HistoryCountAHandle, HistoryCountBHandle, PrevLinearDepthHandle, PrevNormalHandle, GBufferHandles](FPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRestirGIEnabled && RestirGiDenoiserRootSignature && RestirGiPreBlurPipeline && RestirGiTemporalAccumulationPipeline;
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
    }, [this, &FrameState](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        const bool bInputsValid = (RestirGiInputSHSrvBindlessIndex != UINT32_MAX)
            && (RestirGiVarianceSrvBindlessIndex != UINT32_MAX)
            && (VelocityBindlessIndex != UINT32_MAX)
            && (LinearDepthBindlessIndex != UINT32_MAX)
            && (RestirGiPrevLinearDepthSrvBindlessIndex != UINT32_MAX)
            && (GBufferBindlessIndices[0] != UINT32_MAX)
            && (RestirGiPrevNormalSrvBindlessIndex != UINT32_MAX)
            && (RestirGiHistorySHSrvBindlessIndex != UINT32_MAX)
            && (RestirGiHistoryCountASrvBindlessIndex != UINT32_MAX)
            && (RestirGiTemporalSHUavBindlessIndex != UINT32_MAX)
            && (RestirGiHistoryCountBUavBindlessIndex != UINT32_MAX)
            && (RestirGiPrevLinearDepthUavBindlessIndex != UINT32_MAX)
            && (RestirGiPrevNormalUavBindlessIndex != UINT32_MAX);
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
        Constants.Width = static_cast<uint32_t>(Viewport.Width);
        Constants.Height = static_cast<uint32_t>(Viewport.Height);
        Constants.HistoryValid = (bRestirGIHistoryValid && !FrameState.bCameraMoved) ? 1u : 0u;

        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        auto DispatchDenoiserPass = [&](ID3D12PipelineState* Pipeline, uint32_t PassIndex, const uint32_t BindlessIndices[16])
        {
            Constants.PassIndex = PassIndex;
            Constants.MipLevel = 0u;
            LocalCommandList->SetPipelineState(Pipeline);
            LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
            LocalCommandList->SetComputeRoot32BitConstants(1, 16, BindlessIndices, 0);
            LocalCommandList->Dispatch(DispatchX, DispatchY, 1);

            D3D12_RESOURCE_BARRIER UavBarrier = {};
            UavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            UavBarrier.UAV.pResource = nullptr;
            LocalCommandList->ResourceBarrier(1, &UavBarrier);
        };

        FScopedPixEvent Event(LocalCommandList, L"RestirGI Denoiser PreTemporal");
        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RestirGiDenoiserRootSignature.Get());

        const uint32_t PreBlurBindless[16] =
        {
            RestirGiInputSHSrvBindlessIndex,
            RestirGiVarianceSrvBindlessIndex,
            VelocityBindlessIndex,
            LinearDepthBindlessIndex,
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GBufferBindlessIndices[0],
            RestirGiPrevNormalSrvBindlessIndex,
            RestirGiHistorySHSrvBindlessIndex,
            RestirGiHistoryCountASrvBindlessIndex,
            RestirGiTemporalSHUavBindlessIndex,
            RestirGiHistoryIrradianceUavBindlessIndex,
            RestirGiHistorySHUavBindlessIndex,
            RestirGiHistoryCountBUavBindlessIndex,
            RestirGiPrevLinearDepthUavBindlessIndex,
            RestirGiPrevNormalUavBindlessIndex,
            UINT32_MAX
        };

        const uint32_t TemporalBindless[16] =
        {
            RestirGiInputSHSrvBindlessIndex,
            RestirGiVarianceSrvBindlessIndex,
            VelocityBindlessIndex,
            LinearDepthBindlessIndex,
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GBufferBindlessIndices[0],
            RestirGiPrevNormalSrvBindlessIndex,
            RestirGiHistorySHSrvBindlessIndex,
            RestirGiHistoryCountASrvBindlessIndex,
            RestirGiTemporalSHUavBindlessIndex,
            RestirGiHistoryIrradianceUavBindlessIndex,
            RestirGiTemporalSHUavBindlessIndex,
            RestirGiHistoryCountBUavBindlessIndex,
            RestirGiPrevLinearDepthUavBindlessIndex,
            RestirGiPrevNormalUavBindlessIndex,
            UINT32_MAX
        };

        DispatchDenoiserPass(RestirGiPreBlurPipeline.Get(), 0u, PreBlurBindless);
        DispatchDenoiserPass(RestirGiTemporalAccumulationPipeline.Get(), 1u, TemporalBindless);
    });
}

void FDeferredRenderer::AddRestirGiShMipGenPass(FRenderGraph& Graph, uint32_t MipLevel, FRGResourceHandle SourceHandle, FRGResourceHandle DestinationHandle)
{
    struct FPassData { bool bEnabled = false; };
    const char* const PassNames[4] = { "RestirGI Denoiser SH Mip L0", "RestirGI Denoiser SH Mip L1", "RestirGI Denoiser SH Mip L2", "RestirGI Denoiser SH Mip L3" };
    Graph.AddPass<FPassData>(PassNames[(std::min)(MipLevel, 3u)], [this, SourceHandle, DestinationHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRestirGIEnabled && RestirGiDenoiserRootSignature && RestirGiGenerateShMipsPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(SourceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(DestinationHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, MipLevel](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap()) { return; }
        if (MipLevel >= RestirGiShMipSrvBindlessIndices.size() || MipLevel >= RestirGiShMipUavBindlessIndices.size()) { return; }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList || RestirGiShMipUavBindlessIndices[MipLevel] == UINT32_MAX) { return; }

        const uint32_t SourceSrv = (MipLevel == 0u) ? RestirGiTemporalSHSrvBindlessIndex : RestirGiShMipSrvBindlessIndices[MipLevel - 1u];
        if (SourceSrv == UINT32_MAX) { return; }

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Viewport.Width), static_cast<uint32_t>(Viewport.Height), 0u, 2u, 1.03f, 0.9f, 1.0f, MipLevel, 0.0f, 0.0f };
        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        uint32_t Bindless[16] =
        {
            RestirGiInputSHSrvBindlessIndex,
            RestirGiVarianceSrvBindlessIndex,
            VelocityBindlessIndex,
            LinearDepthBindlessIndex,
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GBufferBindlessIndices[0],
            RestirGiPrevNormalSrvBindlessIndex,
            RestirGiHistorySHSrvBindlessIndex,
            RestirGiHistoryCountBSrvBindlessIndex,
            SourceSrv,
            RestirGiHistoryIrradianceUavBindlessIndex,
            RestirGiHistorySHUavBindlessIndex,
            RestirGiHistoryCountBUavBindlessIndex,
            RestirGiPrevLinearDepthUavBindlessIndex,
            RestirGiPrevNormalUavBindlessIndex,
            RestirGiShMipUavBindlessIndices[MipLevel]
        };

        FScopedPixEvent Event(LocalCommandList, L"RestirGI Denoiser SH Mip");
        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetPipelineState(RestirGiGenerateShMipsPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, 16, Bindless, 0);
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FDeferredRenderer::AddRestirGiLinearDepthMipGenPass(FRenderGraph& Graph, uint32_t MipLevel, FRGResourceHandle SourceHandle, FRGResourceHandle DestinationHandle)
{
    struct FPassData { bool bEnabled = false; };
    const char* const PassNames[4] = { "RestirGI Denoiser Depth Mip L0", "RestirGI Denoiser Depth Mip L1", "RestirGI Denoiser Depth Mip L2", "RestirGI Denoiser Depth Mip L3" };
    Graph.AddPass<FPassData>(PassNames[(std::min)(MipLevel, 3u)], [this, SourceHandle, DestinationHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRestirGIEnabled && RestirGiDenoiserRootSignature && RestirGiGenerateLinearDepthMipsPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(SourceHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(DestinationHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, MipLevel](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap()) { return; }
        if (MipLevel >= RestirGiLinearDepthMipSrvBindlessIndices.size() || MipLevel >= RestirGiLinearDepthMipUavBindlessIndices.size()) { return; }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList || RestirGiLinearDepthMipUavBindlessIndices[MipLevel] == UINT32_MAX) { return; }

        const uint32_t SourceSrv = (MipLevel == 0u) ? LinearDepthBindlessIndex : RestirGiLinearDepthMipSrvBindlessIndices[MipLevel - 1u];
        if (SourceSrv == UINT32_MAX) { return; }

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Viewport.Width), static_cast<uint32_t>(Viewport.Height), 0u, 3u, 1.03f, 0.9f, 1.0f, MipLevel, 0.0f, 0.0f };
        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        uint32_t Bindless[16] =
        {
            RestirGiInputSHSrvBindlessIndex,
            RestirGiVarianceSrvBindlessIndex,
            VelocityBindlessIndex,
            SourceSrv,
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GBufferBindlessIndices[0],
            RestirGiPrevNormalSrvBindlessIndex,
            RestirGiHistorySHSrvBindlessIndex,
            RestirGiHistoryCountBSrvBindlessIndex,
            RestirGiTemporalSHUavBindlessIndex,
            RestirGiHistoryIrradianceUavBindlessIndex,
            RestirGiHistorySHUavBindlessIndex,
            RestirGiHistoryCountBUavBindlessIndex,
            RestirGiPrevLinearDepthUavBindlessIndex,
            RestirGiPrevNormalUavBindlessIndex,
            RestirGiLinearDepthMipUavBindlessIndices[MipLevel]
        };

        FScopedPixEvent Event(LocalCommandList, L"RestirGI Denoiser Depth Mip");
        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetPipelineState(RestirGiGenerateLinearDepthMipsPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, 16, Bindless, 0);
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FDeferredRenderer::AddRestirGiHistoryReconstructionPass(FRenderGraph& Graph, uint32_t DispatchMip, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle ShMipHandle, FRGResourceHandle DepthMipHandle)
{
    struct FPassData { bool bEnabled = false; };
    const char* const PassNames[4] = { "RestirGI Denoiser Reconstruction Mip 0", "RestirGI Denoiser Reconstruction Mip 1", "RestirGI Denoiser Reconstruction Mip 2", "RestirGI Denoiser Reconstruction Mip 3" };
    Graph.AddPass<FPassData>(PassNames[(std::min)(DispatchMip, 3u)], [this, GBufferHandles, LinearDepthHandle, HistorySHHandle, HistoryCountHandle, TemporalSHHandle, ShMipHandle, DepthMipHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRestirGIEnabled && RestirGiDenoiserRootSignature && RestirGiHistoryReconstructionPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistorySHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(ShMipHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(DepthMipHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this, DispatchMip](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap()) { return; }
        if (DispatchMip >= RestirGiShMipSrvBindlessIndices.size() || DispatchMip >= RestirGiLinearDepthMipSrvBindlessIndices.size()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Viewport.Width), static_cast<uint32_t>(Viewport.Height), 0u, 4u, 1.03f, 0.9f, 1.0f, DispatchMip, 0.0f, 0.0f };
        const uint32_t DispatchX = (Constants.Width + 7u) / 8u;
        const uint32_t DispatchY = (Constants.Height + 7u) / 8u;

        const uint32_t Bindless[16] =
        {
            RestirGiInputSHSrvBindlessIndex,
            RestirGiVarianceSrvBindlessIndex,
            VelocityBindlessIndex,
            LinearDepthBindlessIndex,
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GBufferBindlessIndices[0],
            RestirGiPrevNormalSrvBindlessIndex,
            RestirGiHistorySHSrvBindlessIndex,
            RestirGiHistoryCountBSrvBindlessIndex,
            RestirGiTemporalSHUavBindlessIndex,
            RestirGiHistoryIrradianceUavBindlessIndex,
            RestirGiHistorySHUavBindlessIndex,
            RestirGiHistoryCountBUavBindlessIndex,
            RestirGiLinearDepthMipSrvBindlessIndices[DispatchMip],
            RestirGiPrevNormalUavBindlessIndex,
            RestirGiShMipSrvBindlessIndices[DispatchMip]
        };

        FScopedPixEvent Event(LocalCommandList, L"RestirGI Denoiser Reconstruction");
        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetPipelineState(RestirGiHistoryReconstructionPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, 16, Bindless, 0);
        LocalCommandList->Dispatch(DispatchX, DispatchY, 1);
    });
}

void FDeferredRenderer::AddRestirGiFinalBlurPass(FRenderGraph& Graph, const std::array<FRGResourceHandle, 4>& GBufferHandles, FRGResourceHandle LinearDepthHandle, FRGResourceHandle TemporalSHHandle, FRGResourceHandle HistoryIrradianceHandle, FRGResourceHandle HistorySHHandle, FRGResourceHandle HistoryCountHandle)
{
    struct FPassData { bool bEnabled = false; };
    Graph.AddPass<FPassData>("RestirGI Denoiser FinalBlur", [this, GBufferHandles, LinearDepthHandle, TemporalSHHandle, HistoryIrradianceHandle, HistorySHHandle, HistoryCountHandle](FPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = bRestirGIEnabled && RestirGiDenoiserRootSignature && RestirGiFinalBlurPipeline;
        if (!Data.bEnabled) { return; }
        Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(LinearDepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(TemporalSHHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(HistoryCountHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(HistoryIrradianceHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Builder.WriteTexture(HistorySHHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [this](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled || !Device || !Device->GetBindlessDescriptorHeap()) { return; }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        if (!LocalCommandList) { return; }

        struct FRestirGiDenoiserConstants { uint32_t Width; uint32_t Height; uint32_t HistoryValid; uint32_t PassIndex; float DepthThresholdScale; float NormalThreshold; float BlendStrength; uint32_t MipLevel; float Padding1; float Padding2; };
        FRestirGiDenoiserConstants Constants = { static_cast<uint32_t>(Viewport.Width), static_cast<uint32_t>(Viewport.Height), 0u, 5u, 1.03f, 0.9f, 1.0f, 0u, 0.0f, 0.0f };

        const uint32_t Bindless[16] =
        {
            RestirGiInputSHSrvBindlessIndex,
            RestirGiVarianceSrvBindlessIndex,
            VelocityBindlessIndex,
            LinearDepthBindlessIndex,
            RestirGiPrevLinearDepthSrvBindlessIndex,
            GBufferBindlessIndices[0],
            RestirGiPrevNormalSrvBindlessIndex,
            RestirGiHistorySHSrvBindlessIndex,
            RestirGiHistoryCountBSrvBindlessIndex,
            RestirGiTemporalSHSrvBindlessIndex,
            RestirGiHistoryIrradianceUavBindlessIndex,
            RestirGiHistorySHUavBindlessIndex,
            RestirGiHistoryCountBUavBindlessIndex,
            RestirGiPrevLinearDepthUavBindlessIndex,
            RestirGiPrevNormalUavBindlessIndex,
            UINT32_MAX
        };

        FScopedPixEvent Event(LocalCommandList, L"RestirGI Denoiser FinalBlur");
        ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap(), Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRootSignature(RestirGiDenoiserRootSignature.Get());
        LocalCommandList->SetPipelineState(RestirGiFinalBlurPipeline.Get());
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(FRestirGiDenoiserConstants) / sizeof(uint32_t), &Constants, 0);
        LocalCommandList->SetComputeRoot32BitConstants(1, 16, Bindless, 0);
        LocalCommandList->Dispatch((Constants.Width + 7u) / 8u, (Constants.Height + 7u) / 8u, 1);
    });
}
