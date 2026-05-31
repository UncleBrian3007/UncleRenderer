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
#include "../../Shaders/PipelineKeyShared.h"
#include "GpuResource.h"
#include "../World/MeshSection.h"

class FDX12Device;
class FCamera;
class FShaderCompiler;
class FWorld;
class FObject;
struct FGltfMaterialTextures;

struct FSceneConstants
{
    DirectX::XMFLOAT4X4 World;
    DirectX::XMFLOAT4X4 WorldInverseTranspose;
    DirectX::XMFLOAT4X4 View;
    DirectX::XMFLOAT4X4 ViewInverse;
    DirectX::XMFLOAT4X4 Projection;
    DirectX::XMFLOAT4X4 ViewProjectionInverse;
    DirectX::XMFLOAT4X4 PreviousWorld;
    uint32_t PaddingPrevVP = 0;
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
    // UV transform offset+scale (all 10 grouped together)
    DirectX::XMFLOAT4 BaseColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 MetallicRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 NormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 EmissiveTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenColorTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 SheenRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatRoughnessTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 ClearcoatNormalTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT4 AnisotropyTransformOffsetScale{ 0.0f, 0.0f, 1.0f, 1.0f };
    // UV transform rotation cos/sin (all 10 grouped as float2 - .zw unused)
    DirectX::XMFLOAT2 BaseColorTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 MetallicRoughnessTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 NormalTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 EmissiveTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 SheenColorTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 SheenRoughnessTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 ClearcoatTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 ClearcoatRoughnessTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 ClearcoatNormalTransformRotation{ 1.0f, 0.0f };
    DirectX::XMFLOAT2 AnisotropyTransformRotation{ 1.0f, 0.0f };
    DirectX::XMUINT4 VertexBufferBindlessIndices{ 0, 0, 0, 0 };
    DirectX::XMUINT4 ExtraBindlessIndices{ 0, 0, 0, 0 };
    DirectX::XMUINT4 SkinningBindlessIndices{ 0, 0, 0, 0 };
    DirectX::XMFLOAT4 ClusterDagPackedPositionOffset{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedPositionScale{ 1.0f, 1.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantUV{ 0.0f, 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 ClusterDagPackedConstantColor{ 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t ClusterDagVertexPackingMode = 0;
    DirectX::XMUINT3 PaddingClusterDagPacking{ 0, 0, 0 };
    DirectX::XMUINT4 MaterialTextureIndices0{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    DirectX::XMUINT4 MaterialTextureIndices1{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    DirectX::XMUINT4 MaterialTextureIndices2{ UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    uint32_t ClusterDagMaterialPipelineKey = 0;
    DirectX::XMUINT3 PaddingClusterDagResolve{ 0, 0, 0 };
    float EnvMapMipCount = 1.0f;
    DirectX::XMFLOAT3 PaddingEnvMap{ 0.0f, 0.0f, 0.0f };
    float GtaoIntensity = 1.0f;
    uint32_t DeferredLightingVisualizationMode = 0;
    float Pad = 0.0f;
    // Padding to reach 1280 bytes (256-byte CBV alignment)
    uint32_t CbvPad0 = 0;
    DirectX::XMUINT4 CbvPad1{};
    DirectX::XMUINT4 CbvPad2{};
    DirectX::XMUINT4 CbvPad3{};
    DirectX::XMUINT4 CbvPad4{};
    DirectX::XMUINT4 CbvPad5{};
    DirectX::XMUINT4 CbvPad6{};
};
static_assert(sizeof(FSceneConstants) == 1280, "FSceneConstants must be 1280 bytes for 256-byte CBV alignment.");
static_assert(sizeof(FSceneConstants) % 256 == 0, "FSceneConstants must be 256-byte aligned for shared CBV+SRV use.");

struct FIndirectDrawCommand
{
    D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferAddress = 0;
    uint32_t DrawIndexStart = 0;
    uint32_t DrawDataIndex = 0;
    D3D12_DRAW_ARGUMENTS DrawArguments{};

    static FIndirectDrawCommand Make(D3D12_GPU_VIRTUAL_ADDRESS CbAddress, uint32_t IndexStart, uint32_t IndexCount, uint32_t InstanceIndex, uint32_t InDrawDataIndex = 0)
    {
        FIndirectDrawCommand Cmd;
        Cmd.ConstantBufferAddress = CbAddress;
        Cmd.DrawIndexStart = IndexStart;
        Cmd.DrawDataIndex = InDrawDataIndex;
        Cmd.DrawArguments.VertexCountPerInstance = IndexCount;
        Cmd.DrawArguments.InstanceCount = 1;
        Cmd.DrawArguments.StartInstanceLocation = InstanceIndex;
        return Cmd;
    }
};

static_assert(sizeof(FIndirectDrawCommand) == 32, "Indirect command layout must be 32 bytes.");
static_assert(offsetof(FIndirectDrawCommand, ConstantBufferAddress) == 0, "Indirect command constant buffer offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawIndexStart) == 8, "Indirect command draw index start offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawDataIndex) == 12, "Indirect command draw data index offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments) == 16, "Indirect command draw arguments offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.VertexCountPerInstance) == 16, "Indirect command vertex count offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.InstanceCount) == 20, "Indirect command instance count offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.StartVertexLocation) == 24, "Indirect command start vertex offset mismatch.");
static_assert(offsetof(FIndirectDrawCommand, DrawArguments.StartInstanceLocation) == 28, "Indirect command start instance offset mismatch.");


namespace RendererUtils
{
    constexpr uint32_t GPipelineKeySkinningBit = RENDER_PIPELINE_KEY_SKINNING_BIT;
    constexpr uint32_t GPipelineKeyDoubleSidedBit = RENDER_PIPELINE_KEY_DOUBLE_SIDED_BIT;
    constexpr uint32_t GPipelineKeyAlphaMaskBit = RENDER_PIPELINE_KEY_ALPHA_MASK_BIT;
    constexpr uint32_t GPipelineKeyAlphaMaskMask = RENDER_PIPELINE_KEY_ALPHA_MASK_MASK;
    constexpr uint32_t GPipelineKeyDoubleSidedMask = RENDER_PIPELINE_KEY_DOUBLE_SIDED_MASK;
    constexpr uint32_t GMaterialBindlessIndexCount = 10;
    using FMaterialBindlessIndices = std::array<uint32_t, GMaterialBindlessIndexCount>;

    struct FUpdateSceneConstantsParams
    {
        const FCamera* Camera = nullptr;
        const FObject* Object = nullptr;
        const FMeshSection* Section = nullptr;
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
        FDX12Device* Device,
        const std::wstring& ShaderPath,
        const std::wstring& EntryPoint,
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
        FWorld& World,
        bool bAllowMeshletCulling);
    void UpdateSceneConstants(const FUpdateSceneConstantsParams& Params);
    FMaterialBindlessIndices BuildMaterialBindlessIndices(const FMeshSection& Section);
    FMaterialBindlessIndices BuildMaterialBindlessIndices(const FSectionRenderData& RenderData);
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
    uint32_t BuildPipelineKey(const FMeshSection& Section);
}
