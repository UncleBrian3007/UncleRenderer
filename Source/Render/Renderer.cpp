#include "Renderer.h"

#include "ShaderCompiler.h"
#include "RendererUtils.h"
#include "TextureLoader.h"
#include "DebugPrintFont.h"
#include "../Scene/Camera.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include "../Core/GpuDebugMarkers.h"
#include "../Core/Logger.h"
#include <array>
#include <algorithm>
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
    bEnableGpuDebugPrint = Options.bEnableGpuDebugPrint;
    FramesInFlight = (std::max)(1u, Options.FramesInFlight);
    CurrentFrameIndex = 0;

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
        FMappedConstantBuffer ConstantBufferResource = {};
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
    ID3D12Resource* IndirectBuffer = GetIndirectCommandBuffer();
    if (!CullingPipeline || !CullingRootSignature || !IndirectBuffer || !ModelBoundsBuffer || IndirectCommandCount == 0)
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

    std::array<uint32_t, 46> Constants = {};
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

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent CullingEvent(CommandList, L"GpuCulling");

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

    CommandList->SetPipelineState(CullingPipeline.Get());
    CommandList->SetComputeRootSignature(CullingRootSignature.Get());
    CommandList->SetComputeRoot32BitConstants(0, static_cast<UINT>(Constants.size()), Constants.data(), 0);
    CommandList->SetComputeRootShaderResourceView(1, ModelBoundsBuffer->GetGPUVirtualAddress());
    CommandList->SetComputeRootUnorderedAccessView(2, IndirectBuffer->GetGPUVirtualAddress());
    CommandList->SetComputeRootUnorderedAccessView(3, GpuDebugPrintBuffer->GetGPUVirtualAddress());
    CommandList->SetComputeRootUnorderedAccessView(4, GpuDebugPrintStatsBuffer->GetGPUVirtualAddress());
    if (CullingDescriptorHeap)
    {
        ID3D12DescriptorHeap* Heaps[] = { CullingDescriptorHeap.Get() };
        CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
        CommandList->SetComputeRootDescriptorTable(5, HZBCullingHandle);
    }

    const uint32_t DispatchCount = (IndirectCommandCount + 63) / 64;
    CommandList->Dispatch(DispatchCount, 1, 1);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = IndirectBuffer;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barrier.Transition.StateBefore = IndirectState;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    CommandList->ResourceBarrier(1, &Barrier);
    IndirectState = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
}

void FRenderer::PrepareGpuDebugPrint(FDX12CommandContext& CmdContext)
{
    if (!bEnableGpuDebugPrint || !GpuDebugPrintBuffer || !GpuDebugPrintUpload || !GpuDebugPrintStatsBuffer || !GpuDebugPrintStatsUpload)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
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

    CommandList->CopyBufferRegion(GpuDebugPrintStatsBuffer.Get(), 0, GpuDebugPrintStatsUpload.Get(), 0, sizeof(uint32_t) * 2);

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

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    HeapDesc.NumDescriptors = 4;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(GpuDebugPrintDescriptorHeap.ReleaseAndGetAddressOf())));

    const UINT DescriptorSize = Device->GetDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle = GpuDebugPrintDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle = GpuDebugPrintDescriptorHeap->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC GlyphSrvDesc = {};
    GlyphSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    GlyphSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    GlyphSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    GlyphSrvDesc.Buffer.FirstElement = 0;
    GlyphSrvDesc.Buffer.NumElements = 128;
    GlyphSrvDesc.Buffer.StructureByteStride = sizeof(FDebugPrintGlyph);
    GlyphSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    Device->GetDevice()->CreateShaderResourceView(GpuDebugPrintGlyphBuffer.Get(), &GlyphSrvDesc, CpuHandle);
    GpuDebugPrintGlyphHandle = GpuHandle;

    CpuHandle.ptr += DescriptorSize;
    GpuHandle.ptr += DescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC FontSrvDesc = {};
    FontSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    FontSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    FontSrvDesc.Format = DXGI_FORMAT_R8_UNORM;
    FontSrvDesc.Texture2D.MostDetailedMip = 0;
    FontSrvDesc.Texture2D.MipLevels = 1;
    Device->GetDevice()->CreateShaderResourceView(GpuDebugPrintFontTexture.Get(), &FontSrvDesc, CpuHandle);
    GpuDebugPrintFontHandle = GpuHandle;

    CpuHandle.ptr += DescriptorSize;
    GpuHandle.ptr += DescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC BufferSrvDesc = {};
    BufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    BufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    BufferSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    BufferSrvDesc.Buffer.FirstElement = 0;
    BufferSrvDesc.Buffer.NumElements = static_cast<UINT>(GpuDebugPrintBufferSize / 4);
    BufferSrvDesc.Buffer.StructureByteStride = 0;
    BufferSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    Device->GetDevice()->CreateShaderResourceView(GpuDebugPrintBuffer.Get(), &BufferSrvDesc, CpuHandle);
    GpuDebugPrintBufferHandle = GpuHandle;

    CpuHandle.ptr += DescriptorSize;
    GpuHandle.ptr += DescriptorSize;

    D3D12_SHADER_RESOURCE_VIEW_DESC StatsSrvDesc = {};
    StatsSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    StatsSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    StatsSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    StatsSrvDesc.Buffer.FirstElement = 0;
    StatsSrvDesc.Buffer.NumElements = 2;
    StatsSrvDesc.Buffer.StructureByteStride = 0;
    StatsSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    Device->GetDevice()->CreateShaderResourceView(GpuDebugPrintStatsBuffer.Get(), &StatsSrvDesc, CpuHandle);
    GpuDebugPrintStatsHandle = GpuHandle;

    return true;
}

