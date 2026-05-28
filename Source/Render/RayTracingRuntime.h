#pragma once

#include <cstdint>
#include <vector>
#include <wrl.h>
#include <d3d12.h>

#include "../RHI/RayTracing.h"
#include "GpuResource.h"

class FDX12CommandContext;
class FDX12Device;
class FRenderer;
class FWorld;
struct FMeshSection;

class FRayTracingRuntime
{
public:
    static constexpr uint32_t RayQueryRootConstantDwordCount = 13u;

    enum class EGBufferSlot : uint32_t
    {
        A = 0,
        B = 1,
        C = 2
    };

    bool CreatePipeline(FRenderer& Owner, FDX12Device* Device);
    static bool BuildSceneBlas(FDX12Device* Device, FWorld& World);
    void UpdateBlasRefit(FRenderer& Owner, FDX12CommandContext& CmdContext);
    void BuildTlas(FRenderer& Owner, FDX12CommandContext& CmdContext);
    uint32_t UpdateDepthSrv(FRenderer& Owner, uint32_t FrameIndex, ID3D12Resource* DepthBuffer);
    uint32_t UpdateGBufferSrv(FRenderer& Owner, EGBufferSlot Slot, ID3D12Resource* GBuffer);
    uint32_t UpdateLightingUav(FRenderer& Owner, ID3D12Resource* OutputTarget);
    uint32_t UpdateShadowMaskUav(FRenderer& Owner, ID3D12Resource* ShadowMask);
    uint32_t UpdateShadowMaskSrv(FRenderer& Owner, ID3D12Resource* ShadowMask);

    bool IsPipelineReady() const { return bRayTracingPipelineReady; }
    void SetPipelineReady(bool bReady) { bRayTracingPipelineReady = bReady; }

    FRayTracingDevice RayTracingDevice;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> RayQueryRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryShadowPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQuerySsrFallbackPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQuerySsrHwPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathDebugPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathVndfPipeline;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> RayQueryPathDebugVndfPipeline;
    bool bRayTracingPipelineReady = false;

    std::vector<uint32_t> RayTracingDepthSrvBindlessIndices;
    std::vector<ID3D12Resource*> RayTracingDepthResources;
    uint32_t RayTracingGBufferASrvBindlessIndex = UINT32_MAX;
    uint32_t RayTracingGBufferBSrvBindlessIndex = UINT32_MAX;
    uint32_t RayTracingGBufferCSrvBindlessIndex = UINT32_MAX;
    uint32_t RayTracingLightingUavBindlessIndex = UINT32_MAX;
    uint32_t RayTracingShadowMaskUavBindlessIndex = UINT32_MAX;
    ID3D12Resource* RayTracingGBufferAResource = nullptr;
    ID3D12Resource* RayTracingGBufferBResource = nullptr;
    ID3D12Resource* RayTracingGBufferCResource = nullptr;
    ID3D12Resource* RayTracingLightingResource = nullptr;
    ID3D12Resource* RayTracingShadowMaskUavResource = nullptr;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> TlasScratchBuffers;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> TlasResultBuffers;
    std::vector<FMappedUploadBuffer> TlasInstanceBuffers;
    std::vector<bool> TlasBuilt;
    std::vector<uint32_t> TlasPrevInstanceCount;
    std::vector<uint64_t> TlasPrevInstanceHash;
    uint32_t TlasInstanceCapacity = 0;

    std::vector<FMappedUploadBuffer> PathTracingInstanceDataBuffers;

};
