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
#include "../Core/Logger.h"
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

    std::string PathToUtf8String(const std::filesystem::path& Path)
    {
        const auto Utf8 = Path.u8string();
        return std::string(Utf8.begin(), Utf8.end());
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

    uint32_t CreateStructuredBufferSrv(FDX12Device* Device, ID3D12Resource* Buffer, uint32_t Stride)
    {
        if (!Buffer || Stride == 0)
        {
            return UINT32_MAX;
        }

        const D3D12_RESOURCE_DESC BufferDesc = Buffer->GetDesc();
        const uint64_t ElementCount = BufferDesc.Width / Stride;
        if (ElementCount == 0)
        {
            return UINT32_MAX;
        }

        return Device->CreateBindlessSrv(Buffer,
            CD3DX12_SHADER_RESOURCE_VIEW_DESC::StructuredBuffer(static_cast<UINT>(ElementCount), Stride));
    }

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

        const std::array<const void*, 7> StreamData =
        {
            Positions->data(),
            Normals->data(),
            UVs->data(),
            Tangents->data(),
            Colors->data(),
            Joints->data(),
            Weights->data()
        };

        const std::array<UINT, 7> StreamStrides =
        {
            static_cast<UINT>(sizeof(FFloat3)),
            static_cast<UINT>(sizeof(FFloat3)),
            static_cast<UINT>(sizeof(FFloat2)),
            static_cast<UINT>(sizeof(FFloat4)),
            static_cast<UINT>(sizeof(FFloat4)),
            static_cast<UINT>(sizeof(FUInt4)),
            static_cast<UINT>(sizeof(FFloat4))
        };

        const std::array<UINT, 7> StreamSizes =
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

        D3D12_HEAP_PROPERTIES DefaultHeap = {};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        DefaultHeap.CreationNodeMask = 1;
        DefaultHeap.VisibleNodeMask = 1;

        D3D12_HEAP_PROPERTIES UploadHeap = {};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        UploadHeap.CreationNodeMask = 1;
        UploadHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC BufferDesc = {};
        BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufferDesc.Height = 1;
        BufferDesc.DepthOrArraySize = 1;
        BufferDesc.MipLevels = 1;
        BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        BufferDesc.SampleDesc.Count = 1;
        BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        OutGeometry.VertexBufferCount = static_cast<uint32_t>(StreamData.size());
        OutGeometry.VertexBufferViews.fill({});

        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 7> UploadBuffers;

        for (size_t StreamIndex = 0; StreamIndex < StreamData.size(); ++StreamIndex)
        {
            BufferDesc.Width = StreamSizes[StreamIndex];
            HR_CHECK(Device->GetDevice()->CreateCommittedResource(
                &DefaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &BufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(OutGeometry.VertexBuffers[StreamIndex].ReleaseAndGetAddressOf())));
            if (OutGeometry.VertexBuffers[StreamIndex])
            {
                const std::wstring BufferName = L"PrimitiveVertexBuffer_" + std::to_wstring(StreamIndex);
                OutGeometry.VertexBuffers[StreamIndex]->SetName(BufferName.c_str());
            }

            HR_CHECK(Device->GetDevice()->CreateCommittedResource(
                &UploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &BufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(UploadBuffers[StreamIndex].ReleaseAndGetAddressOf())));

            void* VertexData = nullptr;
            D3D12_RANGE EmptyRange = { 0, 0 };
            HR_CHECK(UploadBuffers[StreamIndex]->Map(0, &EmptyRange, &VertexData));
            memcpy(VertexData, StreamData[StreamIndex], StreamSizes[StreamIndex]);
            UploadBuffers[StreamIndex]->Unmap(0, nullptr);

            OutGeometry.VertexBufferViews[StreamIndex].BufferLocation = OutGeometry.VertexBuffers[StreamIndex]->GetGPUVirtualAddress();
            OutGeometry.VertexBufferViews[StreamIndex].StrideInBytes = StreamStrides[StreamIndex];
            OutGeometry.VertexBufferViews[StreamIndex].SizeInBytes = StreamSizes[StreamIndex];
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> IndexUploadBuffer;
        if (bCreateIndexBuffer)
        {
            BufferDesc.Width = IndexBufferSize;
            HR_CHECK(Device->GetDevice()->CreateCommittedResource(
                &DefaultHeap,
                D3D12_HEAP_FLAG_NONE,
                &BufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(OutGeometry.IndexBuffer.GetAddressOf())));
            if (OutGeometry.IndexBuffer)
            {
                OutGeometry.IndexBuffer->SetName(L"PrimitiveIndexBuffer");
            }

            OutGeometry.IndexBufferView.BufferLocation = OutGeometry.IndexBuffer->GetGPUVirtualAddress();
            OutGeometry.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
            OutGeometry.IndexBufferView.SizeInBytes = IndexBufferSize;

            HR_CHECK(Device->GetDevice()->CreateCommittedResource(
                &UploadHeap,
                D3D12_HEAP_FLAG_NONE,
                &BufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(IndexUploadBuffer.ReleaseAndGetAddressOf())));

            void* IndexData = nullptr;
            D3D12_RANGE EmptyRange = { 0, 0 };
            HR_CHECK(IndexUploadBuffer->Map(0, &EmptyRange, &IndexData));
            memcpy(IndexData, Primitive.Indices.data(), IndexBufferSize);
            IndexUploadBuffer->Unmap(0, nullptr);
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
            ActiveBatch->AddUploadBuffer(std::move(UploadBuffers[StreamIndex]));
        }

        if (bCreateIndexBuffer && IndexUploadBuffer)
        {
            CommandList->CopyBufferRegion(OutGeometry.IndexBuffer.Get(), 0, IndexUploadBuffer.Get(), 0, IndexBufferSize);
            ActiveBatch->Context.TransitionResource(OutGeometry.IndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
            ActiveBatch->AddUploadBuffer(std::move(IndexUploadBuffer));
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

    float ComputeMaxScale(const DirectX::XMFLOAT4X4& Matrix)
    {
        const float ScaleX = std::sqrt(Matrix._11 * Matrix._11 + Matrix._21 * Matrix._21 + Matrix._31 * Matrix._31);
        const float ScaleY = std::sqrt(Matrix._12 * Matrix._12 + Matrix._22 * Matrix._22 + Matrix._32 * Matrix._32);
        const float ScaleZ = std::sqrt(Matrix._13 * Matrix._13 + Matrix._23 * Matrix._23 + Matrix._33 * Matrix._33);

        return (std::max)((std::max)(ScaleX, ScaleY), ScaleZ);
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

        D3D12_HEAP_PROPERTIES DefaultHeap = {};
        DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        DefaultHeap.CreationNodeMask = 1;
        DefaultHeap.VisibleNodeMask = 1;

        D3D12_HEAP_PROPERTIES UploadHeap = {};
        UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        UploadHeap.CreationNodeMask = 1;
        UploadHeap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC BufferDesc = {};
        BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        BufferDesc.Width = IndexBufferSize;
        BufferDesc.Height = 1;
        BufferDesc.DepthOrArraySize = 1;
        BufferDesc.MipLevels = 1;
        BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        BufferDesc.SampleDesc.Count = 1;
        BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &DefaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &BufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(InOutGeometry.IndexBuffer.ReleaseAndGetAddressOf())));

        if (InOutGeometry.IndexBuffer)
        {
            InOutGeometry.IndexBuffer->SetName(L"PrimitiveIndexBuffer");
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> UploadBuffer;
        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &UploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &BufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(UploadBuffer.ReleaseAndGetAddressOf())));

        InOutGeometry.IndexBufferView.BufferLocation = InOutGeometry.IndexBuffer->GetGPUVirtualAddress();
        InOutGeometry.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
        InOutGeometry.IndexBufferView.SizeInBytes = IndexBufferSize;
        InOutGeometry.IndexCount = static_cast<uint32_t>(Indices.size());

        void* IndexData = nullptr;
        D3D12_RANGE EmptyRange = { 0, 0 };
        HR_CHECK(UploadBuffer->Map(0, &EmptyRange, &IndexData));
        memcpy(IndexData, Indices.data(), IndexBufferSize);
        UploadBuffer->Unmap(0, nullptr);

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
        ActiveBatch->AddUploadBuffer(std::move(UploadBuffer));

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
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutBuffer,
        const wchar_t* Name,
        FUploadBatch* UploadBatch = nullptr)
    {
        if (!Device || Data.empty())
        {
            return false;
        }

        const uint64_t BufferSize = static_cast<uint64_t>(sizeof(T)) * Data.size();

        FBindlessBuffer DefaultBuffer;
        FUploadBuffer UploadBufferObj;
        const std::wstring BufferName = Name ? std::wstring(Name) : std::wstring();
        CreateBindlessBufferWithUpload(
            Device,
            BufferName,
            CreateStructuredBufferDesc<T>(Data.size()),
            D3D12_RESOURCE_STATE_COPY_DEST,
            DefaultBuffer,
            UploadBufferObj,
            Data.data(),
            false,
            false);
        OutBuffer = std::move(DefaultBuffer.Resource);

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

}

bool SceneModelResourceLoader::LoadModelsFromJson(
    FDX12Device* Device,
    const std::wstring& SceneFilePath,
    std::vector<FSceneModelResource>& OutModels,
    DirectX::XMFLOAT3& OutSceneCenter,
    float& OutSceneRadius,
    std::vector<FGltfScene>* OutGltfScenes)
{
    OutModels.clear();
    uint32_t NextObjectId = 1;

    const std::filesystem::path ScenePath(SceneFilePath);
    const std::string ScenePathUtf8 = PathToUtf8String(ScenePath);

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
            LogError("Failed to load mesh from scene: " + PathToUtf8String(MeshPath));
            continue;
        }

        const int SceneIndex = OutGltfScenes ? static_cast<int>(OutGltfScenes->size()) : -1;

        if (LoadedScene.Meshes.empty())
        {
            LogError("No meshes found in glTF: " + PathToUtf8String(MeshPath));
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

            const float NodeScale = ComputeMaxScale(LoadedNode.WorldMatrix);

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

            for (size_t SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
            {
                const FGltfPrimitiveSection& Section = (*PrimitiveSections)[SectionIndex];

                FSceneModelResource ModelResource;
                ModelResource.DrawIndexStart = Section.IndexStart;
                ModelResource.DrawIndexCount = Section.IndexCount;
                ModelResource.BaseIndexCount = Section.IndexCount;

                XMStoreFloat4x4(&ModelResource.WorldMatrix, World);
                XMStoreFloat4x4(&ModelResource.ModelTransform, ModelTransform);
                ModelResource.GltfSceneIndex = SceneIndex;
                ModelResource.GltfNodeIndex = LoadedNode.NodeIndex;
                ModelResource.GltfMeshIndex = static_cast<int>(MeshIndex);
                ModelResource.GltfSkinIndex = LoadedNode.SkinIndex;

                const XMVECTOR CenterVec = XMVector3TransformCoord(XMVectorSet(MeshCenter.x, MeshCenter.y, MeshCenter.z, 1.0f), World);
                XMStoreFloat3(&ModelResource.Center, CenterVec);
                ModelResource.Radius = MeshRadius * NodeScale;

                if (SectionCount > 1)
                {
                    ModelResource.Name = BaseName + "_Prim" + std::to_string(SectionIndex);
                }
                else
                {
                    ModelResource.Name = BaseName;
                }

                ModelResource.ObjectId = NextObjectId++;

                XMStoreFloat3(&ModelResource.BoundsMin, BoundsMin);
                XMStoreFloat3(&ModelResource.BoundsMax, BoundsMax);

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

                ModelResource.BaseColorTexturePath = Model.BaseColorTexturePath.empty() ? BaseColorPath : Model.BaseColorTexturePath;
                ModelResource.MetallicRoughnessTexturePath = Model.MetallicRoughnessTexturePath.empty() ? MetallicRoughnessPath : Model.MetallicRoughnessTexturePath;
                ModelResource.NormalTexturePath = Model.NormalTexturePath.empty() ? NormalPath : Model.NormalTexturePath;
                ModelResource.EmissiveTexturePath = Model.EmissiveTexturePath.empty() ? EmissivePath : Model.EmissiveTexturePath;
                ModelResource.SheenColorTexturePath = SheenColorPath;
                ModelResource.SheenRoughnessTexturePath = SheenRoughnessPath;
                ModelResource.ClearcoatTexturePath = ClearcoatPath;
                ModelResource.ClearcoatRoughnessTexturePath = ClearcoatRoughnessPath;
                ModelResource.ClearcoatNormalTexturePath = ClearcoatNormalPath;
                ModelResource.AnisotropyTexturePath = AnisotropyPath;
                ModelResource.BaseColorFactor = Material.BaseColorFactor;
                ModelResource.BaseColorAlpha = Material.BaseColorAlpha;
                ModelResource.MetallicFactor = Material.MetallicFactor;
                ModelResource.RoughnessFactor = Material.RoughnessFactor;
                ModelResource.EmissiveFactor = Material.EmissiveFactor;
                ModelResource.SheenColorFactor = Material.SheenColorFactor;
                ModelResource.SheenRoughnessFactor = Material.SheenRoughnessFactor;
                ModelResource.ClearcoatFactor = Material.ClearcoatFactor;
                ModelResource.ClearcoatRoughnessFactor = Material.ClearcoatRoughnessFactor;
                ModelResource.AnisotropyStrength = Material.AnisotropyStrength;
                ModelResource.AnisotropyRotation = Material.AnisotropyRotation;
                ModelResource.AlphaCutoff = Material.AlphaCutoff;
                ModelResource.AlphaMode = static_cast<uint32_t>(Material.bAlphaBlend
                    ? EAlphaMode::Blend
                    : (Material.bAlphaMask ? EAlphaMode::Mask : EAlphaMode::Opaque));
                ModelResource.bDoubleSided = Material.bDoubleSided;
                ModelResource.ShadingModelId = Material.ShadingModelId;
                ModelResource.bHasNormalMap = !ModelResource.NormalTexturePath.empty();

                ModelResource.BaseColorTransformOffsetScale = BuildOffsetScale(Material.BaseColorTransform);
                ModelResource.BaseColorTransformRotation = BuildRotationConstants(Material.BaseColorTransform);
                ModelResource.MetallicRoughnessTransformOffsetScale = BuildOffsetScale(Material.MetallicRoughnessTransform);
                ModelResource.MetallicRoughnessTransformRotation = BuildRotationConstants(Material.MetallicRoughnessTransform);
                ModelResource.NormalTransformOffsetScale = BuildOffsetScale(Material.NormalTransform);
                ModelResource.NormalTransformRotation = BuildRotationConstants(Material.NormalTransform);
                ModelResource.EmissiveTransformOffsetScale = BuildOffsetScale(Material.EmissiveTransform);
                ModelResource.EmissiveTransformRotation = BuildRotationConstants(Material.EmissiveTransform);
                ModelResource.SheenColorTransformOffsetScale = BuildOffsetScale(Material.SheenColorTransform);
                ModelResource.SheenColorTransformRotation = BuildRotationConstants(Material.SheenColorTransform);
                ModelResource.SheenRoughnessTransformOffsetScale = BuildOffsetScale(Material.SheenRoughnessTransform);
                ModelResource.SheenRoughnessTransformRotation = BuildRotationConstants(Material.SheenRoughnessTransform);
                ModelResource.ClearcoatTransformOffsetScale = BuildOffsetScale(Material.ClearcoatTransform);
                ModelResource.ClearcoatTransformRotation = BuildRotationConstants(Material.ClearcoatTransform);
                ModelResource.ClearcoatRoughnessTransformOffsetScale = BuildOffsetScale(Material.ClearcoatRoughnessTransform);
                ModelResource.ClearcoatRoughnessTransformRotation = BuildRotationConstants(Material.ClearcoatRoughnessTransform);
                ModelResource.ClearcoatNormalTransformOffsetScale = BuildOffsetScale(Material.ClearcoatNormalTransform);
                ModelResource.ClearcoatNormalTransformRotation = BuildRotationConstants(Material.ClearcoatNormalTransform);
                ModelResource.AnisotropyTransformOffsetScale = BuildOffsetScale(Material.AnisotropyTransform);
                ModelResource.AnisotropyTransformRotation = BuildRotationConstants(Material.AnisotropyTransform);

                if (SectionIndex >= MeshPrimitives.size())
                {
                    continue;
                }

                const FMesh::FMeshletGroup* MeshletGroup = Mesh.GetMeshletGroup(SectionIndex);
                const bool bUseMeshletIndices = Mesh.IsMeshletIndexingAllowed() && MeshletGroup && !MeshletGroup->MeshletIndices.empty();
                if (!CreatePrimitiveGeometry(Device, MeshPrimitives[SectionIndex], ModelResource.Geometry, !bUseMeshletIndices, &UploadBatch))
                {
                    LogError("Failed to create primitive geometry for scene mesh: " + PathToUtf8String(MeshPath));
                    continue;
                }

                if (bUseMeshletIndices)
                {
                    if (CreateIndexBufferFromIndices(Device, MeshletGroup->MeshletIndices, ModelResource.Geometry, &UploadBatch))
                    {
                        ModelResource.bUseMeshletCulling = true;
                        ModelResource.Meshlets = MeshletGroup->Meshlets;
                        ModelResource.MeshletBounds = MeshletGroup->MeshletBounds;
                        ModelResource.MeshletIndices = MeshletGroup->MeshletIndices;
                        ModelResource.DrawIndexStart = 0;
                        ModelResource.DrawIndexCount = static_cast<uint32_t>(MeshletGroup->MeshletIndices.size());
                        ModelResource.BaseIndexCount = ModelResource.DrawIndexCount;
                    }
                }
                else
                {
                    ModelResource.DrawIndexStart = 0;
                    ModelResource.DrawIndexCount = static_cast<uint32_t>(MeshPrimitives[SectionIndex].Indices.size());
                    ModelResource.BaseIndexCount = ModelResource.DrawIndexCount;
                }

                if (Device && Device->GetBindlessDescriptorHeap())
                {
                    ModelResource.VertexBufferBindlessIndices[0] = CreateStructuredBufferSrv(Device, ModelResource.Geometry.VertexBuffers[0].Get(), sizeof(FFloat3));
                    ModelResource.VertexBufferBindlessIndices[1] = CreateStructuredBufferSrv(Device, ModelResource.Geometry.VertexBuffers[1].Get(), sizeof(FFloat3));
                    ModelResource.VertexBufferBindlessIndices[2] = CreateStructuredBufferSrv(Device, ModelResource.Geometry.VertexBuffers[2].Get(), sizeof(FFloat2));
                    ModelResource.VertexBufferBindlessIndices[3] = CreateStructuredBufferSrv(Device, ModelResource.Geometry.VertexBuffers[3].Get(), sizeof(FFloat4));
                    ModelResource.VertexBufferBindlessIndices[4] = CreateStructuredBufferSrv(Device, ModelResource.Geometry.VertexBuffers[4].Get(), sizeof(FFloat4));
                    ModelResource.VertexBufferBindlessIndices[5] = CreateStructuredBufferSrv(Device, ModelResource.Geometry.VertexBuffers[5].Get(), sizeof(FUInt4));
                    ModelResource.VertexBufferBindlessIndices[6] = CreateStructuredBufferSrv(Device, ModelResource.Geometry.VertexBuffers[6].Get(), sizeof(FFloat4));
                    ModelResource.IndexBufferBindlessIndex = CreateStructuredBufferSrv(Device, ModelResource.Geometry.IndexBuffer.Get(), sizeof(uint32_t));
                }

                if (const FClusterDAG* ClusterDAG = Mesh.GetClusterDAG(SectionIndex))
                {
                    ModelResource.ClusterDagMeshIndex = static_cast<uint32_t>(MeshIndex);
                    ModelResource.ClusterDagPrimitiveIndex = static_cast<uint32_t>(SectionIndex);
                    ModelResource.bUseClusterDagRuntime =
                        ClusterDAG->HasRuntimeHierarchy()
                        && LoadedNode.SkinIndex < 0
                        && !Section.Material.bAlphaBlend
                        && !Section.Material.bAlphaMask
                        && !ClusterDAG->RuntimeHierarchy.PackedIndices.empty();
                    ModelResource.ClusterDagRuntimeHierarchy = ClusterDAG->RuntimeHierarchy;

                    if (ModelResource.bUseClusterDagRuntime)
                    {
                        FClusterDAGPackedVertexData ClusterDagPackedVertexData;
                        ModelResource.bUseClusterDagRuntime =
                            BuildClusterDagPackedVertexStreams(
                                *ClusterDAG,
                                ClusterDagPackedVertexData);

                        FMeshGeometryBuffers ClusterDagGeometry = {};
                        const bool bCreatedClusterDagGeometry =
                            ModelResource.bUseClusterDagRuntime
                            && CreateStructuredBufferFromData(
                                Device,
                                ClusterDagPackedVertexData.Positions,
                                ModelResource.ClusterDagVertexBuffers[0],
                                L"ClusterDagPositionBuffer",
                                &UploadBatch)
                            && CreateStructuredBufferFromData(
                                Device,
                                ClusterDagPackedVertexData.Normals,
                                ModelResource.ClusterDagVertexBuffers[1],
                                L"ClusterDagNormalBuffer",
                                &UploadBatch)
                            && (ClusterDagPackedVertexData.UVs.empty()
                                || CreateStructuredBufferFromData(
                                    Device,
                                    ClusterDagPackedVertexData.UVs,
                                    ModelResource.ClusterDagVertexBuffers[2],
                                    L"ClusterDagUvBuffer",
                                    &UploadBatch))
                            && (ClusterDagPackedVertexData.Tangents.empty()
                                || CreateStructuredBufferFromData(
                                    Device,
                                    ClusterDagPackedVertexData.Tangents,
                                    ModelResource.ClusterDagVertexBuffers[3],
                                    L"ClusterDagTangentBuffer",
                                    &UploadBatch))
                            && (ClusterDagPackedVertexData.Colors.empty()
                                || CreateStructuredBufferFromData(
                                    Device,
                                    ClusterDagPackedVertexData.Colors,
                                    ModelResource.ClusterDagColorBuffer,
                                    L"ClusterDagColorBuffer",
                                    &UploadBatch))
                            && CreateIndexBufferFromIndices(Device, ClusterDAG->RuntimeHierarchy.PackedIndices, ClusterDagGeometry, &UploadBatch);
                        if (bCreatedClusterDagGeometry)
                        {
                            ModelResource.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeBasic;
                            ModelResource.ClusterDagPackedPositionOffset = ClusterDagPackedVertexData.PositionOffset;
                            ModelResource.ClusterDagPackedPositionScale = ClusterDagPackedVertexData.PositionScale;
                            ModelResource.ClusterDagPackedConstantUV = DirectX::XMFLOAT4(
                                ClusterDagPackedVertexData.ConstantUV.x,
                                ClusterDagPackedVertexData.ConstantUV.y,
                                0.0f,
                                0.0f);
                            ModelResource.ClusterDagPackedConstantColor = DirectX::XMFLOAT4(
                                ClusterDagPackedVertexData.ConstantColor.x,
                                ClusterDagPackedVertexData.ConstantColor.y,
                                ClusterDagPackedVertexData.ConstantColor.z,
                                ClusterDagPackedVertexData.ConstantColor.w);
                            ModelResource.ClusterDagIndexBuffer = ClusterDagGeometry.IndexBuffer;
                            ModelResource.ClusterDagIndexCount = ClusterDagGeometry.IndexCount;
                            if (Device && Device->GetBindlessDescriptorHeap())
                            {
                                ModelResource.ClusterDagVertexBufferBindlessIndices[0] = CreateStructuredBufferSrv(
                                    Device,
                                    ModelResource.ClusterDagVertexBuffers[0].Get(),
                                    sizeof(FClusterDagPackedPosition));
                                ModelResource.ClusterDagVertexBufferBindlessIndices[1] = CreateStructuredBufferSrv(
                                    Device,
                                    ModelResource.ClusterDagVertexBuffers[1].Get(),
                                    sizeof(uint32_t));
                                if (ModelResource.ClusterDagVertexBuffers[2])
                                {
                                    ModelResource.ClusterDagVertexBufferBindlessIndices[2] = CreateStructuredBufferSrv(
                                        Device,
                                        ModelResource.ClusterDagVertexBuffers[2].Get(),
                                        sizeof(uint32_t));
                                }
                                if (ModelResource.ClusterDagVertexBuffers[3])
                                {
                                    ModelResource.ClusterDagVertexBufferBindlessIndices[3] = CreateStructuredBufferSrv(
                                        Device,
                                        ModelResource.ClusterDagVertexBuffers[3].Get(),
                                        sizeof(uint32_t));
                                }
                                if (ModelResource.ClusterDagColorBuffer)
                                {
                                    ModelResource.ClusterDagColorBufferBindlessIndex = CreateStructuredBufferSrv(
                                        Device,
                                        ModelResource.ClusterDagColorBuffer.Get(),
                                        sizeof(uint32_t));
                                }
                                ModelResource.ClusterDagIndexBufferBindlessIndex = CreateStructuredBufferSrv(
                                    Device,
                                    ModelResource.ClusterDagIndexBuffer.Get(),
                                    sizeof(uint32_t));

                                const bool bDagVertexSrvReady =
                                    AreAllBindlessIndicesValid(
                                        ModelResource.ClusterDagVertexBufferBindlessIndices[0],
                                        ModelResource.ClusterDagVertexBufferBindlessIndices[1],
                                        ModelResource.ClusterDagIndexBufferBindlessIndex);
                                if (!bDagVertexSrvReady)
                                {
                                    ModelResource.bUseClusterDagRuntime = false;
                                    ModelResource.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeNone;
                                }
                            }

                            if (ModelResource.bUseClusterDagRuntime && Device && Device->GetBindlessDescriptorHeap())
                            {
                                const std::vector<uint32_t> DebugColors = BuildClusterDagDebugColorTable(
                                    ClusterDAG->RuntimeHierarchy,
                                    ModelResource.ClusterDagIndexCount);
                                if (!DebugColors.empty()
                                    && CreateStructuredBufferFromData(
                                        Device,
                                        DebugColors,
                                        ModelResource.ClusterDagDebugColorBuffer,
                                        L"ClusterDagDebugColorBuffer",
                                        &UploadBatch))
                                {
                                    ModelResource.ClusterDagDebugColorBufferBindlessIndex = CreateStructuredBufferSrv(
                                        Device,
                                        ModelResource.ClusterDagDebugColorBuffer.Get(),
                                        sizeof(uint32_t));
                                }
                            }
                        }
                        else
                        {
                            ModelResource.bUseClusterDagRuntime = false;
                            ModelResource.ClusterDagVertexPackingMode = GClusterDagVertexPackingModeNone;
                        }
                    }
                }

                if (Device && Device->GetBindlessDescriptorHeap() && ModelResource.GltfSkinIndex >= 0)
                {
                    if (static_cast<size_t>(ModelResource.GltfSkinIndex) < LoadedScene.Skins.size())
                    {
                        const FGltfSkin& Skin = LoadedScene.Skins[static_cast<size_t>(ModelResource.GltfSkinIndex)];
                        ModelResource.BoneMatrixCount = static_cast<uint32_t>(Skin.Joints.size());
                        if (ModelResource.BoneMatrixCount > 0)
                        {
                            const uint64_t BufferSize = sizeof(DirectX::XMFLOAT4X4) * ModelResource.BoneMatrixCount;
                            D3D12_HEAP_PROPERTIES UploadHeap = {};
                            UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
                            UploadHeap.CreationNodeMask = 1;
                            UploadHeap.VisibleNodeMask = 1;

                            D3D12_RESOURCE_DESC BufferDesc = {};
                            BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                            BufferDesc.Width = BufferSize;
                            BufferDesc.Height = 1;
                            BufferDesc.DepthOrArraySize = 1;
                            BufferDesc.MipLevels = 1;
                            BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
                            BufferDesc.SampleDesc.Count = 1;
                            BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                            HR_CHECK(Device->GetDevice()->CreateCommittedResource(
                                &UploadHeap,
                                D3D12_HEAP_FLAG_NONE,
                                &BufferDesc,
                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                nullptr,
                                IID_PPV_ARGS(ModelResource.BoneMatrixBuffer.ReleaseAndGetAddressOf())));

                            if (ModelResource.BoneMatrixBuffer)
                            {
                                ModelResource.BoneMatrixBuffer->SetName(L"SkinMatrixBuffer");
                                D3D12_RANGE EmptyRange = { 0, 0 };
                                HR_CHECK(ModelResource.BoneMatrixBuffer->Map(
                                    0,
                                    &EmptyRange,
                                    reinterpret_cast<void**>(&ModelResource.BoneMatrixBufferMapped)));
                                if (ModelResource.BoneMatrixBufferMapped)
                                {
                                    std::vector<DirectX::XMFLOAT4X4> IdentityMatrices(ModelResource.BoneMatrixCount);
                                    for (uint32_t JointIndex = 0; JointIndex < ModelResource.BoneMatrixCount; ++JointIndex)
                                    {
                                        IdentityMatrices[JointIndex] = DirectX::XMFLOAT4X4(
                                            1.0f, 0.0f, 0.0f, 0.0f,
                                            0.0f, 1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 1.0f, 0.0f,
                                            0.0f, 0.0f, 0.0f, 1.0f);
                                    }
                                    std::memcpy(ModelResource.BoneMatrixBufferMapped, IdentityMatrices.data(), BufferSize);
                                }
                            }

                            ModelResource.BoneMatrixBindlessIndex = CreateStructuredBufferSrv(
                                Device,
                                ModelResource.BoneMatrixBuffer.Get(),
                                sizeof(DirectX::XMFLOAT4X4));

                            ModelResource.bUseSkinning = IsValidBindlessIndex(ModelResource.BoneMatrixBindlessIndex);
                        }
                    }
                }

                UpdateSceneBounds(ModelResource.Center, ModelResource.Radius, SceneMin, SceneMax);

                OutModels.push_back(std::move(ModelResource));
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
