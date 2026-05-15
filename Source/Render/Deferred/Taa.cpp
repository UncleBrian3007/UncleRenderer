#include "Taa.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include "../../Scene/Camera.h"
#include <algorithm>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t kTaaConstantsDwordCount = 4;
    constexpr uint32_t kTaaBindlessDwordCount  = 3;

    float HaltonSequence(uint32_t Index, uint32_t Base)
    {
        float Result = 0.0f;
        float Fraction = 1.0f / static_cast<float>(Base);
        uint32_t Current = Index;
        while (Current > 0)
        {
            Result += static_cast<float>(Current % Base) * Fraction;
            Current /= Base;
            Fraction /= static_cast<float>(Base);
        }
        return Result;
    }

    DirectX::XMFLOAT2 BuildTaaJitter(uint32_t SampleIndex)
    {
        const uint32_t Index = SampleIndex + 1;
        const float JitterX = HaltonSequence(Index, 2) - 0.5f;
        const float JitterY = HaltonSequence(Index, 3) - 0.5f;
        return DirectX::XMFLOAT2(JitterX, JitterY);
    }
}

bool FTaa::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    (void)Owner;
    (void)BackBufferFormat;
    return CreateRootSignature(Device)
        && CreatePipeline(Device);
}

bool FTaa::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
{
    (void)Owner;
    return CreateResources(Device, Width, Height, FrameCount);
}

void FTaa::ImportPersistentResources(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    FTaaFrameResources& OutResources = Context.Resources.Taa;

    OutResources.HistoryHandles.clear();
    OutResources.HistoryHandles.reserve(HistoryTextures.size());
    for (size_t Index = 0; Index < HistoryTextures.size(); ++Index)
    {
        const std::string HandleName = "TaaHistory_" + std::to_string(Index);
        OutResources.HistoryHandles.push_back(ImportBindlessTexture(Graph, HandleName, HistoryTextures[Index]));
    }
}

bool FTaa::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    for (FBindlessTexture& HistoryTexture : HistoryTextures)
    {
        WriteOrCreateBindlessTextureViews(Device, HistoryTexture, true, true);
    }

    return true;
}

