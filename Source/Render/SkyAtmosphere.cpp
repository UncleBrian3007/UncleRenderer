#include "SkyAtmosphere.h"

#include "Deferred/DeferredPassContext.h"
#include "Deferred/Taa.h"
#include "DeferredRenderer.h"
#include "RendererUtils.h"
#include "ShaderCompiler.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12Device.h"
#include "../Scene/Camera.h"

#include <vector>
#include <cstring>
#include <algorithm>

bool FSkyAtmosphere::Initialize(FDX12Device* Device, float InSphereRadius, DXGI_FORMAT RenderTargetFormat, const FSkyPipelineConfig& Config)
{
    SphereRadius = InSphereRadius;

    if (!SkyAtmosphere::CreateResources(Device, SphereRadius, Geometry, ConstantBuffer, ConstantBufferMapped))
    {
        return false;
    }

    ConstantBuffer->SetName(L"SkyConstantBuffer");

    return SkyAtmosphere::CreatePipeline(Device, RenderTargetFormat, Config, RootSignature, PipelineState);
}

bool FSkyAtmosphere::IsReady() const
{
    return PipelineState && RootSignature && Geometry.IndexCount > 0 && ConstantBuffer;
}

void FSkyAtmosphere::AddPass(FDeferredPassContext& Context)
{
    FDeferredRenderer& Owner = Context.Owner;
    FRenderGraph& Graph = Context.Graph;
    const FCamera& Camera = Context.Camera;
    const FRGResourceHandle DepthHandle = Context.Resources.DepthHandle;
    const FRGResourceHandle LightingHandle = Context.Resources.LightingHandle;

    struct FSkyPassData
    {
        bool bEnabled = false;
        FSkyAtmosphereFrameParameters Parameters;
    };

    Graph.AddPass<FSkyPassData>("Sky", [this, &Owner, &Camera, DepthHandle, LightingHandle](FSkyPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = IsReady();
        Data.Parameters.Camera = &Camera;
        Data.Parameters.LightDirection = Owner.GetLightDirection();
        Data.Parameters.LightColor = Owner.GetLightColor();
        Data.Parameters.Viewport = Owner.Viewport;
        Data.Parameters.ScissorRect = Owner.ScissorRect;
        Data.Parameters.Projection = Owner.Taa->UsesJitter() ? Owner.Taa->GetProjection() : Camera.GetProjectionMatrix();

        if (Data.bEnabled)
        {
            Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_DEPTH_READ);
            Builder.WriteTexture(LightingHandle, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }, [this, &Owner](const FSkyPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        Draw(Cmd, Data.Parameters, Owner.LightingRTVHandle, Owner.GetDSVHandle());
    });
}

void FSkyAtmosphere::Draw(
    FDX12CommandContext& Cmd,
    const FSkyAtmosphereFrameParameters& Parameters,
    const D3D12_CPU_DESCRIPTOR_HANDLE& RenderTargetHandle,
    const D3D12_CPU_DESCRIPTOR_HANDLE& DepthStencilHandle,
    bool bClearDepth)
{
    if (!IsReady() || Parameters.Camera == nullptr)
    {
        return;
    }

    if (bClearDepth)
    {
        Cmd.ClearDepth(DepthStencilHandle);
    }

    ID3D12GraphicsCommandList* CommandList = Cmd.GetCommandList();
    CommandList->OMSetRenderTargets(1, &RenderTargetHandle, FALSE, &DepthStencilHandle);
    CommandList->SetPipelineState(PipelineState.Get());
    CommandList->SetGraphicsRootSignature(RootSignature.Get());
    CommandList->RSSetViewports(1, &Parameters.Viewport);
    CommandList->RSSetScissorRects(1, &Parameters.ScissorRect);
    CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    CommandList->IASetVertexBuffers(0, Geometry.VertexBufferCount, Geometry.VertexBufferViews.data());
    CommandList->IASetIndexBuffer(&Geometry.IndexBufferView);

    UpdateConstants(Parameters);
    CommandList->SetGraphicsRootConstantBufferView(0, ConstantBuffer->GetGPUVirtualAddress());
    CommandList->DrawIndexedInstanced(Geometry.IndexCount, 1, 0, 0, 0);
}

void FSkyAtmosphere::UpdateConstants(const FSkyAtmosphereFrameParameters& Parameters)
{
    if (Parameters.Camera == nullptr)
    {
        return;
    }

    using namespace DirectX;

    const FFloat3 CameraPosition = Parameters.Camera->GetPosition();
    const XMMATRIX Scale = XMMatrixScaling(SphereRadius, SphereRadius, SphereRadius);
    const XMMATRIX Translation = XMMatrixTranslation(CameraPosition.x, CameraPosition.y, CameraPosition.z);
    const XMMATRIX World = Scale * Translation;
    const XMVECTOR LightDir = XMLoadFloat3(&Parameters.LightDirection);

    SkyAtmosphere::UpdateConstants(
        *Parameters.Camera,
        World,
        Parameters.Projection,
        LightDir,
        Parameters.LightColor,
        ConstantBufferMapped);
}

bool SkyAtmosphere::CreateResources(
    FDX12Device* Device,
    float SphereRadius,
    FMeshGeometryBuffers& OutGeometry,
    Microsoft::WRL::ComPtr<ID3D12Resource>& OutConstantBuffer,
    uint8_t*& OutConstantBufferMapped)
{
    FMappedUploadBuffer SkyConstantBuffer;
    if (!CreateMappedConstantBuffer(Device, L"SkyAtmosphereConstantBuffer", sizeof(FSkyAtmosphereConstants), SkyConstantBuffer))
    {
        return false;
    }

    OutConstantBuffer = SkyConstantBuffer.Resource;
    OutConstantBufferMapped = SkyConstantBuffer.MappedData;

    return RendererUtils::CreateSphereGeometry(Device, OutGeometry, SphereRadius, 64, 32);
}

bool SkyAtmosphere::CreatePipeline(
    FDX12Device* Device,
    DXGI_FORMAT BackBufferFormat,
    const FSkyPipelineConfig& Config,
    Microsoft::WRL::ComPtr<ID3D12RootSignature>& OutRootSignature,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState)
{
    if (Device == nullptr)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;


    if (!RendererUtils::CompileVertexShader(Compiler, Device, L"Shaders/SkyAtmosphere.hlsl", VSByteCode))
    {
        return false;
    }

    if (!RendererUtils::CompilePixelShader(Compiler, Device, L"Shaders/SkyAtmosphere.hlsl", PSByteCode))
    {
        return false;
    }

    CD3DX12_ROOT_PARAMETER1 RootParams[1] = {};
    RootParams[0].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC RootDesc;
    RootDesc.Init_1_1(_countof(RootParams), RootParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    Microsoft::WRL::ComPtr<ID3DBlob> SerializedSig;
    Microsoft::WRL::ComPtr<ID3DBlob> ErrorBlob;
    HR_CHECK(D3D12SerializeVersionedRootSignature(&RootDesc, SerializedSig.GetAddressOf(), ErrorBlob.GetAddressOf()));

    if (ErrorBlob && ErrorBlob->GetBufferSize() > 0)
    {
        OutputDebugStringA(static_cast<const char*>(ErrorBlob->GetBufferPointer()));
    }

    HR_CHECK(Device->GetDevice()->CreateRootSignature(0, SerializedSig->GetBufferPointer(), SerializedSig->GetBufferSize(), IID_PPV_ARGS(OutRootSignature.GetAddressOf())));

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = OutRootSignature.Get();
    PsoDesc.InputLayout = { InputLayout, _countof(InputLayout) };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
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
    PsoDesc.BlendState.RenderTarget[0] = RtBlend;

    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = Config.DepthEnable;
    PsoDesc.DepthStencilState.DepthWriteMask = Config.DepthWriteMask;
    PsoDesc.DepthStencilState.DepthFunc = Config.DepthFunc;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = BackBufferFormat;
    PsoDesc.DSVFormat = Config.DsvFormat;
    PsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(OutPipelineState.GetAddressOf())));

    return true;
}

void SkyAtmosphere::UpdateConstants(
    const FCamera& Camera,
    const DirectX::XMMATRIX& WorldMatrix,
    const DirectX::XMMATRIX& Projection,
    const DirectX::XMVECTOR& LightDirection,
    const DirectX::XMFLOAT3& LightColor,
    uint8_t* ConstantBufferMapped)
{
    if (ConstantBufferMapped == nullptr)
    {
        return;
    }

    using namespace DirectX;

    const XMMATRIX View = Camera.GetViewMatrix();
    FSkyAtmosphereConstants Constants = {};
    XMStoreFloat4x4(&Constants.World, WorldMatrix);
    XMStoreFloat4x4(&Constants.View, View);
    XMStoreFloat4x4(&Constants.Projection, Projection);
    Constants.CameraPosition = Camera.GetPosition();
    XMStoreFloat3(&Constants.LightDirection, XMVector3Normalize(LightDirection));
    Constants.LightColor = LightColor;

    memcpy(ConstantBufferMapped, &Constants, sizeof(Constants));
}
