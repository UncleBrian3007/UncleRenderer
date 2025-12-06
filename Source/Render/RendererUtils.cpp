#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RendererUtils.h"

#include "../Scene/Mesh.h"
#include "../Scene/GltfLoader.h"
#include "../Scene/SceneJsonLoader.h"
#include "../Scene/Camera.h"
#include "../Core/Logger.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12Commons.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace
{
    std::string PathToUtf8String(const std::filesystem::path& Path)
    {
        const auto Utf8 = Path.u8string();
        return std::string(Utf8.begin(), Utf8.end());
    }

    void ComputeMeshBounds(const FMesh& Mesh, FFloat3& OutCenter, float& OutRadius)
    {
        const auto& Vertices = Mesh.GetVertices();
        if (Vertices.empty())
        {
            OutCenter = FFloat3(0.0f, 0.0f, 0.0f);
            OutRadius = 1.0f;
            return;
        }

        FFloat3 Min = Vertices.front().Position;
        FFloat3 Max = Vertices.front().Position;

        for (const auto& Vertex : Vertices)
        {
            Min.x = std::min(Min.x, Vertex.Position.x);
            Min.y = std::min(Min.y, Vertex.Position.y);
            Min.z = std::min(Min.z, Vertex.Position.z);

            Max.x = std::max(Max.x, Vertex.Position.x);
            Max.y = std::max(Max.y, Vertex.Position.y);
            Max.z = std::max(Max.z, Vertex.Position.z);
        }

        OutCenter = FFloat3(
            0.5f * (Min.x + Max.x),
            0.5f * (Min.y + Max.y),
            0.5f * (Min.z + Max.z));

        const DirectX::XMVECTOR Extents = DirectX::XMVectorSet(Max.x - Min.x, Max.y - Min.y, Max.z - Min.z, 0.0f);
        OutRadius = DirectX::XMVectorGetX(DirectX::XMVector3Length(Extents)) * 0.5f;
        OutRadius = std::max(OutRadius, 1.0f);
    }
}

bool RendererUtils::CreateMeshGeometry(FDX12Device* Device, const FMesh& Mesh, FMeshGeometryBuffers& OutGeometry)
{
    if (Device == nullptr)
    {
        return false;
    }

    OutGeometry.IndexCount = static_cast<uint32_t>(Mesh.GetIndices().size());

    const UINT VertexBufferSize = static_cast<UINT>(Mesh.GetVertices().size() * sizeof(FMesh::FVertex));
    const UINT IndexBufferSize = static_cast<UINT>(Mesh.GetIndices().size() * sizeof(uint32_t));

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

    BufferDesc.Width = VertexBufferSize;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &BufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(OutGeometry.VertexBuffer.GetAddressOf())));

    OutGeometry.VertexBufferView.BufferLocation = OutGeometry.VertexBuffer->GetGPUVirtualAddress();
    OutGeometry.VertexBufferView.StrideInBytes = sizeof(FMesh::FVertex);
    OutGeometry.VertexBufferView.SizeInBytes = VertexBufferSize;

    void* VertexData = nullptr;
    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(OutGeometry.VertexBuffer->Map(0, &EmptyRange, &VertexData));
    memcpy(VertexData, Mesh.GetVertices().data(), VertexBufferSize);
    OutGeometry.VertexBuffer->Unmap(0, nullptr);

    BufferDesc.Width = IndexBufferSize;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &BufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(OutGeometry.IndexBuffer.GetAddressOf())));

    OutGeometry.IndexBufferView.BufferLocation = OutGeometry.IndexBuffer->GetGPUVirtualAddress();
    OutGeometry.IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    OutGeometry.IndexBufferView.SizeInBytes = IndexBufferSize;

    void* IndexData = nullptr;
    HR_CHECK(OutGeometry.IndexBuffer->Map(0, &EmptyRange, &IndexData));
    memcpy(IndexData, Mesh.GetIndices().data(), IndexBufferSize);
    OutGeometry.IndexBuffer->Unmap(0, nullptr);

    return true;
}

bool RendererUtils::CreateCubeGeometry(FDX12Device* Device, FCubeGeometryBuffers& OutGeometry, float Size)
{
    const FMesh Cube = FMesh::CreateCube(Size);
    return CreateMeshGeometry(Device, Cube, OutGeometry);
}

bool RendererUtils::CreateDefaultSceneGeometry(FDX12Device* Device, FMeshGeometryBuffers& OutGeometry, FFloat3& OutCenter, float& OutRadius, std::wstring* OutTexturePath)
{
    FMesh LoadedMesh;
    if (FGltfLoader::LoadMeshFromFile(L"Assets/Duck/Duck.gltf", LoadedMesh, OutTexturePath))
    {
        ComputeMeshBounds(LoadedMesh, OutCenter, OutRadius);
        return CreateMeshGeometry(Device, LoadedMesh, OutGeometry);
    }

    if (OutTexturePath)
    {
        OutTexturePath->clear();
    }

    const FMesh Cube = FMesh::CreateCube();
    ComputeMeshBounds(Cube, OutCenter, OutRadius);
    return CreateMeshGeometry(Device, Cube, OutGeometry);
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
}

