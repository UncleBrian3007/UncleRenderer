#include "DeferredPostProcessPasses.h"

#include "../DeferredRenderer.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"

void FDeferredPostProcessPasses::AddTemporalAAPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FTemporalAAPassData
    {
        bool bEnabled = false;
        DirectX::XMFLOAT2 OutputSize{};
        float HistoryWeight = 0.9f;
        uint32_t UseHistory = 0;
        uint32_t ReadIndex = 0;
        uint32_t WriteIndex = 0;
    };

    Context.Graph.AddPass<FTemporalAAPassData>("TemporalAA", [&](FTemporalAAPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bTaaActive;
        if (Data.bEnabled)
        {
            Data.ReadIndex = Context.FrameState.TaaReadIndex;
            Data.WriteIndex = Context.FrameState.TaaWriteIndex;
            Data.OutputSize = DirectX::XMFLOAT2(Owner.Viewport.Width, Owner.Viewport.Height);
            Data.HistoryWeight = Owner.TaaHistoryWeight;
            Data.UseHistory = Context.FrameState.bTaaHistoryReady ? 1u : 0u;
            Builder.ReadTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadTexture(Context.Resources.TaaHandles[Data.ReadIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteTexture(Context.Resources.TaaHandles[Data.WriteIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [&Owner](const FTemporalAAPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent TaaEvent(LocalCommandList, L"TemporalAA");

        struct FTemporalAAConstants
        {
            uint32_t OutputWidth;
            uint32_t OutputHeight;
            float HistoryWeight;
            uint32_t UseHistory;
        };

        const FTemporalAAConstants Constants =
        {
            static_cast<uint32_t>(Data.OutputSize.x),
            static_cast<uint32_t>(Data.OutputSize.y),
            Data.HistoryWeight,
            Data.UseHistory
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetPipelineState(Owner.TaaPipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.TaaRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
        const uint32_t TaaBindlessIndices[] =
        {
            Owner.LightingBufferBindlessIndex,
            Owner.TaaSrvBindlessIndices[Data.ReadIndex],
            Owner.TaaUavBindlessIndices[Data.WriteIndex]
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(TaaBindlessIndices), TaaBindlessIndices, 0);

        const uint32_t GroupX = (static_cast<uint32_t>(Data.OutputSize.x) + 7u) / 8u;
        const uint32_t GroupY = (static_cast<uint32_t>(Data.OutputSize.y) + 7u) / 8u;
        LocalCommandList->Dispatch(GroupX, GroupY, 1);
    });
}

void FDeferredPostProcessPasses::AddAutoExposurePass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FAutoExposurePassData
    {
        bool bEnabled = false;
        DirectX::XMFLOAT2 InputSize{};
        float DeltaTime = 0.0f;
        float AdaptationSpeedUp = 3.0f;
        float AdaptationSpeedDown = 1.0f;
        uint32_t UseHistory = 0;
        uint32_t ReadIndex = 0;
        uint32_t WriteIndex = 0;
    };

    Context.Graph.AddPass<FAutoExposurePassData>("AutoExposure", [&](FAutoExposurePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.bAutoExposureEnabled && Owner.AutoExposurePipeline && Owner.AutoExposureRootSignature;
        if (Data.bEnabled)
        {
            Data.ReadIndex = 1u - Owner.LuminanceWriteIndex;
            Data.WriteIndex = Owner.LuminanceWriteIndex;
            Data.InputSize = DirectX::XMFLOAT2(Owner.Viewport.Width, Owner.Viewport.Height);
            Data.DeltaTime = Context.DeltaTime;
            Data.AdaptationSpeedUp = Owner.AutoExposureSpeedUp;
            Data.AdaptationSpeedDown = Owner.AutoExposureSpeedDown;
            Data.UseHistory = Owner.bLuminanceHistoryValid ? 1u : 0u;
            Builder.ReadTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadTexture(Context.Resources.LuminanceHandles[Data.ReadIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteTexture(Context.Resources.LuminanceHandles[Data.WriteIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [&Owner](const FAutoExposurePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent AutoExposureEvent(LocalCommandList, L"AutoExposure");

        struct FAutoExposureConstants
        {
            DirectX::XMFLOAT2 InputSize;
            float DeltaTime;
            float AdaptationSpeedUp;
            float AdaptationSpeedDown;
            uint32_t UseHistory;
            float AutoExposureKey;
            float AutoExposureMin;
            float AutoExposureMax;
        };

        const FAutoExposureConstants Constants =
        {
            Data.InputSize,
            Data.DeltaTime,
            Data.AdaptationSpeedUp,
            Data.AdaptationSpeedDown,
            Data.UseHistory,
            Owner.AutoExposureKey,
            Owner.AutoExposureMin,
            Owner.AutoExposureMax
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetPipelineState(Owner.AutoExposurePipeline.Get());
        LocalCommandList->SetComputeRootSignature(Owner.AutoExposureRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
        const uint32_t AutoExposureBindlessIndices[] =
        {
            Owner.LightingBufferBindlessIndex,
            Owner.LuminanceSrvBindlessIndices[Data.ReadIndex],
            Owner.LuminanceUavBindlessIndices[Data.WriteIndex]
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(AutoExposureBindlessIndices), AutoExposureBindlessIndices, 0);
        LocalCommandList->Dispatch(1, 1, 1);
    });
}

void FDeferredPostProcessPasses::AddTonemapPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FTonemapPassData
    {
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        uint32_t InputBindlessIndex = UINT32_MAX;
        bool bUseCas = false;
        bool bUseAutoExposure = false;
        bool bUseTaa = false;
        uint32_t LuminanceIndex = 0;
    };

    Context.Graph.AddPass<FTonemapPassData>("Tonemap", [&](FTonemapPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bUseCas = Context.FrameState.bCasActive;
        Data.OutputHandle = Data.bUseCas ? Owner.TonemapOutputRtvHandle : Context.RtvHandle;
        Data.bUseAutoExposure = Owner.bAutoExposureEnabled;
        Data.bUseTaa = Context.FrameState.bTaaActive;
        Data.LuminanceIndex = Owner.LuminanceWriteIndex;
        Data.InputBindlessIndex = Data.bUseTaa ? Owner.TaaSrvBindlessIndices[Context.FrameState.TaaWriteIndex] : Owner.LightingBufferBindlessIndex;
        if (Data.bUseTaa)
        {
            Builder.ReadTexture(Context.Resources.TaaHandles[Context.FrameState.TaaWriteIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        else
        {
            Builder.ReadTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (Data.bUseCas)
        {
            Builder.WriteTexture(Context.Resources.TonemapOutputResource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        Builder.ReadTexture(Context.Resources.LuminanceHandles[Data.LuminanceIndex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        for (int i = 0; i < 4; ++i)
        {
            Builder.WriteTexture(Context.Resources.GBufferHandles[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [&Owner](const FTonemapPassData& Data, FDX12CommandContext& Cmd)
    {
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent TonemapEvent(LocalCommandList, L"Tonemap");
        Cmd.SetRenderTarget(Data.OutputHandle, nullptr);

        struct FTonemapConstants
        {
            uint32_t Enabled;
            uint32_t AutoExposureEnabled;
            float Exposure;
            float Gamma;
        };

        const FTonemapConstants TonemapConstants =
        {
            Owner.bTonemapEnabled ? 1u : 0u,
            Owner.bAutoExposureEnabled ? 1u : 0u,
            Owner.TonemapExposure,
            Owner.TonemapGamma
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap(), Owner.Device->GetSamplerDescriptorHeap() };
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        LocalCommandList->SetPipelineState(Owner.TonemapPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.TonemapRootSignature.Get());

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(TonemapConstants) / sizeof(uint32_t), &TonemapConstants, 0);
        const uint32_t TonemapBindlessIndices[] =
        {
            Data.InputBindlessIndex,
            Owner.LuminanceSrvBindlessIndices[Data.LuminanceIndex]
        };
        LocalCommandList->SetGraphicsRoot32BitConstants(1, _countof(TonemapBindlessIndices), TonemapBindlessIndices, 0);
        LocalCommandList->DrawInstanced(3, 1, 0, 0);

        Cmd.TransitionResource(Owner.LightingBuffer.Get(), Owner.LightingBufferState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Owner.LightingBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    });
}

void FDeferredPostProcessPasses::AddCasPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FCasPassData
    {
        bool bEnabled = false;
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
        DirectX::XMFLOAT2 TexelDelta{};
        float Sharpness = 0.0f;
    };

    Context.Graph.AddPass<FCasPassData>("CAS", [&](FCasPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Context.FrameState.bCasActive;
        if (!Data.bEnabled)
        {
            return;
        }
        Data.OutputHandle = Context.RtvHandle;
        Data.TexelDelta = DirectX::XMFLOAT2(1.0f / Owner.Viewport.Width, 1.0f / Owner.Viewport.Height);
        Data.Sharpness = Owner.CasSharpness;
        Builder.ReadTexture(Context.Resources.TonemapOutputResource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }, [&Owner](const FCasPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }
        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();
        FScopedPixEvent CasEvent(LocalCommandList, L"CAS");
        Cmd.SetRenderTarget(Data.OutputHandle, nullptr);

        struct FCasConstants
        {
            DirectX::XMFLOAT2 TexelDelta;
            float Sharpness;
            float Padding;
        };

        const FCasConstants CasConstants =
        {
            Data.TexelDelta,
            Data.Sharpness,
            0.0f
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetPipelineState(Owner.CasPipeline.Get());
        LocalCommandList->SetGraphicsRootSignature(Owner.CasRootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        LocalCommandList->RSSetViewports(1, &Owner.Viewport);
        LocalCommandList->RSSetScissorRects(1, &Owner.ScissorRect);

        LocalCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        LocalCommandList->SetGraphicsRoot32BitConstants(0, sizeof(CasConstants) / sizeof(uint32_t), &CasConstants, 0);
        LocalCommandList->SetGraphicsRoot32BitConstant(1, Owner.TonemapOutputBindlessIndex, 0);
        LocalCommandList->DrawInstanced(3, 1, 0, 0);
    });
}

void FDeferredPostProcessPasses::AddDebugPrintPass(FDeferredPassContext& Context) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FDebugPrintPassData
    {
        bool bEnabled = false;
        D3D12_CPU_DESCRIPTOR_HANDLE OutputHandle{};
    };

    Context.Graph.AddPass<FDebugPrintPassData>("GpuDebugPrint", [&Owner, &Context](FDebugPrintPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = Owner.bEnableGpuDebugPrint && Owner.GpuDebugPrintPipeline && Owner.GpuDebugPrintRootSignature
            && Owner.GpuDebugLinePipeline && Owner.GpuDebugLineRootSignature
            && Owner.Device && Owner.Device->GetBindlessDescriptorHeap();
        Data.OutputHandle = Context.RtvHandle;
        if (Data.bEnabled)
        {
            Builder.KeepAlive();
        }
    }, [&Owner](const FDebugPrintPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        Owner.DispatchGpuDebugPrintStats(Cmd);
        Owner.RenderGpuDebugPrint(Cmd, Data.OutputHandle);
        Owner.RenderGpuDebugLine(Cmd, Data.OutputHandle);
    });
}
