#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <cstddef>
#include "../Math/MathTypes.h"
#include "../Scene/Mesh.h"
#include "../Scene/GltfAnimation.h"

class FDX12Device;
class FCamera;
class FMesh;
struct FGltfMaterialTextures;

struct FMeshGeometryBuffers
{
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 7> VertexBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> IndexBuffer;
    std::array<D3D12_VERTEX_BUFFER_VIEW, 7> VertexBufferViews{};
    D3D12_INDEX_BUFFER_VIEW IndexBufferView{};
    uint32_t VertexBufferCount = 0;
    uint32_t IndexCount = 0;
};

using FCubeGeometryBuffers = FMeshGeometryBuffers;

struct FDepthResources
{
    Microsoft::WRL::ComPtr<ID3D12Resource> DepthBuffer;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSVHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilHandle{};
};

struct FMappedConstantBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    uint8_t* MappedData = nullptr;
};

struct FSceneConstants
{
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4X4 WorldInverseTranspose;
    DirectX::XMFLOAT4X4 View;
    DirectX::XMFLOAT4X4 ViewInverse;
    DirectX::XMFLOAT4X4 Projection;
    DirectX::XMFLOAT4X4 ViewProjectionInverse;
    DirectX::XMFLOAT4X4 PreviousViewProjection;
    DirectX::XMFLOAT4X4 PreviousWorld;
    uint32_t HasPreviousViewProjection = 0;
    uint32_t HasPreviousWorld = 0;
    uint32_t HasPreviousSkinning = 0;
    uint32_t PreviousSkinnedPositionBindlessIndex = UINT32_MAX;
    DirectX::XMFLOAT3 BaseColor;
    float LightIntensity = 1.0f;
    DirectX::XMFLOAT3 LightDirection;
    float LightRadius = 0.02f;
    DirectX::XMFLOAT3 CameraPosition;
    float Padding2 = 0.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    float Padding3 = 0.0f;
    DirectX::XMFLOAT3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    float Padding4 = 0.0f;
    DirectX::XMFLOAT4X4 LightViewProjection;
    float ShadowStrength = 1.0f;
    float ShadowBias = 0.0f;
    DirectX::XMFLOAT2 ShadowMapSize{ 0.0f, 0.0f };
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    float BaseColorAlpha = 1.0f;
    float AlphaCutoff = 0.5f;
    uint32_t AlphaMode = 0;
    DirectX::XMUINT3 PaddingMaterial{ 0, 0, 0 };
    DirectX::XMFLOAT3 SheenColorFactor{ 0.0f, 0.0f, 0.0f };
    float SheenRoughnessFactor = 0.0f;
    uint32_t ShadingModelId = 0;
    DirectX::XMUINT3 PaddingShadingModel{ 0, 0, 0 };
    float ClearcoatFactor = 0.0f;
    float ClearcoatRoughnessFactor = 0.0f;
    DirectX::XMFLOAT2 PaddingClearcoat{ 0.0f, 0.0f };
    float AnisotropyStrength = 0.0f;
    float AnisotropyRotation = 0.0f;
    DirectX::XMFLOAT2 PaddingAnisotropy{ 0.0f, 0.0f };
    DirectX::XMFLOAT4 BaseColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 BaseColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 NormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 NormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 EmissiveTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 EmissiveTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 SheenColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 SheenRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatNormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatNormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 AnisotropyTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 AnisotropyTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMUINT4 VertexBufferBindlessIndices{ 0, 0, 0, 0 };
    DirectX::XMUINT4 ExtraBindlessIndices{ 0, 0, 0, 0 };
    DirectX::XMUINT4 SkinningBindlessIndices{ 0, 0, 0, 0 };
    float EnvMapMipCount = 1.0f;
    DirectX::XMFLOAT3 PaddingEnvMap{ 0.0f, 0.0f, 0.0f };
    float GtaoRadius = 0.75f;
    float GtaoIntensity = 1.0f;
    float GtaoPower = 1.5f;
    float GtaoThickness = 0.1f;
    uint32_t GtaoDirectionCount = 6;
    uint32_t GtaoStepCount = 4;
    DirectX::XMFLOAT2 TaaJitter{ 0.0f, 0.0f };
    uint32_t GtaoTemporalIndex = 0;
    uint32_t ObjectId = 0;
    DirectX::XMFLOAT2 PaddingObjectId{ 0.0f, 0.0f };
    DirectX::XMFLOAT2 PaddingObjectId2{ 0.0f, 0.0f };
};

