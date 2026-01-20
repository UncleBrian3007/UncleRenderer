#include "Renderer.h"

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
#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace
{
    uint32_t AlignShaderRecordSize(uint32_t Size)
    {
        const uint32_t Alignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
        return (Size + Alignment - 1u) & ~(Alignment - 1u);
    }
}

FRenderer::~FRenderer() = default;

bool FRenderer::GetSceneModelStats(size_t& OutTotal, size_t& OutCulled) const
{
    return RendererUtils::ComputeSceneModelStats(SceneModels, SceneModelVisibility, OutTotal, OutCulled);
}

void FRenderer::RequestObjectIdReadback(uint32_t X, uint32_t Y)
{
    RendererUtils::RequestObjectIdReadback(
        X,
        Y,
        bObjectIdReadbackRequested,
        bObjectIdReadbackRecorded,
        ObjectIdReadbackX,
        ObjectIdReadbackY);
}

bool FRenderer::ConsumeObjectIdReadback(uint32_t& OutObjectId)
{
    return RendererUtils::ConsumeObjectIdReadback(
        ObjectIdReadback,
        ObjectIdRowPitch,
        bObjectIdReadbackRequested,
        bObjectIdReadbackRecorded,
        OutObjectId);
}

void FRenderer::InitializeCommonSettings(uint32_t Width, uint32_t Height, const FRendererConfig& Config)
{
    bDepthPrepassEnabled = Config.bUseDepthPrepass;
    bShadowsEnabled = Config.bEnableShadows;
    bRayTracedShadowsEnabled = Config.bEnableRayTracedShadows;
    ShadowBias = Config.ShadowBias;
    bLogResourceBarriers = Config.bLogResourceBarriers;
    bEnableGraphDump = Config.bEnableGraphDump;
    bEnableGpuTiming = Config.bEnableGpuTiming;
    bEnableIndirectDraw = Config.bEnableIndirectDraw;
    bEnableGpuDebugPrint = Config.bEnableGpuDebugPrint;
    bGtaoEnabled = Config.bEnableGtao;
    bGtaoJitterEnabled = Config.bEnableGtaoJitter;
    FramesInFlight = (std::max)(1u, Config.FramesInFlight);
    CurrentFrameIndex = 0;

    GtaoRadius = Config.GtaoRadius;
    GtaoIntensity = Config.GtaoIntensity;
    GtaoPower = Config.GtaoPower;
    GtaoThickness = Config.GtaoThickness;
    GtaoDirectionCount = Config.GtaoDirectionCount;
    GtaoStepCount = Config.GtaoStepCount;

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
    DepthStencilHandle = DepthResourcesPerFrame[CurrentFrameIndex].DepthStencilHandle;
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

    return DepthResourcesPerFrame[CurrentFrameIndex].DepthBuffer.Get();
}

D3D12_RESOURCE_STATES& FRenderer::GetDepthBufferState()
{
    if (DepthBufferStates.empty())
    {
        static D3D12_RESOURCE_STATES FallbackState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        return FallbackState;
    }

    return DepthBufferStates[CurrentFrameIndex];
}

ID3D12Resource* FRenderer::GetIndirectCommandBuffer() const
{
    if (IndirectCommandBuffers.empty())
    {
        return nullptr;
    }

    return IndirectCommandBuffers[CurrentFrameIndex].Get();
}

D3D12_RESOURCE_STATES& FRenderer::GetIndirectCommandState()
{
    if (IndirectCommandStates.empty())
    {
        static D3D12_RESOURCE_STATES FallbackState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        return FallbackState;
    }

    return IndirectCommandStates[CurrentFrameIndex];
}

ID3D12Resource* FRenderer::GetMeshletRunCountBuffer() const
{
    if (MeshletRunCountBuffers.empty())
    {
        return nullptr;
    }

    return MeshletRunCountBuffers[CurrentFrameIndex].Get();
}

D3D12_RESOURCE_STATES& FRenderer::GetMeshletRunCountState()
{
    if (MeshletRunCountStates.empty())
    {
        static D3D12_RESOURCE_STATES DefaultState = D3D12_RESOURCE_STATE_COMMON;
        return DefaultState;
    }

    return MeshletRunCountStates[CurrentFrameIndex];
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
    if (SceneConstantBufferMapped.empty())
    {
        return nullptr;
    }

    return SceneConstantBufferMapped[CurrentFrameIndex];
}

ID3D12Resource* FRenderer::GetCullingConstantBuffer() const
{
    if (CullingConstantBuffers.empty())
    {
        return nullptr;
    }

    return CullingConstantBuffers[CurrentFrameIndex].Get();
}

D3D12_GPU_VIRTUAL_ADDRESS FRenderer::GetCullingConstantBufferAddress() const
{
    ID3D12Resource* Buffer = GetCullingConstantBuffer();
    return Buffer ? Buffer->GetGPUVirtualAddress() : 0;
}

uint8_t* FRenderer::GetCullingConstantBufferMapped() const
{
    if (CullingConstantBufferMapped.empty())
    {
        return nullptr;
    }

    return CullingConstantBufferMapped[CurrentFrameIndex];
}

bool FRenderer::CreateDepthResourcesPerFrame(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format)
{
    if (!Device)
    {
        return false;
    }

    DepthResourcesPerFrame.clear();
    DepthBufferStates.clear();

    DepthResourcesPerFrame.resize(FramesInFlight);
    DepthBufferStates.resize(FramesInFlight, D3D12_RESOURCE_STATE_DEPTH_WRITE);

    for (uint32_t Index = 0; Index < FramesInFlight; ++Index)
    {
        if (!RendererUtils::CreateDepthResources(Device, Width, Height, Format, DepthResourcesPerFrame[Index]))
        {
            return false;
        }

        if (DepthResourcesPerFrame[Index].DepthBuffer)
        {
            const std::wstring Name = L"DepthBuffer_Frame" + std::to_wstring(Index);
            DepthResourcesPerFrame[Index].DepthBuffer->SetName(Name.c_str());
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
    if (!Device)
    {
        return false;
    }

    SceneConstantBuffers.clear();
    SceneConstantBufferMapped.clear();
    SceneConstantBuffers.resize(FramesInFlight);
    SceneConstantBufferMapped.resize(FramesInFlight, nullptr);

    for (uint32_t Index = 0; Index < FramesInFlight; ++Index)
    {
        FMappedConstantBuffer ConstantBufferResource;
        if (!RendererUtils::CreateMappedConstantBuffer(Device, BufferSize, ConstantBufferResource))
        {
            return false;
        }

        SceneConstantBuffers[Index] = ConstantBufferResource.Resource;
        SceneConstantBufferMapped[Index] = ConstantBufferResource.MappedData;

        if (SceneConstantBuffers[Index])
        {
            const std::wstring Name = L"SceneConstantBuffer_Frame" + std::to_wstring(Index);
            SceneConstantBuffers[Index]->SetName(Name.c_str());
        }
    }

    return true;
}

bool FRenderer::CreateCullingConstantBuffersPerFrame(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    CullingConstantBuffers.clear();
    CullingConstantBufferMapped.clear();
    CullingConstantBuffers.resize(FramesInFlight);
    CullingConstantBufferMapped.resize(FramesInFlight, nullptr);

    constexpr uint64_t CullingConstantSize = sizeof(uint32_t) * 52;

    for (uint32_t Index = 0; Index < FramesInFlight; ++Index)
    {
        FMappedConstantBuffer ConstantBufferResource;
        if (!RendererUtils::CreateMappedConstantBuffer(Device, CullingConstantSize, ConstantBufferResource))
        {
            return false;
        }

        CullingConstantBuffers[Index] = ConstantBufferResource.Resource;
        CullingConstantBufferMapped[Index] = ConstantBufferResource.MappedData;

        if (CullingConstantBuffers[Index])
        {
            const std::wstring Name = L"CullingConstantBuffer_Frame" + std::to_wstring(Index);
            CullingConstantBuffers[Index]->SetName(Name.c_str());
        }
    }

    return true;
}

bool FRenderer::CreateShadowPipeline(
    FDX12Device* Device,
    ID3D12RootSignature* RootSignature,
    const std::vector<std::wstring>& Defines,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState)
{
    if (!Device || !RootSignature)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/ShadowMap.hlsl", L"VSMain", VSTarget, VSByteCode, Defines))
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
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
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
    Microsoft::WRL::ComPtr<ID3D12Resource>& OutShadowMap,
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& OutShadowDsvHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE& OutShadowDsvHandle,
    D3D12_RESOURCE_STATES& OutShadowState)
{
    if (!Device)
    {
        return false;
    }

    if (InOutWidth == 0 || InOutHeight == 0)
    {
        InOutWidth = 4096;
        InOutHeight = 4096;
    }

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Alignment = 0;
    Desc.Width = InOutWidth;
    Desc.Height = InOutHeight;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = DXGI_FORMAT_R32_TYPELESS;
    Desc.SampleDesc.Count = 1;
    Desc.SampleDesc.Quality = 0;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = DXGI_FORMAT_D32_FLOAT;
    ClearValue.DepthStencil.Depth = 1.0f;
    ClearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &ClearValue,
        IID_PPV_ARGS(OutShadowMap.ReleaseAndGetAddressOf())));

    if (OutShadowMap)
    {
        OutShadowMap->SetName(L"ShadowMap");
    }

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HeapDesc.NumDescriptors = 1;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(OutShadowDsvHeap.ReleaseAndGetAddressOf())));

    if (OutShadowDsvHeap)
    {
        OutShadowDsvHeap->SetName(L"ShadowDSVHeap");
    }

    OutShadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    D3D12_CPU_DESCRIPTOR_HANDLE DsvHandle = OutShadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC DsvDesc = {};
    DsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    DsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    DsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    Device->GetDevice()->CreateDepthStencilView(OutShadowMap.Get(), &DsvDesc, DsvHandle);
    OutShadowDsvHandle = DsvHandle;

    return true;
}

void FRenderer::ConfigureHZBOcclusion(bool bEnabled, uint32_t HZBBindlessIndex, uint32_t Width, uint32_t Height, uint32_t MipCount)
{
    HZBCullingBindlessIndex = HZBBindlessIndex;
    HZBCullingWidth = Width;
    HZBCullingHeight = Height;
    HZBCullingMipCount = MipCount;
	bHZBOcclusionEnabled = bEnabled && HZBCullingBindlessIndex != UINT32_MAX
        && HZBCullingWidth > 0 && HZBCullingHeight > 0 && HZBCullingMipCount > 0;
}

