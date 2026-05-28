#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SceneModelResourceLoader.h"
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

    uint32_t PackClusterDagDebugColor(uint32_t ClusterIndex, uint32_t MipLevel)
    {
        uint32_t Hash = ClusterIndex * 747796405u + 2891336453u + MipLevel * 277803737u;
        Hash ^= Hash >> 16;
        Hash *= 2246822519u;
        Hash ^= Hash >> 13;
        Hash *= 3266489917u;
        Hash ^= Hash >> 16;

        const uint8_t R = static_cast<uint8_t>(96u + (Hash & 0x7fu));
        const uint8_t G = static_cast<uint8_t>(96u + ((Hash >> 8) & 0x7fu));
        const uint8_t B = static_cast<uint8_t>(96u + ((Hash >> 16) & 0x7fu));
        const uint8_t A = static_cast<uint8_t>((std::min)(MipLevel, 255u));
        return static_cast<uint32_t>(R)
            | (static_cast<uint32_t>(G) << 8)
            | (static_cast<uint32_t>(B) << 16)
            | (static_cast<uint32_t>(A) << 24);
    }

    std::vector<uint32_t> BuildClusterDagDebugColorTable(const FRuntimeClusterHierarchy& RuntimeHierarchy, uint32_t IndexCount)
    {
        std::vector<uint32_t> DebugColors(IndexCount, 0xffffffffu);
        for (uint32_t ClusterIndex = 0; ClusterIndex < static_cast<uint32_t>(RuntimeHierarchy.Clusters.size()); ++ClusterIndex)
        {
            const FRuntimeCluster& Cluster = RuntimeHierarchy.Clusters[ClusterIndex];
            const uint32_t PackedColor = PackClusterDagDebugColor(ClusterIndex, Cluster.MipLevel);
            for (uint32_t DrawDataOffset = 0; DrawDataOffset < Cluster.DrawDataCount; ++DrawDataOffset)
            {
                const uint32_t DrawDataIndex = Cluster.DrawDataStart + DrawDataOffset;
                if (DrawDataIndex >= RuntimeHierarchy.DrawDatas.size())
                {
                    continue;
                }

                const FRuntimeClusterDrawData& DrawData = RuntimeHierarchy.DrawDatas[DrawDataIndex];
                if (DrawData.IndexStart >= IndexCount)
                {
                    continue;
                }

                const uint32_t DrawDataEnd = (std::min)(DrawData.IndexStart + DrawData.IndexCount, IndexCount);
                for (uint32_t IndexOffset = DrawData.IndexStart; IndexOffset < DrawDataEnd; ++IndexOffset)
                {
                    DebugColors[IndexOffset] = PackedColor;
                }
            }
        }

        return DebugColors;
    }

    void InitializeModelResourceFromSection(
        const FNodeSectionBuildInputs& Inputs,
        const FGltfPrimitiveSection& Section,
        size_t SectionIndex,
        uint32_t ObjectId,
        FSceneModelResource& OutModelResource)
    {
        using namespace DirectX;

        OutModelResource.DrawIndexStart = Section.IndexStart;
        OutModelResource.DrawIndexCount = Section.IndexCount;
        OutModelResource.BaseIndexCount = Section.IndexCount;

        XMStoreFloat4x4(&OutModelResource.WorldMatrix, Inputs.World);
        XMStoreFloat4x4(&OutModelResource.ModelTransform, Inputs.ModelTransform);
        OutModelResource.GltfSceneIndex = Inputs.SceneIndex;
        OutModelResource.GltfNodeIndex = Inputs.Node.NodeIndex;
        OutModelResource.GltfMeshIndex = static_cast<int>(Inputs.MeshIndex);
        OutModelResource.GltfSkinIndex = Inputs.Node.SkinIndex;

        const XMVECTOR CenterVec = XMVector3TransformCoord(
            XMVectorSet(Inputs.MeshCenter.x, Inputs.MeshCenter.y, Inputs.MeshCenter.z, 1.0f),
            Inputs.World);
        XMStoreFloat3(&OutModelResource.Center, CenterVec);
        OutModelResource.Radius = Inputs.MeshRadius * Inputs.NodeScale;

        if (Inputs.SectionCount > 1)
        {
            OutModelResource.Name = Inputs.BaseName + "_Prim" + std::to_string(SectionIndex);
        }
        else
        {
            OutModelResource.Name = Inputs.BaseName;
        }

        OutModelResource.ObjectId = ObjectId;
        XMStoreFloat3(&OutModelResource.BoundsMin, Inputs.BoundsMin);
        XMStoreFloat3(&OutModelResource.BoundsMax, Inputs.BoundsMax);

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

        FMeshMaterial& OutMaterial = OutModelResource.Material;
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

    bool BuildModelResourceGeometryForSection(
        FDX12Device* Device,
        const FNodeSectionBuildInputs& Inputs,
        size_t SectionIndex,
        FUploadBatch& UploadBatch,
        FSceneModelResource& InOutModelResource)
    {
        if (SectionIndex >= Inputs.MeshPrimitives.size())
        {
            return false;
        }

        const FMesh::FMeshletGroup* MeshletGroup = Inputs.Mesh.GetMeshletGroup(SectionIndex);
        const bool bUseMeshletIndices = Inputs.Mesh.IsMeshletIndexingAllowed() && MeshletGroup && !MeshletGroup->MeshletIndices.empty();
        if (!CreatePrimitiveGeometry(Device, Inputs.MeshPrimitives[SectionIndex], InOutModelResource.Geometry, !bUseMeshletIndices, &UploadBatch))
        {
            LogError("Failed to create primitive geometry for scene mesh: " + StringUtils::PathToUtf8(Inputs.MeshPath));
            return false;
        }

        if (bUseMeshletIndices)
        {
            if (CreateIndexBufferFromIndices(Device, MeshletGroup->MeshletIndices, InOutModelResource.Geometry, &UploadBatch))
            {
                InOutModelResource.bUseMeshletCulling = true;
                InOutModelResource.Meshlets = MeshletGroup->Meshlets;
                InOutModelResource.MeshletBounds = MeshletGroup->MeshletBounds;
                InOutModelResource.MeshletIndices = MeshletGroup->MeshletIndices;
                InOutModelResource.DrawIndexStart = 0;
                InOutModelResource.DrawIndexCount = static_cast<uint32_t>(MeshletGroup->MeshletIndices.size());
                InOutModelResource.BaseIndexCount = InOutModelResource.DrawIndexCount;
            }
        }
        else
        {
            InOutModelResource.DrawIndexStart = 0;
            InOutModelResource.DrawIndexCount = static_cast<uint32_t>(Inputs.MeshPrimitives[SectionIndex].Indices.size());
            InOutModelResource.BaseIndexCount = InOutModelResource.DrawIndexCount;
        }

        return true;
    }

    void ConfigureWholeMeshClusterDagRuntimeForSection(
        FDX12Device* Device,
        const FNodeSectionBuildInputs& Inputs,
        size_t SectionIndex,
        FUploadBatch& UploadBatch,
        FSceneModelResource& InOutModelResource)
    {
        if (!(Inputs.WholeMeshClusterDAG && Inputs.bWholeMeshClusterDagRuntimeAllowed && SectionIndex == 0))
        {
            return;
        }

        const FClusterDAG* ClusterDAG = Inputs.WholeMeshClusterDAG;
        InOutModelResource.ClusterDagMeshIndex = static_cast<uint32_t>(Inputs.MeshIndex);
        InOutModelResource.ClusterDagPrimitiveIndex = 0u;
        InOutModelResource.ClusterDagSourceFilePath = Inputs.MeshPath.wstring();
        std::filesystem::path ClusterDagCachePath = Inputs.MeshPath;
        ClusterDagCachePath.replace_extension(L".vmesh");
        InOutModelResource.ClusterDagCacheFilePath = ClusterDagCachePath.wstring();
        InOutModelResource.bUseClusterDagRuntime =
            ClusterDAG->HasRuntimeHierarchy()
            && Inputs.bWholeMeshClusterDagRuntimeAllowed
            && !ClusterDAG->RuntimeHierarchy.PackedIndices.empty();
        InOutModelResource.ClusterDagRuntimeHierarchy = ClusterDAG->RuntimeHierarchy;

        if (!InOutModelResource.bUseClusterDagRuntime)
        {
            return;
        }

        FClusterDAGPackedVertexData ClusterDagPackedVertexData;
        InOutModelResource.bUseClusterDagRuntime =
            BuildClusterDagPackedVertexStreams(*ClusterDAG, ClusterDagPackedVertexData);

        FMeshGeometryBuffers ClusterDagGeometry = {};
        const bool bCreatedClusterDagGeometry =
            InOutModelResource.bUseClusterDagRuntime
            && CreateStructuredBufferFromData( Device, ClusterDagPackedVertexData.Positions, InOutModelResource.ClusterDagVertexBuffers[0], L"ClusterDagPositionBuffer", &UploadBatch)
			&& CreateStructuredBufferFromData(Device, ClusterDagPackedVertexData.Normals, InOutModelResource.ClusterDagVertexBuffers[1], L"ClusterDagNormalBuffer", &UploadBatch)
            && (ClusterDagPackedVertexData.UVs.empty()
                || CreateStructuredBufferFromData( Device, ClusterDagPackedVertexData.UVs, InOutModelResource.ClusterDagVertexBuffers[2], L"ClusterDagUvBuffer", &UploadBatch))
            && (ClusterDagPackedVertexData.Tangents.empty()
                || CreateStructuredBufferFromData( Device, ClusterDagPackedVertexData.Tangents, InOutModelResource.ClusterDagVertexBuffers[3], L"ClusterDagTangentBuffer", &UploadBatch))
            && (ClusterDagPackedVertexData.Colors.empty()
                || CreateStructuredBufferFromData( Device, ClusterDagPackedVertexData.Colors, InOutModelResource.ClusterDagColorBuffer, L"ClusterDagColorBuffer", &UploadBatch))
            && CreateIndexBufferFromIndices(Device, ClusterDAG->RuntimeHierarchy.PackedIndices, ClusterDagGeometry, &UploadBatch);
        if (bCreatedClusterDagGeometry)
        {
            InOutModelResource.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeBasic;
            InOutModelResource.ClusterDagPackedVertexData = ClusterDagPackedVertexData;
            InOutModelResource.ClusterDagPackedPositionOffset = ClusterDagPackedVertexData.PositionOffset;
            InOutModelResource.ClusterDagPackedPositionScale = ClusterDagPackedVertexData.PositionScale;
            InOutModelResource.ClusterDagPackedConstantUV = DirectX::XMFLOAT4(
                ClusterDagPackedVertexData.ConstantUV.x,
                ClusterDagPackedVertexData.ConstantUV.y,
                0.0f,
                0.0f);
            InOutModelResource.ClusterDagPackedConstantColor = DirectX::XMFLOAT4(
                ClusterDagPackedVertexData.ConstantColor.x,
                ClusterDagPackedVertexData.ConstantColor.y,
                ClusterDagPackedVertexData.ConstantColor.z,
                ClusterDagPackedVertexData.ConstantColor.w);
            InOutModelResource.ClusterDagIndexBuffer = ClusterDagGeometry.IndexBuffer;
            InOutModelResource.ClusterDagIndexCount = ClusterDagGeometry.IndexCount;

            const bool bDagVertexSrvReady = AreAllBindlessIndicesValid(
                    InOutModelResource.ClusterDagVertexBuffers[0].SrvBindlessIndex,
                    InOutModelResource.ClusterDagVertexBuffers[1].SrvBindlessIndex,
                    InOutModelResource.ClusterDagIndexBuffer.SrvBindlessIndex);
            if (!bDagVertexSrvReady)
            {
                InOutModelResource.bUseClusterDagRuntime = false;
                InOutModelResource.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeNone;
            }

            if (InOutModelResource.bUseClusterDagRuntime)
            {
                const std::vector<uint32_t> DebugColors = BuildClusterDagDebugColorTable( ClusterDAG->RuntimeHierarchy, InOutModelResource.ClusterDagIndexCount);
                if (!DebugColors.empty())
                {
                    CreateStructuredBufferFromData( Device, DebugColors, InOutModelResource.ClusterDagDebugColorBuffer, L"ClusterDagDebugColorBuffer", &UploadBatch);
                }
            }
        }
        else
        {
            InOutModelResource.bUseClusterDagRuntime = false;
            InOutModelResource.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeNone;
        }
    }

    void InitializeSkinningForModel(FDX12Device* Device, const FGltfScene& LoadedScene, FSceneModelResource& InOutModelResource)
    {
        if (!(Device && InOutModelResource.GltfSkinIndex >= 0))
        {
            return;
        }

        if (static_cast<size_t>(InOutModelResource.GltfSkinIndex) >= LoadedScene.Skins.size())
        {
            return;
        }

        const FGltfSkin& Skin = LoadedScene.Skins[static_cast<size_t>(InOutModelResource.GltfSkinIndex)];
        InOutModelResource.BoneMatrixCount = static_cast<uint32_t>(Skin.Joints.size());
        if (InOutModelResource.BoneMatrixCount == 0)
        {
            return;
        }

        void* MappedData = nullptr;
        if (!CreateMappedBindlessBuffer(
            Device,
            L"SkinMatrixBuffer",
            CreateStructuredBufferDesc<DirectX::XMFLOAT4X4>(InOutModelResource.BoneMatrixCount),
            InOutModelResource.BoneMatrixBuffer,
            MappedData))
        {
            return;
        }

        InOutModelResource.BoneMatrixBufferMapped = static_cast<uint8_t*>(MappedData);
        const uint64_t BufferSize = sizeof(DirectX::XMFLOAT4X4) * InOutModelResource.BoneMatrixCount;
        std::vector<DirectX::XMFLOAT4X4> IdentityMatrices(InOutModelResource.BoneMatrixCount);
        for (uint32_t JointIndex = 0; JointIndex < InOutModelResource.BoneMatrixCount; ++JointIndex)
        {
            IdentityMatrices[JointIndex] = DirectX::XMFLOAT4X4(
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f);
        }
        std::memcpy(InOutModelResource.BoneMatrixBufferMapped, IdentityMatrices.data(), BufferSize);
        CreateBindlessBufferSrv(Device, InOutModelResource.BoneMatrixBuffer);
        InOutModelResource.bUseSkinning = IsValidBindlessIndex(InOutModelResource.BoneMatrixBuffer.SrvBindlessIndex);
    }

    void PropagateClusterDagOwnerDataToSections(std::vector<FSceneModelResource>& InOutModels, const std::vector<uint32_t>& SectionModelIndices)
    {
        if (SectionModelIndices.empty())
        {
            return;
        }

        FSceneModelResource& OwnerModel = InOutModels[SectionModelIndices.front()];
        if (!OwnerModel.bUseClusterDagRuntime)
        {
            return;
        }

        OwnerModel.ClusterDagSectionModelIndices = SectionModelIndices;
        OwnerModel.bCoveredByClusterDagRuntime = true;
        for (size_t SectionOrdinal = 1; SectionOrdinal < SectionModelIndices.size(); ++SectionOrdinal)
        {
            FSceneModelResource& SectionModel = InOutModels[SectionModelIndices[SectionOrdinal]];
            SectionModel.bCoveredByClusterDagRuntime = true;
            SectionModel.ClusterDagMeshIndex = OwnerModel.ClusterDagMeshIndex;
            SectionModel.ClusterDagPrimitiveIndex = 0u;
            SectionModel.ClusterDagSourceFilePath = OwnerModel.ClusterDagSourceFilePath;
            SectionModel.ClusterDagCacheFilePath = OwnerModel.ClusterDagCacheFilePath;
            SectionModel.ClusterDagVertexPackingMode = OwnerModel.ClusterDagVertexPackingMode;
            SectionModel.ClusterDagPackedVertexData = OwnerModel.ClusterDagPackedVertexData;
            SectionModel.ClusterDagPackedPositionOffset = OwnerModel.ClusterDagPackedPositionOffset;
            SectionModel.ClusterDagPackedPositionScale = OwnerModel.ClusterDagPackedPositionScale;
            SectionModel.ClusterDagPackedConstantUV = OwnerModel.ClusterDagPackedConstantUV;
            SectionModel.ClusterDagPackedConstantColor = OwnerModel.ClusterDagPackedConstantColor;
            SectionModel.ClusterDagVertexBuffers = OwnerModel.ClusterDagVertexBuffers;
            SectionModel.ClusterDagIndexBuffer = OwnerModel.ClusterDagIndexBuffer;
            SectionModel.ClusterDagColorBuffer = OwnerModel.ClusterDagColorBuffer;
            SectionModel.ClusterDagDebugColorBuffer = OwnerModel.ClusterDagDebugColorBuffer;
            SectionModel.ClusterDagIndexCount = OwnerModel.ClusterDagIndexCount;
        }
    }

}

