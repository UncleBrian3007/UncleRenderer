#include "Renderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "TextureLoader.h"
#include "../Scene/Camera.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
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

bool FRenderer::CreateShadowPipeline(FDX12Device* Device, ID3D12RootSignature* RootSignature, Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState)
{
    if (!Device || !RootSignature)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = RendererUtils::BuildShaderTarget(L"vs", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/ShadowMap.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature;
    PsoDesc.InputLayout = { InputLayout, _countof(InputLayout) };
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
        InOutWidth = 2048;
        InOutHeight = 2048;
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
