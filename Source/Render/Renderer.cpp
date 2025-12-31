#include "Renderer.h"

#include "RendererUtils.h"
#include "TextureLoader.h"
#include "../Scene/Camera.h"
#include "../RHI/DX12CommandContext.h"
#include "../Core/GpuDebugMarkers.h"
#include <array>
#include <cstring>

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

void FRenderer::InitializeCommonSettings(uint32_t Width, uint32_t Height, const FRendererOptions& Options)
{
    bDepthPrepassEnabled = Options.bUseDepthPrepass;
    bShadowsEnabled = Options.bEnableShadows;
    ShadowBias = Options.ShadowBias;
    bLogResourceBarriers = Options.bLogResourceBarriers;
    bEnableGraphDump = Options.bEnableGraphDump;
    bEnableGpuTiming = Options.bEnableGpuTiming;
    bEnableIndirectDraw = Options.bEnableIndirectDraw;

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

void FRenderer::ConfigureHZBOcclusion(bool bEnabled, ID3D12DescriptorHeap* DescriptorHeap, D3D12_GPU_DESCRIPTOR_HANDLE Handle, uint32_t Width, uint32_t Height, uint32_t MipCount)
{
    bHZBOcclusionEnabled = bEnabled;
    CullingDescriptorHeap = DescriptorHeap;
    HZBCullingHandle = Handle;
    HZBCullingWidth = Width;
    HZBCullingHeight = Height;
    HZBCullingMipCount = MipCount;
}

void FRenderer::DispatchGpuCulling(FDX12CommandContext& CmdContext, const FCamera& Camera)
{
    if (!CullingPipeline || !CullingRootSignature || !IndirectCommandBuffer || !ModelBoundsBuffer || IndirectCommandCount == 0)
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

    std::array<uint32_t, 45> Constants = {};
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

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent CullingEvent(CommandList, L"GpuCulling");

    if (IndirectCommandState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER Barrier = {};
        Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        Barrier.Transition.pResource = IndirectCommandBuffer.Get();
        Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        Barrier.Transition.StateBefore = IndirectCommandState;
        Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        CommandList->ResourceBarrier(1, &Barrier);
        IndirectCommandState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    CommandList->SetPipelineState(CullingPipeline.Get());
    CommandList->SetComputeRootSignature(CullingRootSignature.Get());
    CommandList->SetComputeRoot32BitConstants(0, static_cast<UINT>(Constants.size()), Constants.data(), 0);
    CommandList->SetComputeRootShaderResourceView(1, ModelBoundsBuffer->GetGPUVirtualAddress());
    CommandList->SetComputeRootUnorderedAccessView(2, IndirectCommandBuffer->GetGPUVirtualAddress());
    if (CullingDescriptorHeap)
    {
        ID3D12DescriptorHeap* Heaps[] = { CullingDescriptorHeap.Get() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootDescriptorTable(3, HZBCullingHandle);
    }

    const uint32_t DispatchCount = (IndirectCommandCount + 63) / 64;
    CommandList->Dispatch(DispatchCount, 1, 1);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = IndirectCommandBuffer.Get();
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barrier.Transition.StateBefore = IndirectCommandState;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    CommandList->ResourceBarrier(1, &Barrier);
    IndirectCommandState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
}
