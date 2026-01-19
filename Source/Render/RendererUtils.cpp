#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RendererUtils.h"

#include "../Scene/Mesh.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/SceneJsonLoader.h"
#include "../Scene/Camera.h"
#include "../Core/Logger.h"
#include "ShaderCompiler.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12Commons.h"
#include "../RHI/DX12CommandContext.h"
#include "../RHI/DX12CommandQueue.h"
#include "../RHI/RayTracing.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <array>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace
{
    std::string PathToUtf8String(const std::filesystem::path& Path)
    {
        const auto Utf8 = Path.u8string();
        return std::string(Utf8.begin(), Utf8.end());
    }

    DirectX::XMFLOAT4 BuildOffsetScale(const FGltfTextureTransform& Transform)
    {
        return DirectX::XMFLOAT4(Transform.Offset.x, Transform.Offset.y, Transform.Scale.x, Transform.Scale.y);
    }

    DirectX::XMFLOAT4 BuildRotationConstants(const FGltfTextureTransform& Transform)
    {
        const float CosR = std::cos(Transform.Rotation);
        const float SinR = std::sin(Transform.Rotation);
        return DirectX::XMFLOAT4(CosR, SinR, 0.0f, 0.0f);
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
        if (!Device || !Device->GetBindlessDescriptorHeap() || !Buffer || Stride == 0)
        {
            return UINT32_MAX;
        }

        const D3D12_RESOURCE_DESC BufferDesc = Buffer->GetDesc();
        const uint64_t ElementCount = BufferDesc.Width / Stride;
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

}

std::wstring RendererUtils::BuildShaderTarget(const wchar_t* StagePrefix, D3D_SHADER_MODEL ShaderModel)
{
    uint32_t ShaderModelValue = static_cast<uint32_t>(ShaderModel);
    uint32_t Major = (ShaderModelValue >> 4) & 0xF;
    uint32_t Minor = ShaderModelValue & 0xF;

    return std::wstring(StagePrefix) + L"_" + std::to_wstring(Major) + L"_" + std::to_wstring(Minor);
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

namespace
{
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

    bool IsSphereInCameraFrustum(const DirectX::XMVECTOR Planes[6], const DirectX::XMFLOAT3& Center, float Radius)
    {
        using namespace DirectX;
        const XMVECTOR CenterVec = XMLoadFloat3(&Center);
        for (size_t PlaneIndex = 0; PlaneIndex < 6; ++PlaneIndex)
        {
            const XMVECTOR Plane = Planes[PlaneIndex];
            const float Distance = XMVectorGetX(XMPlaneDotCoord(Plane, CenterVec));
            if (Distance < -Radius)
            {
                return false;
            }
        }

        return true;
    }

    uint64_t AlignRayTracingBufferSize(uint64_t Size)
    {
        const uint64_t Alignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
        return (Size + Alignment - 1) & ~(Alignment - 1);
    }

    bool CreateRayTracingBuffer(
        FDX12Device* Device,
        uint64_t SizeInBytes,
        D3D12_RESOURCE_FLAGS Flags,
        D3D12_RESOURCE_STATES InitialState,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutResource,
        const wchar_t* Name)
    {
        if (!Device || SizeInBytes == 0)
        {
            return false;
        }

        D3D12_HEAP_PROPERTIES HeapProps = {};
        HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC Desc = {};
        Desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        Desc.Width = AlignRayTracingBufferSize(SizeInBytes);
        Desc.Height = 1;
        Desc.DepthOrArraySize = 1;
        Desc.MipLevels = 1;
        Desc.SampleDesc.Count = 1;
        Desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Desc.Flags = Flags;

        HR_CHECK(Device->GetDevice()->CreateCommittedResource(
            &HeapProps,
            D3D12_HEAP_FLAG_NONE,
            &Desc,
            InitialState,
            nullptr,
            IID_PPV_ARGS(OutResource.ReleaseAndGetAddressOf())));

        if (OutResource && Name)
        {
            OutResource->SetName(Name);
        }

        return OutResource != nullptr;
    }

    bool BuildSceneModelBlas(FDX12Device* Device, std::vector<FSceneModelResource>& Models)
    {
        if (!Device || !Device->IsRayTracingSupported())
        {
            return true;
        }

        FRayTracingDevice RayTracingDevice;
        if (!Device->CreateRayTracingDevice(RayTracingDevice))
        {
            LogWarning("Ray tracing device unavailable; BLAS build skipped.");
            return true;
        }

        ComPtr<ID3D12CommandAllocator> Allocator;
        HR_CHECK(Device->GetDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(Allocator.ReleaseAndGetAddressOf())));

        ComPtr<ID3D12GraphicsCommandList> CommandList;
        HR_CHECK(Device->GetDevice()->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            Allocator.Get(),
            nullptr,
            IID_PPV_ARGS(CommandList.ReleaseAndGetAddressOf())));

        ComPtr<ID3D12GraphicsCommandList4> CommandList4;
        if (FAILED(CommandList.As(&CommandList4)))
        {
            LogWarning("Ray tracing command list interface not available; BLAS build skipped.");
            return true;
        }

        for (FSceneModelResource& Model : Models)
        {
            if (!Model.Geometry.VertexBuffers[0] || !Model.Geometry.IndexBuffer)
            {
                continue;
            }

            const uint32_t VertexStride = Model.Geometry.VertexBufferViews[0].StrideInBytes;
            const uint32_t VertexCount = VertexStride > 0
                ? (Model.Geometry.VertexBufferViews[0].SizeInBytes / VertexStride)
                : 0;
            const uint32_t IndexCount = Model.Geometry.IndexCount;
            if (VertexCount == 0 || IndexCount == 0)
            {
                continue;
            }

            D3D12_RAYTRACING_GEOMETRY_DESC GeometryDesc = {};
            GeometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            GeometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            GeometryDesc.Triangles.VertexBuffer.StartAddress = Model.Geometry.VertexBuffers[0]->GetGPUVirtualAddress();
            GeometryDesc.Triangles.VertexBuffer.StrideInBytes = VertexStride;
            GeometryDesc.Triangles.VertexCount = VertexCount;
            GeometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            GeometryDesc.Triangles.IndexBuffer = Model.Geometry.IndexBuffer->GetGPUVirtualAddress();
            GeometryDesc.Triangles.IndexCount = IndexCount;
            GeometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs = {};
            Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            Inputs.NumDescs = 1;
            Inputs.pGeometryDescs = &GeometryDesc;
            Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            if (Model.bUseSkinning)
            {
                Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
            }

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO PrebuildInfo = {};
            RayTracingDevice.GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&Inputs, &PrebuildInfo);

            if (PrebuildInfo.ResultDataMaxSizeInBytes == 0 || PrebuildInfo.ScratchDataSizeInBytes == 0)
            {
                continue;
            }

            if (!CreateRayTracingBuffer(
                    Device,
                    PrebuildInfo.ResultDataMaxSizeInBytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                    Model.BlasResultBuffer,
                    L"BLAS_Result")
                || !CreateRayTracingBuffer(
                    Device,
                    PrebuildInfo.ScratchDataSizeInBytes,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    Model.BlasScratchBuffer,
                    L"BLAS_Scratch"))
            {
                LogWarning("Failed to allocate BLAS buffers for model: " + Model.Name);
                continue;
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC BuildDesc = {};
            BuildDesc.Inputs = Inputs;
            BuildDesc.DestAccelerationStructureData = Model.BlasResultBuffer->GetGPUVirtualAddress();
            BuildDesc.ScratchAccelerationStructureData = Model.BlasScratchBuffer->GetGPUVirtualAddress();

            CommandList4->BuildRaytracingAccelerationStructure(&BuildDesc, 0, nullptr);

            D3D12_RESOURCE_BARRIER Barrier = {};
            Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            Barrier.UAV.pResource = Model.BlasResultBuffer.Get();
            CommandList4->ResourceBarrier(1, &Barrier);

            Model.BlasGeometryDesc = GeometryDesc;
            Model.bHasRayTracingBlas = true;
        }

        HR_CHECK(CommandList->Close());

        ID3D12CommandList* Lists[] = { CommandList.Get() };
        FDX12CommandQueue* Queue = Device->GetGraphicsQueue();
        if (Queue)
        {
            Queue->ExecuteCommandLists(1, Lists);
            const uint64_t FenceValue = Queue->Signal();
            Queue->Wait(FenceValue);
        }

        return true;
    }
}

