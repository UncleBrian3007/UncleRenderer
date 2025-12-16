#include "DeferredRenderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/Camera.h"
#include "../Scene/Mesh.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12CommandContext.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

FDeferredRenderer::FDeferredRenderer() = default;

namespace
{
    std::wstring BuildShaderTarget(const wchar_t* StagePrefix, D3D_SHADER_MODEL ShaderModel)
    {
        uint32_t ShaderModelValue = static_cast<uint32_t>(ShaderModel);
        uint32_t Major = (ShaderModelValue >> 4) & 0xF;
        uint32_t Minor = ShaderModelValue & 0xF;

        return std::wstring(StagePrefix) + L"_" + std::to_wstring(Major) + L"_" + std::to_wstring(Minor);
    }

    constexpr DXGI_FORMAT GBufferFormats[3] =
    {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM
    };
}

bool FDeferredRenderer::Initialize(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT BackBufferFormat, const FRendererOptions& Options)
{
    if (Device == nullptr)
    {
        LogError("Deferred renderer initialization failed: device is null");
        return false;
    }

    LogInfo("Deferred renderer initialization started");

    bDepthPrepassEnabled = Options.bUseDepthPrepass;

    Viewport.TopLeftX = 0.0f;
    Viewport.TopLeftY = 0.0f;
    Viewport.Width = static_cast<float>(Width);
    Viewport.Height = static_cast<float>(Height);
    Viewport.MinDepth = 0.0f;
    Viewport.MaxDepth = 1.0f;

    ScissorRect.left = 0;
    ScissorRect.top = 0;
    ScissorRect.right = static_cast<LONG>(Width);
    ScissorRect.bottom = static_cast<LONG>(Height);

    LogInfo("Creating deferred renderer base pass root signature...");
    if (!CreateBasePassRootSignature(Device))
    {
        LogError("Deferred renderer initialization failed: base pass root signature creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer lighting root signature...");
    if (!CreateLightingRootSignature(Device))
    {
        LogError("Deferred renderer initialization failed: lighting root signature creation failed");
        return false;
    }

    LogInfo("Creating deferred renderer base pass pipeline...");
    if (!CreateBasePassPipeline(Device))
    {
        LogError("Deferred renderer initialization failed: base pass pipeline creation failed");
        return false;
    }

    if (bDepthPrepassEnabled)
    {
        LogInfo("Creating deferred renderer depth prepass pipeline...");
        if (!CreateDepthPrepassPipeline(Device))
        {
            LogError("Deferred renderer initialization failed: depth prepass pipeline creation failed");
            return false;
        }
    }

    LogInfo("Creating deferred renderer lighting pipeline...");
    if (!CreateLightingPipeline(Device, BackBufferFormat))
    {
        LogError("Deferred renderer initialization failed: lighting pipeline creation failed");
        return false;
    }

    TextureLoader = std::make_unique<FTextureLoader>(Device);

    FDepthResources DepthResources = {};
    if (!RendererUtils::CreateDepthResources(Device, Width, Height, DXGI_FORMAT_D24_UNORM_S8_UINT, DepthResources))
    {
        LogError("Deferred renderer initialization failed: depth resources creation failed");
        return false;
    }
    DepthBuffer = DepthResources.DepthBuffer;
    DSVHeap = DepthResources.DSVHeap;
    DepthStencilHandle = DepthResources.DepthStencilHandle;
    DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    if (!CreateGBufferResources(Device, Width, Height))
    {
        LogError("Deferred renderer initialization failed: GBuffer resource creation failed");
        return false;
    }

    const std::wstring SceneFilePath = Options.SceneFilePath.empty() ? L"Assets/Scenes/Scene.json" : Options.SceneFilePath;
    if (!RendererUtils::CreateSceneModelsFromJson(Device, SceneFilePath, SceneModels, SceneCenter, SceneRadius))
    {
        LogWarning("Falling back to default geometry; scene JSON could not be loaded.");

        FSceneModelResource DefaultModel;
        FGltfMaterialTextures DefaultTextures;
        if (!RendererUtils::CreateDefaultSceneGeometry(Device, DefaultModel.Geometry, SceneCenter, SceneRadius, &DefaultTextures))
        {
            LogError("Deferred renderer initialization failed: default scene geometry creation failed");
            return false;
        }

        const DirectX::XMMATRIX DefaultWorld = DirectX::XMMatrixTranslation(-SceneCenter.x, -SceneCenter.y, -SceneCenter.z);
        DirectX::XMStoreFloat4x4(&DefaultModel.WorldMatrix, DefaultWorld);
        DefaultModel.Center = SceneCenter;
        const FGltfMaterialTextureSet DefaultTextureSet = DefaultTextures.PerMesh.empty() ? FGltfMaterialTextureSet{} : DefaultTextures.PerMesh.front();
        DefaultModel.BaseColorTexturePath = DefaultTextureSet.BaseColor;
        DefaultModel.MetallicRoughnessTexturePath = DefaultTextureSet.MetallicRoughness;
        DefaultModel.NormalTexturePath = DefaultTextureSet.Normal;
        DefaultModel.BaseColorFactor = { 1.0f, 1.0f, 1.0f };
        DefaultModel.MetallicFactor = 0.0f;
        DefaultModel.RoughnessFactor = 1.0f;
        DefaultModel.bHasNormalMap = !DefaultTextureSet.Normal.empty();
        SceneModels.push_back(std::move(DefaultModel));
    }

    SceneWorldMatrix = SceneModels.front().WorldMatrix;

    SceneConstantBufferStride = (sizeof(FSceneConstants) + 255ULL) & ~255ULL;
    const uint64_t ConstantBufferSize = SceneConstantBufferStride * (std::max<uint64_t>(1, SceneModels.size()));

    FMappedConstantBuffer ConstantBufferResource = {};
    if (!RendererUtils::CreateMappedConstantBuffer(Device, ConstantBufferSize, ConstantBufferResource))
    {
        LogError("Deferred renderer initialization failed: constant buffer creation failed");
        return false;
    }
    ConstantBuffer = ConstantBufferResource.Resource;
    ConstantBufferMapped = ConstantBufferResource.MappedData;

    if (!CreateSceneTextures(Device, SceneModels))
    {
        LogError("Deferred renderer initialization failed: scene texture creation failed");
        return false;
    }

    if (!CreateDescriptorHeap(Device))
    {
        LogError("Deferred renderer initialization failed: descriptor heap creation failed");
        return false;
    }

    SkySphereRadius = (std::max)(SceneRadius * 5.0f, 100.0f);
    if (!RendererUtils::CreateSkyAtmosphereResources(Device, SkySphereRadius, SkyGeometry, SkyConstantBuffer, SkyConstantBufferMapped))
    {
        LogError("Deferred renderer initialization failed: sky resource creation failed");
        return false;
    }

    FSkyPipelineConfig SkyPipelineConfig = {};
    SkyPipelineConfig.DepthEnable = true;
    SkyPipelineConfig.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    SkyPipelineConfig.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    SkyPipelineConfig.DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    if (!RendererUtils::CreateSkyAtmospherePipeline(Device, BackBufferFormat, SkyPipelineConfig, SkyRootSignature, SkyPipelineState))
    {
        LogError("Deferred renderer initialization failed: sky pipeline state creation failed");
        return false;
    }

    LogInfo("Deferred renderer initialization completed");
    return true;
}

void FDeferredRenderer::RenderFrame(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& RtvHandle, const FCamera& Camera, float DeltaTime)
{
    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    const bool bDoDepthPrepass = bDepthPrepassEnabled && DepthPrepassPipeline;

    if (bDoDepthPrepass)
    {
        PixSetMarker(CommandList, L"DepthPrepass");

        CmdContext.TransitionResource(DepthBuffer.Get(), DepthBufferState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        CmdContext.ClearDepth(DepthStencilHandle);

        ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
        CommandList->SetPipelineState(DepthPrepassPipeline.Get());
        CommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

        CommandList->RSSetViewports(1, &Viewport);
        CommandList->RSSetScissorRects(1, &ScissorRect);

        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->OMSetRenderTargets(0, nullptr, FALSE, &DepthStencilHandle);

        for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
        {
            const FSceneModelResource& Model = SceneModels[ModelIndex];
            const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

            UpdateSceneConstants(Camera, Model, ConstantBufferOffset);

            CommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
            CommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

            CommandList->SetGraphicsRootConstantBufferView(
                0,
                ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
            CommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

            CommandList->DrawIndexedInstanced(Model.Geometry.IndexCount, 1, 0, 0, 0);
        }
    }

    PixSetMarker(CommandList, L"GBuffer BasePass");

    CmdContext.TransitionResource(DepthBuffer.Get(), DepthBufferState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    CmdContext.SetRenderTarget(GBufferRTVHandles[0], &DepthStencilHandle);
    if (!bDoDepthPrepass)
    {
        CmdContext.ClearDepth(DepthStencilHandle);
    }

    for (const D3D12_CPU_DESCRIPTOR_HANDLE& Handle : GBufferRTVHandles)
    {
        const float ClearValue[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        CmdContext.ClearRenderTarget(Handle, ClearValue);
    }

    CommandList->SetGraphicsRootSignature(BasePassRootSignature.Get());

    ID3D12DescriptorHeap* Heaps[] = { DescriptorHeap.Get() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

    CommandList->RSSetViewports(1, &Viewport);
    CommandList->RSSetScissorRects(1, &ScissorRect);

    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->OMSetRenderTargets(3, GBufferRTVHandles, FALSE, &DepthStencilHandle);

    ID3D12PipelineState* CurrentPipeline = nullptr;
    for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
    {
        const FSceneModelResource& Model = SceneModels[ModelIndex];
        const uint64_t ConstantBufferOffset = SceneConstantBufferStride * ModelIndex;

        UpdateSceneConstants(Camera, Model, ConstantBufferOffset);

        ID3D12PipelineState* DesiredPipeline = Model.bHasNormalMap ? BasePassPipelineWithNormalMap.Get() : BasePassPipelineWithoutNormalMap.Get();
        if (DesiredPipeline != CurrentPipeline)
        {
            CommandList->SetPipelineState(DesiredPipeline);
            CurrentPipeline = DesiredPipeline;
        }

        CommandList->IASetVertexBuffers(0, 1, &Model.Geometry.VertexBufferView);
        CommandList->IASetIndexBuffer(&Model.Geometry.IndexBufferView);

        CommandList->SetGraphicsRootConstantBufferView(
            0,
            ConstantBuffer->GetGPUVirtualAddress() + ConstantBufferOffset);
        CommandList->SetGraphicsRootDescriptorTable(1, Model.TextureHandle);

        CommandList->DrawIndexedInstanced(Model.Geometry.IndexCount, 1, 0, 0, 0);
    }

    CmdContext.TransitionResource(GBufferA.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CmdContext.TransitionResource(GBufferB.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    CmdContext.TransitionResource(GBufferC.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    PixSetMarker(CommandList, L"LightingPass");
    CmdContext.SetRenderTarget(RtvHandle, nullptr);

    CommandList->SetPipelineState(LightingPipeline.Get());
    CommandList->SetGraphicsRootSignature(LightingRootSignature.Get());
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

    CommandList->RSSetViewports(1, &Viewport);
    CommandList->RSSetScissorRects(1, &ScissorRect);

    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());
    CommandList->SetGraphicsRootDescriptorTable(1, GBufferGpuHandles[0]);

    CommandList->DrawInstanced(3, 1, 0, 0);

    if (SkyPipelineState && SkyRootSignature && SkyGeometry.IndexCount > 0)
    {
        CmdContext.TransitionResource(DepthBuffer.Get(), DepthBufferState, D3D12_RESOURCE_STATE_DEPTH_READ);
        DepthBufferState = D3D12_RESOURCE_STATE_DEPTH_READ;

        PixSetMarker(CommandList, L"SkyAtmosphere");
        CommandList->SetPipelineState(SkyPipelineState.Get());
        CommandList->SetGraphicsRootSignature(SkyRootSignature.Get());
        CommandList->RSSetViewports(1, &Viewport);
        CommandList->RSSetScissorRects(1, &ScissorRect);
        CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        CommandList->IASetVertexBuffers(0, 1, &SkyGeometry.VertexBufferView);
        CommandList->IASetIndexBuffer(&SkyGeometry.IndexBufferView);
        CommandList->OMSetRenderTargets(1, &RtvHandle, FALSE, &DepthStencilHandle);

        UpdateSkyConstants(Camera);
        CommandList->SetGraphicsRootConstantBufferView(0, SkyConstantBuffer->GetGPUVirtualAddress());
        CommandList->DrawIndexedInstanced(SkyGeometry.IndexCount, 1, 0, 0, 0);
    }

    CmdContext.TransitionResource(GBufferA.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CmdContext.TransitionResource(GBufferB.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CmdContext.TransitionResource(GBufferC.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

bool FDeferredRenderer::CreateBasePassRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 DescriptorRange = {};
    DescriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    DescriptorRange.NumDescriptors = 3;
    DescriptorRange.BaseShaderRegister = 0;
    DescriptorRange.RegisterSpace = 0;
    DescriptorRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    DescriptorRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    RootParams[1].DescriptorTable.pDescriptorRanges = &DescriptorRange;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(BasePassRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingRootSignature(FDX12Device* Device)
{
    D3D12_DESCRIPTOR_RANGE1 DescriptorRanges[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        DescriptorRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        DescriptorRanges[i].NumDescriptors = 1;
        DescriptorRanges[i].BaseShaderRegister = static_cast<UINT>(i);
        DescriptorRanges[i].RegisterSpace = 0;
        DescriptorRanges[i].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        DescriptorRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    }

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    RootParams[1].DescriptorTable.NumDescriptorRanges = _countof(DescriptorRanges);
    RootParams[1].DescriptorTable.pDescriptorRanges = DescriptorRanges;

    D3D12_STATIC_SAMPLER_DESC SamplerDesc = {};
    SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    SamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    SamplerDesc.MinLOD = 0.0f;
    SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    SamplerDesc.ShaderRegister = 0;
    SamplerDesc.RegisterSpace = 0;
    SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootSigDesc = {};
    RootSigDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootSigDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootSigDesc.Desc_1_1.pParameters = RootParams;
    RootSigDesc.Desc_1_1.NumStaticSamplers = 1;
    RootSigDesc.Desc_1_1.pStaticSamplers = &SamplerDesc;
    RootSigDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootSigDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(LightingRootSignature.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateBasePassPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCodeWithNormalMap;
    std::vector<uint8_t> PSByteCodeWithoutNormalMap;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    const std::vector<std::wstring> WithNormalDefines = { L"USE_NORMAL_MAP=1" };
    const std::vector<std::wstring> WithoutNormalDefines = { L"USE_NORMAL_MAP=0" };

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithNormalMap, WithNormalDefines))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"PSMain", PSTarget, PSByteCodeWithoutNormalMap, WithoutNormalDefines))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    auto InitializeBasePassDesc = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc)
    {
        Desc = {};
        Desc.pRootSignature = BasePassRootSignature.Get();
        Desc.InputLayout = { InputLayout, _countof(InputLayout) };
        Desc.VS = { VSByteCode.data(), VSByteCode.size() };
        Desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        Desc.SampleDesc.Count = 1;
        Desc.SampleMask = UINT_MAX;

        Desc.RasterizerState = {};
        Desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        Desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        Desc.RasterizerState.FrontCounterClockwise = TRUE;
        Desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        Desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        Desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        Desc.RasterizerState.DepthClipEnable = TRUE;
        Desc.RasterizerState.MultisampleEnable = FALSE;
        Desc.RasterizerState.AntialiasedLineEnable = FALSE;
        Desc.RasterizerState.ForcedSampleCount = 0;
        Desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        Desc.BlendState = {};
        Desc.BlendState.AlphaToCoverageEnable = FALSE;
        Desc.BlendState.IndependentBlendEnable = TRUE;
        for (int i = 0; i < 3; ++i)
        {
            D3D12_RENDER_TARGET_BLEND_DESC RtBlend = {};
            RtBlend.BlendEnable = FALSE;
            RtBlend.LogicOpEnable = FALSE;
            RtBlend.SrcBlend = D3D12_BLEND_ONE;
            RtBlend.DestBlend = D3D12_BLEND_ZERO;
            RtBlend.BlendOp = D3D12_BLEND_OP_ADD;
            RtBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
            RtBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
            RtBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            RtBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
            RtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            Desc.BlendState.RenderTarget[i] = RtBlend;
        }

        Desc.DepthStencilState = {};
        Desc.DepthStencilState.DepthEnable = TRUE;
        Desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        Desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        Desc.DepthStencilState.StencilEnable = FALSE;
        Desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        Desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        Desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        Desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        Desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        Desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        Desc.DepthStencilState.BackFace = Desc.DepthStencilState.FrontFace;
        Desc.NumRenderTargets = 3;
        Desc.RTVFormats[0] = GBufferFormats[0];
        Desc.RTVFormats[1] = GBufferFormats[1];
        Desc.RTVFormats[2] = GBufferFormats[2];
        Desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        Desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    InitializeBasePassDesc(PsoDesc);
    PsoDesc.PS = { PSByteCodeWithNormalMap.data(), PSByteCodeWithNormalMap.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithNormalMap.GetAddressOf())));

    InitializeBasePassDesc(PsoDesc);
    PsoDesc.PS = { PSByteCodeWithoutNormalMap.data(), PSByteCodeWithoutNormalMap.size() };
    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(BasePassPipelineWithoutNormalMap.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateDepthPrepassPipeline(FDX12Device* Device)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredBasePass.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = BasePassRootSignature.Get();
    PsoDesc.InputLayout = { InputLayout, _countof(InputLayout) };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PsoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;
    PsoDesc.RasterizerState.MultisampleEnable = FALSE;
    PsoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    PsoDesc.RasterizerState.ForcedSampleCount = 0;
    PsoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    PsoDesc.BlendState.IndependentBlendEnable = FALSE;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    PsoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    PsoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    PsoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    PsoDesc.DepthStencilState.BackFace = PsoDesc.DepthStencilState.FrontFace;
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(DepthPrepassPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateLightingPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/DeferredLighting.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = LightingRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;
    PsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(LightingPipeline.GetAddressOf())));
    return true;
}

bool FDeferredRenderer::CreateGBufferResources(FDX12Device* Device, uint32_t Width, uint32_t Height)
{
    Microsoft::WRL::ComPtr<ID3D12Resource>* Targets[3] = { &GBufferA, &GBufferB, &GBufferC };

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.SampleDesc.Count = 1;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    const UINT RtvDescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = {};
    RtvHandle.ptr = 0;

    D3D12_DESCRIPTOR_HEAP_DESC RtvHeapDesc = {};
    RtvHeapDesc.NumDescriptors = 3;
    RtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    RtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&RtvHeapDesc, IID_PPV_ARGS(GBufferRTVHeap.GetAddressOf())));

    RtvHandle = GBufferRTVHeap->GetCPUDescriptorHandleForHeapStart();

    for (int i = 0; i < 3; ++i)
    {
        Desc.Width = Width;
        Desc.Height = Height;
        Desc.Format = GBufferFormats[i];

        D3D12_CLEAR_VALUE ClearValue = {};
        ClearValue.Format = Desc.Format;
        ClearValue.Color[0] = 0.0f;
        ClearValue.Color[1] = 0.0f;
        ClearValue.Color[2] = 0.0f;
        ClearValue.Color[3] = 1.0f;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            &ClearValue,
            IID_PPV_ARGS(Targets[i]->GetAddressOf())));

        GBufferRTVHandles[i] = RtvHandle;
        D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
        RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        RtvDesc.Format = GBufferFormats[i];
        Device->GetDevice()->CreateRenderTargetView(Targets[i]->Get(), &RtvDesc, RtvHandle);
        RtvHandle.ptr += RtvDescriptorSize;
    }

    return true;
}

bool FDeferredRenderer::CreateDescriptorHeap(FDX12Device* Device)
{
    const UINT TextureCount = static_cast<UINT>(SceneTextures.size());

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = TextureCount * 3 + 3;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(DescriptorHeap.GetAddressOf())));

    const UINT DescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle = DescriptorHeap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC SceneSrvDesc = {};
    SceneSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    SceneSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    SceneSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    SceneSrvDesc.Texture2D.MipLevels = 1;

    for (size_t Index = 0; Index < SceneTextures.size(); ++Index)
    {
        Device->GetDevice()->CreateShaderResourceView(SceneTextures[Index].BaseColor.Get(), &SceneSrvDesc, CpuHandle);
        SceneModels[Index].TextureHandle = GpuHandle;

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        Device->GetDevice()->CreateShaderResourceView(SceneTextures[Index].MetallicRoughness.Get(), &SceneSrvDesc, CpuHandle);

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;

        Device->GetDevice()->CreateShaderResourceView(SceneTextures[Index].Normal.Get(), &SceneSrvDesc, CpuHandle);

        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    ID3D12Resource* Buffers[3] = { GBufferA.Get(), GBufferB.Get(), GBufferC.Get() };
    for (int i = 0; i < 3; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
        SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        SrvDesc.Format = GBufferFormats[i];
        SrvDesc.Texture2D.MipLevels = 1;
        Device->GetDevice()->CreateShaderResourceView(Buffers[i], &SrvDesc, CpuHandle);
        GBufferGpuHandles[i] = GpuHandle;
        CpuHandle.ptr += DescriptorSize;
        GpuHandle.ptr += DescriptorSize;
    }

    return true;
}

namespace
{
    uint32_t ClampToByte(float Value)
    {
        const float Clamped = (std::max)(0.0f, (std::min)(1.0f, Value));
        return static_cast<uint32_t>(std::round(Clamped * 255.0f));
    }

    uint32_t PackColor(const DirectX::XMFLOAT3& Color)
    {
        const uint32_t R = ClampToByte(Color.x);
        const uint32_t G = ClampToByte(Color.y);
        const uint32_t B = ClampToByte(Color.z);
        return 0xff000000 | (R << 16) | (G << 8) | B;
    }

    uint32_t PackMetallicRoughness(float Metallic, float Roughness)
    {
        const uint32_t M = ClampToByte(Metallic);
        const uint32_t R = ClampToByte(Roughness);
        return 0xff000000 | (R << 8) | M;
    }
}

bool FDeferredRenderer::CreateSceneTextures(FDX12Device* Device, const std::vector<FSceneModelResource>& Models)
{
    if (!TextureLoader)
    {
        return false;
    }

    SceneTextures.clear();
    SceneTextures.reserve(Models.size());

    for (const FSceneModelResource& Model : Models)
    {
        FModelTextureSet TextureSet;
        const uint32_t BaseColorValue = PackColor(Model.BaseColorFactor);
        if (Model.BaseColorTexturePath.empty())
        {
            if (!TextureLoader->LoadOrSolidColor(Model.BaseColorTexturePath, BaseColorValue, TextureSet.BaseColor))
            {
                return false;
            }
        }
        else if (!TextureLoader->LoadOrDefault(Model.BaseColorTexturePath, TextureSet.BaseColor))
        {
            return false;
        }

        const uint32_t MetallicRoughnessValue = PackMetallicRoughness(Model.MetallicFactor, Model.RoughnessFactor);
        if (!TextureLoader->LoadOrSolidColor(Model.MetallicRoughnessTexturePath, MetallicRoughnessValue, TextureSet.MetallicRoughness))
        {
            return false;
        }

        if (!TextureLoader->LoadOrSolidColor(Model.NormalTexturePath, 0xff8080ff, TextureSet.Normal))
        {
            return false;
        }

        SceneTextures.push_back(TextureSet);
    }

    return true;
}

void FDeferredRenderer::UpdateSceneConstants(const FCamera& Camera, const FSceneModelResource& Model, uint64_t ConstantBufferOffset)
{
    const DirectX::XMFLOAT3 BaseColor = Model.BaseColorFactor;
    const DirectX::XMVECTOR LightDir = DirectX::XMLoadFloat3(&LightDirection);

    const DirectX::XMMATRIX World = DirectX::XMLoadFloat4x4(&Model.WorldMatrix);
    RendererUtils::UpdateSceneConstants(
        Camera,
        BaseColor,
        LightIntensity,
        LightDir,
        LightColor,
        World,
        ConstantBufferMapped,
        ConstantBufferOffset);
}

void FDeferredRenderer::UpdateSkyConstants(const FCamera& Camera)
{
    using namespace DirectX;

    const FFloat3 CameraPosition = Camera.GetPosition();
    const XMMATRIX Scale = XMMatrixScaling(SkySphereRadius, SkySphereRadius, SkySphereRadius);
    const XMMATRIX Translation = XMMatrixTranslation(CameraPosition.x, CameraPosition.y, CameraPosition.z);
    const XMMATRIX World = Scale * Translation;

    const XMVECTOR LightDir = XMLoadFloat3(&LightDirection);
    RendererUtils::UpdateSkyConstants(Camera, World, LightDir, LightColor, SkyConstantBufferMapped);
}

