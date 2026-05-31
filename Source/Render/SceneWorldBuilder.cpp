#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SceneWorldBuilder.h"
#include "RendererUtils.h"
#include "GpuResource.h"
#include "RayTracingRuntime.h"

#include "../Scene/Mesh.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/SceneJsonLoader.h"
#include "../World/World.h"
#include "../World/StaticMesh.h"
#include "../World/SkeletalMesh.h"
#include "../Core/Logger.h"
#include "../Core/StringUtils.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12Commons.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12CommandQueue.h"
#include <DirectXPackedVector.h>
#include <vector>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cmath>
#include <limits>
#include <utility>
#include <d3dx12.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr uint32_t GClusterDagVertexPackingModeNone = 0u;
    constexpr uint32_t GClusterDagVertexPackingModeBasic = 1u;

    struct FNodeSectionBuildInputs
    {
        const FSceneModelDesc& SceneModel;
        const FGltfNode& Node;
        const std::string& BaseName;
        const std::filesystem::path& MeshPath;
        const FGltfScene& LoadedScene;
        const FMesh& Mesh;
        const std::vector<FMesh::FPrimitive>& MeshPrimitives;
        const FClusterDAG* WholeMeshClusterDAG = nullptr;
        bool bWholeMeshClusterDagRuntimeAllowed = false;
        int SceneIndex = -1;
        size_t MeshIndex = 0;
        size_t SectionCount = 0;
        FFloat3 MeshCenter{};
        float MeshRadius = 0.0f;
        float NodeScale = 1.0f;
        DirectX::XMMATRIX World;
        DirectX::XMMATRIX ModelTransform;
        DirectX::XMVECTOR BoundsMin;
        DirectX::XMVECTOR BoundsMax;
    };

    bool BuildClusterDagPackedVertexStreams(
        const FClusterDAG& ClusterDAG,
        FClusterDAGPackedVertexData& OutPackedVertexData)
    {
        if (ClusterDAG.PackedVertexData.IsValid())
        {
            OutPackedVertexData = ClusterDAG.PackedVertexData;
            return true;
        }

        return BuildClusterDAGPackedVertexData(ClusterDAG, OutPackedVertexData);
    }

    DirectX::XMFLOAT4 BuildOffsetScale(const FGltfTextureTransform& Transform)
    {
        return DirectX::XMFLOAT4(Transform.Offset.x, Transform.Offset.y, Transform.Scale.x, Transform.Scale.y);
    }

    DirectX::XMFLOAT2 BuildRotationConstants(const FGltfTextureTransform& Transform)
    {
        const float CosR = std::cos(Transform.Rotation);
        const float SinR = std::sin(Transform.Rotation);
        return DirectX::XMFLOAT2(CosR, SinR);
    }

    void ComputeMeshBounds(const FMesh& Mesh, FFloat3& OutCenter, float& OutRadius, FFloat3& OutMin, FFloat3& OutMax)
    {
        const auto& Primitives = Mesh.GetPrimitives();
        bool bHasVertex = false;
        FFloat3 Min{};
        FFloat3 Max{};

        for (const FMesh::FPrimitive& Primitive : Primitives)
        {
            const auto& Positions = Primitive.VertexStreams.Positions;
            if (Positions.empty())
            {
                continue;
            }

            if (!bHasVertex)
            {
                Min = Positions.front();
                Max = Positions.front();
                bHasVertex = true;
            }

            for (const auto& Position : Positions)
            {
                Min.x = std::min(Min.x, Position.x);
                Min.y = std::min(Min.y, Position.y);
                Min.z = std::min(Min.z, Position.z);

                Max.x = std::max(Max.x, Position.x);
                Max.y = std::max(Max.y, Position.y);
                Max.z = std::max(Max.z, Position.z);
            }
        }

        if (!bHasVertex)
        {
            OutCenter = FFloat3(0.0f, 0.0f, 0.0f);
            OutRadius = 1.0f;
            OutMin = FFloat3(0.0f, 0.0f, 0.0f);
            OutMax = FFloat3(0.0f, 0.0f, 0.0f);
            return;
        }

        OutCenter = FFloat3(
            0.5f * (Min.x + Max.x),
            0.5f * (Min.y + Max.y),
            0.5f * (Min.z + Max.z));

        const DirectX::XMVECTOR Extents = DirectX::XMVectorSet(Max.x - Min.x, Max.y - Min.y, Max.z - Min.z, 0.0f);
        OutRadius = DirectX::XMVectorGetX(DirectX::XMVector3Length(Extents)) * 0.5f;
        OutRadius = std::max(OutRadius, 1.0f);
        OutMin = Min;
        OutMax = Max;
    }

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
                true,
                false);
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
                true,
                false);
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
                OutGeometry.VertexBuffers[StreamIndex].Get(), 0,
                UploadBuffers[StreamIndex].Get(), 0,
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

    void UpdateSceneBounds(const DirectX::XMFLOAT3& ModelCenter, float ModelRadius, DirectX::XMFLOAT3& OutMin, DirectX::XMFLOAT3& OutMax)
    {
        OutMin.x = std::min(OutMin.x, ModelCenter.x - ModelRadius);
        OutMin.y = std::min(OutMin.y, ModelCenter.y - ModelRadius);
        OutMin.z = std::min(OutMin.z, ModelCenter.z - ModelRadius);

        OutMax.x = std::max(OutMax.x, ModelCenter.x + ModelRadius);
        OutMax.y = std::max(OutMax.y, ModelCenter.y + ModelRadius);
        OutMax.z = std::max(OutMax.z, ModelCenter.z + ModelRadius);
    }

    bool CreateIndexBufferFromIndices(
        FDX12Device* Device,
        const std::vector<uint32_t>& Indices,
        FMeshGeometryBuffers& InOutGeometry,
        FUploadBatch* UploadBatch = nullptr)
    {
        if (!Device || Indices.empty())
        {
            return false;
        }

        const UINT IndexBufferSize = static_cast<UINT>(Indices.size() * sizeof(uint32_t));

        FUploadBuffer UploadBuffer;
        CreateBindlessBufferWithUpload(
            Device,
            L"PrimitiveIndexBuffer",
            CreateStructuredBufferDesc<uint32_t>(Indices.size()),
            D3D12_RESOURCE_STATE_COPY_DEST,
            InOutGeometry.IndexBuffer,
            UploadBuffer,
            Indices.data(),
            true,
            false);

        InOutGeometry.IndexBufferView.BufferLocation = InOutGeometry.IndexBuffer->GetGPUVirtualAddress();
        InOutGeometry.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        InOutGeometry.IndexBufferView.SizeInBytes = IndexBufferSize;
        InOutGeometry.IndexCount = static_cast<uint32_t>(Indices.size());

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
        CommandList->CopyBufferRegion(InOutGeometry.IndexBuffer.Get(), 0, UploadBuffer.Get(), 0, IndexBufferSize);
        ActiveBatch->Context.TransitionResource(InOutGeometry.IndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
        ActiveBatch->AddUploadBuffer(std::move(UploadBuffer.Resource));

        if (!UploadBatch)
        {
            ActiveBatch->ExecuteAndFlush();
        }

        return true;
    }

    template<typename T>
    bool CreateStructuredBufferFromData(
        FDX12Device* Device,
        const std::vector<T>& Data,
        FBindlessBuffer& OutBuffer,
        const wchar_t* Name,
        FUploadBatch* UploadBatch = nullptr)
    {
        if (!Device || Data.empty())
        {
            return false;
        }

        const uint64_t BufferSize = static_cast<uint64_t>(sizeof(T)) * Data.size();

        FUploadBuffer UploadBufferObj;
        CreateBindlessBufferWithUpload(
            Device,
            Name ? std::wstring(Name) : std::wstring(),
            CreateStructuredBufferDesc<T>(Data.size()),
            D3D12_RESOURCE_STATE_COPY_DEST,
            OutBuffer,
            UploadBufferObj,
            Data.data(),
            true,
            false);

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
        CommandList->CopyBufferRegion(OutBuffer.Get(), 0, UploadBufferObj.Get(), 0, BufferSize);
        ActiveBatch->Context.TransitionResource(OutBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
        ActiveBatch->AddUploadBuffer(std::move(UploadBufferObj.Resource));

        if (!UploadBatch)
        {
            ActiveBatch->ExecuteAndFlush();
        }

        return true;
    }

    void InitializeMeshSectionFromSection(
        const FNodeSectionBuildInputs& Inputs,
        const FGltfPrimitiveSection& Section,
        size_t SectionIndex,
        uint32_t ObjectId,
        FMeshSection& OutMeshSection)
    {
        using namespace DirectX;

        OutMeshSection.DrawIndexStart = Section.IndexStart;
        OutMeshSection.DrawIndexCount = Section.IndexCount;
        OutMeshSection.BaseIndexCount = Section.IndexCount;

        XMStoreFloat4x4(&OutMeshSection.ModelTransform, Inputs.ModelTransform);

        const XMVECTOR CenterVec = XMVector3TransformCoord(
            XMVectorSet(Inputs.MeshCenter.x, Inputs.MeshCenter.y, Inputs.MeshCenter.z, 1.0f),
            Inputs.World);
        XMStoreFloat3(&OutMeshSection.Center, CenterVec);
        OutMeshSection.Radius = Inputs.MeshRadius * Inputs.NodeScale;

        if (Inputs.SectionCount > 1)
        {
            OutMeshSection.Name = Inputs.BaseName + "_Sec" + std::to_string(SectionIndex);
        }
        else
        {
            OutMeshSection.Name = Inputs.BaseName;
        }

        OutMeshSection.ObjectId = ObjectId;
        XMStoreFloat3(&OutMeshSection.BoundsMin, Inputs.BoundsMin);
        XMStoreFloat3(&OutMeshSection.BoundsMax, Inputs.BoundsMax);

        const std::wstring EmptyTexture;
        const FGltfMaterialTextureSet& Material = Section.Material;

        const std::wstring& BaseColorPath = !Material.BaseColor.empty() ? Material.BaseColor : EmptyTexture;
        const std::wstring& MetallicRoughnessPath = !Material.MetallicRoughness.empty() ? Material.MetallicRoughness : EmptyTexture;
        const std::wstring& NormalPath = !Material.Normal.empty() ? Material.Normal : EmptyTexture;
        const std::wstring& EmissivePath = !Material.Emissive.empty() ? Material.Emissive : EmptyTexture;
        const std::wstring& SheenColorPath = !Material.SheenColor.empty() ? Material.SheenColor : EmptyTexture;
        const std::wstring& SheenRoughnessPath = !Material.SheenRoughness.empty() ? Material.SheenRoughness : EmptyTexture;
        const std::wstring& ClearcoatPath = !Material.Clearcoat.empty() ? Material.Clearcoat : EmptyTexture;
        const std::wstring& ClearcoatRoughnessPath = !Material.ClearcoatRoughness.empty() ? Material.ClearcoatRoughness : EmptyTexture;
        const std::wstring& ClearcoatNormalPath = !Material.ClearcoatNormal.empty() ? Material.ClearcoatNormal : EmptyTexture;
        const std::wstring& AnisotropyPath = !Material.Anisotropy.empty() ? Material.Anisotropy : EmptyTexture;

        FMeshMaterial& OutMaterial = OutMeshSection.Material;
        OutMaterial.BaseColorTexturePath = Inputs.SceneModel.BaseColorTexturePath.empty() ? BaseColorPath : Inputs.SceneModel.BaseColorTexturePath;
        OutMaterial.MetallicRoughnessTexturePath = Inputs.SceneModel.MetallicRoughnessTexturePath.empty() ? MetallicRoughnessPath : Inputs.SceneModel.MetallicRoughnessTexturePath;
        OutMaterial.NormalTexturePath = Inputs.SceneModel.NormalTexturePath.empty() ? NormalPath : Inputs.SceneModel.NormalTexturePath;
        OutMaterial.EmissiveTexturePath = Inputs.SceneModel.EmissiveTexturePath.empty() ? EmissivePath : Inputs.SceneModel.EmissiveTexturePath;
        OutMaterial.SheenColorTexturePath = SheenColorPath;
        OutMaterial.SheenRoughnessTexturePath = SheenRoughnessPath;
        OutMaterial.ClearcoatTexturePath = ClearcoatPath;
        OutMaterial.ClearcoatRoughnessTexturePath = ClearcoatRoughnessPath;
        OutMaterial.ClearcoatNormalTexturePath = ClearcoatNormalPath;
        OutMaterial.AnisotropyTexturePath = AnisotropyPath;
        OutMaterial.BaseColorFactor = Material.BaseColorFactor;
        OutMaterial.BaseColorAlpha = Material.BaseColorAlpha;
        OutMaterial.MetallicFactor = Material.MetallicFactor;
        OutMaterial.RoughnessFactor = Material.RoughnessFactor;
        OutMaterial.EmissiveFactor = Material.EmissiveFactor;
        OutMaterial.SheenColorFactor = Material.SheenColorFactor;
        OutMaterial.SheenRoughnessFactor = Material.SheenRoughnessFactor;
        OutMaterial.ClearcoatFactor = Material.ClearcoatFactor;
        OutMaterial.ClearcoatRoughnessFactor = Material.ClearcoatRoughnessFactor;
        OutMaterial.AnisotropyStrength = Material.AnisotropyStrength;
        OutMaterial.AnisotropyRotation = Material.AnisotropyRotation;
        OutMaterial.AlphaCutoff = Material.AlphaCutoff;
        OutMaterial.AlphaMode = static_cast<uint32_t>(Material.bAlphaBlend
            ? EAlphaMode::Blend
            : (Material.bAlphaMask ? EAlphaMode::Mask : EAlphaMode::Opaque));
        OutMaterial.bDoubleSided = Material.bDoubleSided;
        OutMaterial.ShadingModelId = Material.ShadingModelId;
        OutMaterial.bHasNormalMap = !OutMaterial.NormalTexturePath.empty();

        OutMaterial.BaseColorTransform.OffsetScale = BuildOffsetScale(Material.BaseColorTransform);
        OutMaterial.BaseColorTransform.Rotation = BuildRotationConstants(Material.BaseColorTransform);
        OutMaterial.MetallicRoughnessTransform.OffsetScale = BuildOffsetScale(Material.MetallicRoughnessTransform);
        OutMaterial.MetallicRoughnessTransform.Rotation = BuildRotationConstants(Material.MetallicRoughnessTransform);
        OutMaterial.NormalTransform.OffsetScale = BuildOffsetScale(Material.NormalTransform);
        OutMaterial.NormalTransform.Rotation = BuildRotationConstants(Material.NormalTransform);
        OutMaterial.EmissiveTransform.OffsetScale = BuildOffsetScale(Material.EmissiveTransform);
        OutMaterial.EmissiveTransform.Rotation = BuildRotationConstants(Material.EmissiveTransform);
        OutMaterial.SheenColorTransform.OffsetScale = BuildOffsetScale(Material.SheenColorTransform);
        OutMaterial.SheenColorTransform.Rotation = BuildRotationConstants(Material.SheenColorTransform);
        OutMaterial.SheenRoughnessTransform.OffsetScale = BuildOffsetScale(Material.SheenRoughnessTransform);
        OutMaterial.SheenRoughnessTransform.Rotation = BuildRotationConstants(Material.SheenRoughnessTransform);
        OutMaterial.ClearcoatTransform.OffsetScale = BuildOffsetScale(Material.ClearcoatTransform);
        OutMaterial.ClearcoatTransform.Rotation = BuildRotationConstants(Material.ClearcoatTransform);
        OutMaterial.ClearcoatRoughnessTransform.OffsetScale = BuildOffsetScale(Material.ClearcoatRoughnessTransform);
        OutMaterial.ClearcoatRoughnessTransform.Rotation = BuildRotationConstants(Material.ClearcoatRoughnessTransform);
        OutMaterial.ClearcoatNormalTransform.OffsetScale = BuildOffsetScale(Material.ClearcoatNormalTransform);
        OutMaterial.ClearcoatNormalTransform.Rotation = BuildRotationConstants(Material.ClearcoatNormalTransform);
        OutMaterial.AnisotropyTransform.OffsetScale = BuildOffsetScale(Material.AnisotropyTransform);
        OutMaterial.AnisotropyTransform.Rotation = BuildRotationConstants(Material.AnisotropyTransform);
    }

    bool BuildMeshSectionGeometryForSection(
        FDX12Device* Device,
        const FNodeSectionBuildInputs& Inputs,
        size_t SectionIndex,
        FUploadBatch& UploadBatch,
        FMeshSection& InOutMeshSection)
    {
        if (SectionIndex >= Inputs.MeshPrimitives.size())
        {
            return false;
        }

        const FMesh::FMeshletGroup* MeshletGroup = Inputs.Mesh.GetMeshletGroup(SectionIndex);
        const bool bUseMeshletIndices = Inputs.Mesh.IsMeshletIndexingAllowed() && MeshletGroup && !MeshletGroup->MeshletIndices.empty();
        if (!CreatePrimitiveGeometry(Device, Inputs.MeshPrimitives[SectionIndex], InOutMeshSection.Geometry, !bUseMeshletIndices, &UploadBatch))
        {
            LogError("Failed to create primitive geometry for scene mesh: " + StringUtils::PathToUtf8(Inputs.MeshPath));
            return false;
        }

        if (bUseMeshletIndices)
        {
            if (CreateIndexBufferFromIndices(Device, MeshletGroup->MeshletIndices, InOutMeshSection.Geometry, &UploadBatch))
            {
                InOutMeshSection.bUseMeshletCulling = true;
                InOutMeshSection.Meshlets = MeshletGroup->Meshlets;
                InOutMeshSection.MeshletBounds = MeshletGroup->MeshletBounds;
                InOutMeshSection.MeshletIndices = MeshletGroup->MeshletIndices;
                InOutMeshSection.DrawIndexStart = 0;
                InOutMeshSection.DrawIndexCount = static_cast<uint32_t>(MeshletGroup->MeshletIndices.size());
                InOutMeshSection.BaseIndexCount = InOutMeshSection.DrawIndexCount;
            }
        }
        else
        {
            InOutMeshSection.DrawIndexStart = 0;
            InOutMeshSection.DrawIndexCount = static_cast<uint32_t>(Inputs.MeshPrimitives[SectionIndex].Indices.size());
            InOutMeshSection.BaseIndexCount = InOutMeshSection.DrawIndexCount;
        }

        return true;
    }

    void ConfigureWholeMeshClusterDagRuntimeForSection(
        FDX12Device* Device,
        const FNodeSectionBuildInputs& Inputs,
        size_t SectionIndex,
        FUploadBatch& UploadBatch,
        FMeshSection& InOutMeshSection)
    {
        if (!(Inputs.WholeMeshClusterDAG && Inputs.bWholeMeshClusterDagRuntimeAllowed && SectionIndex == 0))
        {
            return;
        }

        const FClusterDAG* ClusterDAG = Inputs.WholeMeshClusterDAG;
        InOutMeshSection.ClusterDagMeshIndex = static_cast<uint32_t>(Inputs.MeshIndex);
        InOutMeshSection.ClusterDagSourceFilePath = Inputs.MeshPath.wstring();
        std::filesystem::path ClusterDagCachePath = Inputs.MeshPath;
        ClusterDagCachePath.replace_extension(L".vmesh");
        InOutMeshSection.ClusterDagCacheFilePath = ClusterDagCachePath.wstring();
        InOutMeshSection.bUseClusterDagRuntime =
            ClusterDAG->HasRuntimeHierarchy()
            && Inputs.bWholeMeshClusterDagRuntimeAllowed
            && !ClusterDAG->RuntimeHierarchy.PackedIndices.empty();
        InOutMeshSection.ClusterDagRuntimeHierarchy = ClusterDAG->RuntimeHierarchy;

        if (!InOutMeshSection.bUseClusterDagRuntime)
        {
            return;
        }

        FClusterDAGPackedVertexData ClusterDagPackedVertexData;
        InOutMeshSection.bUseClusterDagRuntime = BuildClusterDagPackedVertexStreams(*ClusterDAG, ClusterDagPackedVertexData);

        FMeshGeometryBuffers ClusterDagGeometry = {};
        const bool bCreatedClusterDagGeometry =
            InOutMeshSection.bUseClusterDagRuntime
            && CreateStructuredBufferFromData(Device, ClusterDagPackedVertexData.Positions, InOutMeshSection.ClusterDagVertexBuffers[kClusterDagVertexStreamPosition], L"ClusterDagPositionBuffer", &UploadBatch)
			&& CreateStructuredBufferFromData(Device, ClusterDagPackedVertexData.Normals, InOutMeshSection.ClusterDagVertexBuffers[kClusterDagVertexStreamNormal], L"ClusterDagNormalBuffer", &UploadBatch)
            && (ClusterDagPackedVertexData.UVs.empty()
                || CreateStructuredBufferFromData(Device, ClusterDagPackedVertexData.UVs, InOutMeshSection.ClusterDagVertexBuffers[kClusterDagVertexStreamUv], L"ClusterDagUvBuffer", &UploadBatch))
            && (ClusterDagPackedVertexData.Tangents.empty()
                || CreateStructuredBufferFromData(Device, ClusterDagPackedVertexData.Tangents, InOutMeshSection.ClusterDagVertexBuffers[kClusterDagVertexStreamTangent], L"ClusterDagTangentBuffer", &UploadBatch))
            && (ClusterDagPackedVertexData.Colors.empty() || CreateStructuredBufferFromData(Device, ClusterDagPackedVertexData.Colors, InOutMeshSection.ClusterDagColorBuffer, L"ClusterDagColorBuffer", &UploadBatch))
            && CreateIndexBufferFromIndices(Device, ClusterDAG->RuntimeHierarchy.PackedIndices, ClusterDagGeometry, &UploadBatch);
        if (bCreatedClusterDagGeometry)
        {
            InOutMeshSection.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeBasic;
            InOutMeshSection.ClusterDagPackedVertexData = ClusterDagPackedVertexData;
            InOutMeshSection.ClusterDagPackedPositionOffset = ClusterDagPackedVertexData.PositionOffset;
            InOutMeshSection.ClusterDagPackedPositionScale = ClusterDagPackedVertexData.PositionScale;
            InOutMeshSection.ClusterDagPackedConstantUV = DirectX::XMFLOAT4(
                ClusterDagPackedVertexData.ConstantUV.x,
                ClusterDagPackedVertexData.ConstantUV.y,
                0.0f,
                0.0f);
            InOutMeshSection.ClusterDagPackedConstantColor = DirectX::XMFLOAT4(
                ClusterDagPackedVertexData.ConstantColor.x,
                ClusterDagPackedVertexData.ConstantColor.y,
                ClusterDagPackedVertexData.ConstantColor.z,
                ClusterDagPackedVertexData.ConstantColor.w);
            InOutMeshSection.ClusterDagIndexBuffer = ClusterDagGeometry.IndexBuffer;
            InOutMeshSection.ClusterDagIndexCount = ClusterDagGeometry.IndexCount;

            const bool bDagVertexSrvReady = AreAllBindlessIndicesValid(
                InOutMeshSection.ClusterDagVertexBuffers[kClusterDagVertexStreamPosition].SrvBindlessIndex,
                InOutMeshSection.ClusterDagVertexBuffers[kClusterDagVertexStreamNormal].SrvBindlessIndex,
                InOutMeshSection.ClusterDagIndexBuffer.SrvBindlessIndex);
            if (!bDagVertexSrvReady)
            {
                InOutMeshSection.bUseClusterDagRuntime = false;
                InOutMeshSection.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeNone;
            }

        }
        else
        {
            InOutMeshSection.bUseClusterDagRuntime = false;
            InOutMeshSection.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeNone;
        }
    }

    void InitializeSkinningForMeshSection(FDX12Device* Device, int SkinIndex, const FGltfScene& LoadedScene, FMeshSection& InOutMeshSection)
    {
        if (!(Device && SkinIndex >= 0))
        {
            return;
        }

        if (static_cast<size_t>(SkinIndex) >= LoadedScene.Skins.size())
        {
            return;
        }

        const FGltfSkin& Skin = LoadedScene.Skins[static_cast<size_t>(SkinIndex)];
        InOutMeshSection.BoneMatrixCount = static_cast<uint32_t>(Skin.Joints.size());
        if (InOutMeshSection.BoneMatrixCount == 0)
        {
            return;
        }

        void* MappedData = nullptr;
        if (!CreateMappedBindlessBuffer(
            Device,
            L"SkinMatrixBuffer",
            CreateStructuredBufferDesc<DirectX::XMFLOAT4X4>(InOutMeshSection.BoneMatrixCount),
            InOutMeshSection.BoneMatrixBuffer,
            MappedData))
        {
            return;
        }

        InOutMeshSection.BoneMatrixBufferMapped = static_cast<uint8_t*>(MappedData);
        const uint64_t BufferSize = sizeof(DirectX::XMFLOAT4X4) * InOutMeshSection.BoneMatrixCount;
        std::vector<DirectX::XMFLOAT4X4> IdentityMatrices(InOutMeshSection.BoneMatrixCount);
        for (uint32_t JointIndex = 0; JointIndex < InOutMeshSection.BoneMatrixCount; ++JointIndex)
        {
            IdentityMatrices[JointIndex] = DirectX::XMFLOAT4X4(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f);
        }
        std::memcpy(InOutMeshSection.BoneMatrixBufferMapped, IdentityMatrices.data(), BufferSize);
        CreateBindlessBufferSrv(Device, InOutMeshSection.BoneMatrixBuffer);
        InOutMeshSection.bUseSkinning = IsValidBindlessIndex(InOutMeshSection.BoneMatrixBuffer.SrvBindlessIndex);
    }

    void PropagateClusterDagOwnerDataToSections(std::vector<FMeshSection>& InOutSections)
    {
        if (InOutSections.empty())
        {
            return;
        }

        FMeshSection& OwnerMeshSection = InOutSections.front();
        if (!OwnerMeshSection.bUseClusterDagRuntime)
        {
            return;
        }

        OwnerMeshSection.bCoveredByClusterDagRuntime = true;
        for (size_t SectionOrdinal = 1; SectionOrdinal < InOutSections.size(); ++SectionOrdinal)
        {
            FMeshSection& MeshSection = InOutSections[SectionOrdinal];
            MeshSection.bCoveredByClusterDagRuntime = true;
            MeshSection.ClusterDagMeshIndex = OwnerMeshSection.ClusterDagMeshIndex;
            MeshSection.ClusterDagSourceFilePath = OwnerMeshSection.ClusterDagSourceFilePath;
            MeshSection.ClusterDagCacheFilePath = OwnerMeshSection.ClusterDagCacheFilePath;
            MeshSection.ClusterDagVertexPackingMode = OwnerMeshSection.ClusterDagVertexPackingMode;
            MeshSection.ClusterDagPackedVertexData = OwnerMeshSection.ClusterDagPackedVertexData;
            MeshSection.ClusterDagPackedPositionOffset = OwnerMeshSection.ClusterDagPackedPositionOffset;
            MeshSection.ClusterDagPackedPositionScale = OwnerMeshSection.ClusterDagPackedPositionScale;
            MeshSection.ClusterDagPackedConstantUV = OwnerMeshSection.ClusterDagPackedConstantUV;
            MeshSection.ClusterDagPackedConstantColor = OwnerMeshSection.ClusterDagPackedConstantColor;
            MeshSection.ClusterDagVertexBuffers = OwnerMeshSection.ClusterDagVertexBuffers;
            MeshSection.ClusterDagIndexBuffer = OwnerMeshSection.ClusterDagIndexBuffer;
            MeshSection.ClusterDagColorBuffer = OwnerMeshSection.ClusterDagColorBuffer;
            MeshSection.ClusterDagDebugColorBuffer = OwnerMeshSection.ClusterDagDebugColorBuffer;
            MeshSection.ClusterDagIndexCount = OwnerMeshSection.ClusterDagIndexCount;
        }
    }

}

bool SceneWorldBuilder::LoadWorldFromSceneFile(
    FDX12Device* Device,
    const std::wstring& SceneFilePath,
    FWorld& OutWorld,
    DirectX::XMFLOAT3& OutSceneCenter,
    float& OutSceneRadius)
{
    OutWorld.Clear();
    std::vector<FGltfAnimationRuntime>& OutAnimationRuntimes = OutWorld.GetGltfAnimationRuntimes();

    uint32_t NextObjectId = 1;

    const std::filesystem::path ScenePath(SceneFilePath);
    const std::string ScenePathUtf8 = StringUtils::PathToUtf8(ScenePath);

    std::vector<FSceneModelDesc> Models;
    if (!FSceneJsonLoader::LoadScene(SceneFilePath, Models) || Models.empty())
    {
        LogError("Scene JSON did not provide any models: " + ScenePathUtf8);
        return false;
    }

    DirectX::XMFLOAT3 SceneMin{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    DirectX::XMFLOAT3 SceneMax{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };

    FUploadBatch UploadBatch;

    for (const FSceneModelDesc& Model : Models)
    {
        std::filesystem::path MeshPath(Model.MeshPath);
        if (!MeshPath.is_absolute())
        {
            std::filesystem::path AssetsRoot = ScenePath.parent_path().parent_path();
            MeshPath = AssetsRoot / MeshPath;
        }

        FGltfScene LoadedScene;
        if (!FGltfLoader::LoadSceneFromFile(MeshPath, LoadedScene))
        {
            LogError("Failed to load mesh from scene: " + StringUtils::PathToUtf8(MeshPath));
            continue;
        }

        const int SceneIndex = static_cast<int>(OutAnimationRuntimes.size());

        if (LoadedScene.Meshes.empty())
        {
            LogError("No meshes found in glTF: " + StringUtils::PathToUtf8(MeshPath));
            continue;
        }

        std::vector<FFloat3> MeshCenters(LoadedScene.Meshes.size());
        std::vector<float> MeshRadii(LoadedScene.Meshes.size());
        std::vector<FFloat3> MeshMins(LoadedScene.Meshes.size());
        std::vector<FFloat3> MeshMaxs(LoadedScene.Meshes.size());

        for (size_t MeshIndex = 0; MeshIndex < LoadedScene.Meshes.size(); ++MeshIndex)
        {
            const FMesh& Mesh = LoadedScene.Meshes[MeshIndex];
            ComputeMeshBounds(Mesh, MeshCenters[MeshIndex], MeshRadii[MeshIndex], MeshMins[MeshIndex], MeshMaxs[MeshIndex]);
        }

        if (LoadedScene.Nodes.empty())
        {
            for (size_t MeshIndex = 0; MeshIndex < LoadedScene.Meshes.size(); ++MeshIndex)
            {
                FGltfNode DefaultNode;
                DefaultNode.MeshIndex = static_cast<int>(MeshIndex);
                DefaultNode.NodeIndex = static_cast<int>(MeshIndex);
                DefaultNode.WorldMatrix = DirectX::XMFLOAT4X4(
                    1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f);
                DefaultNode.Name = "Mesh_" + std::to_string(MeshIndex);
                LoadedScene.Nodes.push_back(DefaultNode);
            }
        }

        for (const FGltfNode& LoadedNode : LoadedScene.Nodes)
        {
            if (LoadedNode.MeshIndex < 0 || static_cast<size_t>(LoadedNode.MeshIndex) >= LoadedScene.Meshes.size())
            {
                continue;
            }

            const size_t MeshIndex = static_cast<size_t>(LoadedNode.MeshIndex);

            const FFloat3 MeshCenter = MeshCenters[MeshIndex];
            float MeshRadius = MeshRadii[MeshIndex];
            const FFloat3 MeshMin = MeshMins[MeshIndex];
            const FFloat3 MeshMax = MeshMaxs[MeshIndex];

            const std::array<float, 3> ScaleComponents = { Model.Scale.x, Model.Scale.y, Model.Scale.z };
            float MaxScale = 0.0f;
            for (float ScaleValue : ScaleComponents)
            {
                MaxScale = (std::max)(MaxScale, std::fabs(ScaleValue));
            }

            const float NodeScale = MatrixMath::ComputeMaxScale(LoadedNode.WorldMatrix);

            MeshRadius *= MaxScale;

            using namespace DirectX;

            const XMMATRIX NodeWorld = XMLoadFloat4x4(&LoadedNode.WorldMatrix);
            const XMMATRIX Scale = XMMatrixScaling(Model.Scale.x, Model.Scale.y, Model.Scale.z);
            const XMMATRIX Rotation = XMMatrixRotationRollPitchYaw(
                XMConvertToRadians(Model.RotationEuler.x),
                XMConvertToRadians(Model.RotationEuler.y),
                XMConvertToRadians(Model.RotationEuler.z));
            const XMMATRIX Translation = XMMatrixTranslation(Model.Position.x, Model.Position.y, Model.Position.z);

            const XMMATRIX ModelTransform = Scale * Rotation * Translation;
            const XMMATRIX World = NodeWorld * ModelTransform;

            const std::array<XMVECTOR, 8> LocalCorners =
            {
                XMVectorSet(MeshMin.x, MeshMin.y, MeshMin.z, 1.0f),
                XMVectorSet(MeshMax.x, MeshMin.y, MeshMin.z, 1.0f),
                XMVectorSet(MeshMin.x, MeshMax.y, MeshMin.z, 1.0f),
                XMVectorSet(MeshMax.x, MeshMax.y, MeshMin.z, 1.0f),
                XMVectorSet(MeshMin.x, MeshMin.y, MeshMax.z, 1.0f),
                XMVectorSet(MeshMax.x, MeshMin.y, MeshMax.z, 1.0f),
                XMVectorSet(MeshMin.x, MeshMax.y, MeshMax.z, 1.0f),
                XMVectorSet(MeshMax.x, MeshMax.y, MeshMax.z, 1.0f)
            };

            XMVECTOR BoundsMin = XMVectorSet(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 1.0f);
            XMVECTOR BoundsMax = XMVectorSet(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), 1.0f);

            for (const XMVECTOR Corner : LocalCorners)
            {
                const XMVECTOR WorldCorner = XMVector3TransformCoord(Corner, World);
                BoundsMin = XMVectorMin(BoundsMin, WorldCorner);
                BoundsMax = XMVectorMax(BoundsMax, WorldCorner);
            }

            const std::string BaseName = LoadedNode.Name.empty() ? ("Mesh_" + std::to_string(MeshIndex)) : LoadedNode.Name;

            std::vector<FGltfPrimitiveSection> DefaultSections;
            const std::vector<FGltfPrimitiveSection>* PrimitiveSections = nullptr;
            if (MeshIndex < LoadedScene.MeshPrimitiveSections.size())
            {
                PrimitiveSections = &LoadedScene.MeshPrimitiveSections[MeshIndex];
            }

            const FMesh& Mesh = LoadedScene.Meshes[MeshIndex];
            const std::vector<FMesh::FPrimitive>& MeshPrimitives = Mesh.GetPrimitives();

            if (!PrimitiveSections || PrimitiveSections->empty())
            {
                FGltfPrimitiveSection Section;
                Section.IndexStart = 0;
                Section.IndexCount = MeshPrimitives.empty() ? 0 : static_cast<uint32_t>(MeshPrimitives.front().Indices.size());
                DefaultSections.push_back(Section);
                PrimitiveSections = &DefaultSections;
            }

            const size_t SectionCount = PrimitiveSections->size();
            const FClusterDAG* WholeMeshClusterDAG = Mesh.GetClusterDAG();
            bool bWholeMeshClusterDagRuntimeAllowed =
                WholeMeshClusterDAG
                && WholeMeshClusterDAG->HasRuntimeHierarchy()
                && LoadedNode.SkinIndex < 0
                && !WholeMeshClusterDAG->RuntimeHierarchy.PackedIndices.empty();
            for (const FGltfPrimitiveSection& Section : *PrimitiveSections)
            {
                if (Section.Material.bAlphaBlend)
                {
                    bWholeMeshClusterDagRuntimeAllowed = false;
                    break;
                }
            }
            const FNodeSectionBuildInputs SectionInputs
            {
                Model,
                LoadedNode,
                BaseName,
                MeshPath,
                LoadedScene,
                Mesh,
                MeshPrimitives,
                WholeMeshClusterDAG,
                bWholeMeshClusterDagRuntimeAllowed,
                SceneIndex,
                MeshIndex,
                SectionCount,
                MeshCenter,
                MeshRadius,
                NodeScale,
                World,
                ModelTransform,
                BoundsMin,
                BoundsMax
            };

            const bool bSkeletal = LoadedNode.SkinIndex >= 0;
            std::unique_ptr<FObject> Object;
            if (bSkeletal)
            {
                auto Skeletal = std::make_unique<FSkeletalMesh>();
                Skeletal->SetGltfSkinIndex(LoadedNode.SkinIndex);
                Skeletal->SetGltfIndices(SceneIndex, LoadedNode.NodeIndex, static_cast<int>(MeshIndex));
                Object = std::move(Skeletal);
            }
            else
            {
                Object = std::make_unique<FStaticMesh>();
            }

            Object->SetName(BaseName);
            std::vector<FMeshSection>& Sections = Object->GetSections();
            Sections.clear();
            Sections.reserve(SectionCount);

            bool bObjectBuildSucceeded = true;
            for (size_t SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
            {
                const FGltfPrimitiveSection& Section = (*PrimitiveSections)[SectionIndex];

                FMeshSection MeshSection;
                InitializeMeshSectionFromSection(SectionInputs, Section, SectionIndex, NextObjectId++, MeshSection);
                if (!BuildMeshSectionGeometryForSection(Device, SectionInputs, SectionIndex, UploadBatch, MeshSection))
                {
                    bObjectBuildSucceeded = false;
                    break;
                }

                ConfigureWholeMeshClusterDagRuntimeForSection(Device, SectionInputs, SectionIndex, UploadBatch, MeshSection);
                InitializeSkinningForMeshSection(Device, SectionInputs.Node.SkinIndex, LoadedScene, MeshSection);

                MeshSection.PipelineKey = RendererUtils::BuildPipelineKey(MeshSection);
                UpdateSceneBounds(MeshSection.Center, MeshSection.Radius, SceneMin, SceneMax);

                Sections.push_back(std::move(MeshSection));
            }

            if (!bObjectBuildSucceeded)
            {
                Sections.clear();
                continue;
            }

            PropagateClusterDagOwnerDataToSections(Sections);

            if (!Sections.empty())
            {
                Object->SetObjectId(Sections.front().ObjectId);
                DirectX::XMFLOAT4X4 ObjectWorldMatrix;
                DirectX::XMStoreFloat4x4(&ObjectWorldMatrix, World);
                Object->SetWorldMatrix(ObjectWorldMatrix);
                Object->ResetPreviousWorldMatrix();

                DirectX::XMFLOAT3 ObjectBoundsMin{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
                DirectX::XMFLOAT3 ObjectBoundsMax{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
                for (const FMeshSection& MeshSection : Sections)
                {
                    ObjectBoundsMin.x = (std::min)(ObjectBoundsMin.x, MeshSection.BoundsMin.x);
                    ObjectBoundsMin.y = (std::min)(ObjectBoundsMin.y, MeshSection.BoundsMin.y);
                    ObjectBoundsMin.z = (std::min)(ObjectBoundsMin.z, MeshSection.BoundsMin.z);
                    ObjectBoundsMax.x = (std::max)(ObjectBoundsMax.x, MeshSection.BoundsMax.x);
                    ObjectBoundsMax.y = (std::max)(ObjectBoundsMax.y, MeshSection.BoundsMax.y);
                    ObjectBoundsMax.z = (std::max)(ObjectBoundsMax.z, MeshSection.BoundsMax.z);
                }
                Object->SetBounds(ObjectBoundsMin, ObjectBoundsMax);
                OutWorld.AddObject(std::move(Object));
            }
        }

        // Move only the animation-relevant subset and let LoadedScene drop its
        // heavy Meshes / MeshPrimitiveSections at end of scope.
        FGltfAnimationRuntime AnimationRuntime;
        AnimationRuntime.Nodes = std::move(LoadedScene.Nodes);
        AnimationRuntime.NodeTransforms = std::move(LoadedScene.NodeTransforms);
        AnimationRuntime.Skins = std::move(LoadedScene.Skins);
        AnimationRuntime.Animations = std::move(LoadedScene.Animations);
        AnimationRuntime.Pose = std::move(LoadedScene.Pose);
        AnimationRuntime.AnimationTime = LoadedScene.AnimationTime;
        OutAnimationRuntimes.push_back(std::move(AnimationRuntime));

    }

    if (UploadBatch.bInitialized)
    {
        UploadBatch.ExecuteAndFlush();
    }

    FRayTracingRuntime::BuildSceneBlas(Device, OutWorld);

    if (OutWorld.GetDrawSectionCount() == 0)
    {
        LogError("No renderable models could be created from scene JSON: " + ScenePathUtf8);
        return false;
    }

    OutSceneCenter = DirectX::XMFLOAT3(
        0.5f * (SceneMin.x + SceneMax.x),
        0.5f * (SceneMin.y + SceneMax.y),
        0.5f * (SceneMin.z + SceneMax.z));

    const DirectX::XMVECTOR Extents = DirectX::XMVectorSet(SceneMax.x - SceneMin.x, SceneMax.y - SceneMin.y, SceneMax.z - SceneMin.z, 0.0f);
    OutSceneRadius = DirectX::XMVectorGetX(DirectX::XMVector3Length(Extents)) * 0.5f;
    OutSceneRadius = std::max(OutSceneRadius, 1.0f);

    return true;
}