bool RendererUtils::CreateSceneModelsFromJson(
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

                ModelResource.BaseColorTexturePath = Model.BaseColorTexturePath.empty() ? BaseColorPath : Model.BaseColorTexturePath;
                ModelResource.MetallicRoughnessTexturePath = Model.MetallicRoughnessTexturePath.empty() ? MetallicRoughnessPath : Model.MetallicRoughnessTexturePath;
                ModelResource.NormalTexturePath = Model.NormalTexturePath.empty() ? NormalPath : Model.NormalTexturePath;
                ModelResource.EmissiveTexturePath = Model.EmissiveTexturePath.empty() ? EmissivePath : Model.EmissiveTexturePath;
                ModelResource.BaseColorFactor = Material.BaseColorFactor;
                ModelResource.BaseColorAlpha = Material.BaseColorAlpha;
                ModelResource.MetallicFactor = Material.MetallicFactor;
                ModelResource.RoughnessFactor = Material.RoughnessFactor;
                ModelResource.EmissiveFactor = Material.EmissiveFactor;
                ModelResource.AlphaCutoff = Material.AlphaCutoff;
                ModelResource.AlphaMode = Material.bAlphaMask ? 1u : 0u;
                ModelResource.bHasNormalMap = !ModelResource.NormalTexturePath.empty();

                ModelResource.BaseColorTransformOffsetScale = BuildOffsetScale(Material.BaseColorTransform);
                ModelResource.BaseColorTransformRotation = BuildRotationConstants(Material.BaseColorTransform);
                ModelResource.MetallicRoughnessTransformOffsetScale = BuildOffsetScale(Material.MetallicRoughnessTransform);
                ModelResource.MetallicRoughnessTransformRotation = BuildRotationConstants(Material.MetallicRoughnessTransform);
                ModelResource.NormalTransformOffsetScale = BuildOffsetScale(Material.NormalTransform);
                ModelResource.NormalTransformRotation = BuildRotationConstants(Material.NormalTransform);
                ModelResource.EmissiveTransformOffsetScale = BuildOffsetScale(Material.EmissiveTransform);
                ModelResource.EmissiveTransformRotation = BuildRotationConstants(Material.EmissiveTransform);

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

                            ModelResource.bUseSkinning = ModelResource.BoneMatrixBindlessIndex != UINT32_MAX;
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

    BuildSceneModelBlas(Device, OutModels);

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

bool RendererUtils::CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format, FDepthResources& OutDepthResources)
{
    if (Device == nullptr)
    {
        return false;
    }

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = Width;
    Desc.Height = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    if (Format == DXGI_FORMAT_D24_UNORM_S8_UINT)
    {
        Desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    }
    else
    {
        Desc.Format = Format;
    }
    Desc.SampleDesc.Count = 1;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = Format;
    ClearValue.DepthStencil.Depth = 0.0f;
    ClearValue.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &ClearValue,
        IID_PPV_ARGS(OutDepthResources.DepthBuffer.GetAddressOf())));

    OutDepthResources.DepthBuffer->SetName(L"DepthBuffer");

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = 1;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(OutDepthResources.DSVHeap.GetAddressOf())));

    OutDepthResources.DepthStencilHandle = OutDepthResources.DSVHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_DEPTH_STENCIL_VIEW_DESC ViewDesc = {};
    ViewDesc.Format = Format;
    ViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ViewDesc.Flags = D3D12_DSV_FLAG_NONE;
    Device->GetDevice()->CreateDepthStencilView(OutDepthResources.DepthBuffer.Get(), &ViewDesc, OutDepthResources.DepthStencilHandle);

    return true;
}