bool FRenderer::CreateGpuDebugPrintPipeline(FDX12Device* Device, DXGI_FORMAT BackBufferFormat)
{
    if (!Device)
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE Range = {};
    Range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors = 4;
    Range.BaseShaderRegister = 0;
    Range.RegisterSpace = 0;
    Range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER Params[2] = {};
    // Params[0]: Debug print draw constants (screen size, font range)
    Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    Params[0].Constants.ShaderRegister = 0;
    Params[0].Constants.RegisterSpace = 0;
    Params[0].Constants.Num32BitValues = 4;
    Params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Params[1]: Debug print SRV table (glyph buffer, font atlas, print buffer, stats buffer)
    Params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[1].DescriptorTable.NumDescriptorRanges = 1;
    Params[1].DescriptorTable.pDescriptorRanges = &Range;
    Params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC Sampler = {};
    Sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    Sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    Sampler.ShaderRegister = 0;
    Sampler.RegisterSpace = 0;
    Sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC RootDesc = {};
    RootDesc.NumParameters = _countof(Params);
    RootDesc.pParameters = Params;
    RootDesc.NumStaticSamplers = 1;
    RootDesc.pStaticSamplers = &Sampler;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
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

    D3D12_DESCRIPTOR_RANGE Range = {};
    Range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    Range.NumDescriptors = 1;
    Range.BaseShaderRegister = 0;
    Range.RegisterSpace = 0;
    Range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER Params[2] = {};
    // Params[0]: Stats buffer SRV table (t0)
    Params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    Params[0].DescriptorTable.NumDescriptorRanges = 1;
    Params[0].DescriptorTable.pDescriptorRanges = &Range;
    Params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Params[1]: Debug print buffer UAV (u0)
    Params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    Params[1].Descriptor.ShaderRegister = 0;
    Params[1].Descriptor.RegisterSpace = 0;
    Params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC RootDesc = {};
    RootDesc.NumParameters = _countof(Params);
    RootDesc.pParameters = Params;
    RootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> SerializedSig;
    ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeRootSignature(&RootDesc, D3D_ROOT_SIGNATURE_VERSION_1, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));
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
    if (!bEnableGpuDebugPrint || !GpuDebugPrintStatsPipeline || !GpuDebugPrintStatsRootSignature || !GpuDebugPrintDescriptorHeap)
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

    ID3D12DescriptorHeap* Heaps[] = { GpuDebugPrintDescriptorHeap.Get() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(GpuDebugPrintStatsPipeline.Get());
    CommandList->SetComputeRootSignature(GpuDebugPrintStatsRootSignature.Get());
    CommandList->SetComputeRootDescriptorTable(0, GpuDebugPrintStatsHandle);
    CommandList->SetComputeRootUnorderedAccessView(1, GpuDebugPrintBuffer->GetGPUVirtualAddress());
    CommandList->Dispatch(1, 1, 1);
}

void FRenderer::RenderGpuDebugPrint(FDX12CommandContext& CmdContext, const D3D12_CPU_DESCRIPTOR_HANDLE& OutputHandle)
{
    if (!bEnableGpuDebugPrint || !GpuDebugPrintPipeline || !GpuDebugPrintRootSignature || !GpuDebugPrintDescriptorHeap)
    {
        return;
    }

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();
    FScopedPixEvent DebugEvent(CommandList, L"GpuDebugPrint");

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

    const FDebugPrintConstants Constants =
    {
        DirectX::XMFLOAT2(Viewport.Width, Viewport.Height),
        GpuDebugPrintFirstChar,
        GpuDebugPrintCharCount
    };

    ID3D12DescriptorHeap* Heaps[] = { GpuDebugPrintDescriptorHeap.Get() };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(GpuDebugPrintPipeline.Get());
    CommandList->SetGraphicsRootSignature(GpuDebugPrintRootSignature.Get());
    CommandList->RSSetViewports(1, &Viewport);
    CommandList->RSSetScissorRects(1, &ScissorRect);
    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / sizeof(uint32_t), &Constants, 0);
    CommandList->SetGraphicsRootDescriptorTable(1, GpuDebugPrintGlyphHandle);
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