bool SceneModelResourceLoader::LoadModelsFromJson(
    FDX12Device* Device,
    const std::wstring& SceneFilePath,
    std::vector<FSceneModelResource>& OutModels,
    DirectX::XMFLOAT3& OutSceneCenter,
    float& OutSceneRadius,
    std::vector<FGltfScene>* OutGltfScenes,
    FWorld* OutWorld)
{
    OutModels.clear();
    if (OutWorld)
    {
        OutWorld->Clear();
    }
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

        const int SceneIndex = OutGltfScenes ? static_cast<int>(OutGltfScenes->size()) : -1;

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
            const FClusterDAG* WholeMeshClusterDAG = Mesh.GetClusterDAG(0);
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
            std::vector<uint32_t> SectionModelIndices;
            SectionModelIndices.reserve(SectionCount);

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

            for (size_t SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
            {
                const FGltfPrimitiveSection& Section = (*PrimitiveSections)[SectionIndex];

                FSceneModelResource ModelResource;
                InitializeModelResourceFromSection(SectionInputs, Section, SectionIndex, NextObjectId++, ModelResource);
                if (!BuildModelResourceGeometryForSection(Device, SectionInputs, SectionIndex, UploadBatch, ModelResource))
                {
                    continue;
                }

                ConfigureWholeMeshClusterDagRuntimeForSection(Device, SectionInputs, SectionIndex, UploadBatch, ModelResource);
                InitializeSkinningForModel(Device, LoadedScene, ModelResource);

                ModelResource.PipelineKey = RendererUtils::BuildPipelineKey(ModelResource);
                UpdateSceneBounds(ModelResource.Center, ModelResource.Radius, SceneMin, SceneMax);

                SectionModelIndices.push_back((uint32_t)OutModels.size());
                OutModels.push_back(std::move(ModelResource));
            }

            PropagateClusterDagOwnerDataToSections(OutModels, SectionModelIndices);

            // Build the logical world object grouping this node's sections.
            if (OutWorld && !SectionModelIndices.empty())
            {
                const bool bSkeletal = LoadedNode.SkinIndex >= 0;
                std::unique_ptr<IObject> Object;
                if (bSkeletal)
                {
                    auto Skeletal = std::make_unique<FSkeletalMesh>();
                    Skeletal->SetGltfSkinIndex(LoadedNode.SkinIndex);
                    Object = std::move(Skeletal);
                }
                else
                {
                    Object = std::make_unique<FStaticMesh>();
                }

                Object->SetName(BaseName);
                Object->SetGltfIndices(SceneIndex, LoadedNode.NodeIndex, static_cast<int>(MeshIndex));
                Object->GetSectionModelIndices() = SectionModelIndices;
                Object->SetObjectId(OutModels[SectionModelIndices.front()].ObjectId);
                DirectX::XMFLOAT4X4 ObjectWorldMatrix;
                DirectX::XMStoreFloat4x4(&ObjectWorldMatrix, World);
                Object->SetWorldMatrix(ObjectWorldMatrix);
                OutWorld->AddObject(std::move(Object));
            }
        }

        if (OutGltfScenes)
        {
            OutGltfScenes->push_back(std::move(LoadedScene));
        }

    }

    if (UploadBatch.bInitialized)
    {
        UploadBatch.ExecuteAndFlush();
    }

    FRayTracingRuntime::BuildSceneModelBlas(Device, OutModels);

    if (OutModels.empty())
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
