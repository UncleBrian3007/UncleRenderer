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

    std::wstring BuildShaderTarget(const wchar_t* StagePrefix, D3D_SHADER_MODEL ShaderModel)
    {
        uint32_t ShaderModelValue = static_cast<uint32_t>(ShaderModel);
        uint32_t Major = (ShaderModelValue >> 4) & 0xF;
        uint32_t Minor = ShaderModelValue & 0xF;

        return std::wstring(StagePrefix) + L"_" + std::to_wstring(Major) + L"_" + std::to_wstring(Minor);
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

bool RendererUtils::CreateSphereGeometry(FDX12Device* Device, FMeshGeometryBuffers& OutGeometry, float Radius, uint32_t SliceCount, uint32_t StackCount)
{
    const FMesh Sphere = FMesh::CreateSphere(Radius, SliceCount, StackCount);
    return CreateMeshGeometry(Device, Sphere, OutGeometry);
}

bool RendererUtils::CreateDefaultSceneGeometry(FDX12Device* Device, FMeshGeometryBuffers& OutGeometry, FFloat3& OutCenter, float& OutRadius, FGltfMaterialTextures* OutTexturePaths)
{
    FGltfScene Scene;
    if (FGltfLoader::LoadSceneFromFile(L"Assets/Duck/Duck.gltf", Scene) && !Scene.Meshes.empty())
    {
        const FMesh& FirstMesh = Scene.Meshes.front();
        ComputeMeshBounds(FirstMesh, OutCenter, OutRadius);

        if (OutTexturePaths)
        {
            OutTexturePaths->PerMesh = Scene.MeshMaterials;
        }

        return CreateMeshGeometry(Device, FirstMesh, OutGeometry);
    }

    if (OutTexturePaths)
    {
        OutTexturePaths->PerMesh.clear();
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

    float ComputeMaxScale(const DirectX::XMFLOAT4X4& Matrix)
    {
        const float ScaleX = std::sqrt(Matrix._11 * Matrix._11 + Matrix._21 * Matrix._21 + Matrix._31 * Matrix._31);
        const float ScaleY = std::sqrt(Matrix._12 * Matrix._12 + Matrix._22 * Matrix._22 + Matrix._32 * Matrix._32);
        const float ScaleZ = std::sqrt(Matrix._13 * Matrix._13 + Matrix._23 * Matrix._23 + Matrix._33 * Matrix._33);

        return (std::max)((std::max)(ScaleX, ScaleY), ScaleZ);
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

        FGltfScene LoadedScene;
        if (!FGltfLoader::LoadSceneFromFile(MeshPath, LoadedScene))
        {
            LogError("Failed to load mesh from scene: " + PathToUtf8String(MeshPath));
            continue;
        }

        if (LoadedScene.Meshes.empty())
        {
            LogError("No meshes found in glTF: " + PathToUtf8String(MeshPath));
            continue;
        }

        std::vector<FMeshGeometryBuffers> MeshGeometries(LoadedScene.Meshes.size());
        std::vector<FFloat3> MeshCenters(LoadedScene.Meshes.size());
        std::vector<float> MeshRadii(LoadedScene.Meshes.size());

        for (size_t MeshIndex = 0; MeshIndex < LoadedScene.Meshes.size(); ++MeshIndex)
        {
            const FMesh& Mesh = LoadedScene.Meshes[MeshIndex];
            ComputeMeshBounds(Mesh, MeshCenters[MeshIndex], MeshRadii[MeshIndex]);

            if (!CreateMeshGeometry(Device, Mesh, MeshGeometries[MeshIndex]))
            {
                LogError("Failed to create geometry for scene mesh: " + PathToUtf8String(MeshPath));
                MeshGeometries.clear();
                break;
            }
        }

        if (MeshGeometries.empty())
        {
            continue;
        }

        if (LoadedScene.Nodes.empty())
        {
            for (size_t MeshIndex = 0; MeshIndex < LoadedScene.Meshes.size(); ++MeshIndex)
            {
                FGltfNode DefaultNode;
                DefaultNode.MeshIndex = static_cast<int>(MeshIndex);
                DefaultNode.WorldMatrix = DirectX::XMFLOAT4X4(
                    1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f);
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

            FSceneModelResource ModelResource = {};
            ModelResource.Geometry = MeshGeometries[MeshIndex];

            const FFloat3 MeshCenter = MeshCenters[MeshIndex];
            float MeshRadius = MeshRadii[MeshIndex];

            const std::array<float, 3> ScaleComponents = { Model.Scale.x, Model.Scale.y, Model.Scale.z };
            float MaxScale = 1.0f;
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

            const XMMATRIX World = NodeWorld * Scale * Rotation * Translation;
            XMStoreFloat4x4(&ModelResource.WorldMatrix, World);

            const XMVECTOR CenterVec = XMVector3TransformCoord(XMVectorSet(MeshCenter.x, MeshCenter.y, MeshCenter.z, 1.0f), World);
            XMStoreFloat3(&ModelResource.Center, CenterVec);
            ModelResource.Radius = MeshRadius * NodeScale;

            const std::wstring EmptyTexture;

            const FGltfMaterialTextureSet* Material = (MeshIndex < LoadedScene.MeshMaterials.size())
                ? &LoadedScene.MeshMaterials[MeshIndex]
                : nullptr;

            const std::wstring& BaseColorPath = (Material && !Material->BaseColor.empty()) ? Material->BaseColor : EmptyTexture;
            const std::wstring& MetallicRoughnessPath = (Material && !Material->MetallicRoughness.empty()) ? Material->MetallicRoughness : EmptyTexture;
            const std::wstring& NormalPath = (Material && !Material->Normal.empty()) ? Material->Normal : EmptyTexture;
            const std::wstring& EmissivePath = (Material && !Material->Emissive.empty()) ? Material->Emissive : EmptyTexture;

            ModelResource.BaseColorTexturePath = Model.BaseColorTexturePath.empty() ? BaseColorPath : Model.BaseColorTexturePath;
            ModelResource.MetallicRoughnessTexturePath = Model.MetallicRoughnessTexturePath.empty() ? MetallicRoughnessPath : Model.MetallicRoughnessTexturePath;
            ModelResource.NormalTexturePath = Model.NormalTexturePath.empty() ? NormalPath : Model.NormalTexturePath;
            ModelResource.EmissiveTexturePath = Model.EmissiveTexturePath.empty() ? EmissivePath : Model.EmissiveTexturePath;
            ModelResource.BaseColorFactor = Material ? Material->BaseColorFactor : FFloat3(1.0f, 1.0f, 1.0f);
            ModelResource.MetallicFactor = Material ? Material->MetallicFactor : 1.0f;
            ModelResource.RoughnessFactor = Material ? Material->RoughnessFactor : 1.0f;
            ModelResource.EmissiveFactor = Material ? Material->EmissiveFactor : FFloat3(0.0f, 0.0f, 0.0f);
            ModelResource.bHasNormalMap = !ModelResource.NormalTexturePath.empty();
            if (Material)
            {
                ModelResource.BaseColorTransformOffsetScale = BuildOffsetScale(Material->BaseColorTransform);
                ModelResource.BaseColorTransformRotation = BuildRotationConstants(Material->BaseColorTransform);
                ModelResource.MetallicRoughnessTransformOffsetScale = BuildOffsetScale(Material->MetallicRoughnessTransform);
                ModelResource.MetallicRoughnessTransformRotation = BuildRotationConstants(Material->MetallicRoughnessTransform);
                ModelResource.NormalTransformOffsetScale = BuildOffsetScale(Material->NormalTransform);
                ModelResource.NormalTransformRotation = BuildRotationConstants(Material->NormalTransform);
                ModelResource.EmissiveTransformOffsetScale = BuildOffsetScale(Material->EmissiveTransform);
                ModelResource.EmissiveTransformRotation = BuildRotationConstants(Material->EmissiveTransform);
            }

            UpdateSceneBounds(ModelResource.Center, ModelResource.Radius, SceneMin, SceneMax);

            OutModels.push_back(std::move(ModelResource));
        }
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
    FMappedConstantBuffer SkyConstantBuffer = {};
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

    D3D12_ROOT_PARAMETER1 RootParam = {};
    RootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    RootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    RootParam.Descriptor.ShaderRegister = 0;
    RootParam.Descriptor.RegisterSpace = 0;
    RootParam.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC RootDesc = {};
    RootDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    RootDesc.Desc_1_1.NumParameters = 1;
    RootDesc.Desc_1_1.pParameters = &RootParam;
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
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
    float ShadowStrength,
    float ShadowBias,
    float ShadowMapWidth,
    float ShadowMapHeight,
    float EnvMapMipCount,
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
    const XMMATRIX Projection = Camera.GetProjectionMatrix();
    const XMMATRIX WorldMatrix = XMLoadFloat4x4(&Model.WorldMatrix);

    const bool bHasEmissiveTexture = !Model.EmissiveTexturePath.empty();
    const XMFLOAT3 BaseColorFactor = Model.BaseColorFactor;
    const XMFLOAT3 EmissiveFactor = Model.EmissiveFactor;

    FSceneConstants Constants = {};
    XMStoreFloat4x4(&Constants.World, WorldMatrix);
    XMStoreFloat4x4(&Constants.View, View);
    XMStoreFloat4x4(&Constants.ViewInverse, ViewInverse);
    XMStoreFloat4x4(&Constants.Projection, Projection);
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
    Constants.EnvMapMipCount = EnvMapMipCount;
    FillTransformConstants(Model.BaseColorTransformOffsetScale, Model.BaseColorTransformRotation, Constants.BaseColorTransformOffsetScale, Constants.BaseColorTransformRotation);
    FillTransformConstants(Model.MetallicRoughnessTransformOffsetScale, Model.MetallicRoughnessTransformRotation, Constants.MetallicRoughnessTransformOffsetScale, Constants.MetallicRoughnessTransformRotation);
    FillTransformConstants(Model.NormalTransformOffsetScale, Model.NormalTransformRotation, Constants.NormalTransformOffsetScale, Constants.NormalTransformRotation);
    FillTransformConstants(Model.EmissiveTransformOffsetScale, Model.EmissiveTransformRotation, Constants.EmissiveTransformOffsetScale, Constants.EmissiveTransformRotation);

    memcpy(ConstantBufferMapped + ConstantBufferOffset, &Constants, sizeof(Constants));
}

void RendererUtils::UpdateSkyConstants(
    const FCamera& Camera,
    const DirectX::XMMATRIX& WorldMatrix,
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
    const XMMATRIX Projection = Camera.GetProjectionMatrix();

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
