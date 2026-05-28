#include "Renderer.h"

#include "EnvironmentMap.h"
#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "TextureLoader.h"
#include "DebugPrintFont.h"
#include "../Scene/Camera.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12CommandQueue.h"
#include "../RHI/DX12Device.h"
#include "../RHI/RayTracing.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include "../Core/RendererConfig.h"
#include <d3dx12.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>

constexpr uint32_t kSkinningConstantsDwordCount = 6;




FRenderer::~FRenderer()
{
}

void FRenderer::ApplyPathTracingConfig(const FRendererConfig& Config)
{
}

D3D12_CPU_DESCRIPTOR_HANDLE FRenderer::GetBindlessCpuHandle(uint32_t Index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE Handle{};
    if (!Device->GetBindlessDescriptorHeap())
    {
        return Handle;
    }

    const uint32_t Stride = Device->GetBindlessDescriptorStride();
    Handle = Device->GetBindlessDescriptorHeap()->GetCPUDescriptorHandleForHeapStart();
    Handle.ptr += static_cast<SIZE_T>(Index) * Stride;
    return Handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE FRenderer::GetBindlessCpuClearHandle(uint32_t Index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE Handle{};
    if (!Device->GetBindlessCpuDescriptorHeap())
    {
        return Handle;
    }

    const uint32_t Stride = Device->GetBindlessDescriptorStride();
    Handle = Device->GetBindlessCpuDescriptorHeap()->GetCPUDescriptorHandleForHeapStart();
    Handle.ptr += static_cast<SIZE_T>(Index) * Stride;
    return Handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE FRenderer::GetBindlessGpuHandle(uint32_t Index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE Handle{};
    if (!Device->GetBindlessDescriptorHeap())
    {
        return Handle;
    }

    const uint32_t Stride = Device->GetBindlessDescriptorStride();
    Handle = Device->GetBindlessDescriptorHeap()->GetGPUDescriptorHandleForHeapStart();
    Handle.ptr += static_cast<UINT64>(Index) * Stride;
    return Handle;
}

bool FRenderer::GetSceneModelStats(size_t& OutTotal, size_t& OutCulled) const
{
    const std::vector<FSceneModelResource>* Models = GetSceneModels();
    if (!Models)
    {
        OutTotal = 0;
        OutCulled = 0;
        return false;
    }
    return ::ComputeSceneModelStats(*Models, SceneModelVisibility, OutTotal, OutCulled);
}


void FRenderer::SetRenderFatalError(const std::string& Reason)
{
    if (bRenderFatalError)
    {
        return;
    }

    bRenderFatalError = true;
    RenderFatalReason = Reason;
    LogError("Renderer fatal error: " + Reason);
}

bool FRenderer::HasRenderFatalError() const
{
    return bRenderFatalError;
}

ID3D12Resource* FRenderer::GetEnvironmentCubeTexture() const
{
    return EnvironmentMap ? EnvironmentMap->GetCubeTexture() : nullptr;
}

ID3D12Resource* FRenderer::GetBrdfLutTexture() const
{
    return EnvironmentMap ? EnvironmentMap->GetBrdfLutTexture() : nullptr;
}

uint32_t FRenderer::GetEnvironmentCubeSrvIndex() const
{
    return EnvironmentMap ? EnvironmentMap->GetCubeSrvIndex() : UINT32_MAX;
}

uint32_t FRenderer::GetBrdfLutSrvIndex() const
{
    return EnvironmentMap ? EnvironmentMap->GetBrdfLutSrvIndex() : UINT32_MAX;
}

float FRenderer::GetEnvironmentMipCount() const
{
    return EnvironmentMap ? EnvironmentMap->GetMipCount() : 1.0f;
}

const FRayTracingRuntime& FRenderer::GetRayTracingRuntime() const
{
    return *RayTracingRuntime;
}

FRayTracingRuntime& FRenderer::GetRayTracingRuntime()
{
    if (!RayTracingRuntime)
    {
        RayTracingRuntime = std::make_unique<FRayTracingRuntime>();
    }
    return *RayTracingRuntime;
}

void FRenderer::InitializeCommonSettings(uint32_t Width, uint32_t Height, const FRendererConfig& Config)
{
    World.LinkSceneModels(&SceneModels);

    if (!RayTracingRuntime)
    {
        RayTracingRuntime = std::make_unique<FRayTracingRuntime>();
    }

    bDepthPrepassEnabled = Config.bUseDepthPrepass;
    bShadowsEnabled = Config.bEnableShadows;
    bRayTracedShadowsEnabled = Config.bEnableRayTracedShadows;
    ShadowBias = Config.ShadowBias;
    bLogResourceBarriers = Config.bLogResourceBarriers;
    bEnableGraphDump = Config.bEnableGraphDump;
    bEnableGpuTiming = Config.bEnableGpuTiming;
    if (Device)
    {
        LogInfo(std::string("Barrier mode: Legacy"));
    }
    bEnableIndirectDraw = Config.bEnableIndirectDraw;
    bEnableSkinningIndirectDraw = Config.bEnableSkinningIndirectDraw;
    GpuDebugState.SetPrintEnabled(Config.bEnableGpuDebugPrint);
    FramesInFlight = (std::max)(1u, Config.FramesInFlight);
    CurrentFrameIndex = 0;
    bRenderFatalError = false;
    RenderFatalReason.clear();

    if (!EnvironmentMap)
    {
        EnvironmentMap = std::make_unique<FEnvironmentMap>();
    }

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

    constexpr uint32_t DefaultShadowMapSize = 2048;
    ShadowMapWidth = DefaultShadowMapSize;
    ShadowMapHeight = DefaultShadowMapSize;

    ShadowViewport.TopLeftX = 0.0f;
    ShadowViewport.TopLeftY = 0.0f;
    ShadowViewport.Width = static_cast<float>(ShadowMapWidth);
    ShadowViewport.Height = static_cast<float>(ShadowMapHeight);
    ShadowViewport.MinDepth = 0.0f;
    ShadowViewport.MaxDepth = 1.0f;

    ShadowScissor.left = 0;
    ShadowScissor.top = 0;
    ShadowScissor.right = static_cast<LONG>(ShadowMapWidth);
    ShadowScissor.bottom = static_cast<LONG>(ShadowMapHeight);
}

void FRenderer::SetFrameIndex(uint32_t FrameIndex)
{
    if (DepthResourcesPerFrame.empty())
    {
        CurrentFrameIndex = 0;
        return;
    }

    CurrentFrameIndex = FrameIndex % static_cast<uint32_t>(DepthResourcesPerFrame.size());
    DepthStencilHandle = DepthResourcesPerFrame[CurrentFrameIndex].DSVHandle;
}

const D3D12_CPU_DESCRIPTOR_HANDLE& FRenderer::GetDSVHandle() const
{
    return DepthStencilHandle;
}

ID3D12Resource* FRenderer::GetDepthBuffer() const
{
    if (DepthResourcesPerFrame.empty())
    {
        return nullptr;
    }

    return DepthResourcesPerFrame[CurrentFrameIndex].Resource.Get();
}

uint32_t FRenderer::GetCurrentDepthSrvBindlessIndex() const
{
    if (DepthResourcesPerFrame.empty())
    {
        return UINT32_MAX;
    }
    return DepthResourcesPerFrame[CurrentFrameIndex].SrvBindlessIndex;
}

DXGI_FORMAT FRenderer::GetDepthTypelessFormat() const
{
    if (DepthResourcesPerFrame.empty())
    {
        return DXGI_FORMAT_UNKNOWN;
    }
    return DepthResourcesPerFrame[CurrentFrameIndex].TypelessFormat;
}

D3D12_RESOURCE_STATES& FRenderer::GetDepthBufferState()
{
    if (DepthResourcesPerFrame.empty())
    {
        static D3D12_RESOURCE_STATES FallbackState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        return FallbackState;
    }

    return DepthResourcesPerFrame[CurrentFrameIndex].State;
}

ID3D12Resource* FRenderer::GetIndirectCommandBuffer() const
{
    return GpuDrivenCullingState.GetIndirectCommandBuffer(CurrentFrameIndex);
}

D3D12_RESOURCE_STATES& FRenderer::GetIndirectCommandState()
{
    D3D12_RESOURCE_STATES* State = GpuDrivenCullingState.GetIndirectCommandState(CurrentFrameIndex);
    if (!State)
    {
        static D3D12_RESOURCE_STATES FallbackState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        return FallbackState;
    }

    return *State;
}

ID3D12Resource* FRenderer::GetMeshletRunCountBuffer() const
{
    return GpuDrivenCullingState.GetMeshletRunCountBuffer(CurrentFrameIndex);
}

D3D12_RESOURCE_STATES& FRenderer::GetMeshletRunCountState()
{
    D3D12_RESOURCE_STATES* State = GpuDrivenCullingState.GetMeshletRunCountState(CurrentFrameIndex);
    if (!State)
    {
        static D3D12_RESOURCE_STATES DefaultState = D3D12_RESOURCE_STATE_COMMON;
        return DefaultState;
    }

    return *State;
}

ID3D12Resource* FRenderer::GetSceneConstantBuffer() const
{
    if (SceneConstantBuffers.empty())
    {
        return nullptr;
    }

    return SceneConstantBuffers[CurrentFrameIndex].Get();
}

D3D12_GPU_VIRTUAL_ADDRESS FRenderer::GetSceneConstantBufferAddress() const
{
    ID3D12Resource* Buffer = GetSceneConstantBuffer();
    return Buffer ? Buffer->GetGPUVirtualAddress() : 0;
}

uint8_t* FRenderer::GetSceneConstantBufferMapped() const
{
    if (SceneConstantBuffers.empty())
    {
        return nullptr;
    }

    return SceneConstantBuffers[CurrentFrameIndex].MappedData;
}

bool FRenderer::CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format, FDepthStencilTarget& OutDepthResources)
{
    CreateDepthTexture2D(Device, L"", Width, Height, Format, 0.0f, OutDepthResources.Resource);
    CreateTexture2DDsv(Device, L"", OutDepthResources.Resource.Get(), Format, OutDepthResources.DSVHeap, OutDepthResources.DSVHandle);
    OutDepthResources.TypelessFormat = GetDepthResourceFormat(Format);
    return true;
}

bool FRenderer::CreateDepthResourcesPerFrame(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format)
{
    DepthResourcesPerFrame.clear();
    SceneDepthFormat = Format;

    DepthResourcesPerFrame.resize(FramesInFlight);

    for (uint32_t Index = 0; Index < FramesInFlight; ++Index)
    {
        if (!CreateDepthResources(Device, Width, Height, Format, DepthResourcesPerFrame[Index]))
        {
            return false;
        }

        if (DepthResourcesPerFrame[Index].Resource)
        {
            const std::wstring Name = L"DepthBuffer_Frame" + std::to_wstring(Index);
            DepthResourcesPerFrame[Index].Resource->SetName(Name.c_str());
        }
        if (DepthResourcesPerFrame[Index].DSVHeap)
        {
            const std::wstring Name = L"DepthDSVHeap_Frame" + std::to_wstring(Index);
            DepthResourcesPerFrame[Index].DSVHeap->SetName(Name.c_str());
        }
    }

    SetFrameIndex(CurrentFrameIndex);
    return true;
}

bool FRenderer::CreateSceneConstantBuffersPerFrame(FDX12Device* Device, uint64_t BufferSize)
{
    SceneConstantBuffers.clear();
    SceneConstantBuffers.resize(FramesInFlight);

    for (uint32_t Index = 0; Index < FramesInFlight; ++Index)
    {
        if (!CreateMappedConstantBuffer(Device, L"SceneConstantBuffer_" + std::to_wstring(Index), BufferSize, SceneConstantBuffers[Index]))
        {
            return false;
        }
    }

    return true;
}

bool FRenderer::CreateCullingConstantBuffersPerFrame(FDX12Device* Device)
{
    return GpuDrivenCullingState.CreateCullingConstantBuffers(Device, FramesInFlight);
}

bool FRenderer::CreateShadowPipeline(
    FDX12Device* Device,
    ID3D12RootSignature* RootSignature,
    const std::vector<std::wstring>& Defines,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState,
    bool bDoubleSided)
{
    if (!RootSignature)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/ShadowMap.hlsl", VSByteCode, Defines))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature;
    PsoDesc.InputLayout = { nullptr, 0 };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = bDoubleSided ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_FRONT;
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

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 0;
    PsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(OutPipelineState.GetAddressOf())));
    return true;
}