bool RendererUtils::CreateObjectIdResources(
    FDX12Device* Device,
    uint32_t Width,
    uint32_t Height,
    ComPtr<ID3D12Resource>& OutTexture,
    ComPtr<ID3D12DescriptorHeap>& OutRtvHeap,
    D3D12_CPU_DESCRIPTOR_HANDLE& OutRtvHandle,
    ComPtr<ID3D12Resource>& OutReadback,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint,
    uint32_t& OutRowPitch)
{
    if (!Device)
    {
        return false;
    }

    D3D12_RESOURCE_DESC Desc = {};
    Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    Desc.Width = Width;
    Desc.Height = Height;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels = 1;
    Desc.Format = DXGI_FORMAT_R32_UINT;
    Desc.SampleDesc.Count = 1;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = DXGI_FORMAT_R32_UINT;

    D3D12_HEAP_PROPERTIES HeapProps = {};
    HeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &Desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        &ClearValue,
        IID_PPV_ARGS(OutTexture.ReleaseAndGetAddressOf())));
    OutTexture->SetName(L"ObjectIdTexture");

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = 1;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(OutRtvHeap.ReleaseAndGetAddressOf())));
    OutRtvHandle = OutRtvHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_RENDER_TARGET_VIEW_DESC RtvDesc = {};
    RtvDesc.Format = DXGI_FORMAT_R32_UINT;
    RtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    Device->GetDevice()->CreateRenderTargetView(OutTexture.Get(), &RtvDesc, OutRtvHandle);

    OutRowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    OutFootprint.Offset = 0;
    OutFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
    OutFootprint.Footprint.Width = 1;
    OutFootprint.Footprint.Height = 1;
    OutFootprint.Footprint.Depth = 1;
    OutFootprint.Footprint.RowPitch = OutRowPitch;

    D3D12_RESOURCE_DESC BufferDesc = {};
    BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufferDesc.Width = OutRowPitch;
    BufferDesc.Height = 1;
    BufferDesc.DepthOrArraySize = 1;
    BufferDesc.MipLevels = 1;
    BufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    BufferDesc.SampleDesc.Count = 1;
    BufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES ReadbackProps = {};
    ReadbackProps.Type = D3D12_HEAP_TYPE_READBACK;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &ReadbackProps,
        D3D12_HEAP_FLAG_NONE,
        &BufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(OutReadback.ReleaseAndGetAddressOf())));
    OutReadback->SetName(L"ObjectIdReadback");
    return true;
}