void FRenderer::DispatchGpuCulling(FDX12CommandContext& CmdContext, const FCamera& Camera)
{
    ID3D12Resource* IndirectBuffer = GetIndirectCommandBuffer();
    ID3D12Resource* RunCountBuffer = GetMeshletRunCountBuffer();
    ID3D12Resource* VisibilityBuffer = MeshletVisibilityBuffers.empty() ? nullptr : MeshletVisibilityBuffers[CurrentFrameIndex].Get();
    ID3D12Resource* TemplateBuffer = IndirectCommandTemplateBuffers.empty() ? nullptr : IndirectCommandTemplateBuffers[CurrentFrameIndex].Get();
    if (!CullingPipeline || !CullingRootSignature || !MeshletRunAppendPipeline || !MeshletRunClearPipeline || !MeshletRunRootSignature
        || !IndirectBuffer || !ModelBoundsBuffer || !VisibilityBuffer || !RunCountBuffer || !MeshletDrawDataBuffer || !MeshletRangeOffsetBuffer
        || !TemplateBuffer || IndirectCommandCount == 0 || IndirectDrawRanges.empty() || !Device || !Device->GetBindlessDescriptorHeap())
    {
        return;
    }

    const FCamera* CullingCamera = GetCullingCameraOverride();
    if (!CullingCamera)
    {
        CullingCamera = &Camera;
    }

    DirectX::XMVECTOR Planes[6] = {};
    RendererUtils::BuildCameraFrustumPlanes(*CullingCamera, Planes);

    std::array<uint32_t, 52> Constants = {};
    for (uint32_t PlaneIndex = 0; PlaneIndex < 6; ++PlaneIndex)
    {
        DirectX::XMFLOAT4 Plane;
        DirectX::XMStoreFloat4(&Plane, Planes[PlaneIndex]);
        std::memcpy(Constants.data() + PlaneIndex * 4, &Plane, sizeof(DirectX::XMFLOAT4));
    }

    const DirectX::XMMATRIX ViewProjection = CullingCamera->GetViewMatrix() * CullingCamera->GetProjectionMatrix();
    DirectX::XMFLOAT4X4 ViewProjectionMatrix = {};
    DirectX::XMStoreFloat4x4(&ViewProjectionMatrix, ViewProjection);
    std::memcpy(Constants.data() + 24, &ViewProjectionMatrix, sizeof(DirectX::XMFLOAT4X4));

    Constants[40] = IndirectCommandCount;
    Constants[41] = bHZBOcclusionEnabled ? 1u : 0u;
    Constants[42] = HZBCullingMipCount;
    Constants[43] = HZBCullingWidth;
    Constants[44] = HZBCullingHeight;
    Constants[45] = bEnableGpuDebugPrint ? 1u : 0u;
    Constants[46] = static_cast<uint32_t>(IndirectDrawRanges.size());
    Constants[47] = 0;
    const DirectX::XMFLOAT3 CameraPosition = CullingCamera->GetPosition();
    std::memcpy(Constants.data() + 48, &CameraPosition, sizeof(DirectX::XMFLOAT3));
    Constants[51] = 0;

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent CullingEvent(CommandList, L"GpuCulling");

    if (ModelBoundsBindlessIndex == UINT32_MAX || MeshletDrawDataBindlessIndex == UINT32_MAX || MeshletRangeOffsetBindlessIndex == UINT32_MAX
        || MeshletVisibilitySrvBindlessIndices.empty() || MeshletVisibilityUavBindlessIndices.empty() || MeshletRunCountUavBindlessIndices.empty()
        || IndirectCommandUavBindlessIndices.empty() || IndirectCommandTemplateBindlessIndices.empty() || MeshletConeAxisBindlessIndex == UINT32_MAX
        || MeshletConeApexBindlessIndex == UINT32_MAX || GpuDebugPrintBufferUavBindlessIndex == UINT32_MAX || GpuDebugPrintStatsUavBindlessIndex == UINT32_MAX
        || (bHZBOcclusionEnabled && HZBCullingBindlessIndex == UINT32_MAX))
    {
        return;
    }

    const uint32_t FrameIndex = CurrentFrameIndex;
    const uint32_t VisibilitySrvIndex = MeshletVisibilitySrvBindlessIndices[FrameIndex];
    const uint32_t VisibilityUavIndex = MeshletVisibilityUavBindlessIndices[FrameIndex];
    const uint32_t RunCountUavIndex = MeshletRunCountUavBindlessIndices[FrameIndex];
    const uint32_t IndirectUavIndex = IndirectCommandUavBindlessIndices[FrameIndex];
    const uint32_t TemplateSrvIndex = IndirectCommandTemplateBindlessIndices[FrameIndex];

    D3D12_RESOURCE_STATES& IndirectState = GetIndirectCommandState();
    if (IndirectState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = IndirectBuffer;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = IndirectState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CommandList->ResourceBarrier(1, &Barrier);
        IndirectState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    D3D12_RESOURCE_STATES& VisibilityState = MeshletVisibilityStates[CurrentFrameIndex];
    if (VisibilityState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = VisibilityBuffer;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = VisibilityState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CommandList->ResourceBarrier(1, &Barrier);
        VisibilityState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    D3D12_RESOURCE_STATES& RunCountState = GetMeshletRunCountState();
    if (RunCountState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = RunCountBuffer;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = RunCountState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CommandList->ResourceBarrier(1, &Barrier);
        RunCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    uint8_t* CullingConstantsMapped = GetCullingConstantBufferMapped();
    if (CullingConstantsMapped)
    {
        std::memcpy(CullingConstantsMapped, Constants.data(), sizeof(Constants));
    }

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);

    struct FMeshletRunBindlessConstants
    {
        uint32_t VisibleMeshletsIndex;
        uint32_t MeshletDrawDataIndex;
        uint32_t RangeOffsetsIndex;
        uint32_t CommandTemplatesIndex;
        uint32_t OutputCommandsIndex;
        uint32_t RunCountsIndex;
    };

    const FMeshletRunBindlessConstants RunBindlessConstants =
    {
        VisibilitySrvIndex,
        MeshletDrawDataBindlessIndex,
        MeshletRangeOffsetBindlessIndex,
        TemplateSrvIndex,
        IndirectUavIndex,
        RunCountUavIndex
    };

    CommandList->SetPipelineState(MeshletRunClearPipeline.Get());
    CommandList->SetComputeRootSignature(MeshletRunRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, GetCullingConstantBufferAddress());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(RunBindlessConstants) / sizeof(uint32_t), &RunBindlessConstants, 0);
    const uint32_t RangeDispatchCount = (static_cast<uint32_t>(IndirectDrawRanges.size()) + 63) / 64;
    CommandList->Dispatch(RangeDispatchCount, 1, 1);

    D3D12_RESOURCE_BARRIER RunCountBarrier = {};
    RunCountBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    RunCountBarrier.UAV.pResource = RunCountBuffer;
    CommandList->ResourceBarrier(1, &RunCountBarrier);

    struct FGpuCullingBindlessConstants
    {
        uint32_t ModelBoundsIndex;
        uint32_t HZBTextureIndex;
        uint32_t MeshletConeAxisIndex;
        uint32_t MeshletConeApexIndex;
        uint32_t VisibleMeshletsIndex;
        uint32_t DebugPrintBufferIndex;
        uint32_t DebugPrintStatsIndex;
    };

    const FGpuCullingBindlessConstants CullingBindlessConstants =
    {
        ModelBoundsBindlessIndex,
        HZBCullingBindlessIndex,
        MeshletConeAxisBindlessIndex,
        MeshletConeApexBindlessIndex,
        VisibilityUavIndex,
        GpuDebugPrintBufferUavBindlessIndex,
        GpuDebugPrintStatsUavBindlessIndex
    };

    CommandList->SetPipelineState(CullingPipeline.Get());
    CommandList->SetComputeRootSignature(CullingRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, GetCullingConstantBufferAddress());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(CullingBindlessConstants) / sizeof(uint32_t), &CullingBindlessConstants, 0);

    const uint32_t DispatchCount = (IndirectCommandCount + 63) / 64;
    CommandList->Dispatch(DispatchCount, 1, 1);

    if (VisibilityState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = VisibilityBuffer;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = VisibilityState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        CommandList->ResourceBarrier(1, &Barrier);
        VisibilityState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    CommandList->SetPipelineState(MeshletRunAppendPipeline.Get());
    CommandList->SetComputeRootSignature(MeshletRunRootSignature.Get());
    CommandList->SetComputeRootConstantBufferView(0, GetCullingConstantBufferAddress());
    CommandList->SetComputeRoot32BitConstants(1, sizeof(RunBindlessConstants) / sizeof(uint32_t), &RunBindlessConstants, 0);
    CommandList->Dispatch(DispatchCount, 1, 1);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = IndirectBuffer;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barrier.Transition.StateBefore = IndirectState;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    CommandList->ResourceBarrier(1, &Barrier);
    IndirectState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;

    if (RunCountState != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)
    {
        D3D12_RESOURCE_BARRIER CountBarrier = {};
        CountBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        CountBarrier.Transition.pResource = RunCountBuffer;
        CountBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        CountBarrier.Transition.StateBefore = RunCountState;
        CountBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        CommandList->ResourceBarrier(1, &CountBarrier);
        RunCountState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
}

void FRenderer::PrepareGpuDebugPrint(FDX12CommandContext& CmdContext)
{
    if (!bEnableGpuDebugPrint || !GpuDebugPrintBuffer || !GpuDebugPrintUpload || !GpuDebugPrintStatsBuffer || !GpuDebugPrintStatsUpload)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
	FScopedPixEvent DebugPrintEvent(CommandList, L"PrepareGpuDebugPrint");

    if (GpuDebugPrintState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = GpuDebugPrintBuffer.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = GpuDebugPrintState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        CommandList->ResourceBarrier(1, &Barrier);
        GpuDebugPrintState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    CommandList->CopyBufferRegion(GpuDebugPrintBuffer.Get(), 0, GpuDebugPrintUpload.Get(), 0, sizeof(uint32_t));

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = GpuDebugPrintBuffer.Get();
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barrier.Transition.StateBefore = GpuDebugPrintState;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    CommandList->ResourceBarrier(1, &Barrier);
    GpuDebugPrintState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (GpuDebugPrintStatsState != D3D12_RESOURCE_STATE_COPY_DEST)
    {
        D3D12_RESOURCE_BARRIER StatsBarrier = {};
        StatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        StatsBarrier.Transition.pResource = GpuDebugPrintStatsBuffer.Get();
        StatsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        StatsBarrier.Transition.StateBefore = GpuDebugPrintStatsState;
        StatsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        CommandList->ResourceBarrier(1, &StatsBarrier);
        GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_COPY_DEST;
    }

    CommandList->CopyBufferRegion(GpuDebugPrintStatsBuffer.Get(), 0, GpuDebugPrintStatsUpload.Get(), 0, sizeof(uint32_t) * 3);

    D3D12_RESOURCE_BARRIER StatsBarrier = {};
    StatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    StatsBarrier.Transition.pResource = GpuDebugPrintStatsBuffer.Get();
    StatsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    StatsBarrier.Transition.StateBefore = GpuDebugPrintStatsState;
    StatsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    CommandList->ResourceBarrier(1, &StatsBarrier);
    GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

bool FRenderer::CreateGpuDebugPrintResources(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    if (!GpuDebugPrintBuffer)
    {
        LogError("GPU debug print buffer is missing.");
        return false;
    }

    const std::wstring FontPath = L"Assets/Fonts/Roboto-Medium.ttf";
    const float FontSize = 16.0f;
    const uint32_t AtlasWidth = 512;
    const uint32_t AtlasHeight = 512;

    FDebugPrintFontResources FontResources;
    if (!CreateDebugPrintFontResources(Device, FontPath, FontSize, AtlasWidth, AtlasHeight, FontResources))
    {
        LogError("Failed to create GPU debug print font resources.");
        return false;
    }

    GpuDebugPrintFontTexture = FontResources.FontTexture;
    GpuDebugPrintGlyphBuffer = FontResources.GlyphBuffer;
    GpuDebugPrintAtlasWidth = FontResources.AtlasWidth;
    GpuDebugPrintAtlasHeight = FontResources.AtlasHeight;
    GpuDebugPrintFirstChar = FontResources.FirstChar;
    GpuDebugPrintCharCount = FontResources.CharCount;
    GpuDebugPrintFontSize = FontResources.FontSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC GlyphSrvDesc = {};
    GlyphSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    GlyphSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    GlyphSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    GlyphSrvDesc.Buffer.FirstElement = 0;
    GlyphSrvDesc.Buffer.NumElements = 128;
    GlyphSrvDesc.Buffer.StructureByteStride = sizeof(FDebugPrintGlyph);
    GlyphSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    GpuDebugPrintGlyphBindlessIndex = Device->CreateBindlessSrv(GpuDebugPrintGlyphBuffer.Get(), GlyphSrvDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC FontSrvDesc = {};
    FontSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    FontSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    FontSrvDesc.Format = DXGI_FORMAT_R8_UNORM;
    FontSrvDesc.Texture2D.MostDetailedMip = 0;
    FontSrvDesc.Texture2D.MipLevels = 1;
    GpuDebugPrintFontBindlessIndex = Device->CreateBindlessSrv(GpuDebugPrintFontTexture.Get(), FontSrvDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC BufferSrvDesc = {};
    BufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    BufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    BufferSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    BufferSrvDesc.Buffer.FirstElement = 0;
    BufferSrvDesc.Buffer.NumElements = static_cast<UINT>(GpuDebugPrintBufferSize / 4);
    BufferSrvDesc.Buffer.StructureByteStride = 0;
    BufferSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    GpuDebugPrintBufferBindlessIndex = Device->CreateBindlessSrv(GpuDebugPrintBuffer.Get(), BufferSrvDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC StatsSrvDesc = {};
    StatsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    StatsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    StatsSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    StatsSrvDesc.Buffer.FirstElement = 0;
    StatsSrvDesc.Buffer.NumElements = 3;
    StatsSrvDesc.Buffer.StructureByteStride = 0;
    StatsSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    GpuDebugPrintStatsBindlessIndex = Device->CreateBindlessSrv(GpuDebugPrintStatsBuffer.Get(), StatsSrvDesc);

    D3D12_UNORDERED_ACCESS_VIEW_DESC DebugBufferUavDesc = {};
    DebugBufferUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    DebugBufferUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    DebugBufferUavDesc.Buffer.FirstElement = 0;
    DebugBufferUavDesc.Buffer.NumElements = static_cast<UINT>(GpuDebugPrintBufferSize / 4);
    DebugBufferUavDesc.Buffer.StructureByteStride = 0;
    DebugBufferUavDesc.Buffer.CounterOffsetInBytes = 0;
    DebugBufferUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    GpuDebugPrintBufferUavBindlessIndex = Device->CreateBindlessUav(GpuDebugPrintBuffer.Get(), nullptr, DebugBufferUavDesc);

    D3D12_UNORDERED_ACCESS_VIEW_DESC StatsUavDesc = {};
    StatsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    StatsUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    StatsUavDesc.Buffer.FirstElement = 0;
    StatsUavDesc.Buffer.NumElements = 3;
    StatsUavDesc.Buffer.StructureByteStride = 0;
    StatsUavDesc.Buffer.CounterOffsetInBytes = 0;
    StatsUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    GpuDebugPrintStatsUavBindlessIndex = Device->CreateBindlessUav(GpuDebugPrintStatsBuffer.Get(), nullptr, StatsUavDesc);

    return true;
}

bool FRenderer::CreateGpuDebugPrintPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    if (!Device)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER1 Params[2] = {};
    // Params[0]: Debug print draw constants (screen size, font range)
    Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[0].Constants.ShaderRegister = 0;
    Params[0].Constants.RegisterSpace = 0;
    Params[0].Constants.Num32BitValues = 4;
    Params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Params[1]: Debug print bindless indices (glyph buffer, font atlas, print buffer)
    Params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[1].Constants.ShaderRegister = 1;
    Params[1].Constants.RegisterSpace = 0;
    Params[1].Constants.Num32BitValues = 3;
    Params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC Sampler = {};
    Sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.ShaderRegister = 0;
    Sampler.RegisterSpace = 0;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC1 RootDesc = {};
    RootDesc.NumParameters = _countof(Params);
    RootDesc.pParameters = Params;
    RootDesc.NumStaticSamplers = 1;
    RootDesc.pStaticSamplers = &Sampler;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        | D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc = {};
    VersionedRootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    VersionedRootDesc.Desc_1_1 = RootDesc;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(GpuDebugPrintRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = RendererUtils::BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/GpuDebugPrint.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/GpuDebugPrint.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = GpuDebugPrintRootSignature.Get();
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    PsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    PsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    PsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    PsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = FALSE;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(GpuDebugPrintPipeline.GetAddressOf())));
    return true;
}

bool FRenderer::CreateGpuDebugPrintStatsPipeline(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER1 RootParams[1] = {};
    // RootParams[0]: Stats bindless indices (stats buffer SRV, debug print buffer UAV), used in Shaders/GpuDebugPrintStats.hlsl CSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].Constants.ShaderRegister = 0;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.Num32BitValues = 2;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 RootDesc = {};
    RootDesc.NumParameters = _countof(RootParams);
    RootDesc.pParameters = RootParams;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc = {};
    VersionedRootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    VersionedRootDesc.Desc_1_1 = RootDesc;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(GpuDebugPrintStatsRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/GpuDebugPrintStats.hlsl", L"CSMain", CSTarget, CsByteCode))
    {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC CsDesc = {};
    CsDesc.pRootSignature = GpuDebugPrintStatsRootSignature.Get();
    CsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&CsDesc, IID_PPV_ARGS(GpuDebugPrintStatsPipeline.GetAddressOf())));
    return true;
}

void FRenderer::DispatchGpuDebugPrintStats(FDX12CommandContext& CmdContext)
{
    if (!bEnableGpuDebugPrint || !GpuDebugPrintStatsPipeline || !GpuDebugPrintStatsRootSignature || !Device || !Device->GetBindlessDescriptorHeap())
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugStatsEvent(CommandList, L"GpuDebugPrintStats");

    if (GpuDebugPrintStatsState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = GpuDebugPrintStatsBuffer.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = GpuDebugPrintStatsState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        CommandList->ResourceBarrier(1, &Barrier);
        GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    if (GpuDebugPrintState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = GpuDebugPrintBuffer.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = GpuDebugPrintState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CommandList->ResourceBarrier(1, &Barrier);
        GpuDebugPrintState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    if (GpuDebugPrintStatsBindlessIndex == UINT32_MAX || GpuDebugPrintBufferUavBindlessIndex == UINT32_MAX)
    {
        return;
    }

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(GpuDebugPrintStatsPipeline.Get());
    CommandList->SetComputeRootSignature(GpuDebugPrintStatsRootSignature.Get());
    const uint32_t BindlessIndices[] = { GpuDebugPrintStatsBindlessIndex, GpuDebugPrintBufferUavBindlessIndex };
    CommandList->SetComputeRoot32BitConstants(0, _countof(BindlessIndices), BindlessIndices, 0);
    CommandList->Dispatch(1, 1, 1);
}

bool FRenderer::CreateRayTracingPipeline(FDX12Device* Device)
{
    bRayTracingPipelineReady = false;

    if (!bRayTracedShadowsEnabled)
    {
        return true;
    }

    if (!Device || !Device->IsRayTracingSupported())
    {
        LogWarning("Ray tracing pipeline skipped: DXR is not supported.");
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> LibraryBytecode;
    if (!Compiler.CompileLibraryFromFile(L"Shaders/ShadowRays.hlsl", L"lib_6_6", LibraryBytecode))
    {
        LogError("Failed to compile ray tracing shader library.");
        return false;
    }

    D3D12_SHADER_BYTECODE Library = {};
    Library.pShaderBytecode = LibraryBytecode.data();
    Library.BytecodeLength = LibraryBytecode.size();

    const wchar_t* Exports[] = { L"RayGen", L"Miss", L"ClosestHit" };
    D3D12_EXPORT_DESC ExportDescs[3] = {};
    for (uint32_t Index = 0; Index < 3; ++Index)
    {
        ExportDescs[Index].Name = Exports[Index];
        ExportDescs[Index].ExportToRename = nullptr;
        ExportDescs[Index].Flags = D3D12_EXPORT_FLAG_NONE;
    }

    D3D12_DXIL_LIBRARY_DESC LibraryDesc = {};
    LibraryDesc.DXILLibrary = Library;
    LibraryDesc.NumExports = _countof(ExportDescs);
    LibraryDesc.pExports = ExportDescs;

    D3D12_HIT_GROUP_DESC HitGroupDesc = {};
    HitGroupDesc.HitGroupExport = L"ShadowHitGroup";
    HitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    HitGroupDesc.ClosestHitShaderImport = L"ClosestHit";

    D3D12_RAYTRACING_SHADER_CONFIG ShaderConfig = {};
    ShaderConfig.MaxPayloadSizeInBytes = sizeof(uint32_t);
    ShaderConfig.MaxAttributeSizeInBytes = sizeof(float) * 2;

    D3D12_RAYTRACING_PIPELINE_CONFIG PipelineConfig = {};
    PipelineConfig.MaxTraceRecursionDepth = 1;

    D3D12_DESCRIPTOR_RANGE DepthSrvRange = {};
    DepthSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    DepthSrvRange.NumDescriptors = 2;
    DepthSrvRange.BaseShaderRegister = 1;
    DepthSrvRange.RegisterSpace = 0;
    DepthSrvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_DESCRIPTOR_RANGE UavRange = {};
    UavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    UavRange.NumDescriptors = 1;
    UavRange.BaseShaderRegister = 0;
    UavRange.RegisterSpace = 0;
    UavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER RootParameters[4] = {};
    RootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    RootParameters[0].Descriptor.ShaderRegister = 0;
    RootParameters[0].Descriptor.RegisterSpace = 0;
    RootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    RootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParameters[1].Descriptor.ShaderRegister = 0;
    RootParameters[1].Descriptor.RegisterSpace = 0;
    RootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    RootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParameters[2].DescriptorTable.NumDescriptorRanges = 1;
    RootParameters[2].DescriptorTable.pDescriptorRanges = &DepthSrvRange;
    RootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    RootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    RootParameters[3].DescriptorTable.NumDescriptorRanges = 1;
    RootParameters[3].DescriptorTable.pDescriptorRanges = &UavRange;
    RootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC GlobalRootDesc = {};
    GlobalRootDesc.NumParameters = _countof(RootParameters);
    GlobalRootDesc.pParameters = RootParameters;
    GlobalRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> GlobalSignatureBlob;
    ComPtr<ID3DBlob> GlobalErrorBlob;
    HR_CHECK(D3D12SerializeRootSignature(&GlobalRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, GlobalSignatureBlob.GetAddressOf(), GlobalErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(
        0,
        GlobalSignatureBlob->GetBufferPointer(),
        GlobalSignatureBlob->GetBufferSize(),
        IID_PPV_ARGS(RayTracingGlobalRootSignature.ReleaseAndGetAddressOf())));

    D3D12_ROOT_SIGNATURE_DESC LocalRootDesc = {};
    LocalRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

    ComPtr<ID3DBlob> LocalSignatureBlob;
    ComPtr<ID3DBlob> LocalErrorBlob;
    HR_CHECK(D3D12SerializeRootSignature(&LocalRootDesc, D3D_ROOT_SIGNATURE_VERSION_1, LocalSignatureBlob.GetAddressOf(), LocalErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(
        0,
        LocalSignatureBlob->GetBufferPointer(),
        LocalSignatureBlob->GetBufferSize(),
        IID_PPV_ARGS(RayTracingLocalRootSignature.ReleaseAndGetAddressOf())));

    D3D12_STATE_SUBOBJECT Subobjects[7] = {};
    uint32_t SubobjectIndex = 0;

    Subobjects[SubobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    Subobjects[SubobjectIndex].pDesc = &LibraryDesc;
    ++SubobjectIndex;

    Subobjects[SubobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    Subobjects[SubobjectIndex].pDesc = &HitGroupDesc;
    ++SubobjectIndex;

    Subobjects[SubobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    Subobjects[SubobjectIndex].pDesc = &ShaderConfig;
    ++SubobjectIndex;

    D3D12_GLOBAL_ROOT_SIGNATURE GlobalRootSignatureDesc = {};
    GlobalRootSignatureDesc.pGlobalRootSignature = RayTracingGlobalRootSignature.Get();
    Subobjects[SubobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    Subobjects[SubobjectIndex].pDesc = &GlobalRootSignatureDesc;
    ++SubobjectIndex;

    D3D12_LOCAL_ROOT_SIGNATURE LocalRootSignatureDesc = {};
    LocalRootSignatureDesc.pLocalRootSignature = RayTracingLocalRootSignature.Get();
    Subobjects[SubobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
    Subobjects[SubobjectIndex].pDesc = &LocalRootSignatureDesc;
    const uint32_t LocalRootIndex = SubobjectIndex;
    ++SubobjectIndex;

    const wchar_t* LocalExports[] = { L"ShadowHitGroup" };
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION Association = {};
    Association.pSubobjectToAssociate = &Subobjects[LocalRootIndex];
    Association.NumExports = _countof(LocalExports);
    Association.pExports = LocalExports;
    Subobjects[SubobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    Subobjects[SubobjectIndex].pDesc = &Association;
    ++SubobjectIndex;

    Subobjects[SubobjectIndex].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    Subobjects[SubobjectIndex].pDesc = &PipelineConfig;
    ++SubobjectIndex;

    D3D12_STATE_OBJECT_DESC StateObjectDesc = {};
    StateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    StateObjectDesc.NumSubobjects = SubobjectIndex;
    StateObjectDesc.pSubobjects = Subobjects;

    if (!RayTracingDevice.IsValid() && !Device->CreateRayTracingDevice(RayTracingDevice))
    {
        return false;
    }

    if (!RayTracingPipeline.Initialize(RayTracingDevice.GetDevice(), StateObjectDesc))
    {
        LogError("Ray tracing pipeline state creation failed.");
        return false;
    }

    ID3D12StateObjectProperties* Properties = RayTracingPipeline.GetStateObjectProperties();
    if (!Properties)
    {
        LogError("Ray tracing pipeline state properties missing.");
        return false;
    }

    const void* RayGenId = Properties->GetShaderIdentifier(L"RayGen");
    const void* MissId = Properties->GetShaderIdentifier(L"Miss");
    const void* HitGroupId = Properties->GetShaderIdentifier(L"ShadowHitGroup");
    if (!RayGenId || !MissId || !HitGroupId)
    {
        LogError("Ray tracing shader identifiers are missing.");
        return false;
    }

    RayTracingShaderRecordSize = AlignShaderRecordSize(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    RayTracingShaderTableSize = RayTracingShaderRecordSize * 3;

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC TableDesc = {};
    TableDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    TableDesc.Width = RayTracingShaderTableSize;
    TableDesc.Height = 1;
    TableDesc.DepthOrArraySize = 1;
    TableDesc.MipLevels = 1;
    TableDesc.SampleDesc.Count = 1;
    TableDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &TableDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(RayTracingShaderTable.ReleaseAndGetAddressOf())));

    if (!RayTracingShaderTable)
    {
        LogError("Ray tracing shader table allocation failed.");
        return false;
    }

    RayTracingShaderTable->SetName(L"RayTracingShaderTable");

    uint8_t* MappedData = nullptr;
    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(RayTracingShaderTable->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    std::memcpy(MappedData, RayGenId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(MappedData + RayTracingShaderRecordSize, MissId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(MappedData + RayTracingShaderRecordSize * 2, HitGroupId, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    RayTracingShaderTable->Unmap(0, nullptr);

    if (!RayTracingUavHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
        HeapDesc.NumDescriptors = 3;
        HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(RayTracingUavHeap.ReleaseAndGetAddressOf())));
        if (RayTracingUavHeap)
        {
            RayTracingUavHeap->SetName(L"RayTracingUavHeap");
        }
    }

    bRayTracingPipelineReady = true;
    return true;
}

bool FRenderer::CreateSkinnedPositionBuffers()
{
    if (!Device || !Device->GetBindlessDescriptorHeap())
    {
        return false;
    }

    bool bAllReady = true;
    for (FSceneModelResource& Model : SceneModels)
    {
        if (Model.BoneMatrixBindlessIndex == UINT32_MAX || Model.BoneMatrixCount == 0)
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

        const uint64_t BufferSize = static_cast<uint64_t>(VertexCount) * sizeof(FFloat3);
        Model.SkinnedPositionBuffers.assign(FramesInFlight, nullptr);
        Model.SkinnedPositionSrvBindlessIndices.assign(FramesInFlight, UINT32_MAX);
        Model.SkinnedPositionUavBindlessIndices.assign(FramesInFlight, UINT32_MAX);
        Model.SkinnedPositionStates.assign(FramesInFlight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        D3D12_HEAP_PROPERTIES DefaultHeap = {};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        DefaultHeap.CreationNodeMask = 1;
        DefaultHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC BufferDesc = {};
        BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufferDesc.Width = BufferSize;
        BufferDesc.Height = 1;
        BufferDesc.DepthOrArraySize = 1;
        BufferDesc.MipLevels = 1;
        BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        BufferDesc.SampleDesc.Count = 1;
        BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        BufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        const auto CreateStructuredBufferSrv = [&](ID3D12Resource* Buffer, uint32_t Stride)
        {
            if (!Buffer || Stride == 0)
            {
                return UINT32_MAX;
            }
            const D3D12_RESOURCE_DESC BufferDescLocal = Buffer->GetDesc();
            const uint64_t ElementCount = BufferDescLocal.Width / Stride;
            if (ElementCount == 0)
            {
                return UINT32_MAX;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC SrvDesc = {};
            SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            SrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            SrvDesc.Format = DXGI_FORMAT_UNKNOWN;
            SrvDesc.Buffer.FirstElement = 0;
            SrvDesc.Buffer.NumElements = static_cast<UINT>(ElementCount);
            SrvDesc.Buffer.StructureByteStride = Stride;
            SrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            return Device->CreateBindlessSrv(Buffer, SrvDesc);
        };

        const auto CreateStructuredBufferUav = [&](ID3D12Resource* Buffer, uint32_t Stride)
        {
            if (!Buffer || Stride == 0)
            {
                return UINT32_MAX;
            }
            const D3D12_RESOURCE_DESC BufferDescLocal = Buffer->GetDesc();
            const uint64_t ElementCount = BufferDescLocal.Width / Stride;
            if (ElementCount == 0)
            {
                return UINT32_MAX;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC UavDesc = {};
            UavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            UavDesc.Format = DXGI_FORMAT_UNKNOWN;
            UavDesc.Buffer.FirstElement = 0;
            UavDesc.Buffer.NumElements = static_cast<UINT>(ElementCount);
            UavDesc.Buffer.StructureByteStride = Stride;
            UavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
            return Device->CreateBindlessUav(Buffer, nullptr, UavDesc);
        };

        for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
        {
            HR_CHECK(Device->GetDevice()->CreateCommittedResource(
                &DefaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &BufferDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                nullptr,
                IID_PPV_ARGS(Model.SkinnedPositionBuffers[FrameIndex].ReleaseAndGetAddressOf())));

            if (Model.SkinnedPositionBuffers[FrameIndex])
            {
                const std::wstring BufferName = L"SkinnedPositionBuffer_" + std::to_wstring(FrameIndex);
                Model.SkinnedPositionBuffers[FrameIndex]->SetName(BufferName.c_str());
                Model.SkinnedPositionSrvBindlessIndices[FrameIndex] = CreateStructuredBufferSrv(
                    Model.SkinnedPositionBuffers[FrameIndex].Get(),
                    sizeof(FFloat3));
                Model.SkinnedPositionUavBindlessIndices[FrameIndex] = CreateStructuredBufferUav(
                    Model.SkinnedPositionBuffers[FrameIndex].Get(),
                    sizeof(FFloat3));
            }
        }

        bool bReady = true;
        for (uint32_t FrameIndex = 0; FrameIndex < FramesInFlight; ++FrameIndex)
        {
            bReady = bReady && Model.SkinnedPositionBuffers[FrameIndex]
                && Model.SkinnedPositionSrvBindlessIndices[FrameIndex] != UINT32_MAX
                && Model.SkinnedPositionUavBindlessIndices[FrameIndex] != UINT32_MAX;
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
        const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0
            && FrameIndex < Model.SkinnedPositionBuffers.size()
            && FrameIndex < Model.SkinnedPositionUavBindlessIndices.size();

        if (bUseSkinning)
        {
            Model.SkinnedPositionBuffer = Model.SkinnedPositionBuffers[FrameIndex];
            Model.SkinnedPositionSrvBindlessIndex = Model.SkinnedPositionSrvBindlessIndices[FrameIndex];
            Model.SkinnedPositionUavBindlessIndex = Model.SkinnedPositionUavBindlessIndices[FrameIndex];
        }
        else
        {
            Model.SkinnedPositionBuffer.Reset();
            Model.SkinnedPositionSrvBindlessIndex = UINT32_MAX;
            Model.SkinnedPositionUavBindlessIndex = UINT32_MAX;
        }

        if (!bUseSkinning || !Model.SkinnedPositionBuffer || Model.SkinnedPositionUavBindlessIndex == UINT32_MAX)
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

    if (!Device || !SkinningPipeline || !SkinningRootSignature || !Device->GetBindlessDescriptorHeap())
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
        const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0
            && FrameIndex < Model.SkinnedPositionBuffers.size()
            && FrameIndex < Model.SkinnedPositionUavBindlessIndices.size()
            && FrameIndex < Model.SkinnedPositionStates.size();
        if (!bUseSkinning || !SceneModelSkinningVisibility[ModelIndex])
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

        if (Model.SkinnedPositionStates[FrameIndex] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            D3D12_RESOURCE_BARRIER Barrier = {};
            Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            Barrier.Transition.pResource = SkinnedBuffer;
            Barrier.Transition.StateBefore = Model.SkinnedPositionStates[FrameIndex];
            Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            CommandList->ResourceBarrier(1, &Barrier);
            Model.SkinnedPositionStates[FrameIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        const uint32_t Constants[] =
        {
            VertexCount,
            Model.VertexBufferBindlessIndices[0],
            Model.VertexBufferBindlessIndices[5],
            Model.VertexBufferBindlessIndices[6],
            Model.BoneMatrixBindlessIndex,
            Model.SkinnedPositionUavBindlessIndex
        };
        CommandList->SetComputeRoot32BitConstants(0, _countof(Constants), Constants, 0);

        const uint32_t DispatchCount = (VertexCount + 63) / 64;
        CommandList->Dispatch(DispatchCount, 1, 1);

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = SkinnedBuffer;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        CommandList->ResourceBarrier(1, &Barrier);
        Model.SkinnedPositionStates[FrameIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
}


void FRenderer::UpdateRayTracingBlasRefit(FDX12CommandContext& CmdContext)
{
    if (!bRayTracedShadowsEnabled)
    {
        return;
    }

    if (!Device || !Device->IsRayTracingSupported() || !bRayTracingPipelineReady)
    {
        return;
    }

    if (!RayTracingDevice.IsValid() && !Device->CreateRayTracingDevice(RayTracingDevice))
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
    D3D12_RESOURCE_BARRIER SkinningBarrier = {};
    SkinningBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    SkinningBarrier.UAV.pResource = nullptr;
    CommandList4->ResourceBarrier(1, &SkinningBarrier);

    const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();

    for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
    {
        FSceneModelResource& Model = SceneModels[ModelIndex];
        if (!Model.bHasRayTracingBlas || !Model.BlasResultBuffer || !Model.BlasScratchBuffer)
        {
            continue;
        }

        const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0
            && FrameIndex < Model.SkinnedPositionBuffers.size();
        if (!bUseSkinning)
        {
            continue;
        }

        if (!SceneModelSkinningVisibility.empty() && ModelIndex < SceneModelSkinningVisibility.size()
            && !SceneModelSkinningVisibility[ModelIndex])
        {
            continue;
        }

        ID3D12Resource* SkinnedBuffer = Model.SkinnedPositionBuffers[FrameIndex].Get();
        if (!SkinnedBuffer)
        {
            continue;
        }

        Model.BlasGeometryDesc.Triangles.VertexBuffer.StartAddress = SkinnedBuffer->GetGPUVirtualAddress();

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
        Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        Inputs.NumDescs = 1;
        Inputs.pGeometryDescs = &Model.BlasGeometryDesc;
        Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BuildDesc = {};
        BuildDesc.Inputs = Inputs;
        BuildDesc.DestAccelerationStructureData = Model.BlasResultBuffer->GetGPUVirtualAddress();
        BuildDesc.SourceAccelerationStructureData = Model.BlasResultBuffer->GetGPUVirtualAddress();
        BuildDesc.ScratchAccelerationStructureData = Model.BlasScratchBuffer->GetGPUVirtualAddress();

        CommandList4->BuildRaytracingAccelerationStructure(&BuildDesc, 0, nullptr);

        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Barrier.UAV.pResource = Model.BlasResultBuffer.Get();
        CommandList4->ResourceBarrier(1, &Barrier);
        bHasUpdates = true;
    }

    if (bHasUpdates)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        Barrier.UAV.pResource = nullptr;
        CommandList4->ResourceBarrier(1, &Barrier);
    }
}

void FRenderer::BuildRayTracingTlas(FDX12CommandContext& CmdContext)
{
    if (!bRayTracedShadowsEnabled)
    {
        return;
    }

    if (!Device || !Device->IsRayTracingSupported() || !bRayTracingPipelineReady)
    {
        return;
    }

    if (!RayTracingDevice.IsValid() && !Device->CreateRayTracingDevice(RayTracingDevice))
    {
        return;
    }

    ID3D12GraphicsCommandList4* CommandList4 = CmdContext.GetCommandList4();
    if (!CommandList4)
    {
        return;
    }

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> Instances;
    Instances.reserve(SceneModels.size());

    uint32_t InstanceId = 0;
    for (size_t ModelIndex = 0; ModelIndex < SceneModels.size(); ++ModelIndex)
    {
        const FSceneModelResource& Model = SceneModels[ModelIndex];
        if (!Model.bHasRayTracingBlas || !Model.BlasResultBuffer)
        {
            continue;
        }

        if (!SceneModelVisibility.empty() && !SceneModelVisibility[ModelIndex])
        {
            continue;
        }

        D3D12_RAYTRACING_INSTANCE_DESC InstanceDesc = {};
        InstanceDesc.InstanceID = InstanceId++;
        InstanceDesc.InstanceMask = 0xFF;
        InstanceDesc.InstanceContributionToHitGroupIndex = 0;
        InstanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        InstanceDesc.AccelerationStructure = Model.BlasResultBuffer->GetGPUVirtualAddress();

        DirectX::XMFLOAT4X4 World = {};
        DirectX::XMStoreFloat4x4(&World, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&Model.WorldMatrix)));
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
    }

    if (Instances.empty())
    {
        return;
    }

    const uint32_t FrameIndex = CmdContext.GetCurrentFrameIndex();
    if (TlasScratchBuffers.size() != GetFramesInFlight())
    {
        TlasScratchBuffers.resize(GetFramesInFlight());
        TlasResultBuffers.resize(GetFramesInFlight());
        TlasInstanceBuffers.resize(GetFramesInFlight());
        TlasBuilt.assign(GetFramesInFlight(), false);
        TlasInstanceCapacity = 0;
    }

    const uint32_t InstanceCount = static_cast<uint32_t>(Instances.size());
    if (InstanceCount > TlasInstanceCapacity)
    {
        TlasInstanceCapacity = InstanceCount;
    }

    const uint64_t InstanceBufferSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * TlasInstanceCapacity;
    if (!TlasInstanceBuffers[FrameIndex] || TlasInstanceBuffers[FrameIndex]->GetDesc().Width < InstanceBufferSize)
    {
        D3D12_HEAP_PROPERTIES UploadHeap = {};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC InstanceBufferDesc = {};
        InstanceBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        InstanceBufferDesc.Width = InstanceBufferSize;
        InstanceBufferDesc.Height = 1;
        InstanceBufferDesc.DepthOrArraySize = 1;
        InstanceBufferDesc.MipLevels = 1;
        InstanceBufferDesc.SampleDesc.Count = 1;
        InstanceBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &UploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &InstanceBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(TlasInstanceBuffers[FrameIndex].ReleaseAndGetAddressOf())));
    }

    if (TlasInstanceBuffers[FrameIndex])
    {
        TlasInstanceBuffers[FrameIndex]->SetName(L"TLAS_InstanceBuffer");
        void* MappedData = nullptr;
        D3D12_RANGE EmptyRange = { 0, 0 };
        HR_CHECK(TlasInstanceBuffers[FrameIndex]->Map(0, &EmptyRange, &MappedData));
        std::memcpy(MappedData, Instances.data(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * Instances.size());
        TlasInstanceBuffers[FrameIndex]->Unmap(0, nullptr);
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
    Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    Inputs.InstanceDescs = TlasInstanceBuffers[FrameIndex]->GetGPUVirtualAddress();
    Inputs.NumDescs = InstanceCount;
    Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO PrebuildInfo = {};
    RayTracingDevice.GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &PrebuildInfo);

    const uint64_t ScratchSize = (PrebuildInfo.ScratchDataSizeInBytes + 255) & ~255ull;
    const uint64_t ResultSize = (PrebuildInfo.ResultDataMaxSizeInBytes + 255) & ~255ull;

    const auto EnsureBuffer = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& Buffer, uint64_t Size, D3D12_RESOURCE_STATES State, const wchar_t* Name)
    {
        if (Buffer && Buffer->GetDesc().Width >= Size)
        {
            return;
        }

        D3D12_HEAP_PROPERTIES HeapProps = {};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Desc = {};
        Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width = Size;
        Desc.Height = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels = 1;
        Desc.SampleDesc.Count = 1;
        Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            State,
            nullptr,
            IID_PPV_ARGS(Buffer.ReleaseAndGetAddressOf())));

        if (Buffer && Name)
        {
            Buffer->SetName(Name);
        }
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

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    Barrier.UAV.pResource = TlasResultBuffers[FrameIndex].Get();
    CommandList4->ResourceBarrier(1, &Barrier);

    TlasBuilt[FrameIndex] = true;
}

void FRenderer::RenderGpuDebugPrint(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle)
{
    if (!bEnableGpuDebugPrint || !GpuDebugPrintPipeline || !GpuDebugPrintRootSignature || !Device || !Device->GetBindlessDescriptorHeap())
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugEvent(CommandList, L"GpuDebugPrint");

    if (GpuDebugPrintGlyphBindlessIndex == UINT32_MAX || GpuDebugPrintFontBindlessIndex == UINT32_MAX || GpuDebugPrintBufferBindlessIndex == UINT32_MAX)
    {
        return;
    }

    if (GpuDebugPrintState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = GpuDebugPrintBuffer.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = GpuDebugPrintState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        CommandList->ResourceBarrier(1, &Barrier);
        GpuDebugPrintState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }

    CmdContext.SetRenderTarget(OutputHandle, nullptr);

    struct FDebugPrintConstants
    {
        DirectX::XMFLOAT2 ScreenSize;
        uint32_t FirstChar;
        uint32_t CharCount;
    };

    struct FDebugPrintBindlessConstants
    {
        uint32_t GlyphBufferIndex;
        uint32_t FontAtlasIndex;
        uint32_t DebugPrintBufferIndex;
    };

    const FDebugPrintConstants Constants =
    {
        DirectX::XMFLOAT2(Viewport.Width, Viewport.Height),
        GpuDebugPrintFirstChar,
        GpuDebugPrintCharCount
    };

    const FDebugPrintBindlessConstants BindlessConstants =
    {
        GpuDebugPrintGlyphBindlessIndex,
        GpuDebugPrintFontBindlessIndex,
        GpuDebugPrintBufferBindlessIndex
    };

    ID3D12DescriptorHeap* Heaps[] = { Device->GetBindlessDescriptorHeap() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(GpuDebugPrintPipeline.Get());
    CommandList->SetGraphicsRootSignature(GpuDebugPrintRootSignature.Get());
    CommandList->RSSetViewports(1, &Viewport);
    CommandList->RSSetScissorRects(1, &ScissorRect);
    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
    CommandList->SetGraphicsRoot32BitConstants(1, sizeof(BindlessConstants) / sizeof(uint32_t), &BindlessConstants, 0);
    CommandList->DrawInstanced(6 * GpuDebugPrintMaxEntries, 1, 0, 0);

    if (GpuDebugPrintState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = GpuDebugPrintBuffer.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = GpuDebugPrintState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CommandList->ResourceBarrier(1, &Barrier);
        GpuDebugPrintState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
}

bool FRenderer::PrepareGpuDrivenDrawData(FGpuDrivenPreparedData& OutData)
{
    if (SceneModels.empty() || !GetSceneConstantBuffer())
    {
        return false;
    }

    IndirectDrawRanges.clear();

    // Pre-compute pipeline keys to avoid redundant calculations during sorting
    std::vector<uint32_t> PipelineKeys(SceneModels.size());
    for (size_t Index = 0; Index < SceneModels.size(); ++Index)
    {
        PipelineKeys[Index] = RendererUtils::BuildPipelineKey(SceneModels[Index]);
    }

    std::vector<uint32_t> SortedIndices(SceneModels.size());
    for (uint32_t Index = 0; Index < SortedIndices.size(); ++Index)
    {
        SortedIndices[Index] = Index;
    }

    std::sort(SortedIndices.begin(), SortedIndices.end(), [&](uint32_t A, uint32_t B)
    {
        const uint32_t KeyA = PipelineKeys[A];
        const uint32_t KeyB = PipelineKeys[B];
        if (KeyA != KeyB)
        {
            return KeyA < KeyB;
        }
        const std::array<uint32_t, 4> IndicesA =
        {
            SceneModels[A].BaseColorBindlessIndex,
            SceneModels[A].MetallicRoughnessBindlessIndex,
            SceneModels[A].NormalBindlessIndex,
            SceneModels[A].EmissiveBindlessIndex
        };
        const std::array<uint32_t, 4> IndicesB =
        {
            SceneModels[B].BaseColorBindlessIndex,
            SceneModels[B].MetallicRoughnessBindlessIndex,
            SceneModels[B].NormalBindlessIndex,
            SceneModels[B].EmissiveBindlessIndex
        };
        return IndicesA < IndicesB;
    });

    size_t TotalCommandCount = 0;
    for (const FSceneModelResource& Model : SceneModels)
    {
        const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
        if (bUseSkinning || Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
        {
            continue;
        }

        if (Model.bUseMeshletCulling && !Model.Meshlets.empty())
        {
            TotalCommandCount += Model.Meshlets.size();
        }
        else
        {
            TotalCommandCount += 1;
        }
    }

    OutData.Commands.clear();
    OutData.Commands.reserve(TotalCommandCount);
    OutData.MeshletDrawData.clear();
    OutData.MeshletDrawData.reserve(TotalCommandCount);
    OutData.Bounds.clear();
    OutData.Bounds.reserve(TotalCommandCount);
    OutData.ConeAxisCutoff.clear();
    OutData.ConeAxisCutoff.reserve(TotalCommandCount);
    OutData.ConeApex.clear();
    OutData.ConeApex.reserve(TotalCommandCount);

    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = GetSceneConstantBufferAddress();
    uint32_t GroupIndex = 0;

    for (uint32_t SortedIndex : SortedIndices)
    {
        const FSceneModelResource& Model = SceneModels[SortedIndex];
        const uint32_t PipelineKey = PipelineKeys[SortedIndex];
        const bool bUseSkinning = Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0;
        if (bUseSkinning || Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Blend))
        {
            continue;
        }

        const std::array<uint32_t, 4> MaterialIndices =
        {
            Model.BaseColorBindlessIndex,
            Model.MetallicRoughnessBindlessIndex,
            Model.NormalBindlessIndex,
            Model.EmissiveBindlessIndex
        };
        if (IndirectDrawRanges.empty()
            || IndirectDrawRanges.back().PipelineKey != PipelineKey
            || IndirectDrawRanges.back().MaterialBindlessIndices != MaterialIndices)
        {
            FIndirectDrawRange Range;
            Range.Start = static_cast<uint32_t>(OutData.Commands.size());
            Range.Count = 0;
            Range.PipelineKey = PipelineKey;
            Range.MaterialBindlessIndices = MaterialIndices;
            if (!Model.Name.empty())
            {
                Range.Name.assign(Model.Name.begin(), Model.Name.end());
            }
            IndirectDrawRanges.push_back(Range);
        }

        const uint32_t RangeIndex = static_cast<uint32_t>(IndirectDrawRanges.size() - 1);
        if (Model.bUseMeshletCulling && !Model.Meshlets.empty() && !Model.MeshletBounds.empty())
        {
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

                FIndirectDrawCommand Command;
                Command.ConstantBufferAddress = ConstantBufferBase + SceneConstantBufferStride * SortedIndex;
                Command.DrawArguments.VertexCountPerInstance = Meshlet.IndexCount;
                Command.DrawArguments.InstanceCount = 1;
                Command.DrawArguments.StartVertexLocation = Meshlet.IndexOffset;
                Command.DrawArguments.StartInstanceLocation = SortedIndex;
                OutData.Commands.push_back(Command);

                FMeshletDrawData DrawData;
                DrawData.StartIndex = Meshlet.IndexOffset;
                DrawData.IndexCount = Meshlet.IndexCount;
                DrawData.RangeIndex = RangeIndex;
                DrawData.GroupIndex = GroupIndex;
                OutData.MeshletDrawData.push_back(DrawData);

                const DirectX::XMVECTOR LocalCenter = DirectX::XMLoadFloat3(&BoundsData.Center);
                const DirectX::XMVECTOR WorldCenter = DirectX::XMVector3TransformCoord(LocalCenter, World);
                DirectX::XMFLOAT3 Center{};
                DirectX::XMStoreFloat3(&Center, WorldCenter);
                const float Radius = BoundsData.Radius * ModelScale;
                OutData.Bounds.emplace_back(Center.x, Center.y, Center.z, Radius);

                const DirectX::XMVECTOR LocalAxis = DirectX::XMLoadFloat3(&BoundsData.ConeAxis);
                const DirectX::XMVECTOR WorldAxis = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(LocalAxis, NormalMatrix));
                DirectX::XMFLOAT3 Axis{};
                DirectX::XMStoreFloat3(&Axis, WorldAxis);
                OutData.ConeAxisCutoff.emplace_back(Axis.x, Axis.y, Axis.z, BoundsData.ConeCutoff);

                const DirectX::XMVECTOR LocalApex = DirectX::XMLoadFloat3(&BoundsData.ConeApex);
                const DirectX::XMVECTOR WorldApex = DirectX::XMVector3TransformCoord(LocalApex, World);
                DirectX::XMFLOAT3 Apex{};
                DirectX::XMStoreFloat3(&Apex, WorldApex);
                OutData.ConeApex.emplace_back(Apex.x, Apex.y, Apex.z, 0.0f);

                IndirectDrawRanges.back().Count += 1;
            }
        }
        else
        {
            FIndirectDrawCommand Command;
            Command.ConstantBufferAddress = ConstantBufferBase + SceneConstantBufferStride * SortedIndex;
            Command.DrawArguments.VertexCountPerInstance = Model.DrawIndexCount;
            Command.DrawArguments.InstanceCount = 1;
            Command.DrawArguments.StartVertexLocation = Model.DrawIndexStart;
            Command.DrawArguments.StartInstanceLocation = SortedIndex;
            OutData.Commands.push_back(Command);

            FMeshletDrawData DrawData;
            DrawData.StartIndex = Model.DrawIndexStart;
            DrawData.IndexCount = Model.DrawIndexCount;
            DrawData.RangeIndex = RangeIndex;
            DrawData.GroupIndex = GroupIndex;
            OutData.MeshletDrawData.push_back(DrawData);

            const DirectX::XMFLOAT3 Center{
                (Model.BoundsMin.x + Model.BoundsMax.x) * 0.5f,
                (Model.BoundsMin.y + Model.BoundsMax.y) * 0.5f,
                (Model.BoundsMin.z + Model.BoundsMax.z) * 0.5f
            };
            const float Radius = std::sqrt(
                (Model.BoundsMax.x - Center.x) * (Model.BoundsMax.x - Center.x) +
                (Model.BoundsMax.y - Center.y) * (Model.BoundsMax.y - Center.y) +
                (Model.BoundsMax.z - Center.z) * (Model.BoundsMax.z - Center.z));
            OutData.Bounds.emplace_back(Center.x, Center.y, Center.z, Radius);
            OutData.ConeAxisCutoff.emplace_back(0.0f, 0.0f, 1.0f, -1.0f);
            OutData.ConeApex.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
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
    if (!Device || Data.Commands.empty())
    {
        return false;
    }

    const uint64_t CommandBufferSize = sizeof(FIndirectDrawCommand) * Data.Commands.size();

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    UploadHeap.CreationNodeMask = 1;
    UploadHeap.VisibleNodeMask = 1;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC BufferDesc = {};
    BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufferDesc.Width = CommandBufferSize;
    BufferDesc.Height = 1;
    BufferDesc.DepthOrArraySize = 1;
    BufferDesc.MipLevels = 1;
    BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    BufferDesc.SampleDesc.Count = 1;
    BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    BufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC UploadDesc = BufferDesc;
    UploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_RESOURCE_DESC TemplateDesc = BufferDesc;
    TemplateDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_RANGE EmptyRange = { 0, 0 };

    IndirectCommandBuffers.clear();
    IndirectCommandTemplateBuffers.clear();
    IndirectCommandStates.clear();
    MeshletVisibilityBuffers.clear();
    MeshletRunCountBuffers.clear();
    MeshletVisibilityStates.clear();
    MeshletRunCountStates.clear();
    IndirectCommandBuffers.resize(GetFramesInFlight());
    IndirectCommandTemplateBuffers.resize(GetFramesInFlight());
    IndirectCommandStates.resize(GetFramesInFlight(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    MeshletVisibilityBuffers.resize(GetFramesInFlight());
    MeshletRunCountBuffers.resize(GetFramesInFlight());
    MeshletVisibilityStates.resize(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    MeshletRunCountStates.resize(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    IndirectCommandUavBindlessIndices.clear();
    IndirectCommandTemplateBindlessIndices.clear();
    MeshletVisibilitySrvBindlessIndices.clear();
    MeshletVisibilityUavBindlessIndices.clear();
    MeshletRunCountUavBindlessIndices.clear();
    IndirectCommandUavBindlessIndices.resize(GetFramesInFlight(), UINT32_MAX);
    IndirectCommandTemplateBindlessIndices.resize(GetFramesInFlight(), UINT32_MAX);
    MeshletVisibilitySrvBindlessIndices.resize(GetFramesInFlight(), UINT32_MAX);
    MeshletVisibilityUavBindlessIndices.resize(GetFramesInFlight(), UINT32_MAX);
    MeshletRunCountUavBindlessIndices.resize(GetFramesInFlight(), UINT32_MAX);

    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &BufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(IndirectCommandBuffers[FrameIndex].GetAddressOf())));
        if (IndirectCommandBuffers[FrameIndex])
        {
            const std::wstring Name = L"IndirectCommandBuffer_Frame" + std::to_wstring(FrameIndex);
            IndirectCommandBuffers[FrameIndex]->SetName(Name.c_str());
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC IndirectUavDesc = {};
        IndirectUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        IndirectUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        IndirectUavDesc.Buffer.FirstElement = 0;
        IndirectUavDesc.Buffer.NumElements = static_cast<UINT>(CommandBufferSize / 4);
        IndirectUavDesc.Buffer.StructureByteStride = 0;
        IndirectUavDesc.Buffer.CounterOffsetInBytes = 0;
        IndirectUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        IndirectCommandUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(IndirectCommandBuffers[FrameIndex].Get(), nullptr, IndirectUavDesc);

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &TemplateDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(IndirectCommandTemplateBuffers[FrameIndex].GetAddressOf())));
        if (IndirectCommandTemplateBuffers[FrameIndex])
        {
            const std::wstring Name = L"IndirectCommandTemplateBuffer_Frame" + std::to_wstring(FrameIndex);
            IndirectCommandTemplateBuffers[FrameIndex]->SetName(Name.c_str());
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC TemplateSrvDesc = {};
        TemplateSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        TemplateSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        TemplateSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        TemplateSrvDesc.Buffer.FirstElement = 0;
        TemplateSrvDesc.Buffer.NumElements = static_cast<UINT>(CommandBufferSize / 4);
        TemplateSrvDesc.Buffer.StructureByteStride = 0;
        TemplateSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        IndirectCommandTemplateBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(IndirectCommandTemplateBuffers[FrameIndex].Get(), TemplateSrvDesc);

        D3D12_RESOURCE_DESC VisibilityDesc = BufferDesc;
        VisibilityDesc.Width = sizeof(uint32_t) * IndirectCommandCount;
        VisibilityDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &VisibilityDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(MeshletVisibilityBuffers[FrameIndex].GetAddressOf())));
        if (MeshletVisibilityBuffers[FrameIndex])
        {
            const std::wstring Name = L"MeshletVisibilityBuffer_Frame" + std::to_wstring(FrameIndex);
            MeshletVisibilityBuffers[FrameIndex]->SetName(Name.c_str());
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC VisibilitySrvDesc = {};
        VisibilitySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        VisibilitySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        VisibilitySrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        VisibilitySrvDesc.Buffer.FirstElement = 0;
        VisibilitySrvDesc.Buffer.NumElements = IndirectCommandCount;
        VisibilitySrvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
        VisibilitySrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        MeshletVisibilitySrvBindlessIndices[FrameIndex] = Device->CreateBindlessSrv(MeshletVisibilityBuffers[FrameIndex].Get(), VisibilitySrvDesc);

        D3D12_UNORDERED_ACCESS_VIEW_DESC VisibilityUavDesc = {};
        VisibilityUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        VisibilityUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        VisibilityUavDesc.Buffer.FirstElement = 0;
        VisibilityUavDesc.Buffer.NumElements = IndirectCommandCount;
        VisibilityUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
        VisibilityUavDesc.Buffer.CounterOffsetInBytes = 0;
        VisibilityUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        MeshletVisibilityUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(MeshletVisibilityBuffers[FrameIndex].Get(), nullptr, VisibilityUavDesc);

        D3D12_RESOURCE_DESC RunCountDesc = BufferDesc;
        RunCountDesc.Width = sizeof(uint32_t) * Data.RangeOffsets.size();
        RunCountDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &RunCountDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(MeshletRunCountBuffers[FrameIndex].GetAddressOf())));
        if (MeshletRunCountBuffers[FrameIndex])
        {
            const std::wstring Name = L"MeshletRunCountBuffer_Frame" + std::to_wstring(FrameIndex);
            MeshletRunCountBuffers[FrameIndex]->SetName(Name.c_str());
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC RunCountUavDesc = {};
        RunCountUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        RunCountUavDesc.Format = DXGI_FORMAT_UNKNOWN;
        RunCountUavDesc.Buffer.FirstElement = 0;
        RunCountUavDesc.Buffer.NumElements = static_cast<UINT>(Data.RangeOffsets.size());
        RunCountUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);
        RunCountUavDesc.Buffer.CounterOffsetInBytes = 0;
        RunCountUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        MeshletRunCountUavBindlessIndices[FrameIndex] = Device->CreateBindlessUav(MeshletRunCountBuffers[FrameIndex].Get(), nullptr, RunCountUavDesc);
    }

    return true;
}

bool FRenderer::CreateSharedGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data)
{
    if (!Device || Data.Bounds.empty())
    {
        return false;
    }

    const uint64_t BoundsBufferSize = sizeof(DirectX::XMFLOAT4) * Data.Bounds.size();
    const uint64_t MeshletDrawDataSize = sizeof(FMeshletDrawData) * Data.MeshletDrawData.size();
    const uint64_t RangeOffsetSize = sizeof(uint32_t) * Data.RangeOffsets.size();
    const uint64_t ConeBufferSize = sizeof(DirectX::XMFLOAT4) * Data.ConeAxisCutoff.size();

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    UploadHeap.CreationNodeMask = 1;
    UploadHeap.VisibleNodeMask = 1;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC BufferDesc = {};
    BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufferDesc.Width = BoundsBufferSize;
    BufferDesc.Height = 1;
    BufferDesc.DepthOrArraySize = 1;
    BufferDesc.MipLevels = 1;
    BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    BufferDesc.SampleDesc.Count = 1;
    BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    BufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_RANGE EmptyRange = { 0, 0 };

    // Bounds buffer
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &BufferDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(ModelBoundsBuffer.GetAddressOf())));
    if (ModelBoundsBuffer)
    {
        ModelBoundsBuffer->SetName(L"ModelBoundsBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &BufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(ModelBoundsUpload.GetAddressOf())));
    if (ModelBoundsUpload)
    {
        ModelBoundsUpload->SetName(L"ModelBoundsUpload");
        void* UploadData = nullptr;
        HR_CHECK(ModelBoundsUpload->Map(0, &EmptyRange, &UploadData));
        std::memcpy(UploadData, Data.Bounds.data(), BoundsBufferSize);
        ModelBoundsUpload->Unmap(0, nullptr);
    }

    // Meshlet draw data buffer
    D3D12_RESOURCE_DESC DrawDataDesc = BufferDesc;
    DrawDataDesc.Width = MeshletDrawDataSize;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &DrawDataDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletDrawDataBuffer.GetAddressOf())));
    if (MeshletDrawDataBuffer)
    {
        MeshletDrawDataBuffer->SetName(L"MeshletDrawDataBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &DrawDataDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletDrawDataUpload.GetAddressOf())));
    if (MeshletDrawDataUpload)
    {
        MeshletDrawDataUpload->SetName(L"MeshletDrawDataUpload");
        void* UploadData = nullptr;
        HR_CHECK(MeshletDrawDataUpload->Map(0, &EmptyRange, &UploadData));
        std::memcpy(UploadData, Data.MeshletDrawData.data(), MeshletDrawDataSize);
        MeshletDrawDataUpload->Unmap(0, nullptr);
    }

    // Range offset buffer
    D3D12_RESOURCE_DESC RangeOffsetDesc = BufferDesc;
    RangeOffsetDesc.Width = RangeOffsetSize;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &RangeOffsetDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletRangeOffsetBuffer.GetAddressOf())));
    if (MeshletRangeOffsetBuffer)
    {
        MeshletRangeOffsetBuffer->SetName(L"MeshletRangeOffsetBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &RangeOffsetDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletRangeOffsetUpload.GetAddressOf())));
    if (MeshletRangeOffsetUpload)
    {
        MeshletRangeOffsetUpload->SetName(L"MeshletRangeOffsetUpload");
        void* UploadData = nullptr;
        HR_CHECK(MeshletRangeOffsetUpload->Map(0, &EmptyRange, &UploadData));
        std::memcpy(UploadData, Data.RangeOffsets.data(), RangeOffsetSize);
        MeshletRangeOffsetUpload->Unmap(0, nullptr);
    }

    // Cone axis buffer
    D3D12_RESOURCE_DESC ConeDesc = BufferDesc;
    ConeDesc.Width = ConeBufferSize;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletConeAxisBuffer.GetAddressOf())));
    if (MeshletConeAxisBuffer)
    {
        MeshletConeAxisBuffer->SetName(L"MeshletConeAxisBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletConeAxisUpload.GetAddressOf())));
    if (MeshletConeAxisUpload)
    {
        MeshletConeAxisUpload->SetName(L"MeshletConeAxisUpload");
        void* UploadData = nullptr;
        HR_CHECK(MeshletConeAxisUpload->Map(0, &EmptyRange, &UploadData));
        std::memcpy(UploadData, Data.ConeAxisCutoff.data(), ConeBufferSize);
        MeshletConeAxisUpload->Unmap(0, nullptr);
    }

    // Cone apex buffer
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(MeshletConeApexBuffer.GetAddressOf())));
    if (MeshletConeApexBuffer)
    {
        MeshletConeApexBuffer->SetName(L"MeshletConeApexBuffer");
    }

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &ConeDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(MeshletConeApexUpload.GetAddressOf())));
    if (MeshletConeApexUpload)
    {
        MeshletConeApexUpload->SetName(L"MeshletConeApexUpload");
        void* UploadData = nullptr;
        HR_CHECK(MeshletConeApexUpload->Map(0, &EmptyRange, &UploadData));
        std::memcpy(UploadData, Data.ConeApex.data(), ConeBufferSize);
        MeshletConeApexUpload->Unmap(0, nullptr);
    }

    // GPU debug print buffers
    D3D12_RESOURCE_DESC DebugDesc = BufferDesc;
    DebugDesc.Width = GpuDebugPrintBufferSize;
    DebugDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &DebugDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintBuffer.GetAddressOf())));
    if (GpuDebugPrintBuffer)
    {
        GpuDebugPrintBuffer->SetName(L"GpuDebugPrintBuffer");
    }

    D3D12_RESOURCE_DESC DebugUploadDesc = BufferDesc;
    DebugUploadDesc.Width = sizeof(uint32_t);

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &DebugUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintUpload.GetAddressOf())));
    if (GpuDebugPrintUpload)
    {
        GpuDebugPrintUpload->SetName(L"GpuDebugPrintUpload");
        void* DebugUploadData = nullptr;
        HR_CHECK(GpuDebugPrintUpload->Map(0, &EmptyRange, &DebugUploadData));
        if (DebugUploadData)
        {
            std::memset(DebugUploadData, 0, sizeof(uint32_t));
        }
        GpuDebugPrintUpload->Unmap(0, nullptr);
    }

    D3D12_RESOURCE_DESC StatsDesc = BufferDesc;
    StatsDesc.Width = sizeof(uint32_t) * 3;
    StatsDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &StatsDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintStatsBuffer.GetAddressOf())));
    if (GpuDebugPrintStatsBuffer)
    {
        GpuDebugPrintStatsBuffer->SetName(L"GpuDebugPrintStatsBuffer");
    }

    D3D12_RESOURCE_DESC StatsUploadDesc = StatsDesc;
    StatsUploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &StatsUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(GpuDebugPrintStatsUpload.GetAddressOf())));
    if (GpuDebugPrintStatsUpload)
    {
        GpuDebugPrintStatsUpload->SetName(L"GpuDebugPrintStatsUpload");
        void* StatsUploadData = nullptr;
        HR_CHECK(GpuDebugPrintStatsUpload->Map(0, &EmptyRange, &StatsUploadData));
        if (StatsUploadData)
        {
            std::memset(StatsUploadData, 0, sizeof(uint32_t) * 3);
        }
        GpuDebugPrintStatsUpload->Unmap(0, nullptr);
    }

    if (!GpuDebugPrintBuffer || !GpuDebugPrintUpload || !GpuDebugPrintStatsBuffer || !GpuDebugPrintStatsUpload)
    {
        LogError("Failed to create GPU debug print resources");
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC BoundsSrvDesc = {};
    BoundsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    BoundsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    BoundsSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    BoundsSrvDesc.Buffer.FirstElement = 0;
    BoundsSrvDesc.Buffer.NumElements = static_cast<UINT>(Data.Bounds.size());
    BoundsSrvDesc.Buffer.StructureByteStride = sizeof(DirectX::XMFLOAT4);
    BoundsSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    ModelBoundsBindlessIndex = Device->CreateBindlessSrv(ModelBoundsBuffer.Get(), BoundsSrvDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC DrawDataSrvDesc = {};
    DrawDataSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    DrawDataSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    DrawDataSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    DrawDataSrvDesc.Buffer.FirstElement = 0;
    DrawDataSrvDesc.Buffer.NumElements = static_cast<UINT>(Data.MeshletDrawData.size());
    DrawDataSrvDesc.Buffer.StructureByteStride = sizeof(FMeshletDrawData);
    DrawDataSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    MeshletDrawDataBindlessIndex = Device->CreateBindlessSrv(MeshletDrawDataBuffer.Get(), DrawDataSrvDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC RangeOffsetSrvDesc = {};
    RangeOffsetSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    RangeOffsetSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    RangeOffsetSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    RangeOffsetSrvDesc.Buffer.FirstElement = 0;
    RangeOffsetSrvDesc.Buffer.NumElements = static_cast<UINT>(Data.RangeOffsets.size());
    RangeOffsetSrvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
    RangeOffsetSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    MeshletRangeOffsetBindlessIndex = Device->CreateBindlessSrv(MeshletRangeOffsetBuffer.Get(), RangeOffsetSrvDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC ConeAxisSrvDesc = {};
    ConeAxisSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    ConeAxisSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ConeAxisSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    ConeAxisSrvDesc.Buffer.FirstElement = 0;
    ConeAxisSrvDesc.Buffer.NumElements = static_cast<UINT>(Data.ConeAxisCutoff.size());
    ConeAxisSrvDesc.Buffer.StructureByteStride = sizeof(DirectX::XMFLOAT4);
    ConeAxisSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    MeshletConeAxisBindlessIndex = Device->CreateBindlessSrv(MeshletConeAxisBuffer.Get(), ConeAxisSrvDesc);

    D3D12_SHADER_RESOURCE_VIEW_DESC ConeApexSrvDesc = ConeAxisSrvDesc;
    ConeApexSrvDesc.Buffer.NumElements = static_cast<UINT>(Data.ConeApex.size());
    MeshletConeApexBindlessIndex = Device->CreateBindlessSrv(MeshletConeApexBuffer.Get(), ConeApexSrvDesc);

    return true;
}

bool FRenderer::UploadGpuDrivenBuffers(FDX12Device* Device, const FGpuDrivenPreparedData& Data)
{
    if (!Device || Data.Commands.empty())
    {
        return false;
    }

    const uint64_t CommandBufferSize = sizeof(FIndirectDrawCommand) * Data.Commands.size();
    const uint64_t BoundsBufferSize = sizeof(DirectX::XMFLOAT4) * Data.Bounds.size();
    const uint64_t MeshletDrawDataSize = sizeof(FMeshletDrawData) * Data.MeshletDrawData.size();
    const uint64_t RangeOffsetSize = sizeof(uint32_t) * Data.RangeOffsets.size();
    const uint64_t ConeBufferSize = sizeof(DirectX::XMFLOAT4) * Data.ConeAxisCutoff.size();

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    UploadHeap.CreationNodeMask = 1;
    UploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC UploadDesc = {};
    UploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    UploadDesc.Width = CommandBufferSize;
    UploadDesc.Height = 1;
    UploadDesc.DepthOrArraySize = 1;
    UploadDesc.MipLevels = 1;
    UploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    UploadDesc.SampleDesc.Count = 1;
    UploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    UploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_RANGE EmptyRange = { 0, 0 };

    // Create upload buffers for commands
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> IndirectCommandUploads;
    IndirectCommandUploads.resize(GetFramesInFlight());

    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &UploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &UploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(IndirectCommandUploads[FrameIndex].GetAddressOf())));

        if (IndirectCommandUploads[FrameIndex])
        {
            void* UploadData = nullptr;
            HR_CHECK(IndirectCommandUploads[FrameIndex]->Map(0, &EmptyRange, &UploadData));
            std::vector<FIndirectDrawCommand> FrameCommands = Data.Commands;
            const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = SceneConstantBuffers[FrameIndex]
                ? SceneConstantBuffers[FrameIndex]->GetGPUVirtualAddress()
                : 0;
            for (uint32_t CommandIndex = 0; CommandIndex < FrameCommands.size(); ++CommandIndex)
            {
                const uint32_t InstanceIndex = FrameCommands[CommandIndex].DrawArguments.StartInstanceLocation;
                FrameCommands[CommandIndex].ConstantBufferAddress =
                    ConstantBufferBase + SceneConstantBufferStride * InstanceIndex;
            }
            std::memcpy(UploadData, FrameCommands.data(), CommandBufferSize);
            IndirectCommandUploads[FrameIndex]->Unmap(0, nullptr);
        }
    }

    // Create command list for upload
    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

    // Pre-copy barriers
    std::vector<D3D12_RESOURCE_BARRIER> PreCopyBarriers;
    PreCopyBarriers.reserve(GetFramesInFlight() * 2 + GpuDrivenSharedBufferCount);

    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = IndirectCommandTemplateBuffers[FrameIndex].Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        PreCopyBarriers.push_back(Barrier);
    }

    D3D12_RESOURCE_BARRIER DrawDataBarrier = {};
    DrawDataBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    DrawDataBarrier.Transition.pResource = MeshletDrawDataBuffer.Get();
    DrawDataBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    DrawDataBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    DrawDataBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(DrawDataBarrier);

    D3D12_RESOURCE_BARRIER RangeOffsetBarrier = {};
    RangeOffsetBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    RangeOffsetBarrier.Transition.pResource = MeshletRangeOffsetBuffer.Get();
    RangeOffsetBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    RangeOffsetBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    RangeOffsetBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(RangeOffsetBarrier);

    D3D12_RESOURCE_BARRIER BoundsBarrier = {};
    BoundsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    BoundsBarrier.Transition.pResource = ModelBoundsBuffer.Get();
    BoundsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    BoundsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    BoundsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(BoundsBarrier);

    D3D12_RESOURCE_BARRIER ConeAxisBarrier = {};
    ConeAxisBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ConeAxisBarrier.Transition.pResource = MeshletConeAxisBuffer.Get();
    ConeAxisBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ConeAxisBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    ConeAxisBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(ConeAxisBarrier);

    D3D12_RESOURCE_BARRIER ConeApexBarrier = {};
    ConeApexBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ConeApexBarrier.Transition.pResource = MeshletConeApexBuffer.Get();
    ConeApexBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ConeApexBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    ConeApexBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(ConeApexBarrier);

    D3D12_RESOURCE_BARRIER DebugBarrier = {};
    DebugBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    DebugBarrier.Transition.pResource = GpuDebugPrintBuffer.Get();
    DebugBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    DebugBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    DebugBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(DebugBarrier);

    D3D12_RESOURCE_BARRIER StatsBarrier = {};
    StatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    StatsBarrier.Transition.pResource = GpuDebugPrintStatsBuffer.Get();
    StatsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    StatsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    StatsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    PreCopyBarriers.push_back(StatsBarrier);

    UploadList->ResourceBarrier(static_cast<UINT>(PreCopyBarriers.size()), PreCopyBarriers.data());

    // Copy operations
    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        UploadList->CopyBufferRegion(
            IndirectCommandTemplateBuffers[FrameIndex].Get(),
            0,
            IndirectCommandUploads[FrameIndex].Get(),
            0,
            CommandBufferSize);
    }
    UploadList->CopyBufferRegion(ModelBoundsBuffer.Get(), 0, ModelBoundsUpload.Get(), 0, BoundsBufferSize);
    UploadList->CopyBufferRegion(MeshletDrawDataBuffer.Get(), 0, MeshletDrawDataUpload.Get(), 0, MeshletDrawDataSize);
    UploadList->CopyBufferRegion(MeshletRangeOffsetBuffer.Get(), 0, MeshletRangeOffsetUpload.Get(), 0, RangeOffsetSize);
    UploadList->CopyBufferRegion(MeshletConeAxisBuffer.Get(), 0, MeshletConeAxisUpload.Get(), 0, ConeBufferSize);
    UploadList->CopyBufferRegion(MeshletConeApexBuffer.Get(), 0, MeshletConeApexUpload.Get(), 0, ConeBufferSize);
    if (GpuDebugPrintBuffer && GpuDebugPrintUpload)
    {
        UploadList->CopyBufferRegion(GpuDebugPrintBuffer.Get(), 0, GpuDebugPrintUpload.Get(), 0, sizeof(uint32_t));
    }
    if (GpuDebugPrintStatsBuffer && GpuDebugPrintStatsUpload)
    {
        UploadList->CopyBufferRegion(GpuDebugPrintStatsBuffer.Get(), 0, GpuDebugPrintStatsUpload.Get(), 0, sizeof(uint32_t) * 3);
    }

    // Post-copy barriers
    std::vector<D3D12_RESOURCE_BARRIER> PostCopyBarriers;
    PostCopyBarriers.reserve(GetFramesInFlight() * 2 + GpuDrivenSharedBufferCount);

    for (uint32_t FrameIndex = 0; FrameIndex < GetFramesInFlight(); ++FrameIndex)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = IndirectCommandTemplateBuffers[FrameIndex].Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        PostCopyBarriers.push_back(Barrier);
    }

    D3D12_RESOURCE_BARRIER PostBoundsBarrier = {};
    PostBoundsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostBoundsBarrier.Transition.pResource = ModelBoundsBuffer.Get();
    PostBoundsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostBoundsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostBoundsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostBoundsBarrier);

    D3D12_RESOURCE_BARRIER PostDrawDataBarrier = {};
    PostDrawDataBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostDrawDataBarrier.Transition.pResource = MeshletDrawDataBuffer.Get();
    PostDrawDataBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostDrawDataBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostDrawDataBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostDrawDataBarrier);

    D3D12_RESOURCE_BARRIER PostRangeOffsetBarrier = {};
    PostRangeOffsetBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostRangeOffsetBarrier.Transition.pResource = MeshletRangeOffsetBuffer.Get();
    PostRangeOffsetBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostRangeOffsetBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostRangeOffsetBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostRangeOffsetBarrier);

    D3D12_RESOURCE_BARRIER PostConeAxisBarrier = {};
    PostConeAxisBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostConeAxisBarrier.Transition.pResource = MeshletConeAxisBuffer.Get();
    PostConeAxisBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostConeAxisBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostConeAxisBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostConeAxisBarrier);

    D3D12_RESOURCE_BARRIER PostConeApexBarrier = {};
    PostConeApexBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostConeApexBarrier.Transition.pResource = MeshletConeApexBuffer.Get();
    PostConeApexBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostConeApexBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostConeApexBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    PostCopyBarriers.push_back(PostConeApexBarrier);

    D3D12_RESOURCE_BARRIER PostDebugBarrier = {};
    PostDebugBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostDebugBarrier.Transition.pResource = GpuDebugPrintBuffer.Get();
    PostDebugBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostDebugBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostDebugBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PostCopyBarriers.push_back(PostDebugBarrier);

    D3D12_RESOURCE_BARRIER PostStatsBarrier = {};
    PostStatsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    PostStatsBarrier.Transition.pResource = GpuDebugPrintStatsBuffer.Get();
    PostStatsBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    PostStatsBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    PostStatsBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    PostCopyBarriers.push_back(PostStatsBarrier);

    UploadList->ResourceBarrier(static_cast<UINT>(PostCopyBarriers.size()), PostCopyBarriers.data());

    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    IndirectCommandStates.assign(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    MeshletVisibilityStates.assign(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    MeshletRunCountStates.assign(GetFramesInFlight(), D3D12_RESOURCE_STATE_COMMON);
    GpuDebugPrintState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    GpuDebugPrintStatsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    return true;
}

bool FRenderer::CreateCullingPipelines(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER1 RootParams[2] = {};
    // RootParams[0]: CullingConstants CBV (b0), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RootParams[1]: Culling bindless indices (b1), used in Shaders/CullMeshletVisibility.hlsl CSMain
    RootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[1].Constants.ShaderRegister = 1;
    RootParams[1].Constants.RegisterSpace = 0;
    RootParams[1].Constants.Num32BitValues = 7;
    RootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 RootDesc = {};
    RootDesc.NumParameters = _countof(RootParams);
    RootDesc.pParameters = RootParams;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc = {};
    VersionedRootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    VersionedRootDesc.Desc_1_1 = RootDesc;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(CullingRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);
    if (!Compiler.CompileFromFile(L"Shaders/CullMeshletVisibility.hlsl", L"CSMain", CSTarget, CsByteCode))
    {
        LogError("Failed to compile culling compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC CsDesc = {};
    CsDesc.pRootSignature = CullingRootSignature.Get();
    CsDesc.CS = { CsByteCode.data(), CsByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&CsDesc, IID_PPV_ARGS(CullingPipeline.GetAddressOf())));

    // Meshlet run root signature and pipelines
    D3D12_ROOT_PARAMETER1 RunRootParams[2] = {};
    // RunRootParams[0]: CullingConstants CBV (b0), used in Shaders/ClearMeshletRunCounts.hlsl CSMain and Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RunRootParams[0].Descriptor.ShaderRegister = 0;
    RunRootParams[0].Descriptor.RegisterSpace = 0;
    RunRootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    RunRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // RunRootParams[1]: Meshlet run bindless indices (b1), used in Shaders/ClearMeshletRunCounts.hlsl CSMain and Shaders/BuildMeshletRunsAppend.hlsl CSMain
    RunRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RunRootParams[1].Constants.ShaderRegister = 1;
    RunRootParams[1].Constants.RegisterSpace = 0;
    RunRootParams[1].Constants.Num32BitValues = 6;
    RunRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 RunRootDesc = {};
    RunRootDesc.NumParameters = _countof(RunRootParams);
    RunRootDesc.pParameters = RunRootParams;
    RunRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RunVersionedDesc = {};
    RunVersionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RunVersionedDesc.Desc_1_1 = RunRootDesc;

    ComPtr<ID3DBlob> RunSerializedSig;
    ComPtr<ID3DBlob> RunErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RunVersionedDesc, RunSerializedSig.GetAddressOf(), RunErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, RunSerializedSig->GetBufferPointer(), RunSerializedSig->GetBufferSize(), IID_PPV_ARGS(MeshletRunRootSignature.GetAddressOf())));

    std::vector<uint8_t> ClearByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/ClearMeshletRunCounts.hlsl", L"CSMain", CSTarget, ClearByteCode))
    {
        LogError("Failed to compile meshlet run clear compute shader");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC ClearDesc = {};
    ClearDesc.pRootSignature = MeshletRunRootSignature.Get();
    ClearDesc.CS = { ClearByteCode.data(), ClearByteCode.size() };
    HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&ClearDesc, IID_PPV_ARGS(MeshletRunClearPipeline.GetAddressOf())));

    std::vector<uint8_t> RunByteCode;
    if (!Compiler.CompileFromFile(L"Shaders/BuildMeshletRunsAppend.hlsl", L"CSMain", CSTarget, RunByteCode))
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

