#include "RendererUtils.h"

#include "../Scene/Mesh.h"
#include "../Scene/Camera.h"
#include "../RHI/DX12Device.h"
#include "../RHI/DX12Commons.h"
#include <vector>
#include <cstring>

using Microsoft::WRL::ComPtr;

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

bool RendererUtils::CreateDefaultGridTexture(FDX12Device* Device, ComPtr<ID3D12Resource>& OutTexture)
{
    if (Device == nullptr)
    {
        return false;
    }

    const uint32_t Width = 256;
    const uint32_t Height = 256;
    const uint32_t CellSize = 32;

    const uint32_t LightColor = 0xffb5b5b5;
    const uint32_t DarkColor = 0xff5f5f5f;
    std::vector<uint32_t> TextureData(Width * Height, LightColor);

    for (uint32_t y = 0; y < Height; ++y)
    {
        const uint32_t CellY = y / CellSize;
        for (uint32_t x = 0; x < Width; ++x)
        {
            const uint32_t CellX = x / CellSize;
            const bool UseDark = ((CellX + CellY) % 2) == 0;
            TextureData[y * Width + x] = UseDark ? DarkColor : LightColor;
        }
    }

    D3D12_RESOURCE_DESC TextureDesc = {};
    TextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    TextureDesc.Width = Width;
    TextureDesc.Height = Height;
    TextureDesc.DepthOrArraySize = 1;
    TextureDesc.MipLevels = 1;
    TextureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    TextureDesc.SampleDesc.Count = 1;
    TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    D3D12_HEAP_PROPERTIES DefaultHeap = {};
    DefaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    DefaultHeap.CreationNodeMask = 1;
    DefaultHeap.VisibleNodeMask = 1;

    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &DefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &TextureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(OutTexture.GetAddressOf())));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout = {};
    UINT NumRows = 0;
    UINT64 RowSizeInBytes = 0;
    UINT64 UploadBufferSize = 0;
    Device->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, 1, 0, &Layout, &NumRows, &RowSizeInBytes, &UploadBufferSize);

    D3D12_HEAP_PROPERTIES UploadHeap = {};
    UploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    UploadHeap.CreationNodeMask = 1;
    UploadHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC UploadDesc = {};
    UploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    UploadDesc.Width = UploadBufferSize;
    UploadDesc.Height = 1;
    UploadDesc.DepthOrArraySize = 1;
    UploadDesc.MipLevels = 1;
    UploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    UploadDesc.SampleDesc.Count = 1;
    UploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> UploadResource;
    HR_CHECK(Device->GetDevice()->CreateCommittedResource(
        &UploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &UploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(UploadResource.GetAddressOf())));

    uint8_t* MappedData = nullptr;
    D3D12_RANGE EmptyRange = { 0, 0 };
    HR_CHECK(UploadResource->Map(0, &EmptyRange, reinterpret_cast<void**>(&MappedData)));

    for (UINT Row = 0; Row < NumRows; ++Row)
    {
        const uint8_t* SrcRow = reinterpret_cast<const uint8_t*>(&TextureData[Row * Width]);
        memcpy(MappedData + Layout.Offset + Row * Layout.Footprint.RowPitch, SrcRow, Width * sizeof(uint32_t));
    }

    UploadResource->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));

    D3D12_TEXTURE_COPY_LOCATION DstLocation = {};
    DstLocation.pResource = OutTexture.Get();
    DstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    DstLocation.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION SrcLocation = {};
    SrcLocation.pResource = UploadResource.Get();
    SrcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    SrcLocation.PlacedFootprint = Layout;

    UploadList->CopyTextureRegion(&DstLocation, 0, 0, 0, &SrcLocation, nullptr);

    D3D12_RESOURCE_BARRIER Barrier = {};
    Barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource = OutTexture.Get();
    Barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    UploadList->ResourceBarrier(1, &Barrier);

    HR_CHECK(UploadList->Close());

    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->GetD3DQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

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

void RendererUtils::UpdateSceneConstants(const FCamera& Camera, float DeltaTime, float& RotationAngle, const DirectX::XMFLOAT3& BaseColor, const DirectX::XMVECTOR& LightDirection, uint8_t* ConstantBufferMapped)
{
    if (ConstantBufferMapped == nullptr)
    {
        return;
    }

    using namespace DirectX;

    RotationAngle += DeltaTime * 0.5f;

    const XMMATRIX World = XMMatrixRotationY(RotationAngle) * XMMatrixRotationX(RotationAngle * 0.5f);
    const XMMATRIX View = Camera.GetViewMatrix();
    const XMMATRIX Projection = Camera.GetProjectionMatrix();

    FSceneConstants Constants = {};
    XMStoreFloat4x4(&Constants.World, World);
    XMStoreFloat4x4(&Constants.View, View);
    XMStoreFloat4x4(&Constants.Projection, Projection);
    Constants.BaseColor = BaseColor;
    XMStoreFloat3(&Constants.LightDirection, XMVector3Normalize(LightDirection));

    memcpy(ConstantBufferMapped, &Constants, sizeof(Constants));
}