void FTaa::AddPass(FDeferredPassContext& Context) const
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
        if (!Data.bEnabled)
        {
            return;
        }

        Data.ReadIndex = Context.FrameState.TaaReadIndex;
        Data.WriteIndex = Context.FrameState.TaaWriteIndex;
        Data.OutputSize = DirectX::XMFLOAT2(Owner.Viewport.Width, Owner.Viewport.Height);
        Data.HistoryWeight = HistoryWeight;
        Data.UseHistory = Context.FrameState.bTaaHistoryReady ? 1u : 0u;
        Builder.ReadTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.ReadTexture(Context.Resources.Taa.HistoryHandles[Data.ReadIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Builder.WriteTexture(Context.Resources.Taa.HistoryHandles[Data.WriteIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }, [&, this](const FTemporalAAPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

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
        LocalCommandList->SetPipelineState(Pipeline.Get());
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        static_assert(sizeof(FTemporalAAConstants) / sizeof(uint32_t) <= kTaaConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
        const uint32_t TaaBindlessIndices[kTaaBindlessDwordCount] =
        {
            Owner.LightingBuffer.SrvBindlessIndex,
            GetHistorySrvBindlessIndex(Data.ReadIndex),
            GetHistoryUavBindlessIndex(Data.WriteIndex)
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(TaaBindlessIndices), TaaBindlessIndices, 0);

        const uint32_t GroupX = (static_cast<uint32_t>(Data.OutputSize.x) + 7u) / 8u;
        const uint32_t GroupY = (static_cast<uint32_t>(Data.OutputSize.y) + 7u) / 8u;
        LocalCommandList->Dispatch(GroupX, GroupY, 1);
    });
}

void FTaa::PrepareFrameState(FDeferredRenderer& Owner, const FCamera& Camera, bool bUseGtaoJitter,
    bool& bTaaActive, bool& bTaaHistoryReady, uint32_t& TaaFrameIndex, uint32_t& TaaReadIndex, uint32_t& TaaWriteIndex)
{
    bTaaActive = IsReady();
    TaaFrameIndex = Owner.GetFrameIndex();
    const uint32_t FrameCount = static_cast<uint32_t>(HistoryTextures.size());
    TaaReadIndex = FrameCount > 0 ? (TaaFrameIndex + FrameCount - 1u) % FrameCount : 0u;
    TaaWriteIndex = FrameCount > 0 ? TaaFrameIndex % FrameCount : 0u;
    bTaaHistoryReady = bTaaActive && TaaReadIndex < HistoryValid.size()
        ? HistoryValid[TaaReadIndex]
        : false;

    bUseJitter = bTaaActive && bTaaHistoryReady;
    const bool bNeedJitter = bUseJitter || bUseGtaoJitter;
    if (bNeedJitter)
    {
        Jitter = BuildTaaJitter(SampleIndex);
    }
    else
    {
        Jitter = DirectX::XMFLOAT2(0.0f, 0.0f);
    }

    DirectX::XMFLOAT4X4 ProjectionMatrix = {};
    DirectX::XMStoreFloat4x4(&ProjectionMatrix, Camera.GetProjectionMatrix());
    if (bUseJitter && Owner.Viewport.Width > 0.0f && Owner.Viewport.Height > 0.0f)
    {
        const float JitterX = (2.0f * Jitter.x) / Owner.Viewport.Width;
        const float JitterY = (2.0f * Jitter.y) / Owner.Viewport.Height;
        ProjectionMatrix._31 += JitterX;
        ProjectionMatrix._32 += JitterY;
    }
    Projection = DirectX::XMLoadFloat4x4(&ProjectionMatrix);
}

void FTaa::FinalizeFrameState(bool bTaaActive, bool bGtaoJitterActive)
{
    if (bTaaActive || bGtaoJitterActive)
    {
        SampleIndex = (SampleIndex + 1u) % 8u;
    }
    else
    {
        ResetHistoryState();
    }
}

void FTaa::OnFrameFenceSignaled(uint32_t FrameIndex)
{
    if (!bEnabled || HistoryValid.empty())
    {
        return;
    }

    const uint32_t TaaWriteIndex = FrameIndex % static_cast<uint32_t>(HistoryValid.size());
    if (TaaWriteIndex < HistoryValid.size())
    {
        HistoryValid[TaaWriteIndex] = true;
    }
}

void FTaa::SetEnabled(bool bEnabled)
{
    if (this->bEnabled == bEnabled)
    {
        return;
    }

    this->bEnabled = bEnabled;
    ResetHistoryState();
}

bool FTaa::IsReady() const
{
    return bEnabled && Pipeline && RootSignature && !HistoryTextures.empty();
}

uint32_t FTaa::GetHistorySrvBindlessIndex(uint32_t Index) const
{
    return Index < HistoryTextures.size() ? HistoryTextures[Index].SrvBindlessIndex : UINT32_MAX;
}

uint32_t FTaa::GetHistoryUavBindlessIndex(uint32_t Index) const
{
    return Index < HistoryTextures.size() ? HistoryTextures[Index].UavBindlessIndex : UINT32_MAX;
}

bool FTaa::CreateResources(FDX12Device* Device, uint32_t Width, uint32_t Height, uint32_t FrameCount)
{
    const uint32_t EffectiveFrameCount = (std::max)(1u, FrameCount);
    const FRGTextureDesc HistoryDesc = { Width, Height, FDeferredRenderer::LightingBufferFormat };

    HistoryTextures.clear();
    HistoryTextures.resize(EffectiveFrameCount);
    for (uint32_t Index = 0; Index < EffectiveFrameCount; ++Index)
    {
        const std::wstring ResourceName = L"TaaHistory_" + std::to_wstring(Index);
        CreateBindlessTexture(
            Device,
            ResourceName,
            HistoryDesc,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            HistoryTextures[Index],
            false,
            false);
    }

    HistoryValid.assign(EffectiveFrameCount, false);
    ResetHistoryState();
    return true;
}

bool FTaa::CreateRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(kTaaConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kTaaBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(RootSignature.GetAddressOf())));
    return true;
}

bool FTaa::CreatePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;

    std::vector<uint8_t> CSByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/TemporalAA.hlsl", CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(Pipeline.GetAddressOf())));
    return true;
}

void FTaa::ResetHistoryState()
{
    std::fill(HistoryValid.begin(), HistoryValid.end(), false);
    SampleIndex = 0;
    Jitter = DirectX::XMFLOAT2(0.0f, 0.0f);
    Projection = DirectX::XMMatrixIdentity();
    bUseJitter = false;
}