bool FRenderer::CreateSkinningPipeline(FDX12Device* Device)
{
    if (!Device)
    {
        return false;
    }

    D3D12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    RootParams[0].Constants.ShaderRegister = 0;
    RootParams[0].Constants.RegisterSpace = 0;
    RootParams[0].Constants.Num32BitValues = 6;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 RootDesc = {};
    RootDesc.NumParameters = _countof(RootParams);
    RootDesc.pParameters = RootParams;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC VersionedRootDesc = {};
    VersionedRootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    VersionedRootDesc.Desc_1_1 = RootDesc;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&VersionedRootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(SkinningRootSignature.GetAddressOf())));

    FShaderCompiler Compiler;
    std::vector<uint8_t> CsByteCode;
    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring CSTarget = RendererUtils::BuildShaderTarget(L"cs", ShaderModel);
    if (!Compiler.CompileFromFile(L"Shaders/SkinningCS.hlsl", L"CSMain", CSTarget, CsByteCode))
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
    if (!Device || !RootSignature)
    {
        return false;
    }

    D3D12_INDIRECT_ARGUMENT_DESC IndirectArgs[2] = {};
    IndirectArgs[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
    IndirectArgs[0].ConstantBufferView.RootParameterIndex = 0;
    IndirectArgs[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC CommandDesc = {};
    CommandDesc.pArgumentDescs = IndirectArgs;
    CommandDesc.NumArgumentDescs = _countof(IndirectArgs);
    CommandDesc.ByteStride = sizeof(FIndirectDrawCommand);
    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&CommandDesc, RootSignature, IID_PPV_ARGS(IndirectCommandSignature.GetAddressOf())));

    return true;
}
