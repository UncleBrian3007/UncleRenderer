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
#include <limits>
#include "GpuResource.h"
#include "SceneModelResource.h"
#include "../Scene/GltfAnimation.h"

class FDX12Device;
class FCamera;
class FShaderCompiler;
struct FGltfMaterialTextures;

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
    DirectX::XMFLOAT4 ClusterDagPackedPositionOffset{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedPositionScale{ 1.0f, 1.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantUV{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t ClusterDagVertexPackingMode = 0;
    DirectX::XMUINT3 PaddingClusterDagPacking{ 0, 0, 0 };
    float EnvMapMipCount = 1.0f;
    DirectX::XMFLOAT3 PaddingEnvMap{ 0.0f, 0.0f, 0.0f };
    float GtaoIntensity = 1.0f;
    uint32_t DeferredLightingVisualizationMode = 0;
    float Pad = 0.0f;
};

enum class EAlphaMode : uint32_t
{
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

struct FIndirectDrawCommand
{
    D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = 0;
    uint32_t DrawIndexStart = 0;
    D3D12_DRAW_ARGUMENTS DrawArguments{};
    uint32_t Padding = 0;
};

static_assert(sizeof(FIndirectDrawCommand) == 32, "Indirect command layout must be 32 bytes.");
static_assert(offsetof(FIndirectDrawCommand, ConstantBufferAddress) == 0, "Indirect command constant buffer offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawIndexStart) == 8, "Indirect command draw index start offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments) == 12, "Indirect command draw arguments offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.VertexCountPerInstance) == 12, "Indirect command vertex count offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.InstanceCount) == 16, "Indirect command instance count offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.StartVertexLocation) == 20, "Indirect command start vertex offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.StartInstanceLocation) == 24, "Indirect command start instance offset mismatch.");


namespace RendererUtils
{
    struct FUpdateSceneConstantsParams
    {
        const FCamera* Camera = nullptr;
        const FSceneModelResource* Model = nullptr;
        float LightIntensity = 1.0f;
        DirectX::XMVECTOR LightDirection = DirectX::XMVectorZero();
        DirectX::XMFLOAT3 LightColor{ 1.0f, 1.0f, 1.0f };
        DirectX::XMMATRIX LightViewProjection = DirectX::XMMatrixIdentity();
        DirectX::XMMATRIX Projection = DirectX::XMMatrixIdentity();
        float ShadowStrength = 0.0f;
        float ShadowBias = 0.0f;
        float ShadowMapWidth = 0.0f;
        float ShadowMapHeight = 0.0f;
        float EnvMapMipCount = 1.0f;
        bool bGtaoEnabled = false;
        float GtaoIntensity = 1.0f;
        uint8_t* ConstantBufferMapped = nullptr;
        uint64_t ConstantBufferOffset = 0u;
        DirectX::XMMATRIX PreviousViewProjection = DirectX::XMMatrixIdentity();
        bool bHasPreviousViewProjection = false;
        DirectX::XMMATRIX PreviousWorld = DirectX::XMMatrixIdentity();
        bool bHasPreviousWorld = false;
        uint32_t PreviousSkinnedPositionBindlessIndex = UINT32_MAX;
        bool bHasPreviousSkinning = false;
        uint32_t DeferredLightingVisualizationMode = 0u;
        bool bUseClusterDagIndexBuffer = false;
        bool bUseClusterDagDebugColor = false;
    };

    std::wstring BuildShaderTarget(const wchar_t* StagePrefix, D3D_SHADER_MODEL ShaderModel);
    std::wstring BuildShaderEntryPoint(const std::wstring& ShaderPath, const wchar_t* StageSuffix);
    std::wstring BuildComputeShaderEntryPoint(const std::wstring& ShaderPath);
    std::wstring BuildVertexShaderEntryPoint(const std::wstring& ShaderPath);
    std::wstring BuildPixelShaderEntryPoint(const std::wstring& ShaderPath);
    bool CompileVertexShader(
        FShaderCompiler& Compiler,
        FDX12Device* Device,
        const std::wstring& ShaderPath,
        std::vector<uint8_t>& OutByteCode,
        const std::vector<std::wstring>& Defines = {});
    bool CompilePixelShader(
        FShaderCompiler& Compiler,
        FDX12Device* Device,
        const std::wstring& ShaderPath,
        std::vector<uint8_t>& OutByteCode,
        const std::vector<std::wstring>& Defines = {});
    bool CompileComputeShader(
        FShaderCompiler& Compiler,
        FDX12Device* Device,
        const std::wstring& ShaderPath,
        std::vector<uint8_t>& OutByteCode,
        const std::vector<std::wstring>& Defines = {});
    bool CompileComputeShader(
        FShaderCompiler& Compiler,
        const std::wstring& Target,
        const std::wstring& ShaderPath,
        std::vector<uint8_t>& OutByteCode,
        const std::vector<std::wstring>& Defines = {});
    std::string ResourceStateToString(D3D12_RESOURCE_STATES State);
    bool CreateMeshGeometry(FDX12Device* Device, const FMesh& Mesh, FMeshGeometryBuffers& OutGeometry);
    bool CreateCubeGeometry(FDX12Device* Device, FCubeGeometryBuffers& OutGeometry, float Size = 1.0f);
    bool CreateSphereGeometry(
        FDX12Device* Device,
        FMeshGeometryBuffers& OutGeometry,
        float Radius = 1.0f,
        uint32_t SliceCount = 32,
        uint32_t StackCount = 16);
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
    void UpdateSceneConstants(const FUpdateSceneConstantsParams& Params);
    bool UpdateGltfSceneAnimation(
        std::vector<FSceneModelResource>& Models,
        const std::vector<FGltfScene>& Scenes,
        std::vector<FGltfAnimationPose>& ScenePoses,
        std::vector<float>& SceneTimes,
        float DeltaTime);
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