struct FSkyAtmosphereConstants
{
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4X4 View;
    DirectX::XMFLOAT4X4 Projection;
    DirectX::XMFLOAT3 CameraPosition;
    float Padding0 = 0.0f;
    DirectX::XMFLOAT3 LightDirection{ 0.0f, -1.0f, 0.0f };
    float Padding1 = 0.0f;
    DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
    float Padding2 = 0.0f;
};

enum class EAlphaMode : uint32_t
{
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

struct FSkyPipelineConfig
{
    bool DepthEnable = false;
    D3D12_COMPARISON_FUNC DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_DEPTH_WRITE_MASK DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    DXGI_FORMAT DsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
};

struct FIndirectDrawCommand
{
    D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = 0;
    D3D12_DRAW_ARGUMENTS DrawArguments{};
    uint32_t Padding[2]{};
};

static_assert(sizeof(FIndirectDrawCommand) == 32, "Indirect command layout must be 32 bytes.");
static_assert(offsetof(FIndirectDrawCommand, ConstantBufferAddress) == 0, "Indirect command constant buffer offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments) == 8, "Indirect command draw arguments offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.VertexCountPerInstance) == 8, "Indirect command vertex count offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.InstanceCount) == 12, "Indirect command instance count offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.StartVertexLocation) == 16, "Indirect command start vertex offset mismatch.");

struct FMeshletDrawData
{
    uint32_t StartIndex = 0;
    uint32_t IndexCount = 0;
    uint32_t RangeIndex = 0;
    uint32_t GroupIndex = 0;
};