bool FRenderer::CreateShadowResources(
    FDX12Device* Device,
    uint32_t& InOutWidth,
    uint32_t& InOutHeight,
    FBindlessTexture& OutShadowMap,
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& OutShadowDsvHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE& OutShadowDsvHandle)
{
    if (InOutWidth == 0 || InOutHeight == 0)
    {
        InOutWidth = 4096;
        InOutHeight = 4096;
    }

    CreateDepthTexture2D(Device, L"ShadowMap", InOutWidth, InOutHeight, DXGI_FORMAT_D32_FLOAT, 1.0f, OutShadowMap.Resource);
    InitializeBindlessTexture(OutShadowMap, { InOutWidth, InOutHeight, DXGI_FORMAT_R32_FLOAT }, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    WriteOrCreateBindlessTextureSrv(Device, OutShadowMap);
    CreateTexture2DDsv(Device, L"ShadowDSVHeap", OutShadowMap.Get(), DXGI_FORMAT_D32_FLOAT, OutShadowDsvHeap, OutShadowDsvHandle);
    return true;
}

void FRenderer::ConfigureHZBOcclusion(bool bEnabled, uint32_t HZBBindlessIndex, uint32_t Width, uint32_t Height, uint32_t MipCount)
{
    GpuDrivenCullingState.ConfigureHZBOcclusion(bEnabled, HZBBindlessIndex, Width, Height, MipCount);
}

void FRenderer::RefreshGpuDrivenPersistentValidation()
{
    GpuDrivenCullingState.RefreshCullingPersistentValidation(GetFramesInFlight());
    GpuDrivenCullingState.RefreshVisibilityPersistentValidation(GetFramesInFlight());

    bGpuDrivenCullingPersistentInputsValid =
        GpuDrivenCullingState.HasCullingDispatchResources() &&
        GpuDrivenCullingState.HasSharedInputs() &&
        IndirectCommandCount != 0 &&
        !IndirectDrawRanges.empty();

    bGpuDrivenCullingPersistentInputsValid = bGpuDrivenCullingPersistentInputsValid && GpuDrivenCullingState.HasMeshletVisibilityInputs();
}

void FRenderer::DispatchGpuCulling(
    FDX12CommandContext& CmdContext,
    const FCamera& Camera,
    ECullingMode Mode,
    uint32_t VisibilityInputIndex,
    uint32_t VisibilityInputFrameIndex,
    uint32_t CullingListIndex,
    uint32_t CullingListCountIndex,
    bool bUseLateVisibility)
{
    if (!bGpuDrivenCullingPersistentInputsValid || !GpuDebugState.IsGpuDrivenCullingReady())
    {
        return;
    }

    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    const bool bUseCullingList = AreAllBindlessIndicesValid(CullingListIndex, CullingListCountIndex);
    FGpuDrivenCulling::FGpuCullingDispatchConfig DispatchConfig;
    DispatchConfig.BindlessDescriptorHeap = Device ? Device->GetBindlessDescriptorHeap() : nullptr;
    DispatchConfig.IndirectCommandCount = IndirectCommandCount;
    DispatchConfig.RangeCount = static_cast<uint32_t>(IndirectDrawRanges.size());
    DispatchConfig.Mode = static_cast<uint32_t>(Mode);
    DispatchConfig.bGpuDebugPrintEnabled = GpuDebugState.IsPrintEnabled();

    FGpuDrivenCulling::FGpuCullingDispatchFrameData DispatchFrameData = GpuDrivenCullingState.GetDispatchFrameData(CurrentFrameIndex);

    FGpuDrivenCulling::FGpuCullingDispatchIndices DispatchIndices;
    GpuDrivenCullingState.FillDispatchSharedIndices(DispatchIndices);
    DispatchIndices.VisibilityInputIndex = VisibilityInputIndex;
    DispatchIndices.CullingListIndex = CullingListIndex;
    DispatchIndices.CullingListCountIndex = CullingListCountIndex;
    DispatchIndices.DebugPrintBufferIndex = GpuDebugState.GetPrintBufferUavBindlessIndex();
    DispatchIndices.DebugPrintStatsIndex = GpuDebugState.GetPrintStatsUavBindlessIndex();
    DispatchIndices.bUseCullingList = bUseCullingList;

    GpuDrivenCullingState.DispatchGpuCulling(
        DispatchConfig,
        DispatchFrameData,
        DispatchIndices,
        CmdContext,
        *CullingCamera,
        CurrentFrameIndex,
        bUseLateVisibility,
        VisibilityInputFrameIndex);
}

bool FRenderer::CreateSkinnedPositionBuffers()
{
    bool bAllReady = true;
    for (FSceneModelResource& Model : SceneModels)
    {
        if (!IsValidBindlessIndex(Model.BoneMatrixBuffer.SrvBindlessIndex) || Model.BoneMatrixCount == 0)
        {
            continue;
        }

        const uint32_t VertexStride = Model.Geometry.VertexBufferViews[0].StrideInBytes;
        const uint32_t VertexCount = VertexStride > 0
            ? (Model.Geometry.VertexBufferViews[0].SizeInBytes / VertexStride)
            : 0;
        if (VertexCount == 0)
        {
            continue;
        }

        Model.SkinnedPositionBuffers.resize(FramesInFlight);
        for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
        {
            CreateBindlessBuffer(Device,
                L"SkinnedPositionBuffer_" + std::to_wstring(FrameIndex),
                CreateStructuredBufferDesc<FFloat3>(VertexCount),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                Model.SkinnedPositionBuffers[FrameIndex],
                true, true);
        }

        bool bReady = true;
        for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
        {
            bReady = bReady && Model.SkinnedPositionBuffers[FrameIndex].IsFullyBound();
        }

        Model.bUseSkinning = Model.bUseSkinning && bReady;
        bAllReady = bAllReady && bReady;
    }

    return bAllReady;
}

void FRenderer::DispatchSkinning(FDX12CommandContext& CmdContext, const DirectX::XMMATRIX& LightViewProjection)
{
    SceneModelSkinningVisibility.assign(SceneModels.size(), false);
    if (SceneModels.empty())
    {
        return;
    }

    const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();

    DirectX::XMVECTOR LightPlanes[6] = {};
    RendererUtils::BuildFrustumPlanesFromMatrix(LightViewProjection, LightPlanes);

    bool bHasVisibleSkinning = false;
    for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
    {
        FSceneModelResource& Model = SceneModels[ModelIndex];
        const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBuffer.SrvBindlessIndex) && Model.BoneMatrixCount > 0
            && FrameIndex < Model.SkinnedPositionBuffers.size();

        if (bUseSkinning)
        {
            const uint32_t SlotToUse = (Model.bSkinningUpdatedThisFrame || Model.LastSkinnedSlot == UINT32_MAX)
                ? FrameIndex
                : Model.LastSkinnedSlot;
            Model.SkinnedPositionBuffer = Model.SkinnedPositionBuffers[SlotToUse];
        }
        else
        {
            Model.SkinnedPositionBuffer = {};
        }

        if (!bUseSkinning || !Model.SkinnedPositionBuffer || !IsValidBindlessIndex(Model.SkinnedPositionBuffer.UavBindlessIndex))
        {
            continue;
        }

        const bool bCameraVisible = SceneModelVisibility.empty() ? true : SceneModelVisibility[ModelIndex];
        const bool bShadowVisible = RendererUtils::IsAabbInCameraFrustum(LightPlanes, Model.BoundsMin, Model.BoundsMax);
        const bool bVisible = bCameraVisible || bShadowVisible;
        SceneModelSkinningVisibility[ModelIndex] = bVisible;
        bHasVisibleSkinning = bHasVisibleSkinning || bVisible;
    }

    if (!bHasVisibleSkinning)
    {
        return;
    }

    if (!SkinningPipeline || !SkinningRootSignature)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    if (!CommandList)
    {
        return;
    }
	FScopedPixEvent SkinningEvent(CommandList, L"DispatchSkinning");

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(SkinningRootSignature.Get());
    CommandList->SetPipelineState(SkinningPipeline.Get());

    for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
    {
        FSceneModelResource& Model = SceneModels[ModelIndex];
        const bool bUseSkinning = IsValidBindlessIndex(Model.BoneMatrixBuffer.SrvBindlessIndex) && Model.BoneMatrixCount > 0
            && FrameIndex < Model.SkinnedPositionBuffers.size();
        if (!bUseSkinning || !SceneModelSkinningVisibility[ModelIndex])
        {
            continue;
        }

        if (!Model.bSkinningUpdatedThisFrame && Model.LastSkinnedSlot != UINT32_MAX)
        {
            continue;
        }

        ID3D12Resource* SkinnedBuffer = Model.SkinnedPositionBuffers[FrameIndex].Get();
        if (!SkinnedBuffer)
        {
            continue;
        }

        const uint32_t VertexStride = Model.Geometry.VertexBufferViews[0].StrideInBytes;
        const uint32_t VertexCount = VertexStride > 0
            ? (Model.Geometry.VertexBufferViews[0].SizeInBytes / VertexStride)
            : 0;
        if (VertexCount == 0)
        {
            continue;
        }

        if (Model.SkinnedPositionBuffers[FrameIndex].State != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(SkinnedBuffer, Model.SkinnedPositionBuffers[FrameIndex].State, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            CommandList->ResourceBarrier(1, &Barrier);
            Model.SkinnedPositionBuffers[FrameIndex].State = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        const uint32_t Constants[kSkinningConstantsDwordCount] =
        {
            VertexCount,
            Model.Geometry.VertexBuffers[0].SrvBindlessIndex,
            Model.Geometry.VertexBuffers[5].SrvBindlessIndex,
            Model.Geometry.VertexBuffers[6].SrvBindlessIndex,
            Model.BoneMatrixBuffer.SrvBindlessIndex,
            Model.SkinnedPositionBuffer.UavBindlessIndex
        };
        CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

        const uint32_t DispatchCount = (VertexCount + 63) / 64;
        CommandList->Dispatch(DispatchCount, 1, 1);

        const auto UavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(SkinnedBuffer);
        CommandList->ResourceBarrier(1, &UavBarrier);

        const auto Barrier = CD3DX12_RESOURCE_BARRIER::Transition(SkinnedBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        CommandList->ResourceBarrier(1, &Barrier);
        Model.SkinnedPositionBuffers[FrameIndex].State = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Model.LastSkinnedSlot = FrameIndex;
    }
}


bool FRenderer::PrepareGpuDrivenDrawData(FGpuDrivenPreparedData& OutData)
{
    if (SceneModels.empty() || !GetSceneConstantBuffer())
    {
        return false;
    }

    IndirectDrawRanges.clear();

    std::vector<uint32_t> SortedIndices(SceneModels.size());
    for (uint32_t Index = 0; Index < SortedIndices.size(); ++Index)
    {
        SortedIndices[Index] = Index;
    }

    std::sort(SortedIndices.begin(), SortedIndices.end(), [&](uint32_t A, uint32_t B)
    {
        const uint32_t KeyA = SceneModels[A].PipelineKey;
        const uint32_t KeyB = SceneModels[B].PipelineKey;
        if (KeyA != KeyB)
        {
            return KeyA < KeyB;
        }
        const RendererUtils::FMaterialBindlessIndices IndicesA = RendererUtils::BuildMaterialBindlessIndices(SceneModels[A]);
        const RendererUtils::FMaterialBindlessIndices IndicesB = RendererUtils::BuildMaterialBindlessIndices(SceneModels[B]);
        return IndicesA < IndicesB;
    });

    size_t TotalCommandCount = 0;
    for (const FSceneModelResource& Model : SceneModels)
    {
        if (Model.Material.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
        {
            continue;
        }

        if (ShouldUseClusterDagRuntimePath(Model))
        {
            continue;
        }

        if (!Model.bUseMeshletCulling || Model.Meshlets.empty() || Model.MeshletBounds.empty())
        {
            continue;
        }

        TotalCommandCount += Model.Meshlets.size();
    }

    OutData.Reserve(TotalCommandCount);

    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = GetSceneConstantBufferAddress();
    uint32_t GroupIndex = 0;

    for (uint32_t SortedIndex : SortedIndices)
    {
        const FSceneModelResource& Model = SceneModels[SortedIndex];
        const uint32_t PipelineKey = Model.PipelineKey;
        if (Model.Material.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
        {
            continue;
        }

        if (ShouldUseClusterDagRuntimePath(Model))
        {
            continue;
        }

        if (!Model.bUseMeshletCulling || Model.Meshlets.empty() || Model.MeshletBounds.empty())
        {
            continue;
        }

        const RendererUtils::FMaterialBindlessIndices MaterialIndices = RendererUtils::BuildMaterialBindlessIndices(Model);
        if (IndirectDrawRanges.empty()
            || IndirectDrawRanges.back().PipelineKey != PipelineKey
            || IndirectDrawRanges.back().MaterialBindlessIndices != MaterialIndices)
        {
            IndirectDrawRanges.push_back(FIndirectDrawRange::Make(
                static_cast<uint32_t>(OutData.Commands.size()),
                PipelineKey, MaterialIndices, Model.Name));
        }

        const uint32_t RangeIndex = static_cast<uint32_t>(IndirectDrawRanges.size() - 1);
        const DirectX::XMMATRIX World = DirectX::XMLoadFloat4x4(&Model.WorldMatrix);
        const DirectX::XMMATRIX NormalMatrix = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, World));
        const float ScaleX = std::sqrt(Model.WorldMatrix._11 * Model.WorldMatrix._11 + Model.WorldMatrix._21 * Model.WorldMatrix._21 + Model.WorldMatrix._31 * Model.WorldMatrix._31);
        const float ScaleY = std::sqrt(Model.WorldMatrix._12 * Model.WorldMatrix._12 + Model.WorldMatrix._22 * Model.WorldMatrix._22 + Model.WorldMatrix._32 * Model.WorldMatrix._32);
        const float ScaleZ = std::sqrt(Model.WorldMatrix._13 * Model.WorldMatrix._13 + Model.WorldMatrix._23 * Model.WorldMatrix._23 + Model.WorldMatrix._33 * Model.WorldMatrix._33);
        const float ModelScale = (std::max)((std::max)(ScaleX, ScaleY), ScaleZ);

        const size_t MeshletCount = (std::min)(Model.Meshlets.size(), Model.MeshletBounds.size());
        for (size_t MeshletIndex = 0; MeshletIndex < MeshletCount; ++MeshletIndex)
        {
            const FMesh::FMeshlet& Meshlet = Model.Meshlets[MeshletIndex];
            const FMesh::FMeshletBounds& BoundsData = Model.MeshletBounds[MeshletIndex];

            const FIndirectDrawCommand Command = FIndirectDrawCommand::Make(
                ConstantBufferBase + SceneConstantBufferStride * SortedIndex,
                Meshlet.IndexOffset, Meshlet.IndexCount, SortedIndex);
            const FMeshletDrawData DrawData = FMeshletDrawData::Make(
                Meshlet.IndexOffset, Meshlet.IndexCount, RangeIndex, GroupIndex);

            const DirectX::XMVECTOR LocalCenter = DirectX::XMLoadFloat3(&BoundsData.Center);
            const DirectX::XMVECTOR WorldCenter = DirectX::XMVector3TransformCoord(LocalCenter, World);
            DirectX::XMFLOAT3 Center{};
            DirectX::XMStoreFloat3(&Center, WorldCenter);

            const DirectX::XMVECTOR LocalAxis = DirectX::XMLoadFloat3(&BoundsData.ConeAxis);
            const DirectX::XMVECTOR WorldAxis = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(LocalAxis, NormalMatrix));
            DirectX::XMFLOAT3 Axis{};
            DirectX::XMStoreFloat3(&Axis, WorldAxis);

            const DirectX::XMVECTOR LocalApex = DirectX::XMLoadFloat3(&BoundsData.ConeApex);
            const DirectX::XMVECTOR WorldApex = DirectX::XMVector3TransformCoord(LocalApex, World);
            DirectX::XMFLOAT3 Apex{};
            DirectX::XMStoreFloat3(&Apex, WorldApex);

            OutData.PushDraw(
                Command,
                DrawData,
                { Center.x, Center.y, Center.z, BoundsData.Radius * ModelScale },
                { Axis.x, Axis.y, Axis.z, Model.Material.bDoubleSided ? -1.0f : BoundsData.ConeCutoff },
                { Apex.x, Apex.y, Apex.z, 0.0f });

            IndirectDrawRanges.back().Count += 1;
        }

        GroupIndex += 1;
    }

    IndirectCommandCount = static_cast<uint32_t>(OutData.Commands.size());

    OutData.RangeOffsets.clear();
    OutData.RangeOffsets.reserve(IndirectDrawRanges.size());
    for (const FIndirectDrawRange& Range : IndirectDrawRanges)
    {
        OutData.RangeOffsets.push_back(Range.Start);
    }

    return true;
}

bool FRenderer::CreatePerFrameIndirectBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data)
{
    bGpuDrivenCullingPersistentInputsValid = false;

    if (Data.Commands.empty())
    {
        return false;
    }

    const uint64_t CommandBufferSize = sizeof(FIndirectDrawCommand) * Data.Commands.size();
    const uint32_t Frames = GetFramesInFlight();

    if (!GpuDrivenCullingState.CreatePerFrameCullingResources(
        Device,
        Frames,
        CommandBufferSize,
        IndirectCommandCount,
        static_cast<uint32_t>(Data.RangeOffsets.size())))
    {
        return false;
    }

    if (!GpuDrivenCullingState.CreateVisibilityResources(Device, Frames, IndirectCommandCount))
    {
        return false;
    }

    RefreshGpuDrivenPersistentValidation();
    return true;
}

bool FRenderer::CreateSharedGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data)
{
    bGpuDrivenCullingPersistentInputsValid = false;

    const uint64_t BoundsBufferSize = sizeof(DirectX::XMFLOAT4) * Data.Bounds.size();
    const uint64_t MeshletDrawDataSize = sizeof(FMeshletDrawData) * Data.MeshletDrawData.size();
    const uint64_t RangeOffsetSize = sizeof(uint32_t) * Data.RangeOffsets.size();
    const uint64_t ConeBufferSize = sizeof(DirectX::XMFLOAT4) * Data.ConeAxisCutoff.size();
    FGpuDrivenCulling::FGpuCullingSharedInputData SharedInputData;
    SharedInputData.BoundsData = Data.Bounds.empty() ? nullptr : Data.Bounds.data();
    SharedInputData.BoundsSizeInBytes = BoundsBufferSize;
    SharedInputData.BoundsElementCount = static_cast<uint32_t>(Data.Bounds.size());
    SharedInputData.MeshletDrawData = Data.MeshletDrawData.empty() ? nullptr : Data.MeshletDrawData.data();
    SharedInputData.MeshletDrawDataSizeInBytes = MeshletDrawDataSize;
    SharedInputData.MeshletDrawDataElementCount = static_cast<uint32_t>(Data.MeshletDrawData.size());
    SharedInputData.RangeOffsetsData = Data.RangeOffsets.empty() ? nullptr : Data.RangeOffsets.data();
    SharedInputData.RangeOffsetsSizeInBytes = RangeOffsetSize;
    SharedInputData.RangeOffsetsElementCount = static_cast<uint32_t>(Data.RangeOffsets.size());
    SharedInputData.ConeAxisData = Data.ConeAxisCutoff.empty() ? nullptr : Data.ConeAxisCutoff.data();
    SharedInputData.ConeAxisSizeInBytes = ConeBufferSize;
    SharedInputData.ConeAxisElementCount = static_cast<uint32_t>(Data.ConeAxisCutoff.size());
    SharedInputData.ConeApexData = Data.ConeApex.empty() ? nullptr : Data.ConeApex.data();
    SharedInputData.ConeApexSizeInBytes = ConeBufferSize;
    SharedInputData.ConeApexElementCount = static_cast<uint32_t>(Data.ConeApex.size());

    if (!GpuDrivenCullingState.CreateSharedInputResources(Device, SharedInputData))
    {
        return false;
    }

    if (!GpuDebugState.CreateBufferResources(Device))
    {
        LogError("Failed to create GPU debug print resources");
        return false;
    }

    RefreshGpuDrivenPersistentValidation();
    return true;
}

