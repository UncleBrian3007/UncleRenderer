#include "AutoExposure.h"

#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../RHI/DX12Device.h"
#include <d3dx12.h>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

constexpr uint32_t kAutoExposureConstantsDwordCount = 9;
constexpr uint32_t kAutoExposureBindlessDwordCount  = 3;

bool FAutoExposure::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    return CreateRootSignature(Device) && CreatePipeline(Device);
}

bool FAutoExposure::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    return CreateResources(Device);
}

void FAutoExposure::ImportPersistentResources(FDeferredPassContext& Context)
{
    FRenderGraph& Graph = Context.Graph;
    FAutoExposureFrameResources& OutResources = Context.Resources.AutoExposure;

    OutResources.LuminanceHandles =
    {
        ImportBindlessTexture(Graph, "LuminanceA", LuminanceTextures[0]),
        ImportBindlessTexture(Graph, "LuminanceB", LuminanceTextures[1])
    };
}

bool FAutoExposure::CreatePersistentDescriptors(FDeferredRenderer& Owner, FDX12Device* Device)
{
    (void)Owner;
    for (FBindlessTexture& LuminanceTexture : LuminanceTextures)
    {
        CreateBindlessTextureViews(Device, LuminanceTexture, true, true);
    }

    return true;
}

void FAutoExposure::AddPass(FDeferredPassContext& Context) const
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
        Data.bEnabled = bEnabled && Pipeline && RootSignature;
        if (Data.bEnabled)
        {
            Data.ReadIndex = 1u - LuminanceWriteIndex;
            Data.WriteIndex = LuminanceWriteIndex;
            Data.InputSize = DirectX::XMFLOAT2(Owner.Viewport.Width, Owner.Viewport.Height);
            Data.DeltaTime = Context.DeltaTime;
            Data.AdaptationSpeedUp = SpeedUp;
            Data.AdaptationSpeedDown = SpeedDown;
            Data.UseHistory = bHistoryValid ? 1u : 0u;
            Builder.ReadTexture(Context.Resources.LightingHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.ReadTexture(Context.Resources.AutoExposure.LuminanceHandles[Data.ReadIndex], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteTexture(Context.Resources.AutoExposure.LuminanceHandles[Data.WriteIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }, [&, this](const FAutoExposurePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        ID3D12GraphicsCommandList* LocalCommandList = Cmd.GetCommandList();

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
            ExposureKey,
            MinExposure,
            MaxExposure
        };

        ID3D12DescriptorHeap* Heaps[] = { Owner.Device->GetBindlessDescriptorHeap() };
        LocalCommandList->SetPipelineState(Pipeline.Get());
        LocalCommandList->SetComputeRootSignature(RootSignature.Get());
        LocalCommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        static_assert(sizeof(FAutoExposureConstants) / sizeof(uint32_t) <= kAutoExposureConstantsDwordCount);
        LocalCommandList->SetComputeRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
        const uint32_t AutoExposureBindlessIndices[kAutoExposureBindlessDwordCount] =
        {
            Owner.LightingBuffer.SrvBindlessIndex,
            LuminanceTextures[Data.ReadIndex].SrvBindlessIndex,
            LuminanceTextures[Data.WriteIndex].UavBindlessIndex
        };
        LocalCommandList->SetComputeRoot32BitConstants(1, _countof(AutoExposureBindlessIndices), AutoExposureBindlessIndices, 0);
        LocalCommandList->Dispatch(1, 1, 1);
    });
}

void FAutoExposure::FinalizeFrame()
{
    if (bEnabled)
    {
        bHistoryValid = true;
        LuminanceWriteIndex = 1u - LuminanceWriteIndex;
    }
    else
    {
        bHistoryValid = false;
    }
}

bool FAutoExposure::CreateResources(FDX12Device* Device)
{
    const FRGTextureDesc TextureDesc = { 1u, 1u, DXGI_FORMAT_R32_FLOAT };
    constexpr D3D12_RESOURCE_FLAGS TextureFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    CreateBindlessTexture(
        Device, L"LogAverageLuminanceA", TextureDesc, TextureFlags, InitialState, LuminanceTextures[0], false, false);
    CreateBindlessTexture(
        Device, L"LogAverageLuminanceB", TextureDesc, TextureFlags, InitialState, LuminanceTextures[1], false, false);
    return true;
}

bool FAutoExposure::CreateRootSignature(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].InitAsConstants(kAutoExposureConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);
    RootParams[1].InitAsConstants(kAutoExposureBindlessDwordCount, 1, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC SamplerDesc;
    SamplerDesc.Init(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 0,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc;
    RootSigDesc.Init_1_1(
        _countof(RootParams), RootParams,
        1, &SamplerDesc,
        D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

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

bool FAutoExposure::CreatePipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;

    std::vector<uint8_t> CSByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/AutoExposure.hlsl", CSByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature.Get();
    PsoDesc.CS = { CSByteCode.data(), CSByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PsoDesc, IID_PPV_ARGS(Pipeline.GetAddressOf())));
    return true;
}