bool RendererUtils::CreateObjectIdPipeline(
    FDX12Device* Device,
    ID3D12RootSignature* RootSignature,
    ComPtr<ID3D12PipelineState>& OutPipelineState)
{
    if (!Device || !RootSignature)
    {
        return false;
    }

    FShaderCompiler Compiler;
    std::vector<uint8_t> VSByteCode;
    std::vector<uint8_t> PSByteCode;

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/ObjectId.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/ObjectId.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC InputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    2, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC PsoDesc = {};
    PsoDesc.pRootSignature = RootSignature;
    PsoDesc.InputLayout = { InputLayout, _countof(InputLayout) };
    PsoDesc.VS = { VSByteCode.data(), VSByteCode.size() };
    PsoDesc.PS = { PSByteCode.data(), PSByteCode.size() };
    PsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    PsoDesc.SampleDesc.Count = 1;
    PsoDesc.SampleMask = UINT_MAX;

    PsoDesc.RasterizerState = {};
    PsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    PsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    PsoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    PsoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    PsoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    PsoDesc.RasterizerState.DepthClipEnable = TRUE;

    PsoDesc.BlendState = {};
    PsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    PsoDesc.NumRenderTargets = 1;
    PsoDesc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    PsoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    PsoDesc.DepthStencilState = {};
    PsoDesc.DepthStencilState.DepthEnable = TRUE;
    PsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    PsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    PsoDesc.DepthStencilState.StencilEnable = FALSE;

    HR_CHECK(Device->GetDevice()->CreateGraphicsPipelineState(&PsoDesc, IID_PPV_ARGS(OutPipelineState.ReleaseAndGetAddressOf())));
    return true;
}