std::vector<FUploadBuffer> FRenderer::PrepareIndirectCommandUploads(FDX12Device* Device, const std::vector<FIndirectDrawCommand>& Commands)
{
    const uint64_t BufferSize = sizeof(FIndirectDrawCommand) * Commands.size();
    std::vector<FIndirectDrawCommand> FrameCommands = Commands;
    std::vector<FUploadBuffer> Uploads(GetFramesInFlight());
    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = SceneConstantBuffers[FrameIndex].Get()
            ? SceneConstantBuffers[FrameIndex].GetGPUVirtualAddress()
            : 0;
        for (FIndirectDrawCommand& Cmd : FrameCommands)
        {
            Cmd.ConstantBufferAddress =
                ConstantBufferBase + SceneConstantBufferStride * Cmd.DrawArguments.StartInstanceLocation;
        }
        CreateUploadBuffer(Device,
            L"IndirectCommandUpload_" + std::to_wstring(FrameIndex),
            BufferSize,
            Uploads[FrameIndex],
            FrameCommands.data());
    }
    return Uploads;
}

bool FRenderer::UploadGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data)
{
    const uint64_t CommandBufferSize = sizeof(FIndirectDrawCommand) * Data.Commands.size();
    std::vector<FUploadBuffer> IndirectCommandUploads;
    if (!Data.Commands.empty() && GpuDrivenCullingState.GetIndirectCommandTemplateBuffer(0) != nullptr)
    {
        IndirectCommandUploads = PrepareIndirectCommandUploads(Device, Data.Commands);
    }

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"Renderer_InitUpload_CL");

    // Pre-copy barriers
    std::vector<D3D12_RESOURCE_BARRIER> PreCopyBarriers;
    PreCopyBarriers.reserve(GetFramesInFlight() * 2 + GpuDrivenSharedBufferCount);
    GpuDrivenCullingState.AddInitialUploadPreCopyBarriers(PreCopyBarriers, GetFramesInFlight());
    GpuDebugState.AddUploadPreCopyBarriers(PreCopyBarriers);
    if (!PreCopyBarriers.empty())
    {
        UploadList->ResourceBarrier(static_cast<UINT>(PreCopyBarriers.size()), PreCopyBarriers.data());
    }

    // Copy
    GpuDrivenCullingState.CopyInitialData(UploadList.Get(), IndirectCommandUploads, CommandBufferSize);
    GpuDebugState.UploadInitialData(UploadList.Get());

    // Post-copy barriers
    std::vector<D3D12_RESOURCE_BARRIER> PostCopyBarriers;
    PostCopyBarriers.reserve(GetFramesInFlight() * 2 + GpuDrivenSharedBufferCount);
    GpuDrivenCullingState.AddInitialUploadPostCopyBarriers(PostCopyBarriers, GetFramesInFlight());
    GpuDebugState.AddUploadPostCopyBarriers(PostCopyBarriers);
    if (!PostCopyBarriers.empty())
    {
        UploadList->ResourceBarrier(static_cast<UINT>(PostCopyBarriers.size()), PostCopyBarriers.data());
    }

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    GpuDrivenCullingState.ResetInitialUploadStates(GetFramesInFlight());
    GpuDebugState.SetUploadCompletedStates();

    return true;
}