struct FSceneModelResource
{
    FMeshGeometryBuffers Geometry;
    uint32_t DrawIndexStart = 0;
    uint32_t DrawIndexCount = 0;
    uint32_t BaseIndexCount = 0;
    DirectX::XMFLOAT4X4 WorldMatrix{};
    DirectX::XMFLOAT4X4 PreviousWorldMatrix{};
    bool bHasPreviousWorldMatrix = false;
    DirectX::XMFLOAT3 Center{ 0.0f, 0.0f, 0.0f };
    float Radius = 1.0f;
    DirectX::XMFLOAT3 BaseColorFactor{ 1.0f, 1.0f, 1.0f };
    float BaseColorAlpha = 1.0f;
    float MetallicFactor = 1.0f;
    float RoughnessFactor = 1.0f;
    DirectX::XMFLOAT3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 SheenColorFactor{ 0.0f, 0.0f, 0.0f };
    float SheenRoughnessFactor = 0.0f;
    float ClearcoatFactor = 0.0f;
    float ClearcoatRoughnessFactor = 0.0f;
    float AnisotropyStrength = 0.0f;
    float AnisotropyRotation = 0.0f;
    float AlphaCutoff = 0.5f;
    uint32_t AlphaMode = 0;
    bool bDoubleSided = false;
    uint32_t ShadingModelId = 0;
    std::wstring BaseColorTexturePath;
    std::wstring MetallicRoughnessTexturePath;
    std::wstring NormalTexturePath;
    std::wstring EmissiveTexturePath;
    std::wstring SheenColorTexturePath;
    std::wstring SheenRoughnessTexturePath;
    std::wstring ClearcoatTexturePath;
    std::wstring ClearcoatRoughnessTexturePath;
    std::wstring ClearcoatNormalTexturePath;
    std::wstring AnisotropyTexturePath;
    bool bHasNormalMap = true;
    std::array<uint32_t, 7> VertexBufferBindlessIndices{ { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX } };
    uint32_t IndexBufferBindlessIndex = UINT32_MAX;
    uint32_t BaseColorBindlessIndex = UINT32_MAX;
    uint32_t MetallicRoughnessBindlessIndex = UINT32_MAX;
    uint32_t NormalBindlessIndex = UINT32_MAX;
    uint32_t EmissiveBindlessIndex = UINT32_MAX;
    uint32_t SheenColorBindlessIndex = UINT32_MAX;
    uint32_t SheenRoughnessBindlessIndex = UINT32_MAX;
    uint32_t ClearcoatBindlessIndex = UINT32_MAX;
    uint32_t ClearcoatRoughnessBindlessIndex = UINT32_MAX;
    uint32_t ClearcoatNormalBindlessIndex = UINT32_MAX;
    uint32_t AnisotropyBindlessIndex = UINT32_MAX;
    DirectX::XMFLOAT4 BaseColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 BaseColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 NormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 NormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 EmissiveTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 EmissiveTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 SheenColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenColorTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 SheenRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatRoughnessTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClearcoatNormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatNormalTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 AnisotropyTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 AnisotropyTransformRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    std::string Name;
    DirectX::XMFLOAT3 BoundsMin{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 BoundsMax{ 0.0f, 0.0f, 0.0f };
    uint32_t ObjectId = 0;
    bool bUseSkinning = false;
    bool bSkinningUpdatedThisFrame = false;
    bool bUseMeshletCulling = false;
    DirectX::XMFLOAT4X4 ModelTransform{};
    int GltfSceneIndex = -1;
    int GltfNodeIndex = -1;
    int GltfSkinIndex = -1;
    uint32_t BoneMatrixBindlessIndex = UINT32_MAX;
    uint32_t BoneMatrixCount = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> BoneMatrixBuffer;
    uint8_t* BoneMatrixBufferMapped = nullptr;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> SkinnedPositionBuffers;
    std::vector<uint32_t> SkinnedPositionSrvBindlessIndices;
    std::vector<uint32_t> SkinnedPositionUavBindlessIndices;
    std::vector<D3D12_RESOURCE_STATES> SkinnedPositionStates;
    Microsoft::WRL::ComPtr<ID3D12Resource> SkinnedPositionBuffer;
    uint32_t SkinnedPositionSrvBindlessIndex = UINT32_MAX;
    uint32_t SkinnedPositionUavBindlessIndex = UINT32_MAX;
    std::vector<FMesh::FMeshlet> Meshlets;
    std::vector<FMesh::FMeshletBounds> MeshletBounds;
    std::vector<uint32_t> MeshletIndices;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> BlasResultBuffer;
    D3D12_RAYTRACING_GEOMETRY_DESC BlasGeometryDesc{};
    bool bHasRayTracingBlas = false;
};

namespace RendererUtils
{
    std::wstring BuildShaderTarget(const wchar_t* StagePrefix, D3D_SHADER_MODEL ShaderModel);
    std::string ResourceStateToString(D3D12_RESOURCE_STATES State);
    bool CreateMeshGeometry(FDX12Device* Device, const FMesh& Mesh, FMeshGeometryBuffers& OutGeometry);
    bool CreateCubeGeometry(FDX12Device* Device, FCubeGeometryBuffers& OutGeometry, float Size = 1.0f);
    bool CreateSphereGeometry(
        FDX12Device* Device,
        FMeshGeometryBuffers& OutGeometry,
        float Radius = 1.0f,
        uint32_t SliceCount = 32,
        uint32_t StackCount = 16);
    bool CreateSceneModelsFromJson(
        FDX12Device* Device,
        const std::wstring& SceneFilePath,
        std::vector<FSceneModelResource>& OutModels,
        DirectX::XMFLOAT3& OutSceneCenter,
        float& OutSceneRadius,
        std::vector<FGltfScene>* OutGltfScenes = nullptr);
    bool CreateDepthResources(FDX12Device* Device, uint32_t Width, uint32_t Height, DXGI_FORMAT Format, FDepthResources& OutDepthResources);
    bool CreateObjectIdResources(
        FDX12Device* Device,
        uint32_t Width,
        uint32_t Height,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture,
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>& OutRtvHeap,
        D3D12_CPU_DESCRIPTOR_HANDLE& OutRtvHandle,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutReadback,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint,
        uint32_t& OutRowPitch);
    bool CreateObjectIdPipeline(
        FDX12Device* Device,
        ID3D12RootSignature* RootSignature,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState);
    void RequestObjectIdReadback(
        uint32_t X,
        uint32_t Y,
        bool& OutRequested,
        bool& OutRecorded,
        uint32_t& OutX,
        uint32_t& OutY);
    bool ConsumeObjectIdReadback(
        const Microsoft::WRL::ComPtr<ID3D12Resource>& ReadbackResource,
        uint32_t RowPitch,
        bool& InOutRequested,
        bool& InOutRecorded,
        uint32_t& OutObjectId);
    bool ComputeSceneModelStats(
        const std::vector<FSceneModelResource>& Models,
        const std::vector<bool>& Visibility,
        size_t& OutTotal,
        size_t& OutCulled);
    void BuildCameraFrustumPlanes(
        const FCamera& Camera,
        DirectX::XMVECTOR OutPlanes[6]);
    void BuildFrustumPlanesFromMatrix(
        const DirectX::XMMATRIX& ViewProjection,
        DirectX::XMVECTOR OutPlanes[6]);
    void UpdateCullingVisibility(
        const FCamera& Camera,
        std::vector<FSceneModelResource>& Models,
        std::vector<bool>& OutVisibility,
        bool bAllowMeshletCulling);
    bool CreateMappedConstantBuffer(FDX12Device* Device, uint64_t BufferSize, FMappedConstantBuffer& OutConstantBuffer);
    bool CreateSkyAtmosphereResources(
        FDX12Device* Device,
        float SkySphereRadius,
        FMeshGeometryBuffers& OutGeometry,
        Microsoft::WRL::ComPtr<ID3D12Resource>& OutConstantBuffer,
        uint8_t*& OutConstantBufferMapped);
    bool CreateSkyAtmospherePipeline(
        FDX12Device* Device,
        DXGI_FORMAT BackBufferFormat,
        const FSkyPipelineConfig& Config,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& OutRootSignature,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& OutPipelineState);
    void UpdateSceneConstants(
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
        uint64_t ConstantBufferOffset = 0,
        const DirectX::XMMATRIX& PreviousViewProjection = DirectX::XMMatrixIdentity(),
        bool bHasPreviousViewProjection = false,
        const DirectX::XMMATRIX& PreviousWorld = DirectX::XMMatrixIdentity(),
        bool bHasPreviousWorld = false,
        uint32_t PreviousSkinnedPositionBindlessIndex = UINT32_MAX,
        bool bHasPreviousSkinning = false);
    bool UpdateGltfSceneAnimation(
        std::vector<FSceneModelResource>& Models,
        const std::vector<FGltfScene>& Scenes,
        std::vector<FGltfAnimationPose>& ScenePoses,
        std::vector<float>& SceneTimes,
        float DeltaTime);
    void UpdateSkyConstants(
        const FCamera& Camera,
        const DirectX::XMMATRIX& WorldMatrix,
        const DirectX::XMMATRIX& Projection,
        const DirectX::XMVECTOR& LightDirection,
        const DirectX::XMFLOAT3& LightColor,
        uint8_t* ConstantBufferMapped);
    DirectX::XMMATRIX BuildDirectionalLightViewProjection(
        const DirectX::XMFLOAT3& SceneCenter,
        float SceneRadius,
        const DirectX::XMFLOAT3& LightDirection);
    bool IsAabbInCameraFrustum(
        const DirectX::XMVECTOR Planes[6],
        const DirectX::XMFLOAT3& BoundsMin,
        const DirectX::XMFLOAT3& BoundsMax);

    // Builds a pipeline key from material properties for shader permutation selection
    // bit 0: Normal, bit 1: MR, bit 2: BaseColor, bit 3: Emissive, bit 4: AlphaMask
    uint32_t BuildPipelineKey(const FSceneModelResource& Model);
}