bool RendererUtils::CreateSceneModelsFromJson(
    FDX12Device* Device,
    const std::wstring& SceneFilePath,
    std::vector<FSceneModelResource>& OutModels,
    DirectX::XMFLOAT3& OutSceneCenter,
    float& OutSceneRadius)
{
    OutModels.clear();

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

    for (const FSceneModelDesc& Model : Models)
    {
        std::filesystem::path MeshPath(Model.MeshPath);
        if (!MeshPath.is_absolute())
        {
            std::filesystem::path AssetsRoot = ScenePath.parent_path().parent_path();
            MeshPath = AssetsRoot / MeshPath;
        }

        FMesh LoadedMesh;
        std::wstring TexturePath;
        if (!FGltfLoader::LoadMeshFromFile(MeshPath, LoadedMesh, &TexturePath))
        {
            LogError("Failed to load mesh from scene: " + PathToUtf8String(MeshPath));
            continue;
        }

        FSceneModelResource ModelResource = {};
        if (!CreateMeshGeometry(Device, LoadedMesh, ModelResource.Geometry))
        {
            LogError("Failed to create geometry for scene mesh: " + PathToUtf8String(MeshPath));
            continue;
        }

        FFloat3 MeshCenter;
        float MeshRadius;
        ComputeMeshBounds(LoadedMesh, MeshCenter, MeshRadius);

        const std::array<float, 3> ScaleComponents = { Model.Scale.x, Model.Scale.y, Model.Scale.z };
        float MaxScale = 1.0f;
        for (float ScaleValue : ScaleComponents)
        {
            MaxScale = (std::max)(MaxScale, std::fabs(ScaleValue));
        }

        MeshRadius *= MaxScale;

        using namespace DirectX;

        const XMMATRIX Offset = XMMatrixTranslation(-MeshCenter.x, -MeshCenter.y, -MeshCenter.z);
        const XMMATRIX Scale = XMMatrixScaling(Model.Scale.x, Model.Scale.y, Model.Scale.z);
        const XMMATRIX Rotation = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(Model.RotationEuler.x),
            XMConvertToRadians(Model.RotationEuler.y),
            XMConvertToRadians(Model.RotationEuler.z));
        const XMMATRIX Translation = XMMatrixTranslation(Model.Position.x, Model.Position.y, Model.Position.z);

        const XMMATRIX World = Offset * Scale * Rotation * Translation;
        XMStoreFloat4x4(&ModelResource.WorldMatrix, World);

        const XMVECTOR CenterVec = XMVector3TransformCoord(XMVectorSet(MeshCenter.x, MeshCenter.y, MeshCenter.z, 1.0f), World);
        XMStoreFloat3(&ModelResource.Center, CenterVec);
        ModelResource.Radius = MeshRadius;
        ModelResource.BaseColorTexturePath = Model.BaseColorTexturePath.empty() ? TexturePath : Model.BaseColorTexturePath;

        UpdateSceneBounds(ModelResource.Center, ModelResource.Radius, SceneMin, SceneMax);

        OutModels.push_back(std::move(ModelResource));
    }

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
    Desc.Format = Format;
    Desc.SampleDesc.Count = 1;
    Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    Desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE ClearValue = {};
    ClearValue.Format = Desc.Format;
    ClearValue.DepthStencil.Depth = 1.0f;
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

    D3D12_DESCRIPTOR_HEAP_DESC HeapDesc = {};
    HeapDesc.NumDescriptors = 1;
    HeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    HeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR_CHECK(Device->GetDevice()->CreateDescriptorHeap(&HeapDesc, IID_PPV_ARGS(OutDepthResources.DSVHeap.GetAddressOf())));

    OutDepthResources.DepthStencilHandle = OutDepthResources.DSVHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_DEPTH_STENCIL_VIEW_DESC ViewDesc = {};
    ViewDesc.Format = Desc.Format;
    ViewDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ViewDesc.Flags = D3D12_DSV_FLAG_NONE;
    Device->GetDevice()->CreateDepthStencilView(OutDepthResources.DepthBuffer.Get(), &ViewDesc, OutDepthResources.DepthStencilHandle);

    return true;
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

    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(OutConstantBuffer.Resource->Map(0, &EmptyRange, reinterpret_cast<void**>(&OutConstantBuffer.MappedData)));
    return true;
}

void RendererUtils::UpdateSceneConstants(const FCamera& Camera, const DirectX::XMFLOAT3& BaseColor, const DirectX::XMVECTOR& LightDirection, const DirectX::XMMATRIX& WorldMatrix, uint8_t* ConstantBufferMapped)
{
    if (ConstantBufferMapped == nullptr)
    {
        return;
    }

    using namespace DirectX;

    const XMMATRIX View = Camera.GetViewMatrix();
    const XMMATRIX Projection = Camera.GetProjectionMatrix();

    FSceneConstants Constants = {};
    XMStoreFloat4x4(&Constants.World, WorldMatrix);
    XMStoreFloat4x4(&Constants.View, View);
    XMStoreFloat4x4(&Constants.Projection, Projection);
    Constants.BaseColor = BaseColor;
    XMStoreFloat3(&Constants.LightDirection, XMVector3Normalize(LightDirection));
    Constants.CameraPosition = Camera.GetPosition();

    memcpy(ConstantBufferMapped, &Constants, sizeof(Constants));
}