bool FRenderer::CreateCullingPipelines(FDX12Device* Device)
{
    bGpuDrivenCullingPersistentInputsValid = false;

    if (!GpuDrivenCullingState.HasCullingPipelines() && !GpuDrivenCullingState.CreateCullingPipelines(Device))
    {
        return false;
    }

    RefreshGpuDrivenPersistentValidation();
    return true;
}

bool FRenderer::CreateSkinningPipeline(FDX12Device* Device)
{
    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstants(kSkinningConstantsDwordCount, 0, 0, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc;
    VersionedRootDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED);

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SkinningRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;
    if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/SkinningCS.hlsl", CsByteCode))
    {
        LogError("Failed to compile skinning compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC CsDesc = {};
    CsDesc.pRootSignature = SkinningRootSignature.Get();
    CsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&CsDesc, IID_PPV_ARGS(SkinningPipeline.GetAddressOf())));

    return true;
}

bool FRenderer::CreateIndirectCommandSignature(FDX12Device* Device, ID3D12RootSignature* RootSignature)
{
    if (!RootSignature)
    {
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC IndirectArgs[3] = {};
// IndirectArgs[0] = CONSTANT_BUFFER_VIEW   8 bytes (GPU VA)
// IndirectArgs[1] = CONSTANT (2)           8 bytes, DrawIndexStart + DrawDataIndex (b2 register)
// IndirectArgs[2] = DRAW                   16 bytes (D3D12_DRAW_ARGUMENTS)
    IndirectArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    IndirectArgs[0].ConstantBufferView.RootParameterIndex = 0;
    IndirectArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    IndirectArgs[1].Constant.RootParameterIndex = 2;
    IndirectArgs[1].Constant.Num32BitValuesToSet = 2;
    IndirectArgs[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    // 0	ConstantBufferAddress (8 bytes)
    // 8	DrawIndexStart (b2 shader constant, dword 0)
    // 12	DrawDataIndex (b2 shader constant, dword 1)
    // 16	DrawArguments.VertexCountPerInstance
    // 20	DrawArguments.InstanceCount
    // 24	DrawArguments.StartVertexLocation
    // 28	DrawArguments.StartInstanceLocation

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.pArgumentDescs = IndirectArgs;
    CommandDesc.NumArgumentDescs = _countof(IndirectArgs);
    CommandDesc.ByteStride = sizeof(FIndirectDrawCommand);
    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, RootSignature, IID_PPV_ARGS(IndirectCommandSignature.GetAddressOf())));

    return true;
}