void RendererUtils::RequestObjectIdReadback(
    uint32_t X,
    uint32_t Y,
    bool& OutRequested,
    bool& OutRecorded,
    uint32_t& OutX,
    uint32_t& OutY)
{
    OutRequested = true;
    OutRecorded = false;
    OutX = X;
    OutY = Y;
}

bool RendererUtils::ConsumeObjectIdReadback(
    const ComPtr<ID3D12Resource>& ReadbackResource,
    uint32_t RowPitch,
    bool& InOutRequested,
    bool& InOutRecorded,
    uint32_t& OutObjectId)
{
    if (!InOutRecorded || !ReadbackResource)
    {
        return false;
    }

    void* MappedData = nullptr;
    D3D12_RANGE ReadRange = { 0, RowPitch };
    if (FAILED(ReadbackResource->Map(0, &ReadRange, &MappedData)) || !MappedData)
    {
        return false;
    }

    OutObjectId = *static_cast<const uint32_t*>(MappedData);
    D3D12_RANGE WriteRange = { 0, 0 };
    ReadbackResource->Unmap(0, &WriteRange);
    InOutRequested = false;
    InOutRecorded = false;
    return true;
}

bool RendererUtils::ComputeSceneModelStats(
    const std::vector<FSceneModelResource>& Models,
    const std::vector<bool>& Visibility,
    size_t& OutTotal,
    size_t& OutCulled)
{
    OutTotal = Models.size();
    if (Visibility.empty())
    {
        OutCulled = 0;
        return true;
    }

    const size_t VisibleCountMax = (std::min)(Models.size(), Visibility.size());
    size_t VisibleCount = 0;
    for (size_t Index = 0; Index < VisibleCountMax; ++Index)
    {
        if (Visibility[Index])
        {
            ++VisibleCount;
        }
    }

    OutCulled = OutTotal > VisibleCount ? (OutTotal - VisibleCount) : 0;
    return true;
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

void RendererUtils::UpdateGltfSceneAnimation(
    std::vector<FSceneModelResource>& Models,
    const std::vector<FGltfScene>& Scenes,
    std::vector<FGltfAnimationPose>& ScenePoses,
    std::vector<float>& SceneTimes,
    float DeltaTime)
{
    if (Scenes.empty() || Models.empty())
    {
        return;
    }

    if (ScenePoses.size() != Scenes.size())
    {
        ScenePoses.resize(Scenes.size());
        for (size_t Index = 0; Index < Scenes.size(); ++Index)
        {
            InitializeGltfAnimationPose(Scenes[Index], ScenePoses[Index]);
        }
    }

    if (SceneTimes.size() != Scenes.size())
    {
        SceneTimes.assign(Scenes.size(), 0.0f);
    }

    for (size_t Index = 0; Index < Scenes.size(); ++Index)
    {
        SceneTimes[Index] += DeltaTime;
        UpdateGltfAnimationPose(Scenes[Index], SceneTimes[Index], ScenePoses[Index]);
    }

    for (FSceneModelResource& Model : Models)
    {
        if (Model.GltfSceneIndex < 0 || Model.GltfNodeIndex < 0)
        {
            continue;
        }

        const size_t SceneIndex = static_cast<size_t>(Model.GltfSceneIndex);
        if (SceneIndex >= ScenePoses.size())
        {
            continue;
        }

        const std::vector<DirectX::XMFLOAT4X4>& WorldMatrices = ScenePoses[SceneIndex].WorldMatrices;
        const size_t NodeIndex = static_cast<size_t>(Model.GltfNodeIndex);
        if (NodeIndex >= WorldMatrices.size())
        {
            continue;
        }

        using namespace DirectX;
        const XMMATRIX NodeWorld = XMLoadFloat4x4(&WorldMatrices[NodeIndex]);
        const XMMATRIX ModelTransform = XMLoadFloat4x4(&Model.ModelTransform);
        const XMMATRIX World = XMMatrixMultiply(NodeWorld, ModelTransform);
        XMStoreFloat4x4(&Model.WorldMatrix, World);

        if (Model.GltfSkinIndex >= 0 && Model.BoneMatrixBufferMapped)
        {
            const size_t SkinIndex = static_cast<size_t>(Model.GltfSkinIndex);
            if (SkinIndex < Scenes[SceneIndex].Skins.size()
                && SkinIndex < ScenePoses[SceneIndex].SkinMatrices.size())
            {
                const FGltfSkin& Skin = Scenes[SceneIndex].Skins[SkinIndex];
                const std::vector<DirectX::XMFLOAT4X4>& SkinMatrices = ScenePoses[SceneIndex].SkinMatrices[SkinIndex];
                const size_t MatrixCount = std::min(SkinMatrices.size(), static_cast<size_t>(Model.BoneMatrixCount));

                const XMMATRIX MeshWorld = XMLoadFloat4x4(&Model.WorldMatrix);
                const XMMATRIX MeshWorldInv = XMMatrixInverse(nullptr, MeshWorld);

                std::vector<DirectX::XMFLOAT4X4> FinalMatrices(MatrixCount);
                for (size_t JointIndex = 0; JointIndex < MatrixCount; ++JointIndex)
                {
                    const XMMATRIX SkinMatrix = XMLoadFloat4x4(&SkinMatrices[JointIndex]);
					const XMMATRIX FinalMatrix = XMMatrixMultiply(SkinMatrix, MeshWorldInv); // To Mesh Local Space
                    XMStoreFloat4x4(&FinalMatrices[JointIndex], FinalMatrix);
                }

                const size_t CopyBytes = MatrixCount * sizeof(DirectX::XMFLOAT4X4);
                std::memcpy(Model.BoneMatrixBufferMapped, FinalMatrices.data(), CopyBytes);
            }
        }
    }
}

bool RendererUtils::CreateMappedConstantBuffer(FDX12Device* Device, uint64_t BufferSize, FMappedConstantBuffer& OutConstantBuffer)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint64_t ConstantBufferSize = (BufferSize + 255ULL) & ~255ULL;

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    UploadHeap.CreationNodeMask = 1;
    UploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC BufferDesc = {};
    BufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    BufferDesc.Width = ConstantBufferSize;
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
        IID_PPV_ARGS(OutConstantBuffer.Resource.GetAddressOf())));

    OutConstantBuffer.Resource->SetName(L"MappedConstantBuffer");

    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(OutConstantBuffer.Resource->Map(0, &EmptyRange, reinterpret_cast<void**>(&OutConstantBuffer.MappedData)));
    return true;
}

