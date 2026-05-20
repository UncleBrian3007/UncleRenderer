#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RendererUtils.h"
#include "GpuResource.h"

#include "../Scene/Mesh.h"
#include "../Scene/Camera.h"
#include "../Core/Logger.h"
#include "ShaderCompiler.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12Commons.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12CommandQueue.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <array>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <limits>
#include <utility>
#include <d3dx12.h>

using Microsoft::WRL::ComPtr;

namespace
{
    struct FUploadBatch
    {
        FDX12CommandContext Context;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> UploadBuffers;
        bool bInitialized = false;

        void Begin(FDX12Device* Device)
        {
            Context.Initialize(Device, Device->GetGraphicsQueue(), 1);
            Context.BeginFrame(0);
            bInitialized = true;
        }

        void AddUploadBuffer(Microsoft::WRL::ComPtr<ID3D12Resource>&& Buffer)
        {
            UploadBuffers.push_back(std::move(Buffer));
        }

        void ExecuteAndFlush()
        {
            Context.CloseAndExecute();
            Context.GetQueue()->Flush();
            UploadBuffers.clear();
        }
    };

    bool CreatePrimitiveGeometry(
        FDX12Device* Device,
        const FMesh::FPrimitive& Primitive,
        FMeshGeometryBuffers& OutGeometry,
        bool bCreateIndexBuffer = true,
        FUploadBatch* UploadBatch = nullptr)
    {
        if (Device == nullptr)
        {
            return false;
        }

        if (Primitive.VertexStreams.Positions.empty() || Primitive.Indices.empty())
        {
            return false;
        }

        OutGeometry.IndexCount = static_cast<uint32_t>(Primitive.Indices.size());

        const std::vector<FFloat3>* Positions = &Primitive.VertexStreams.Positions;
        const size_t VertexCount = Positions->size();
        if (VertexCount == 0)
        {
            return false;
        }

        std::vector<FFloat3> DefaultNormals;
        const std::vector<FFloat3>* Normals = &Primitive.VertexStreams.Normals;
        if (Normals->size() != VertexCount)
        {
            DefaultNormals.assign(VertexCount, FFloat3(0.0f, 0.0f, 1.0f));
            Normals = &DefaultNormals;
        }

        std::vector<FFloat2> DefaultUVs;
        const std::vector<FFloat2>* UVs = &Primitive.VertexStreams.UVs;
        if (UVs->size() != VertexCount)
        {
            DefaultUVs.assign(VertexCount, FFloat2(0.0f, 0.0f));
            UVs = &DefaultUVs;
        }

        std::vector<FFloat4> DefaultTangents;
        const std::vector<FFloat4>* Tangents = &Primitive.VertexStreams.Tangents;
        if (Tangents->size() != VertexCount)
        {
            DefaultTangents.assign(VertexCount, FFloat4(0.0f, 0.0f, 0.0f, 1.0f));
            Tangents = &DefaultTangents;
        }

        std::vector<FFloat4> DefaultColors;
        const std::vector<FFloat4>* Colors = &Primitive.VertexStreams.Colors;
        if (Colors->size() != VertexCount)
        {
            DefaultColors.assign(VertexCount, FFloat4(1.0f, 1.0f, 1.0f, 1.0f));
            Colors = &DefaultColors;
        }

        std::vector<FUInt4> DefaultJoints;
        const std::vector<FUInt4>* Joints = &Primitive.VertexStreams.Joints;
        if (Joints->size() != VertexCount)
        {
            DefaultJoints.assign(VertexCount, FUInt4{});
            Joints = &DefaultJoints;
        }

        std::vector<FFloat4> DefaultWeights;
        const std::vector<FFloat4>* Weights = &Primitive.VertexStreams.Weights;
        if (Weights->size() != VertexCount)
        {
            DefaultWeights.assign(VertexCount, FFloat4(0.0f, 0.0f, 0.0f, 0.0f));
            Weights = &DefaultWeights;
        }

        const std::array<const void*, kMeshVertexStreamCount> StreamData =
        {
            Positions->data(),
            Normals->data(),
            UVs->data(),
            Tangents->data(),
            Colors->data(),
            Joints->data(),
            Weights->data()
        };

        const std::array<UINT, kMeshVertexStreamCount> StreamStrides =
        {
            static_cast<UINT>(sizeof(FFloat3)),
            static_cast<UINT>(sizeof(FFloat3)),
            static_cast<UINT>(sizeof(FFloat2)),
            static_cast<UINT>(sizeof(FFloat4)),
            static_cast<UINT>(sizeof(FFloat4)),
            static_cast<UINT>(sizeof(FUInt4)),
            static_cast<UINT>(sizeof(FFloat4))
        };

        const std::array<UINT, kMeshVertexStreamCount> StreamSizes =
        {
            static_cast<UINT>(VertexCount * sizeof(FFloat3)),
            static_cast<UINT>(VertexCount * sizeof(FFloat3)),
            static_cast<UINT>(VertexCount * sizeof(FFloat2)),
            static_cast<UINT>(VertexCount * sizeof(FFloat4)),
            static_cast<UINT>(VertexCount * sizeof(FFloat4)),
            static_cast<UINT>(VertexCount * sizeof(FUInt4)),
            static_cast<UINT>(VertexCount * sizeof(FFloat4))
        };

        const UINT IndexBufferSize = static_cast<UINT>(Primitive.Indices.size() * sizeof(uint32_t));

        OutGeometry.VertexBufferCount = static_cast<uint32_t>(StreamData.size());
        OutGeometry.VertexBufferViews.fill({});

        std::array<FUploadBuffer, kMeshVertexStreamCount> UploadBuffers;

        for (size_t StreamIndex = 0; StreamIndex < StreamData.size(); ++StreamIndex)
        {
            CreateBindlessBufferWithUpload(
                Device,
                L"PrimitiveVertexBuffer_" + std::to_wstring(StreamIndex),
                CreateStructuredBufferDesc(VertexCount, StreamStrides[StreamIndex]),
                D3D12_RESOURCE_STATE_COPY_DEST,
                OutGeometry.VertexBuffers[StreamIndex],
                UploadBuffers[StreamIndex],
                StreamData[StreamIndex],
                false, false);

            OutGeometry.VertexBufferViews[StreamIndex].BufferLocation = OutGeometry.VertexBuffers[StreamIndex]->GetGPUVirtualAddress();
            OutGeometry.VertexBufferViews[StreamIndex].StrideInBytes = StreamStrides[StreamIndex];
            OutGeometry.VertexBufferViews[StreamIndex].SizeInBytes = StreamSizes[StreamIndex];
        }

        FUploadBuffer IndexUploadBuffer;
        if (bCreateIndexBuffer)
        {
            CreateBindlessBufferWithUpload(
                Device,
                L"PrimitiveIndexBuffer",
                CreateStructuredBufferDesc<uint32_t>(Primitive.Indices.size()),
                D3D12_RESOURCE_STATE_COPY_DEST,
                OutGeometry.IndexBuffer,
                IndexUploadBuffer,
                Primitive.Indices.data(),
                false, false);

            OutGeometry.IndexBufferView.BufferLocation = OutGeometry.IndexBuffer->GetGPUVirtualAddress();
            OutGeometry.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
            OutGeometry.IndexBufferView.SizeInBytes = IndexBufferSize;
        }

        if (UploadBatch && !UploadBatch->bInitialized)
        {
            UploadBatch->Begin(Device);
        }

        FUploadBatch LocalBatch;
        FUploadBatch* ActiveBatch = UploadBatch;
        if (!ActiveBatch)
        {
            LocalBatch.Begin(Device);
            ActiveBatch = &LocalBatch;
        }

        ID3D12GraphicsCommandList* CommandList = ActiveBatch->Context.GetCommandList();
        for (size_t StreamIndex = 0; StreamIndex < StreamData.size(); ++StreamIndex)
        {
            CommandList->CopyBufferRegion(
                OutGeometry.VertexBuffers[StreamIndex].Get(),
                0,
                UploadBuffers[StreamIndex].Get(),
                0,
                StreamSizes[StreamIndex]);
            ActiveBatch->Context.TransitionResource(
                OutGeometry.VertexBuffers[StreamIndex].Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_GENERIC_READ);
            ActiveBatch->AddUploadBuffer(std::move(UploadBuffers[StreamIndex].Resource));
        }

        if (bCreateIndexBuffer && IndexUploadBuffer)
        {
            CommandList->CopyBufferRegion(OutGeometry.IndexBuffer.Get(), 0, IndexUploadBuffer.Get(), 0, IndexBufferSize);
            ActiveBatch->Context.TransitionResource(OutGeometry.IndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
            ActiveBatch->AddUploadBuffer(std::move(IndexUploadBuffer.Resource));
        }

        if (!UploadBatch)
        {
            ActiveBatch->ExecuteAndFlush();
        }

        return true;
    }

}

std::wstring RendererUtils::BuildShaderTarget(const wchar_t* StagePrefix, D3D_SHADER_MODEL ShaderModel)
{
    uint32_t ShaderModelValue = static_cast<uint32_t>(ShaderModel);
    uint32_t Major = (ShaderModelValue >> 4) & 0xF;
    uint32_t Minor = ShaderModelValue & 0xF;

    return std::wstring(StagePrefix) + L"_" + std::to_wstring(Major) + L"_" + std::to_wstring(Minor);
}

std::wstring RendererUtils::BuildShaderEntryPoint(const std::wstring& ShaderPath, const wchar_t* StageSuffix)
{
    std::wstring EntryPoint = std::filesystem::path(ShaderPath).stem().wstring();
    if (EntryPoint.size() >= 2)
    {
        const std::wstring ExistingSuffix = EntryPoint.substr(EntryPoint.size() - 2);
        if (ExistingSuffix == L"VS" || ExistingSuffix == L"PS" || ExistingSuffix == L"CS")
        {
            EntryPoint.resize(EntryPoint.size() - 2);
        }
    }

    EntryPoint += StageSuffix;
    return EntryPoint;
}

std::wstring RendererUtils::BuildComputeShaderEntryPoint(const std::wstring& ShaderPath)
{
    return BuildShaderEntryPoint(ShaderPath, L"CS");
}

std::wstring RendererUtils::BuildVertexShaderEntryPoint(const std::wstring& ShaderPath)
{
    return BuildShaderEntryPoint(ShaderPath, L"VS");
}

std::wstring RendererUtils::BuildPixelShaderEntryPoint(const std::wstring& ShaderPath)
{
    return BuildShaderEntryPoint(ShaderPath, L"PS");
}

bool RendererUtils::CompileVertexShader(
    FShaderCompiler& Compiler,
    FDX12Device* Device,
    const std::wstring& ShaderPath,
    std::vector<uint8_t>& OutByteCode,
    const std::vector<std::wstring>& Defines)
{
    const std::wstring VSTarget = BuildShaderTarget(L"vs", Device->GetShaderModel());
    return Compiler.CompileFromFile(
        ShaderPath,
        BuildVertexShaderEntryPoint(ShaderPath),
        VSTarget,
        OutByteCode,
        Defines);
}

bool RendererUtils::CompilePixelShader(
    FShaderCompiler& Compiler,
    FDX12Device* Device,
    const std::wstring& ShaderPath,
    std::vector<uint8_t>& OutByteCode,
    const std::vector<std::wstring>& Defines)
{
    const std::wstring PSTarget = BuildShaderTarget(L"ps", Device->GetShaderModel());
    return Compiler.CompileFromFile(
        ShaderPath,
        BuildPixelShaderEntryPoint(ShaderPath),
        PSTarget,
        OutByteCode,
        Defines);
}

bool RendererUtils::CompileComputeShader(
    FShaderCompiler& Compiler,
    FDX12Device* Device,
    const std::wstring& ShaderPath,
    std::vector<uint8_t>& OutByteCode,
    const std::vector<std::wstring>& Defines)
{
    const std::wstring CSTarget = BuildShaderTarget(L"cs", Device->GetShaderModel());
    return Compiler.CompileFromFile(
        ShaderPath,
        BuildComputeShaderEntryPoint(ShaderPath),
        CSTarget,
        OutByteCode,
        Defines);
}

bool RendererUtils::CompileComputeShader(
    FShaderCompiler& Compiler,
    FDX12Device* Device,
    const std::wstring& ShaderPath,
    const std::wstring& EntryPoint,
    std::vector<uint8_t>& OutByteCode,
    const std::vector<std::wstring>& Defines)
{
    const std::wstring CSTarget = BuildShaderTarget(L"cs", Device->GetShaderModel());
    return Compiler.CompileFromFile(ShaderPath, EntryPoint, CSTarget, OutByteCode, Defines);
}

bool RendererUtils::CompileComputeShader(
    FShaderCompiler& Compiler,
    const std::wstring& Target,
    const std::wstring& ShaderPath,
    std::vector<uint8_t>& OutByteCode,
    const std::vector<std::wstring>& Defines)
{
    return Compiler.CompileFromFile(
        ShaderPath,
        BuildComputeShaderEntryPoint(ShaderPath),
        Target,
        OutByteCode,
        Defines);
}

std::string RendererUtils::ResourceStateToString(D3D12_RESOURCE_STATES State)
{
    if (State == D3D12_RESOURCE_STATE_COMMON)
    {
        return "COMMON";
    }

    struct FStateName
    {
        D3D12_RESOURCE_STATES State;
        const char* Name;
    };

    static constexpr FStateName StateNames[] =
    {
        { D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, "VERTEX|CONSTANT" },
        { D3D12_RESOURCE_STATE_INDEX_BUFFER, "INDEX" },
        { D3D12_RESOURCE_STATE_RENDER_TARGET, "RENDER_TARGET" },
        { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "UNORDERED_ACCESS" },
        { D3D12_RESOURCE_STATE_DEPTH_WRITE, "DEPTH_WRITE" },
        { D3D12_RESOURCE_STATE_DEPTH_READ, "DEPTH_READ" },
        { D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "NON_PIXEL_SHADER_RESOURCE" },
        { D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "PIXEL_SHADER_RESOURCE" },
        { D3D12_RESOURCE_STATE_STREAM_OUT, "STREAM_OUT" },
        { D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, "INDIRECT_ARGUMENT" },
        { D3D12_RESOURCE_STATE_COPY_DEST, "COPY_DEST" },
        { D3D12_RESOURCE_STATE_COPY_SOURCE, "COPY_SOURCE" },
        { D3D12_RESOURCE_STATE_RESOLVE_DEST, "RESOLVE_DEST" },
        { D3D12_RESOURCE_STATE_RESOLVE_SOURCE, "RESOLVE_SOURCE" },
        { D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "RAYTRACING_ACCELERATION_STRUCTURE" },
        { D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, "SHADING_RATE_SOURCE" },
    };

    std::ostringstream Stream;
    bool bFirst = true;
    D3D12_RESOURCE_STATES Remaining = State;

    for (const FStateName& Entry : StateNames)
    {
        if ((State & Entry.State) != 0)
        {
            if (!bFirst)
            {
                Stream << " | ";
            }

            Stream << Entry.Name;
            bFirst = false;
            Remaining &= ~Entry.State;
        }
    }

    if (Remaining != 0 || bFirst)
    {
        if (!bFirst)
        {
            Stream << " | ";
        }

        Stream << "0x" << std::hex << static_cast<uint32_t>(Remaining);
    }

    return Stream.str();
}

bool RendererUtils::CreateMeshGeometry(FDX12Device* Device, const FMesh& Mesh, FMeshGeometryBuffers& OutGeometry)
{
    const std::vector<FMesh::FPrimitive>& Primitives = Mesh.GetPrimitives();
    if (Primitives.size() != 1)
    {
        return false;
    }

    return CreatePrimitiveGeometry(Device, Primitives.front(), OutGeometry);
}

bool RendererUtils::CreateCubeGeometry(FDX12Device* Device, FCubeGeometryBuffers& OutGeometry, float Size)
{
    const FMesh Cube = FMesh::CreateCube(Size);
    return CreateMeshGeometry(Device, Cube, OutGeometry);
}

bool RendererUtils::CreateSphereGeometry(FDX12Device* Device, FMeshGeometryBuffers& OutGeometry, float Radius, uint32_t SliceCount, uint32_t StackCount)
{
    const FMesh Sphere = FMesh::CreateSphere(Radius, SliceCount, StackCount);
    return CreateMeshGeometry(Device, Sphere, OutGeometry);
}

void RendererUtils::UpdateCullingVisibility(
    const FCamera& Camera,
    std::vector<FSceneModelResource>& Models,
    std::vector<bool>& OutVisibility,
    bool bAllowMeshletCulling)
{
    OutVisibility.assign(Models.size(), true);
    DirectX::XMVECTOR Planes[6] = {};
    RendererUtils::BuildCameraFrustumPlanes(Camera, Planes);
    for (size_t ModelIndex = 0; ModelIndex < Models.size(); ++ModelIndex)
    {
        FSceneModelResource& Model = Models[ModelIndex];
        const bool bModelVisible = RendererUtils::IsAabbInCameraFrustum(Planes, Model.BoundsMin, Model.BoundsMax);
        OutVisibility[ModelIndex] = bModelVisible;
    }
}

bool RendererUtils::UpdateGltfSceneAnimation(
    std::vector<FSceneModelResource>& Models,
    std::vector<FGltfScene>& Scenes,
    float DeltaTime)
{
    for (FSceneModelResource& Model : Models)
    {
        Model.bSkinningUpdatedThisFrame = false;
    }

    if (Scenes.empty() || Models.empty())
    {
        return false;
    }

    const bool bHasSkinning = std::any_of(Models.begin(), Models.end(), [](const FSceneModelResource& Model)
    {
        return Model.GltfSkinIndex >= 0 && Model.BoneMatrixBufferMapped != nullptr;
    });
    if (!bHasSkinning)
    {
        return false;
    }

    const bool bAnimationTimeAdvanced = std::abs(DeltaTime) > 1e-6f;
    for (FGltfScene& Scene : Scenes)
    {
        if (Scene.Pose.LocalMatrices.empty())
        {
            InitializeGltfAnimationPose(Scene, Scene.Pose);
        }
        Scene.AnimationTime += DeltaTime;
        UpdateGltfAnimationPose(Scene, Scene.AnimationTime, Scene.Pose);
    }

    bool bAnySkinningUpdated = false;
    for (FSceneModelResource& Model : Models)
    {
        if (Model.GltfSceneIndex < 0 || Model.GltfNodeIndex < 0)
        {
            continue;
        }

        const size_t SceneIndex = static_cast<size_t>(Model.GltfSceneIndex);
        if (SceneIndex >= Scenes.size())
        {
            continue;
        }

        const FGltfScene& Scene = Scenes[SceneIndex];
        const std::vector<DirectX::XMFLOAT4X4>& WorldMatrices = Scene.Pose.WorldMatrices;
        const size_t NodeIndex = static_cast<size_t>(Model.GltfNodeIndex);
        if (NodeIndex >= WorldMatrices.size())
        {
            continue;
        }

        using namespace DirectX;
        const XMMATRIX NodeWorld = XMLoadFloat4x4(&WorldMatrices[NodeIndex]);

        if (Model.GltfSkinIndex >= 0 && Model.BoneMatrixBufferMapped)
        {
            const size_t SkinIndex = static_cast<size_t>(Model.GltfSkinIndex);
            if (SkinIndex < Scene.Skins.size()
                && SkinIndex < Scene.Pose.SkinMatrices.size())
            {
                const FGltfSkin& Skin = Scene.Skins[SkinIndex];
                const std::vector<DirectX::XMFLOAT4X4>& SkinMatrices = Scene.Pose.SkinMatrices[SkinIndex];
                const size_t MatrixCount = std::min(SkinMatrices.size(), static_cast<size_t>(Model.BoneMatrixCount));

                const XMMATRIX NodeWorldInv = XMMatrixInverse(nullptr, NodeWorld);

                std::vector<DirectX::XMFLOAT4X4> FinalMatrices(MatrixCount);
                for (size_t JointIndex = 0; JointIndex < MatrixCount; ++JointIndex)
                {
                    const XMMATRIX SkinMatrix = XMLoadFloat4x4(&SkinMatrices[JointIndex]);
					const XMMATRIX FinalMatrix = XMMatrixMultiply(SkinMatrix, NodeWorldInv); // To Node Local Space
                    XMStoreFloat4x4(&FinalMatrices[JointIndex], FinalMatrix);
                }

                const size_t CopyBytes = MatrixCount * sizeof(DirectX::XMFLOAT4X4);
                std::memcpy(Model.BoneMatrixBufferMapped, FinalMatrices.data(), CopyBytes);

                if (bAnimationTimeAdvanced && !Scene.Animations.empty() && MatrixCount > 0)
                {
                    Model.bSkinningUpdatedThisFrame = true;
                    bAnySkinningUpdated = true;
                }
            }
        }
    }

    return bAnySkinningUpdated;
}

void RendererUtils::UpdateSceneConstants(const FUpdateSceneConstantsParams& Params)
{
    if (Params.ConstantBufferMapped == nullptr)
    {
        return;
    }

    using namespace DirectX;

    const FCamera& Camera = *Params.Camera;
    const FSceneModelResource& Model = *Params.Model;

    const XMMATRIX View = Camera.GetViewMatrix();
    const XMMATRIX ViewInverse = XMMatrixInverse(nullptr, View);
    const XMMATRIX WorldMatrix = XMLoadFloat4x4(&Model.WorldMatrix);

    const bool bHasEmissiveTexture = !Model.EmissiveTexturePath.empty();
    const XMFLOAT3 BaseColorFactor = Model.BaseColorFactor;
    const XMFLOAT3 EmissiveFactor = Model.EmissiveFactor;

    FSceneConstants Constants = {};
    XMStoreFloat4x4(&Constants.World, WorldMatrix);
    XMStoreFloat4x4(&Constants.WorldInverseTranspose, XMMatrixTranspose(XMMatrixInverse(nullptr, WorldMatrix)));
    XMStoreFloat4x4(&Constants.View, View);
    XMStoreFloat4x4(&Constants.ViewInverse, ViewInverse);
    XMStoreFloat4x4(&Constants.Projection, Params.Projection);
    const XMMATRIX ViewProjection = View * Params.Projection;
    XMStoreFloat4x4(&Constants.ViewProjectionInverse, XMMatrixInverse(nullptr, ViewProjection));
    XMStoreFloat4x4(&Constants.PreviousWorld, Params.PreviousWorld);
    Constants.HasPreviousWorld = Params.bHasPreviousWorld ? 1u : 0u;
    Constants.HasPreviousSkinning = Params.bHasPreviousSkinning ? 1u : 0u;
    Constants.PreviousSkinnedPositionBindlessIndex = Params.PreviousSkinnedPositionBindlessIndex;
    Constants.BaseColor = BaseColorFactor;
    Constants.LightIntensity = Params.LightIntensity;
    XMStoreFloat3(&Constants.LightDirection, XMVector3Normalize(Params.LightDirection));
    Constants.LightRadius = 0.02f;
    Constants.CameraPosition = Camera.GetPosition();
    Constants.LightColor = Params.LightColor;
    Constants.EmissiveFactor = EmissiveFactor;
    XMStoreFloat4x4(&Constants.LightViewProjection, Params.LightViewProjection);
    Constants.ShadowStrength = Params.ShadowStrength;
    Constants.ShadowBias = Params.ShadowBias;
    Constants.ShadowMapSize = DirectX::XMFLOAT2(Params.ShadowMapWidth, Params.ShadowMapHeight);
    Constants.MetallicFactor = Model.MetallicFactor;
    Constants.RoughnessFactor = Model.RoughnessFactor;
    Constants.BaseColorAlpha = Model.BaseColorAlpha;
    Constants.AlphaCutoff = Model.AlphaCutoff;
    Constants.AlphaMode = Model.AlphaMode;
    Constants.SheenColorFactor = Model.SheenColorFactor;
    Constants.SheenRoughnessFactor = Model.SheenRoughnessFactor;
    Constants.ShadingModelId = Model.ShadingModelId;
    Constants.ClearcoatFactor = Model.ClearcoatFactor;
    Constants.ClearcoatRoughnessFactor = Model.ClearcoatRoughnessFactor;
    Constants.AnisotropyStrength = Model.AnisotropyStrength;
    Constants.AnisotropyRotation = Model.AnisotropyRotation;
    Constants.EnvMapMipCount = Params.EnvMapMipCount;
    const bool bUseClusterDagVertexBuffers =
        Params.bUseClusterDagIndexBuffer
        && AreAllBindlessIndicesValid(
            Model.ClusterDagVertexBuffers[0].SrvBindlessIndex,
            Model.ClusterDagVertexBuffers[1].SrvBindlessIndex,
            Model.ClusterDagIndexBuffer.SrvBindlessIndex);
    Constants.VertexBufferBindlessIndices = DirectX::XMUINT4(
        bUseClusterDagVertexBuffers ? Model.ClusterDagVertexBuffers[0].SrvBindlessIndex : Model.Geometry.VertexBuffers[0].SrvBindlessIndex,
        bUseClusterDagVertexBuffers ? Model.ClusterDagVertexBuffers[1].SrvBindlessIndex : Model.Geometry.VertexBuffers[1].SrvBindlessIndex,
        bUseClusterDagVertexBuffers ? Model.ClusterDagVertexBuffers[2].SrvBindlessIndex : Model.Geometry.VertexBuffers[2].SrvBindlessIndex,
        bUseClusterDagVertexBuffers ? Model.ClusterDagVertexBuffers[3].SrvBindlessIndex : Model.Geometry.VertexBuffers[3].SrvBindlessIndex);
    Constants.ExtraBindlessIndices = DirectX::XMUINT4(
        bUseClusterDagVertexBuffers
            ? Model.ClusterDagColorBuffer.SrvBindlessIndex
            : Model.Geometry.VertexBuffers[4].SrvBindlessIndex,
        bUseClusterDagVertexBuffers
            ? Model.ClusterDagIndexBuffer.SrvBindlessIndex
            : Model.Geometry.IndexBuffer.SrvBindlessIndex,
        Params.bUseClusterDagDebugColor && IsValidBindlessIndex(Model.ClusterDagDebugColorBuffer.SrvBindlessIndex)
            ? Model.ClusterDagDebugColorBuffer.SrvBindlessIndex
            : UINT32_MAX,
        0u);
    Constants.SkinningBindlessIndices = DirectX::XMUINT4(
        Model.Geometry.VertexBuffers[5].SrvBindlessIndex,
        Model.Geometry.VertexBuffers[6].SrvBindlessIndex,
        Model.BoneMatrixBuffer.SrvBindlessIndex,
        Model.SkinnedPositionBuffer.SrvBindlessIndex);
    Constants.ClusterDagPackedPositionOffset = Model.ClusterDagPackedPositionOffset;
    Constants.ClusterDagPackedPositionScale = Model.ClusterDagPackedPositionScale;
    Constants.ClusterDagPackedConstantUV = Model.ClusterDagPackedConstantUV;
    Constants.ClusterDagPackedConstantColor = Model.ClusterDagPackedConstantColor;
    Constants.ClusterDagVertexPackingMode =
        bUseClusterDagVertexBuffers
        ? Model.ClusterDagVertexPackingMode
        : 0u;
    Constants.MaterialTextureIndices0 = DirectX::XMUINT4(
        Model.BaseColor.SrvBindlessIndex,
        Model.MetallicRoughness.SrvBindlessIndex,
        Model.Normal.SrvBindlessIndex,
        Model.Emissive.SrvBindlessIndex);
    Constants.MaterialTextureIndices1 = DirectX::XMUINT4(
        Model.SheenColor.SrvBindlessIndex,
        Model.SheenRoughness.SrvBindlessIndex,
        Model.Clearcoat.SrvBindlessIndex,
        Model.ClearcoatRoughness.SrvBindlessIndex);
    Constants.MaterialTextureIndices2 = DirectX::XMUINT4(
        Model.ClearcoatNormal.SrvBindlessIndex,
        Model.Anisotropy.SrvBindlessIndex,
        UINT32_MAX,
        UINT32_MAX);
    Constants.ClusterDagMaterialPipelineKey = Model.PipelineKey;
    Constants.GtaoIntensity = Params.bGtaoEnabled ? Params.GtaoIntensity : 0.0f;

    Constants.DeferredLightingVisualizationMode = Params.DeferredLightingVisualizationMode;
    Constants.BaseColorTransformOffsetScale = Model.BaseColorTransform.OffsetScale;
    Constants.BaseColorTransformRotation = Model.BaseColorTransform.Rotation;
    Constants.MetallicRoughnessTransformOffsetScale = Model.MetallicRoughnessTransform.OffsetScale;
    Constants.MetallicRoughnessTransformRotation = Model.MetallicRoughnessTransform.Rotation;
    Constants.NormalTransformOffsetScale = Model.NormalTransform.OffsetScale;
    Constants.NormalTransformRotation = Model.NormalTransform.Rotation;
    Constants.EmissiveTransformOffsetScale = Model.EmissiveTransform.OffsetScale;
    Constants.EmissiveTransformRotation = Model.EmissiveTransform.Rotation;
    Constants.SheenColorTransformOffsetScale = Model.SheenColorTransform.OffsetScale;
    Constants.SheenColorTransformRotation = Model.SheenColorTransform.Rotation;
    Constants.SheenRoughnessTransformOffsetScale = Model.SheenRoughnessTransform.OffsetScale;
    Constants.SheenRoughnessTransformRotation = Model.SheenRoughnessTransform.Rotation;
    Constants.ClearcoatTransformOffsetScale = Model.ClearcoatTransform.OffsetScale;
    Constants.ClearcoatTransformRotation = Model.ClearcoatTransform.Rotation;
    Constants.ClearcoatRoughnessTransformOffsetScale = Model.ClearcoatRoughnessTransform.OffsetScale;
    Constants.ClearcoatRoughnessTransformRotation = Model.ClearcoatRoughnessTransform.Rotation;
    Constants.ClearcoatNormalTransformOffsetScale = Model.ClearcoatNormalTransform.OffsetScale;
    Constants.ClearcoatNormalTransformRotation = Model.ClearcoatNormalTransform.Rotation;
    Constants.AnisotropyTransformOffsetScale = Model.AnisotropyTransform.OffsetScale;
    Constants.AnisotropyTransformRotation = Model.AnisotropyTransform.Rotation;

    memcpy(Params.ConstantBufferMapped + Params.ConstantBufferOffset, &Constants, sizeof(Constants));
}

RendererUtils::FMaterialBindlessIndices RendererUtils::BuildMaterialBindlessIndices(const FSceneModelResource& Model)
{
    return
    {
        Model.BaseColor.SrvBindlessIndex,
        Model.MetallicRoughness.SrvBindlessIndex,
        Model.Normal.SrvBindlessIndex,
        Model.Emissive.SrvBindlessIndex,
        Model.SheenColor.SrvBindlessIndex,
        Model.SheenRoughness.SrvBindlessIndex,
        Model.Clearcoat.SrvBindlessIndex,
        Model.ClearcoatRoughness.SrvBindlessIndex,
        Model.ClearcoatNormal.SrvBindlessIndex,
        Model.Anisotropy.SrvBindlessIndex
    };
}

DirectX::XMMATRIX RendererUtils::BuildDirectionalLightViewProjection(
    const DirectX::XMFLOAT3& SceneCenter,
    float SceneRadius,
    const DirectX::XMFLOAT3& LightDirection)
{
    using namespace DirectX;

    const XMVECTOR Direction = XMVector3Normalize(XMLoadFloat3(&LightDirection));
    const XMVECTOR SceneCenterVec = XMLoadFloat3(&SceneCenter);
    const float LightDistance = SceneRadius * 2.5f;
    const XMVECTOR LightPosition = XMVectorAdd(SceneCenterVec, XMVectorScale(Direction, LightDistance));
    const XMVECTOR Up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    const XMMATRIX View = XMMatrixLookAtLH(LightPosition, SceneCenterVec, Up);
    const float OrthoSize = SceneRadius * 2.0f;
    const float NearZ = 0.1f;
    const float FarZ = SceneRadius * 5.0f;
    const XMMATRIX Projection = XMMatrixOrthographicLH(OrthoSize, OrthoSize, NearZ, FarZ);

    return XMMatrixMultiply(View, Projection);
}

void RendererUtils::BuildCameraFrustumPlanes(
    const FCamera& Camera,
    DirectX::XMVECTOR OutPlanes[6])
{
    using namespace DirectX;

    const XMMATRIX View = Camera.GetViewMatrix();
    const XMMATRIX Projection = Camera.GetProjectionMatrix();
    const XMMATRIX ViewProjection = XMMatrixMultiply(View, Projection);
    BuildFrustumPlanesFromMatrix(ViewProjection, OutPlanes);
}

void RendererUtils::BuildFrustumPlanesFromMatrix(
    const DirectX::XMMATRIX& ViewProjection,
    DirectX::XMVECTOR OutPlanes[6])
{
    using namespace DirectX;

    XMFLOAT4X4 ViewProjectionMatrix;
    XMStoreFloat4x4(&ViewProjectionMatrix, ViewProjection);

    OutPlanes[0] = XMPlaneNormalize(XMVectorSet(
        ViewProjectionMatrix._14 + ViewProjectionMatrix._11,
        ViewProjectionMatrix._24 + ViewProjectionMatrix._21,
        ViewProjectionMatrix._34 + ViewProjectionMatrix._31,
        ViewProjectionMatrix._44 + ViewProjectionMatrix._41));
    OutPlanes[1] = XMPlaneNormalize(XMVectorSet(
        ViewProjectionMatrix._14 - ViewProjectionMatrix._11,
        ViewProjectionMatrix._24 - ViewProjectionMatrix._21,
        ViewProjectionMatrix._34 - ViewProjectionMatrix._31,
        ViewProjectionMatrix._44 - ViewProjectionMatrix._41));
    OutPlanes[2] = XMPlaneNormalize(XMVectorSet(
        ViewProjectionMatrix._14 + ViewProjectionMatrix._12,
        ViewProjectionMatrix._24 + ViewProjectionMatrix._22,
        ViewProjectionMatrix._34 + ViewProjectionMatrix._32,
        ViewProjectionMatrix._44 + ViewProjectionMatrix._42));
    OutPlanes[3] = XMPlaneNormalize(XMVectorSet(
        ViewProjectionMatrix._14 - ViewProjectionMatrix._12,
        ViewProjectionMatrix._24 - ViewProjectionMatrix._22,
        ViewProjectionMatrix._34 - ViewProjectionMatrix._32,
        ViewProjectionMatrix._44 - ViewProjectionMatrix._42));
    OutPlanes[4] = XMPlaneNormalize(XMVectorSet(
        ViewProjectionMatrix._13,
        ViewProjectionMatrix._23,
        ViewProjectionMatrix._33,
        ViewProjectionMatrix._43));
    OutPlanes[5] = XMPlaneNormalize(XMVectorSet(
        ViewProjectionMatrix._14 - ViewProjectionMatrix._13,
        ViewProjectionMatrix._24 - ViewProjectionMatrix._23,
        ViewProjectionMatrix._34 - ViewProjectionMatrix._33,
        ViewProjectionMatrix._44 - ViewProjectionMatrix._43));
}

bool RendererUtils::IsAabbInCameraFrustum(
    const DirectX::XMVECTOR Planes[6],
    const DirectX::XMFLOAT3& BoundsMin,
    const DirectX::XMFLOAT3& BoundsMax)
{
    using namespace DirectX;

    for (int PlaneIndex = 0; PlaneIndex < 6; ++PlaneIndex)
    {
        const XMVECTOR Plane = Planes[PlaneIndex];
        const float PlaneX = XMVectorGetX(Plane);
        const float PlaneY = XMVectorGetY(Plane);
        const float PlaneZ = XMVectorGetZ(Plane);

        const float X = PlaneX >= 0.0f ? BoundsMax.x : BoundsMin.x;
        const float Y = PlaneY >= 0.0f ? BoundsMax.y : BoundsMin.y;
        const float Z = PlaneZ >= 0.0f ? BoundsMax.z : BoundsMin.z;
        const XMVECTOR Vertex = XMVectorSet(X, Y, Z, 1.0f);

        if (XMVectorGetX(XMVector4Dot(Plane, Vertex)) < 0.0f)
        {
            return false;
        }
    }

    return true;
}

uint32_t RendererUtils::BuildPipelineKey(const FSceneModelResource& Model)
{
    const uint32_t UseNormal = Model.bHasNormalMap ? 1u : 0u;
    const uint32_t UseMr = !Model.MetallicRoughnessTexturePath.empty() ? 1u : 0u;
    const uint32_t UseBase = !Model.BaseColorTexturePath.empty() ? 1u : 0u;
    const uint32_t UseEmissive = !Model.EmissiveTexturePath.empty() ? 1u : 0u;
    const uint32_t UseAlphaMask = (Model.AlphaMode == 1u) ? 1u : 0u;
    const uint32_t UseSheenModel = (Model.ShadingModelId == 1u) ? 1u : 0u;
    const uint32_t UseClearcoatModel = (Model.ShadingModelId == 2u) ? 1u : 0u;
    const uint32_t UseAnisotropyModel = (Model.ShadingModelId == 3u) ? 1u : 0u;
    const uint32_t UseSkinning = (IsValidBindlessIndex(Model.BoneMatrixBuffer.SrvBindlessIndex) && Model.BoneMatrixCount > 0) ? 1u : 0u;
    const uint32_t UseDoubleSided = Model.bDoubleSided ? 1u : 0u;
    return (UseNormal) | (UseMr << 1) | (UseBase << 2) | (UseEmissive << 3) | (UseAlphaMask << 4)
        | (UseSheenModel << 5) | (UseClearcoatModel << 6) | (UseAnisotropyModel << 7)
        | (UseSkinning << GPipelineKeySkinningBit) | (UseDoubleSided << GPipelineKeyDoubleSidedBit);
}