bool RendererUtils::CreateSkyAtmosphereResources(
    FDX12Device* Device,
    float SkySphereRadius,
    FMeshGeometryBuffers& OutGeometry,
    Microsoft::WRL::ComPtr<ID3D12Resource>& OutConstantBuffer,
    uint8_t*& OutConstantBufferMapped)
{
    FMappedConstantBuffer SkyConstantBuffer;
    if (!CreateMappedConstantBuffer(Device, sizeof(FSkyAtmosphereConstants), SkyConstantBuffer))
    {
        return false;
    }

    OutConstantBuffer = SkyConstantBuffer.Resource;
    OutConstantBufferMapped = SkyConstantBuffer.MappedData;

    return CreateSphereGeometry(Device, OutGeometry, SkySphereRadius, 64, 32);
}

bool RendererUtils::CreateSkyAtmospherePipeline(
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

    const D3D_SHADER_MODEL ShaderModel = Device->GetShaderModel();
    const std::wstring VSTarget = BuildShaderTarget(L"vs", ShaderModel);
    const std::wstring PSTarget = BuildShaderTarget(L"ps", ShaderModel);

    if (!Compiler.CompileFromFile(L"Shaders/SkyAtmosphere.hlsl", L"VSMain", VSTarget, VSByteCode))
    {
        return false;
    }

    if (!Compiler.CompileFromFile(L"Shaders/SkyAtmosphere.hlsl", L"PSMain", PSTarget, PSByteCode))
    {
        return false;
    }

    D3D12_ROOT_PARAMETER1 RootParams[1] = {};
    // RootParams[0]: Sky constants (b0), used in Shaders/SkyAtmosphere.hlsl VSMain and PSMain
    RootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParams[0].Descriptor.ShaderRegister = 0;
    RootParams[0].Descriptor.RegisterSpace = 0;
    RootParams[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootDesc = {};
    RootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootDesc.Desc_1_1.NumParameters = _countof(RootParams);
    RootDesc.Desc_1_1.pParameters = RootParams;
    RootDesc.Desc_1_1.NumStaticSamplers = 0;
    RootDesc.Desc_1_1.pStaticSamplers = nullptr;
    RootDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

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
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    2, 0,   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 3, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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

namespace
{
    void FillTransformConstants(const DirectX::XMFLOAT4& OffsetScale, const DirectX::XMFLOAT4& RotationTexCoord, DirectX::XMFLOAT4& OutOffsetScale, DirectX::XMFLOAT4& OutRotation)
    {
        OutOffsetScale = OffsetScale;
        OutRotation = RotationTexCoord;
    }
}

void RendererUtils::UpdateSceneConstants(
    const FCamera& Camera,
    const FSceneModelResource& Model,
    float LightIntensity,
    const DirectX::XMVECTOR& LightDirection,
    const DirectX::XMFLOAT3& LightColor,
    const DirectX::XMMATRIX& LightViewProjection,
    const DirectX::XMMATRIX& Projection,
    float ShadowStrength,
    float ShadowBias,
    float ShadowMapWidth,
    float ShadowMapHeight,
    float EnvMapMipCount,
    const DirectX::XMFLOAT2& TaaJitter,
    uint32_t GtaoTemporalIndex,
    bool bGtaoEnabled,
    float GtaoRadius,
    float GtaoIntensity,
    float GtaoPower,
    float GtaoThickness,
    uint32_t GtaoDirectionCount,
    uint32_t GtaoStepCount,
    uint8_t* ConstantBufferMapped,
    uint64_t ConstantBufferOffset)
{
    if (ConstantBufferMapped == nullptr)
    {
        return;
    }

    using namespace DirectX;

    const XMMATRIX View = Camera.GetViewMatrix();
    const XMMATRIX ViewInverse = XMMatrixInverse(nullptr, View);
    const XMMATRIX WorldMatrix = XMLoadFloat4x4(&Model.WorldMatrix);

    const bool bHasEmissiveTexture = !Model.EmissiveTexturePath.empty();
    const XMFLOAT3 BaseColorFactor = Model.BaseColorFactor;
    const XMFLOAT3 EmissiveFactor = Model.EmissiveFactor;

    FSceneConstants Constants = {};
    XMStoreFloat4x4(&Constants.World, WorldMatrix);
    XMStoreFloat4x4(&Constants.View, View);
    XMStoreFloat4x4(&Constants.ViewInverse, ViewInverse);
    XMStoreFloat4x4(&Constants.Projection, Projection);
    const XMMATRIX ViewProjection = View * Projection;
    XMStoreFloat4x4(&Constants.ViewProjectionInverse, XMMatrixInverse(nullptr, ViewProjection));
    Constants.BaseColor = BaseColorFactor;
    Constants.LightIntensity = LightIntensity;
    XMStoreFloat3(&Constants.LightDirection, XMVector3Normalize(LightDirection));
    Constants.CameraPosition = Camera.GetPosition();
    Constants.LightColor = LightColor;
    Constants.EmissiveFactor = EmissiveFactor;
    XMStoreFloat4x4(&Constants.LightViewProjection, LightViewProjection);
    Constants.ShadowStrength = ShadowStrength;
    Constants.ShadowBias = ShadowBias;
    Constants.ShadowMapSize = DirectX::XMFLOAT2(ShadowMapWidth, ShadowMapHeight);
    Constants.MetallicFactor = Model.MetallicFactor;
    Constants.RoughnessFactor = Model.RoughnessFactor;
    Constants.BaseColorAlpha = Model.BaseColorAlpha;
    Constants.AlphaCutoff = Model.AlphaCutoff;
    Constants.AlphaMode = Model.AlphaMode;
    Constants.EnvMapMipCount = EnvMapMipCount;
    Constants.TaaJitter = TaaJitter;
    Constants.GtaoTemporalIndex = GtaoTemporalIndex;
    Constants.VertexBufferBindlessIndices = DirectX::XMUINT4(
        Model.VertexBufferBindlessIndices[0],
        Model.VertexBufferBindlessIndices[1],
        Model.VertexBufferBindlessIndices[2],
        Model.VertexBufferBindlessIndices[3]);
    Constants.ExtraBindlessIndices = DirectX::XMUINT4(
        Model.VertexBufferBindlessIndices[4],
        Model.IndexBufferBindlessIndex,
        0u,
        0u);
    Constants.SkinningBindlessIndices = DirectX::XMUINT4(
        Model.VertexBufferBindlessIndices[5],
        Model.VertexBufferBindlessIndices[6],
        Model.BoneMatrixBindlessIndex,
        Model.SkinnedPositionSrvBindlessIndex);
    Constants.GtaoRadius = GtaoRadius;
    Constants.GtaoIntensity = bGtaoEnabled ? GtaoIntensity : 0.0f;
    Constants.GtaoPower = GtaoPower;
    Constants.GtaoThickness = GtaoThickness;
    Constants.GtaoDirectionCount = GtaoDirectionCount;
    Constants.GtaoStepCount = GtaoStepCount;
    Constants.ObjectId = Model.ObjectId;
    FillTransformConstants(Model.BaseColorTransformOffsetScale, Model.BaseColorTransformRotation, Constants.BaseColorTransformOffsetScale, Constants.BaseColorTransformRotation);
    FillTransformConstants(Model.MetallicRoughnessTransformOffsetScale, Model.MetallicRoughnessTransformRotation, Constants.MetallicRoughnessTransformOffsetScale, Constants.MetallicRoughnessTransformRotation);
    FillTransformConstants(Model.NormalTransformOffsetScale, Model.NormalTransformRotation, Constants.NormalTransformOffsetScale, Constants.NormalTransformRotation);
    FillTransformConstants(Model.EmissiveTransformOffsetScale, Model.EmissiveTransformRotation, Constants.EmissiveTransformOffsetScale, Constants.EmissiveTransformRotation);

    memcpy(ConstantBufferMapped + ConstantBufferOffset, &Constants, sizeof(Constants));
}

void RendererUtils::UpdateSkyConstants(
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
    const uint32_t UseSkinning = (Model.BoneMatrixBindlessIndex != UINT32_MAX && Model.BoneMatrixCount > 0) ? 1u : 0u;
    return (UseNormal) | (UseMr << 1) | (UseBase << 2) | (UseEmissive << 3) | (UseAlphaMask << 4) | (UseSkinning << 5);
}
