#include "ClusterDagRuntime.h"

#include "ClusterDagStreamingManager.h"
#include "DeferredPassContext.h"
#include "../DeferredRenderer.h"
#include "../../Core/RendererConfig.h"
#include "../RendererUtils.h"
#include "../ShaderCompiler.h"
#include "../../Core/GpuDebugMarkers.h"
#include "../../Core/Logger.h"
#include "../../RHI/DX12CommandContext.h"
#include "../../RHI/DX12CommandQueue.h"
#include "../../RHI/DX12Device.h"
#include "../../Scene/Camera.h"
#include <d3dx12.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace
{
    enum class EClusterDagSelectPermutation : uint32_t
    {
        Default = 0,
        Debug = 1,
        Fast = 2,
        FastDebug = 3
    };

    constexpr uint32_t ClusterDagSelectPermutationCount = 4u;
    constexpr uint32_t ClusterDagFastPermutationCount = 2u;

    uint32_t GetClusterDagSelectPermutationIndex(EClusterDagSelectPermutation Permutation)
    {
        return static_cast<uint32_t>(Permutation);
    }

    const char* GetClusterDagSelectPermutationName(EClusterDagSelectPermutation Permutation)
    {
        switch (Permutation)
        {
        case EClusterDagSelectPermutation::Default:
            return "default";
        case EClusterDagSelectPermutation::Debug:
            return "debug";
        case EClusterDagSelectPermutation::Fast:
            return "fast";
        case EClusterDagSelectPermutation::FastDebug:
            return "fast_debug";
        default:
            return "unknown";
        }
    }

    std::vector<std::wstring> BuildClusterDagSelectDefines(EClusterDagSelectPermutation Permutation)
    {
        const bool bDebug = Permutation == EClusterDagSelectPermutation::Debug
            || Permutation == EClusterDagSelectPermutation::FastDebug;
        const bool bFast = Permutation == EClusterDagSelectPermutation::Fast
            || Permutation == EClusterDagSelectPermutation::FastDebug;
        return
        {
            bDebug
                ? L"USE_CLUSTER_DAG_DEBUG=1"
                : L"USE_CLUSTER_DAG_DEBUG=0",
            bFast
                ? L"USE_CLUSTER_DAG_FAST=1"
                : L"USE_CLUSTER_DAG_FAST=0"
        };
    }

    uint32_t GetClusterDagInitPipelineIndex(bool bClusterDagFastEnabled, bool bPersistentQueue)
    {
        return (bClusterDagFastEnabled ? 1u : 0u)
            | (bPersistentQueue ? 2u : 0u);
    }

    std::vector<std::wstring> BuildClusterDagInitDefines(bool bClusterDagFastEnabled, bool bPersistentQueue)
    {
        return
        {
            bClusterDagFastEnabled
                ? L"USE_CLUSTER_DAG_FAST=1"
                : L"USE_CLUSTER_DAG_FAST=0",
            bPersistentQueue
                ? L"USE_CLUSTER_DAG_PERSISTENT_QUEUE=1"
                : L"USE_CLUSTER_DAG_PERSISTENT_QUEUE=0"
        };
    }

    EClusterDagSelectPermutation ResolveClusterDagSelectPermutation(bool bClusterDagDebugEnabled, bool bClusterDagFastEnabled)
    {
        if (bClusterDagFastEnabled)
        {
            return bClusterDagDebugEnabled
                ? EClusterDagSelectPermutation::FastDebug
                : EClusterDagSelectPermutation::Fast;
        }

        return bClusterDagDebugEnabled
            ? EClusterDagSelectPermutation::Debug
            : EClusterDagSelectPermutation::Default;
    }

    float ComputeMaxScale(const DirectX::XMFLOAT4X4& Matrix)
    {
        const float ScaleX = std::sqrt(Matrix._11 * Matrix._11 + Matrix._21 * Matrix._21 + Matrix._31 * Matrix._31);
        const float ScaleY = std::sqrt(Matrix._12 * Matrix._12 + Matrix._22 * Matrix._22 + Matrix._32 * Matrix._32);
        const float ScaleZ = std::sqrt(Matrix._13 * Matrix._13 + Matrix._23 * Matrix._23 + Matrix._33 * Matrix._33);

        return (std::max)((std::max)(ScaleX, ScaleY), ScaleZ);
    }

    DirectX::XMFLOAT4 TransformBoundingSphere(const DirectX::XMFLOAT4& Sphere, const DirectX::XMMATRIX& World, float MaxScale)
    {
        const DirectX::XMVECTOR LocalCenter = DirectX::XMVectorSet(Sphere.x, Sphere.y, Sphere.z, 1.0f);
        const DirectX::XMVECTOR WorldCenter = DirectX::XMVector3TransformCoord(LocalCenter, World);

        DirectX::XMFLOAT3 Center = {};
        DirectX::XMStoreFloat3(&Center, WorldCenter);
        return DirectX::XMFLOAT4(Center.x, Center.y, Center.z, Sphere.w * MaxScale);
    }

    constexpr uint32_t GClusterDagQueueStateUintCount = 18u; // 16 base + 2 committed-write offsets (Pass0 Group + Candidate)
    constexpr uint32_t GClusterDagQueueStateBufferSize = GClusterDagQueueStateUintCount * sizeof(uint32_t);
    constexpr uint32_t GClusterDagLevelSplitQueueStateUintCount = 6u;
    constexpr uint32_t GClusterDagLevelSplitQueueStateBufferSize = GClusterDagLevelSplitQueueStateUintCount * sizeof(uint32_t);
    constexpr uint32_t GClusterDagLevelSplitNodeArgsUintCount = 6u;
    constexpr uint32_t GClusterDagLevelSplitNodeArgsBufferSize = GClusterDagLevelSplitNodeArgsUintCount * sizeof(uint32_t);
    constexpr uint32_t GClusterDagLevelSplitClusterArgsBufferSize = sizeof(D3D12_DISPATCH_ARGUMENTS);
    constexpr uint32_t GClusterDagSwRasterArgsBufferSize = sizeof(D3D12_DISPATCH_ARGUMENTS);
    constexpr uint32_t GClusterDagTraversalEpochMask = 0x7fffffffu;
    constexpr uint32_t GClusterDagGroupFlagsLowMask = 0x0000ffffu;
    constexpr uint32_t GClusterDagGroupPageIndexShift = 16u;
    constexpr uint32_t GClusterDagRootStreamingPageIndex = 0u;

    uint32_t ComputeTraversalEpoch(const FDeferredRenderer& Owner)
    {
        return static_cast<uint32_t>((Owner.GetFrameNumber() & GClusterDagTraversalEpochMask) + 1u);
    }

    struct FClusterDagStreamingShaderBindings
    {
        uint32_t PageTableBufferIndex = UINT32_MAX;
        uint32_t PageDataBufferIndex = UINT32_MAX;
        uint32_t StreamingRequestBufferIndex = UINT32_MAX;
        uint32_t StreamingRequestCapacity = 0;
        uint32_t StreamingResourceId = 0;
        uint32_t PageSlotBytes = 0;
    };

    FClusterDagStreamingShaderBindings BuildClusterDagStreamingShaderBindings(const FDeferredRenderer& Owner, uint32_t FrameIndex)
    {
        FClusterDagStreamingShaderBindings Bindings;
        const FClusterDagStreamingManager* StreamingManager = Owner.GetClusterDagStreamingManager();
        if (StreamingManager != nullptr && StreamingManager->IsEnabled())
        {
            Bindings.PageTableBufferIndex = StreamingManager->GetPageTableSrvBindlessIndex();
            Bindings.PageDataBufferIndex = StreamingManager->GetPageDataSrvBindlessIndex();
            Bindings.StreamingRequestBufferIndex = StreamingManager->GetFeedbackUavBindlessIndex(FrameIndex);
            Bindings.StreamingRequestCapacity = StreamingManager->GetRequestCapacity();
            Bindings.StreamingResourceId = StreamingManager->GetStreamingResourceId();
            Bindings.PageSlotBytes = StreamingManager->GetPageSlotBytes();
        }
        return Bindings;
    }

}

void FClusterDagRuntime::ApplyConfig(const FRendererConfig& Config)
{
    bEnabled = Config.bEnableClusterDAGRuntime;
    bFastShaderEnabled = Config.bEnableClusterDAGFastShader;
    bDebugEnabled = Config.bEnableClusterDAGDebug;
    ActiveTraversalMode = Config.ClusterDAGTraversalMode;
    TargetErrorPixels = Config.ClusterDAGTargetErrorPixels;
    SwRasterThresholdPixels = Config.ClusterDAGSwRasterThresholdPixels;
    bForceMipEnabled = Config.bEnableClusterDAGForceMip;
    ForceMipLevel = Config.ClusterDAGForceMipLevel;
    bForceMipSkipFrustumCull = Config.bEnableClusterDAGForceMipSkipFrustumCull;
    bForceSoftwareRaster = Config.bEnableClusterDAGForceSoftwareRaster;
}

bool FClusterDagRuntime::InitializePipelines(FDeferredRenderer& Owner, FDX12Device* Device)
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    if (!GpuDrivenCullingProvider.CullingRootSignature || !GpuDrivenCullingProvider.MeshletRunRootSignature)
    {
        return false;
    }

    FShaderCompiler Compiler;

    for (uint32_t PermutationIndex = 0; PermutationIndex < ClusterDagSelectPermutationCount; ++PermutationIndex)
    {
        const EClusterDagSelectPermutation Permutation = static_cast<EClusterDagSelectPermutation>(PermutationIndex);
        const std::vector<std::wstring> Defines = BuildClusterDagSelectDefines(Permutation);

        std::vector<uint8_t> PersistentByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/PersistentClusterDagCull.hlsl", PersistentByteCode, Defines))
        {
            LogError(std::string("Failed to compile persistent DAG cull compute shader permutation: ")
                + GetClusterDagSelectPermutationName(Permutation));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC PersistentDesc = {};
        PersistentDesc.pRootSignature = GpuDrivenCullingProvider.CullingRootSignature;
        PersistentDesc.CS = { PersistentByteCode.data(), PersistentByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&PersistentDesc, IID_PPV_ARGS(PersistentCullPipelines[PermutationIndex].ReleaseAndGetAddressOf())));

        std::vector<uint8_t> LevelSplitNodeByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/ClusterDagLevelSplitNodeCull.hlsl", LevelSplitNodeByteCode, Defines))
        {
            LogError(std::string("Failed to compile level split DAG node cull compute shader permutation: ")
                + GetClusterDagSelectPermutationName(Permutation));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC LevelSplitNodeDesc = {};
        LevelSplitNodeDesc.pRootSignature = GpuDrivenCullingProvider.CullingRootSignature;
        LevelSplitNodeDesc.CS = { LevelSplitNodeByteCode.data(), LevelSplitNodeByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&LevelSplitNodeDesc, IID_PPV_ARGS(LevelSplitNodeCullPipelines[PermutationIndex].ReleaseAndGetAddressOf())));

        std::vector<uint8_t> LevelSplitClusterByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/ClusterDagLevelSplitClusterCull.hlsl", LevelSplitClusterByteCode, Defines))
        {
            LogError(std::string("Failed to compile level split DAG cluster cull compute shader permutation: ")
                + GetClusterDagSelectPermutationName(Permutation));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC LevelSplitClusterDesc = {};
        LevelSplitClusterDesc.pRootSignature = GpuDrivenCullingProvider.CullingRootSignature;
        LevelSplitClusterDesc.CS = { LevelSplitClusterByteCode.data(), LevelSplitClusterByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&LevelSplitClusterDesc, IID_PPV_ARGS(LevelSplitClusterCullPipelines[PermutationIndex].ReleaseAndGetAddressOf())));
    }

    for (uint32_t PersistentIndex = 0; PersistentIndex < 2u; ++PersistentIndex)
    {
        const bool bPersistent = PersistentIndex != 0u;
        for (uint32_t FastIndex = 0; FastIndex < ClusterDagFastPermutationCount; ++FastIndex)
        {
            const bool bFast = FastIndex != 0u;
            const std::vector<std::wstring> Defines = BuildClusterDagInitDefines(bFast, bPersistent);
            std::vector<uint8_t> InitByteCode;
            if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/InitClusterDagQueues.hlsl", InitByteCode, Defines))
            {
                LogError(std::string("Failed to compile DAG queue init compute shader permutation: ")
                    + (bPersistent ? "persistent_" : "")
                    + (bFast ? "fast" : "default"));
                return false;
            }

            D3D12_COMPUTE_PIPELINE_STATE_DESC InitDesc = {};
            InitDesc.pRootSignature = GpuDrivenCullingProvider.CullingRootSignature;
            InitDesc.CS = { InitByteCode.data(), InitByteCode.size() };
            const uint32_t InitPipelineIndex = GetClusterDagInitPipelineIndex(bFast, bPersistent);
            HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&InitDesc, IID_PPV_ARGS(InitQueuePipelines[InitPipelineIndex].ReleaseAndGetAddressOf())));
        }
    }

    for (uint32_t FastIndex = 0; FastIndex < ClusterDagFastPermutationCount; ++FastIndex)
    {
        const bool bFast = FastIndex != 0u;
        const std::vector<std::wstring> Defines = BuildClusterDagInitDefines(bFast, false);

        std::vector<uint8_t> LevelSplitInitByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/InitClusterDagLevelSplitQueues.hlsl", LevelSplitInitByteCode, Defines))
        {
            LogError(std::string("Failed to compile level split DAG queue init compute shader permutation: ")
                + (bFast ? "fast" : "default"));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC LevelSplitInitDesc = {};
        LevelSplitInitDesc.pRootSignature = GpuDrivenCullingProvider.CullingRootSignature;
        LevelSplitInitDesc.CS = { LevelSplitInitByteCode.data(), LevelSplitInitByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&LevelSplitInitDesc, IID_PPV_ARGS(LevelSplitInitPipelines[FastIndex].ReleaseAndGetAddressOf())));

        std::vector<uint8_t> LevelSplitPrepareNodeByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/PrepareClusterDagLevelSplitNodeArgs.hlsl", LevelSplitPrepareNodeByteCode, Defines))
        {
            LogError(std::string("Failed to compile level split DAG node args prepare compute shader permutation: ")
                + (bFast ? "fast" : "default"));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC LevelSplitPrepareNodeDesc = {};
        LevelSplitPrepareNodeDesc.pRootSignature = GpuDrivenCullingProvider.CullingRootSignature;
        LevelSplitPrepareNodeDesc.CS = { LevelSplitPrepareNodeByteCode.data(), LevelSplitPrepareNodeByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&LevelSplitPrepareNodeDesc, IID_PPV_ARGS(LevelSplitPrepareNodePipelines[FastIndex].ReleaseAndGetAddressOf())));

        std::vector<uint8_t> LevelSplitPrepareClusterByteCode;
        if (!RendererUtils::CompileComputeShader(Compiler, Device, L"Shaders/ClusterDag/PrepareClusterDagLevelSplitClusterArgs.hlsl", LevelSplitPrepareClusterByteCode, Defines))
        {
            LogError(std::string("Failed to compile level split DAG cluster args prepare compute shader permutation: ")
                + (bFast ? "fast" : "default"));
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC LevelSplitPrepareClusterDesc = {};
        LevelSplitPrepareClusterDesc.pRootSignature = GpuDrivenCullingProvider.CullingRootSignature;
        LevelSplitPrepareClusterDesc.CS = { LevelSplitPrepareClusterByteCode.data(), LevelSplitPrepareClusterByteCode.size() };
        HR_CHECK(Device->GetDevice()->CreateComputePipelineState(&LevelSplitPrepareClusterDesc, IID_PPV_ARGS(LevelSplitPrepareClusterPipelines[FastIndex].ReleaseAndGetAddressOf())));
    }

    D3D12_INDIRECT_ARGUMENT_DESC DispatchArgumentDesc = {};
    DispatchArgumentDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

    D3D12_COMMAND_SIGNATURE_DESC DispatchCommandDesc = {};
    DispatchCommandDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    DispatchCommandDesc.NumArgumentDescs = 1;
    DispatchCommandDesc.pArgumentDescs = &DispatchArgumentDesc;
    HR_CHECK(Device->GetDevice()->CreateCommandSignature(&DispatchCommandDesc, nullptr, IID_PPV_ARGS(DispatchCommandSignature.ReleaseAndGetAddressOf())));
    return true;
}

bool FClusterDagRuntime::InitializeResources(FDeferredRenderer& Owner, FDX12Device* Device)
{
    ActiveTraversalMode = Owner.GetClusterDagTraversalMode();
    IndirectDrawRanges.clear();
    RuntimeGroupCount = 0;
    RuntimeCommandCount = 0;
    RuntimeChildRefCount = 0;
    StreamingPageCount = 1;
    StreamingPageSources.clear();
    RuntimeMaxTraversalLevels = 1;
    bResourcesReady = false;
    VisibleRootCount = 0; 
    ClusterCount = 0;

    if (!Owner.IsClusterDagEnabled())
    {
        return true;
    }

    FPreparedData PreparedData;
    if (!PrepareRuntimeData(Owner, PreparedData))
    {
        return true;
    }

    const bool bCreateSucceeded = CreateRuntimeResources(Owner, Device, PreparedData);
    bResourcesReady = bCreateSucceeded;
    return bCreateSucceeded;
}

ID3D12Resource* FClusterDagRuntime::GetDrawDataBuffer() const
{
    return DrawDataBuffer.Get();
}

D3D12_RESOURCE_STATES& FClusterDagRuntime::GetDrawDataState()
{
    return DrawDataBuffer.State;
}

void FClusterDagRuntime::AddPasses(FDeferredPassContext& Context) const
{
    FClusterDagStreamingManager* StreamingManager = Context.Owner.GetClusterDagStreamingManager();

    if (ActiveTraversalMode == EClusterDAGTraversalMode::PersistentQueue)
    {
		StreamingManager->AddBeginFramePass(Context);
        AddInitQueuePass(Context, "Init ClusterDAG Queues");
        AddPersistentCullPass(Context, "Persistent ClusterDAG Cull");
		StreamingManager->AddFeedbackReadbackPass(Context);
        AddFinalizeIndirectArgsPass(Context, "Finalize ClusterDAG Indirect Args");
    }
    else if (ActiveTraversalMode == EClusterDAGTraversalMode::LevelSplitQueue)
    {
		StreamingManager->AddBeginFramePass(Context);
        AddLevelSplitInitPass(Context, "Level Split ClusterDAG Init");
        for (uint32_t Level = 0; Level < RuntimeMaxTraversalLevels; ++Level)
        {
            AddLevelSplitPrepareNodePass(Context, "Level Split ClusterDAG Prepare Node", Level);
            AddLevelSplitNodeCullPass(Context, "Level Split ClusterDAG Node Cull", Level);
        }
        AddLevelSplitPrepareClusterPass(Context, "Level Split ClusterDAG Prepare Cluster");
        AddLevelSplitClusterCullPass(Context, "Level Split ClusterDAG Cluster Cull");
		StreamingManager->AddFeedbackReadbackPass(Context);
        AddFinalizeIndirectArgsPass(Context, "Level Split ClusterDAG Finalize Indirect Args", "Level Split ClusterDAG");
    }
}

bool FClusterDagRuntime::HasResources() const
{
    return bResourcesReady;
}

bool FClusterDagRuntime::UsesRuntimePath(const FDeferredRenderer& Owner, const FSceneModelResource& Model) const
{
    return Owner.IsClusterDagEnabled()
        && HasResources()
        && Model.bUseClusterDagRuntime
        && Model.BoneMatrixBuffer.SrvBindlessIndex == UINT32_MAX
        && Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Opaque);
}

ID3D12Resource* FClusterDagRuntime::GetIndirectCommandBuffer(const FDeferredRenderer& Owner) const
{
    assert(Owner.GetFrameIndex() < IndirectCommandBuffers.size());
    return IndirectCommandBuffers[Owner.GetFrameIndex()].Get();
}

D3D12_RESOURCE_STATES& FClusterDagRuntime::GetIndirectCommandState(FDeferredRenderer& Owner)
{
    assert(Owner.GetFrameIndex() < IndirectCommandBuffers.size());
    return IndirectCommandBuffers[Owner.GetFrameIndex()].State;
}

ID3D12Resource* FClusterDagRuntime::GetRunCountBuffer(const FDeferredRenderer& Owner) const
{
    assert(Owner.GetFrameIndex() < RunCountBuffers.size());
    return RunCountBuffers[Owner.GetFrameIndex()].Get();
}

D3D12_RESOURCE_STATES& FClusterDagRuntime::GetRunCountState(FDeferredRenderer& Owner)
{
    assert(Owner.GetFrameIndex() < RunCountBuffers.size());
    return RunCountBuffers[Owner.GetFrameIndex()].State;
}

bool FClusterDagRuntime::ValidatePreparedRuntimeData(const FPreparedData& Data) const
{
    static_assert(sizeof(FRuntimeClusterChildRef) == 8);

    const auto Fail = [&](const std::string& Reason)
    {
        LogError("ClusterDagRuntime validation failed: " + Reason);
        return false;
    };

    if (Data.Clusters.empty())
    {
        return Fail("cluster list is empty");
    }

    if (Data.RootGroups.empty())
    {
        return Fail("root group list is empty");
    }

    if (Data.DrawDatas.empty())
    {
        return Fail("draw packet list is empty");
    }

    for (uint32_t GroupIndex = 0; GroupIndex < static_cast<uint32_t>(Data.Groups.size()); ++GroupIndex)
    {
        const FSceneGroupData& Group = Data.Groups[GroupIndex];
        if (Group.ChildRefStart > Data.ChildRefs.size()
            || Group.ChildRefCount > Data.ChildRefs.size() - Group.ChildRefStart)
        {
            return Fail("group child ref range is out of bounds; groupIndex=" + std::to_string(GroupIndex)
                + ", childRefStart=" + std::to_string(Group.ChildRefStart)
                + ", childRefCount=" + std::to_string(Group.ChildRefCount)
                + ", totalChildRefs=" + std::to_string(Data.ChildRefs.size()));
        }
    }

    for (uint32_t RootOrdinal = 0; RootOrdinal < static_cast<uint32_t>(Data.RootGroups.size()); ++RootOrdinal)
    {
        const uint32_t RootGroupIndex = Data.RootGroups[RootOrdinal];
        if (RootGroupIndex >= Data.Groups.size())
        {
            return Fail("root group out of range; rootOrdinal=" + std::to_string(RootOrdinal)
                + ", groupIndex=" + std::to_string(RootGroupIndex)
                + ", groupCount=" + std::to_string(Data.Groups.size()));
        }

        const FSceneGroupData& RootGroup = Data.Groups[RootGroupIndex];
        if (RootGroup.ChildRefCount == 0)
        {
            return Fail("root group has no child clusters; rootOrdinal=" + std::to_string(RootOrdinal)
                + ", groupIndex=" + std::to_string(RootGroupIndex));
        }
    }

    for (uint32_t ClusterIndex = 0; ClusterIndex < static_cast<uint32_t>(Data.Clusters.size()); ++ClusterIndex)
    {
        const FSceneClusterData& Cluster = Data.Clusters[ClusterIndex];
        if (Cluster.GroupIndex != GClusterDAGInvalidIndex && Cluster.GroupIndex >= Data.Groups.size())
        {
            return Fail("cluster group index is out of range; clusterIndex=" + std::to_string(ClusterIndex)
                + ", groupIndex=" + std::to_string(Cluster.GroupIndex)
                + ", groupCount=" + std::to_string(Data.Groups.size()));
        }

        if (Cluster.GeneratingGroupIndex != GClusterDAGInvalidIndex && Cluster.GeneratingGroupIndex >= Data.Groups.size())
        {
            return Fail("cluster generating group index is out of range; clusterIndex=" + std::to_string(ClusterIndex)
                + ", generatingGroupIndex=" + std::to_string(Cluster.GeneratingGroupIndex)
                + ", groupCount=" + std::to_string(Data.Groups.size()));
        }

        if (Cluster.GeneratingGroupIndex != GClusterDAGInvalidIndex
            && Data.Groups[Cluster.GeneratingGroupIndex].ChildRefCount == 0)
        {
            return Fail("cluster generating group has no child refs; clusterIndex=" + std::to_string(ClusterIndex)
                + ", generatingGroupIndex=" + std::to_string(Cluster.GeneratingGroupIndex));
        }

        if (Cluster.DrawDataStart > Data.DrawDatas.size()
            || Cluster.DrawDataCount > Data.DrawDatas.size() - Cluster.DrawDataStart)
        {
            return Fail("cluster draw packet range is out of bounds; clusterIndex=" + std::to_string(ClusterIndex)
                + ", drawDataStart=" + std::to_string(Cluster.DrawDataStart)
                + ", drawDataCount=" + std::to_string(Cluster.DrawDataCount)
                + ", drawDataTotal=" + std::to_string(Data.DrawDatas.size()));
        }
    }

    for (uint32_t ChildRefIndex = 0; ChildRefIndex < static_cast<uint32_t>(Data.ChildRefs.size()); ++ChildRefIndex)
    {
        const FRuntimeClusterChildRef& ChildRef = Data.ChildRefs[ChildRefIndex];
        if (ChildRef.InstanceIndex != GClusterDAGInvalidIndex)
        {
            return Fail("instance child ref is not supported in runtime v1; childRefIndex=" + std::to_string(ChildRefIndex)
                + ", instanceIndex=" + std::to_string(ChildRef.InstanceIndex));
        }

        if (ChildRef.ClusterIndex == GClusterDAGInvalidIndex)
        {
            return Fail("invalid child ref cluster index is not supported in runtime v1; childRefIndex=" + std::to_string(ChildRefIndex));
        }

        if (ChildRef.ClusterIndex >= Data.Clusters.size())
        {
            return Fail("child ref cluster index is out of range; childRefIndex=" + std::to_string(ChildRefIndex)
                + ", clusterIndex=" + std::to_string(ChildRef.ClusterIndex)
                + ", clusterCount=" + std::to_string(Data.Clusters.size()));
        }
    }

    uint32_t MaxTraversalDepth = 0;
    uint32_t VisitedClusterCount = 0;
    std::vector<uint8_t> VisitState(Data.Clusters.size(), 0u);
    std::function<bool(uint32_t, uint32_t)> VisitCluster = [&](uint32_t ClusterIndex, uint32_t Depth) -> bool
    {
        MaxTraversalDepth = (std::max)(MaxTraversalDepth, Depth);
        if (VisitState[ClusterIndex] == 1u)
        {
            return Fail("cycle detected while traversing runtime hierarchy; clusterIndex=" + std::to_string(ClusterIndex)
                + ", depth=" + std::to_string(Depth));
        }

        if (VisitState[ClusterIndex] == 2u)
        {
            return true;
        }

        VisitState[ClusterIndex] = 1u;
        VisitedClusterCount += 1u;

        const FSceneClusterData& Cluster = Data.Clusters[ClusterIndex];
        if (Cluster.GeneratingGroupIndex != GClusterDAGInvalidIndex)
        {
            const FSceneGroupData& Group = Data.Groups[Cluster.GeneratingGroupIndex];
            for (uint32_t ChildOffset = 0; ChildOffset < Group.ChildRefCount; ++ChildOffset)
            {
                const uint32_t ChildRefIndex = Group.ChildRefStart + ChildOffset;
                const FRuntimeClusterChildRef& ChildRef = Data.ChildRefs[ChildRefIndex];
                if (ChildRef.ClusterIndex == GClusterDAGInvalidIndex)
                {
                    continue;
                }

                const FSceneClusterData& ChildCluster = Data.Clusters[ChildRef.ClusterIndex];
                if (ChildCluster.MipLevel >= Cluster.MipLevel)
                {
                    return Fail("non-decreasing mip traversal detected; parentCluster=" + std::to_string(ClusterIndex)
                        + ", parentMip=" + std::to_string(Cluster.MipLevel)
                        + ", childCluster=" + std::to_string(ChildRef.ClusterIndex)
                        + ", childMip=" + std::to_string(ChildCluster.MipLevel));
                }

                if (!VisitCluster(ChildRef.ClusterIndex, Depth + 1u))
                {
                    return false;
                }
            }
        }

        VisitState[ClusterIndex] = 2u;
        return true;
    };

    for (uint32_t RootGroupIndex : Data.RootGroups)
    {
        const FSceneGroupData& RootGroup = Data.Groups[RootGroupIndex];
        for (uint32_t ChildOffset = 0; ChildOffset < RootGroup.ChildRefCount; ++ChildOffset)
        {
            const uint32_t ChildRefIndex = RootGroup.ChildRefStart + ChildOffset;
            const FRuntimeClusterChildRef& ChildRef = Data.ChildRefs[ChildRefIndex];
            if (ChildRef.ClusterIndex == GClusterDAGInvalidIndex)
            {
                continue;
            }

            if (!VisitCluster(ChildRef.ClusterIndex, 1u))
            {
                return false;
            }
        }
    }

    LogInfo("ClusterDagRuntime validation succeeded: rootGroups=" + std::to_string(Data.RootGroups.size())
        + ", groups=" + std::to_string(Data.Groups.size())
        + ", clusters=" + std::to_string(Data.Clusters.size())
        + ", childRefs=" + std::to_string(Data.ChildRefs.size())
        + ", drawDatas=" + std::to_string(Data.DrawDatas.size())
        + ", visitedClusters=" + std::to_string(VisitedClusterCount)
        + ", maxDepth=" + std::to_string(MaxTraversalDepth));
    return true;
}

bool FClusterDagRuntime::PrepareRuntimeData(FDeferredRenderer& Owner, FPreparedData& OutData)
{
    OutData = {};
    IndirectDrawRanges.clear();
    RuntimeGroupCount = 0;
    RuntimeChildRefCount = 0;
    StreamingPageCount = 1;
    StreamingPageSources.clear();
    VisibleRootCount = 0; 
    ClusterCount = 0;

    std::vector<FSceneModelResource>& SceneModels = Owner.GetSceneModelsMutable();
    const uint64_t SceneConstantBufferStride = Owner.GetSceneConstantBufferStride();

    if (SceneModels.empty() || !Owner.GetSceneConstantBuffer())
    {
        return false;
    }

    auto IsRuntimeDagModel = [](const FSceneModelResource& Model)
    {
        return Model.bUseClusterDagRuntime
            && Model.BoneMatrixBuffer.SrvBindlessIndex == UINT32_MAX
            && Model.AlphaMode == static_cast<uint32_t>(EAlphaMode::Opaque)
            && Model.ClusterDagRuntimeHierarchy.IsValid()
            && !Model.ClusterDagRuntimeHierarchy.Clusters.empty()
            && !Model.ClusterDagRuntimeHierarchy.DrawDatas.empty();
    };

    std::vector<uint32_t> SortedIndices(SceneModels.size());
    for (uint32_t Index = 0; Index < SortedIndices.size(); ++Index)
    {
        SortedIndices[Index] = Index;
    }

    std::sort(SortedIndices.begin(), SortedIndices.end(), [&](uint32_t A, uint32_t B)
    {
        const bool bDagA = IsRuntimeDagModel(SceneModels[A]);
        const bool bDagB = IsRuntimeDagModel(SceneModels[B]);
        if (bDagA != bDagB)
        {
            return bDagA > bDagB;
        }

        const uint32_t KeyA = SceneModels[A].PipelineKey;
        const uint32_t KeyB = SceneModels[B].PipelineKey;
        if (KeyA != KeyB)
        {
            return KeyA < KeyB;
        }

        const std::array<uint32_t, 4> IndicesA =
        {
            SceneModels[A].BaseColor.SrvBindlessIndex,
            SceneModels[A].MetallicRoughness.SrvBindlessIndex,
            SceneModels[A].Normal.SrvBindlessIndex,
            SceneModels[A].Emissive.SrvBindlessIndex
        };
        const std::array<uint32_t, 4> IndicesB =
        {
            SceneModels[B].BaseColor.SrvBindlessIndex,
            SceneModels[B].MetallicRoughness.SrvBindlessIndex,
            SceneModels[B].Normal.SrvBindlessIndex,
            SceneModels[B].Emissive.SrvBindlessIndex
        };
        return IndicesA < IndicesB;
    });

    size_t TotalGroups = 0;
    size_t TotalClusters = 0;
    size_t TotalChildRefs = 0;
    size_t TotalRootGroups = 0;
    size_t TotalDrawDatas = 0;
    for (uint32_t SortedIndex : SortedIndices)
    {
        FSceneModelResource& Model = SceneModels[SortedIndex];
        Model.ClusterDagRuntimeClusterOffset = 0;
        Model.ClusterDagRuntimeClusterCount = 0;
        if (!IsRuntimeDagModel(Model))
        {
            continue;
        }

        const FRuntimeClusterHierarchy& RuntimeHierarchy = Model.ClusterDagRuntimeHierarchy;
        TotalGroups += RuntimeHierarchy.Groups.size();
        TotalClusters += RuntimeHierarchy.Clusters.size();
        TotalChildRefs += RuntimeHierarchy.ChildRefs.size();
        TotalRootGroups += 1;
        TotalDrawDatas += RuntimeHierarchy.DrawDatas.size();
    }

    if (TotalClusters == 0 || TotalRootGroups == 0 || TotalDrawDatas == 0)
    {
        return false;
    }

    OutData.Groups.reserve(TotalGroups);
    OutData.Clusters.reserve(TotalClusters);
    OutData.ChildRefs.reserve(TotalChildRefs);
    OutData.RootGroups.reserve(TotalRootGroups);
    OutData.DrawDatas.reserve(TotalDrawDatas);
    OutData.CommandTemplates.reserve(TotalDrawDatas);

    const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = Owner.GetClusterDagSceneConstantBufferAddress();

    for (uint32_t SortedIndex : SortedIndices)
    {
        FSceneModelResource& Model = SceneModels[SortedIndex];
        if (!IsRuntimeDagModel(Model))
        {
            continue;
        }

        const uint32_t PipelineKey = Model.PipelineKey;
        const std::array<uint32_t, 10> MaterialIndices =
        {
            Model.BaseColor.SrvBindlessIndex,
            Model.MetallicRoughness.SrvBindlessIndex,
            Model.Normal.SrvBindlessIndex,
            Model.Emissive.SrvBindlessIndex,
            Model.SheenColor.SrvBindlessIndex,
            Model.SheenRoughness.SrvBindlessIndex,
            Model.Clearcoat.SrvBindlessIndex,
            Model.ClearcoatRoughness.SrvBindlessIndex,
            Model.ClearcoatNormal.SrvBindlessIndex,
            Model.Anisotropy.SrvBindlessIndex
        };

        if (IndirectDrawRanges.empty()
            || IndirectDrawRanges.back().PipelineKey != PipelineKey
            || IndirectDrawRanges.back().MaterialBindlessIndices != MaterialIndices)
        {
            IndirectDrawRanges.push_back(FRenderer::FIndirectDrawRange::Make(
                static_cast<uint32_t>(OutData.CommandTemplates.size()),
                PipelineKey, MaterialIndices, Model.Name));
        }

        const uint32_t RangeIndex = static_cast<uint32_t>(IndirectDrawRanges.size() - 1);
        const FRuntimeClusterHierarchy& RuntimeHierarchy = Model.ClusterDagRuntimeHierarchy;
        const uint32_t BaseGroupIndex = static_cast<uint32_t>(OutData.Groups.size());
        const uint32_t BaseClusterIndex = static_cast<uint32_t>(OutData.Clusters.size());
        const uint32_t BaseChildRefIndex = static_cast<uint32_t>(OutData.ChildRefs.size());
        const uint32_t BaseDrawDataIndex = static_cast<uint32_t>(OutData.DrawDatas.size());
        const DirectX::XMMATRIX World = DirectX::XMLoadFloat4x4(&Model.WorldMatrix);
        const float ModelScale = ComputeMaxScale(Model.WorldMatrix);

        Model.ClusterDagRuntimeClusterOffset = BaseClusterIndex;
        Model.ClusterDagRuntimeClusterCount = static_cast<uint32_t>(RuntimeHierarchy.Clusters.size());

        for (uint32_t LocalGroupIndex = 0; LocalGroupIndex < static_cast<uint32_t>(RuntimeHierarchy.Groups.size()); ++LocalGroupIndex)
        {
            const FRuntimeClusterGroup& RuntimeGroup = RuntimeHierarchy.Groups[LocalGroupIndex];
            FSceneGroupData GroupData;
            GroupData.Bounds = TransformBoundingSphere(
                DirectX::XMFLOAT4(RuntimeGroup.BoundsCenter.x, RuntimeGroup.BoundsCenter.y, RuntimeGroup.BoundsCenter.z, RuntimeGroup.BoundsRadius),
                World,
                ModelScale);
            GroupData.LodBounds = TransformBoundingSphere(
                DirectX::XMFLOAT4(RuntimeGroup.LodBoundsCenter.x, RuntimeGroup.LodBoundsCenter.y, RuntimeGroup.LodBoundsCenter.z, RuntimeGroup.LodBoundsRadius),
                World,
                ModelScale);
            GroupData.ParentLODError = RuntimeGroup.ParentLODError * ModelScale;
            GroupData.ChildRefStart = BaseChildRefIndex + RuntimeGroup.ChildRefStart; // Local to global index
            GroupData.ChildRefCount = RuntimeGroup.ChildRefCount;
            const uint32_t PageIndex = LocalGroupIndex == RuntimeHierarchy.RootGroupIndex
                ? GClusterDagRootStreamingPageIndex
                : BaseGroupIndex + LocalGroupIndex + 1u;
            if (PageIndex != GClusterDagRootStreamingPageIndex)
            {
                if (OutData.StreamingPageSources.size() <= PageIndex)
                {
                    OutData.StreamingPageSources.resize(PageIndex + 1u);
                }

                FClusterDagStreamingPageSource& PageSource = OutData.StreamingPageSources[PageIndex];
                PageSource.bValid = !Model.ClusterDagSourceFilePath.empty()
                    && !Model.ClusterDagCacheFilePath.empty()
                    && Model.ClusterDagMeshIndex != GClusterDAGInvalidIndex
                    && Model.ClusterDagPrimitiveIndex != GClusterDAGInvalidIndex
                    && Model.ClusterDagPackedVertexData.IsValid();
                PageSource.PageIndex = PageIndex;
                PageSource.MeshIndex = Model.ClusterDagMeshIndex;
                PageSource.DagIndex = Model.ClusterDagPrimitiveIndex;
                PageSource.LocalPageIndex = LocalGroupIndex + 1u;
                PageSource.GlobalGroupIndex = BaseGroupIndex + LocalGroupIndex;
                PageSource.SceneGroupBounds[0] = GroupData.Bounds.x;
                PageSource.SceneGroupBounds[1] = GroupData.Bounds.y;
                PageSource.SceneGroupBounds[2] = GroupData.Bounds.z;
                PageSource.SceneGroupBounds[3] = GroupData.Bounds.w;
                PageSource.SceneGroupLodBounds[0] = GroupData.LodBounds.x;
                PageSource.SceneGroupLodBounds[1] = GroupData.LodBounds.y;
                PageSource.SceneGroupLodBounds[2] = GroupData.LodBounds.z;
                PageSource.SceneGroupLodBounds[3] = GroupData.LodBounds.w;
                PageSource.SceneGroupParentLODError = GroupData.ParentLODError;
                PageSource.SceneGroupChildRefCount = RuntimeGroup.ChildRefCount;
                PageSource.SceneGroupFlags = (RuntimeGroup.Flags & GClusterDagGroupFlagsLowMask)
                    | ((PageIndex & GClusterDagGroupFlagsLowMask) << GClusterDagGroupPageIndexShift);
                PageSource.SceneGroupMipLevel = RuntimeGroup.MipLevel;
                PageSource.SceneGroupChildRefs.clear();
                PageSource.SceneGroupChildRefs.reserve(RuntimeGroup.ChildRefCount);
                for (uint32_t ChildRefOffset = 0u; ChildRefOffset < RuntimeGroup.ChildRefCount; ++ChildRefOffset)
                {
                    FRuntimeClusterChildRef ChildRef = RuntimeHierarchy.ChildRefs[RuntimeGroup.ChildRefStart + ChildRefOffset];
                    if (ChildRef.ClusterIndex != GClusterDAGInvalidIndex)
                    {
                        ChildRef.ClusterIndex += BaseClusterIndex;
                    }
                    PageSource.SceneGroupChildRefs.push_back(ChildRef);
                }

                std::vector<uint32_t> PageClusterIndices;
                PageClusterIndices.reserve(RuntimeGroup.ChildRefCount);
                for (uint32_t ChildRefOffset = 0u; ChildRefOffset < RuntimeGroup.ChildRefCount; ++ChildRefOffset)
                {
                    const FRuntimeClusterChildRef& ChildRef = RuntimeHierarchy.ChildRefs[RuntimeGroup.ChildRefStart + ChildRefOffset];
                    if (ChildRef.ClusterIndex != GClusterDAGInvalidIndex && ChildRef.ClusterIndex < RuntimeHierarchy.Clusters.size())
                    {
                        PageClusterIndices.push_back(ChildRef.ClusterIndex);
                    }
                }
                std::sort(PageClusterIndices.begin(), PageClusterIndices.end());
                PageClusterIndices.erase(std::unique(PageClusterIndices.begin(), PageClusterIndices.end()), PageClusterIndices.end());

                PageSource.ScenePageClusters.clear();
                PageSource.ScenePageClusters.reserve(PageClusterIndices.size());
                PageSource.ScenePageDrawDatas.clear();
                PageSource.ScenePagePackedIndices.clear();
                PageSource.ScenePagePackedPositions.clear();
                PageSource.ScenePagePackedNormals.clear();
                PageSource.ScenePagePackedUVs.clear();
                PageSource.ScenePagePackedTangents.clear();
                PageSource.ScenePagePackedColors.clear();
                std::unordered_map<uint32_t, uint32_t> PageVertexRemap;
                for (uint32_t LocalClusterIndex : PageClusterIndices)
                {
                    const FRuntimeCluster& RuntimeCluster = RuntimeHierarchy.Clusters[LocalClusterIndex];
                    FClusterDagStreamingPageSource::FSceneClusterRecord ClusterRecord;
                    ClusterRecord.GlobalClusterIndex = BaseClusterIndex + LocalClusterIndex;

                    const DirectX::XMFLOAT4 SceneBounds = TransformBoundingSphere(
                        DirectX::XMFLOAT4(RuntimeCluster.Bounds.Center.x, RuntimeCluster.Bounds.Center.y, RuntimeCluster.Bounds.Center.z, RuntimeCluster.Bounds.Radius),
                        World,
                        ModelScale);
                    const DirectX::XMFLOAT4 SceneLodBounds = TransformBoundingSphere(
                        DirectX::XMFLOAT4(RuntimeCluster.LodBoundsCenter.x, RuntimeCluster.LodBoundsCenter.y, RuntimeCluster.LodBoundsCenter.z, RuntimeCluster.LodBoundsRadius),
                        World,
                        ModelScale);
                    ClusterRecord.Bounds[0] = SceneBounds.x;
                    ClusterRecord.Bounds[1] = SceneBounds.y;
                    ClusterRecord.Bounds[2] = SceneBounds.z;
                    ClusterRecord.Bounds[3] = SceneBounds.w;
                    ClusterRecord.LodBounds[0] = SceneLodBounds.x;
                    ClusterRecord.LodBounds[1] = SceneLodBounds.y;
                    ClusterRecord.LodBounds[2] = SceneLodBounds.z;
                    ClusterRecord.LodBounds[3] = SceneLodBounds.w;
                    ClusterRecord.LODError = RuntimeCluster.LODError * ModelScale;
                    ClusterRecord.MaxEdgeLength = RuntimeCluster.MaxEdgeLength * ModelScale;
                    ClusterRecord.GroupIndex = RuntimeCluster.GroupIndex != GClusterDAGInvalidIndex ? BaseGroupIndex + RuntimeCluster.GroupIndex : GClusterDAGInvalidIndex;
                    ClusterRecord.GeneratingGroupIndex = RuntimeCluster.GeneratingGroupIndex != GClusterDAGInvalidIndex ? BaseGroupIndex + RuntimeCluster.GeneratingGroupIndex : GClusterDAGInvalidIndex;
                    ClusterRecord.DrawDataStart = BaseDrawDataIndex + RuntimeCluster.DrawDataStart;
                    ClusterRecord.DrawDataCount = RuntimeCluster.DrawDataCount;
                    ClusterRecord.TriangleCount = RuntimeCluster.TriangleCount;
                    ClusterRecord.MipLevel = RuntimeCluster.MipLevel;
                    PageSource.ScenePageClusters.push_back(ClusterRecord);

                    if (RuntimeCluster.DrawDataStart > RuntimeHierarchy.DrawDatas.size()
                        || RuntimeCluster.DrawDataCount > RuntimeHierarchy.DrawDatas.size() - RuntimeCluster.DrawDataStart)
                    {
                        PageSource.bValid = false;
                        continue;
                    }

                    for (uint32_t DrawDataOffset = 0u; DrawDataOffset < RuntimeCluster.DrawDataCount; ++DrawDataOffset)
                    {
                        const uint32_t LocalDrawDataIndex = RuntimeCluster.DrawDataStart + DrawDataOffset;
                        const FRuntimeClusterDrawData& RuntimeDrawData = RuntimeHierarchy.DrawDatas[LocalDrawDataIndex];
                        const uint32_t PageLocalIndexStart = static_cast<uint32_t>(PageSource.ScenePagePackedIndices.size());
                        FClusterDagStreamingPageSource::FSceneDrawDataRecord DrawRecord;
                        DrawRecord.GlobalDrawDataIndex = BaseDrawDataIndex + LocalDrawDataIndex;
                        DrawRecord.StartIndex = PageLocalIndexStart;
                        DrawRecord.IndexCount = RuntimeDrawData.IndexCount;
                        DrawRecord.RangeIndex = RangeIndex;
                        DrawRecord.RangeCommandStart = IndirectDrawRanges[RangeIndex].Start;
                        DrawRecord.ModelIndex = SortedIndex;
                        PageSource.ScenePageDrawDatas.push_back(DrawRecord);

                        if (RuntimeDrawData.IndexStart <= RuntimeHierarchy.PackedIndices.size()
                            && RuntimeDrawData.IndexCount <= RuntimeHierarchy.PackedIndices.size() - RuntimeDrawData.IndexStart)
                        {
                            for (uint32_t IndexOffset = 0u; IndexOffset < RuntimeDrawData.IndexCount; ++IndexOffset)
                            {
                                const uint32_t SourceVertexIndex = RuntimeHierarchy.PackedIndices[RuntimeDrawData.IndexStart + IndexOffset];
                                uint32_t PageVertexIndex = 0u;
                                const auto RemapIt = PageVertexRemap.find(SourceVertexIndex);
                                if (RemapIt != PageVertexRemap.end())
                                {
                                    PageVertexIndex = RemapIt->second;
                                }
                                else
                                {
                                    PageVertexIndex = static_cast<uint32_t>(PageSource.ScenePagePackedPositions.size());
                                    PageVertexRemap.emplace(SourceVertexIndex, PageVertexIndex);
                                    if (SourceVertexIndex < Model.ClusterDagPackedVertexData.Positions.size())
                                    {
                                        PageSource.ScenePagePackedPositions.push_back(Model.ClusterDagPackedVertexData.Positions[SourceVertexIndex]);
                                    }
                                    if (SourceVertexIndex < Model.ClusterDagPackedVertexData.Normals.size())
                                    {
                                        PageSource.ScenePagePackedNormals.push_back(Model.ClusterDagPackedVertexData.Normals[SourceVertexIndex]);
                                    }
                                    if (SourceVertexIndex < Model.ClusterDagPackedVertexData.UVs.size())
                                    {
                                        PageSource.ScenePagePackedUVs.push_back(Model.ClusterDagPackedVertexData.UVs[SourceVertexIndex]);
                                    }
                                    if (SourceVertexIndex < Model.ClusterDagPackedVertexData.Tangents.size())
                                    {
                                        PageSource.ScenePagePackedTangents.push_back(Model.ClusterDagPackedVertexData.Tangents[SourceVertexIndex]);
                                    }
                                    if (SourceVertexIndex < Model.ClusterDagPackedVertexData.Colors.size())
                                    {
                                        PageSource.ScenePagePackedColors.push_back(Model.ClusterDagPackedVertexData.Colors[SourceVertexIndex]);
                                    }
                                }
                                PageSource.ScenePagePackedIndices.push_back(PageVertexIndex);
                            }
                        }
                    }
                }
                PageSource.SourceFilePath = Model.ClusterDagSourceFilePath;
                PageSource.CacheFilePath = Model.ClusterDagCacheFilePath;
            }
            GroupData.Flags = (RuntimeGroup.Flags & GClusterDagGroupFlagsLowMask)
                | ((PageIndex & GClusterDagGroupFlagsLowMask) << GClusterDagGroupPageIndexShift);
            GroupData.MipLevel = RuntimeGroup.MipLevel;
            OutData.Groups.push_back(GroupData);
            OutData.StreamingPageCount = (std::max)(OutData.StreamingPageCount, PageIndex + 1u);
        }

        for (const FRuntimeClusterChildRef& RuntimeChildRef : RuntimeHierarchy.ChildRefs)
        {
            FRuntimeClusterChildRef ChildRef = RuntimeChildRef;
            if (ChildRef.ClusterIndex != GClusterDAGInvalidIndex)
            {
                ChildRef.ClusterIndex += BaseClusterIndex; // Local to global index
            }
            OutData.ChildRefs.push_back(ChildRef);
        }

        OutData.RootGroups.push_back(BaseGroupIndex + RuntimeHierarchy.RootGroupIndex); // Local to global index

        for (const FRuntimeCluster& RuntimeCluster : RuntimeHierarchy.Clusters)
        {
            FSceneClusterData ClusterData;
            ClusterData.Bounds = TransformBoundingSphere(
                DirectX::XMFLOAT4(RuntimeCluster.Bounds.Center.x, RuntimeCluster.Bounds.Center.y, RuntimeCluster.Bounds.Center.z, RuntimeCluster.Bounds.Radius),
                World,
                ModelScale);
            ClusterData.LodBounds = TransformBoundingSphere(
                DirectX::XMFLOAT4(RuntimeCluster.LodBoundsCenter.x, RuntimeCluster.LodBoundsCenter.y, RuntimeCluster.LodBoundsCenter.z, RuntimeCluster.LodBoundsRadius),
                World,
                ModelScale);
            ClusterData.LODError = RuntimeCluster.LODError * ModelScale;
            ClusterData.MaxEdgeLength = RuntimeCluster.MaxEdgeLength * ModelScale;
            ClusterData.GroupIndex = RuntimeCluster.GroupIndex != GClusterDAGInvalidIndex ? BaseGroupIndex + RuntimeCluster.GroupIndex : GClusterDAGInvalidIndex; // Local to global index
            ClusterData.GeneratingGroupIndex = RuntimeCluster.GeneratingGroupIndex != GClusterDAGInvalidIndex ? BaseGroupIndex + RuntimeCluster.GeneratingGroupIndex : GClusterDAGInvalidIndex; // Local to global index
            ClusterData.DrawDataStart = BaseDrawDataIndex + RuntimeCluster.DrawDataStart; // Local to global index
            ClusterData.DrawDataCount = RuntimeCluster.DrawDataCount;
            ClusterData.TriangleCount = RuntimeCluster.TriangleCount;
            ClusterData.MipLevel = RuntimeCluster.MipLevel;
            OutData.Clusters.push_back(ClusterData);
        }

        // FIndirectDrawRange.Start is the first command slot index in the flat command buffer for this range (= number of commands accumulated so far)
        // Range 0: Pipeline A, Material X, CommandTemplates[0 ~ 9]
        // Range 1: Pipeline B, Material Y, CommandTemplates[10 ~ 17]
        // Range 2: Pipeline A, Material Z, CommandTemplates[18 ~ 25]
        // RangeIndex 1 -> RangeCommandStart = 10
        // Currently Cluster:DrawData:CommandTemplate is 1:1:1; DrawDataCount > 1 per cluster is reserved for multi-material clusters
        for (const FRuntimeClusterDrawData& RuntimeDrawData : RuntimeHierarchy.DrawDatas)
        {
            OutData.DrawDatas.push_back(FClusterDrawData::Make(
                RuntimeDrawData.IndexStart, RuntimeDrawData.IndexCount,
                RangeIndex, IndirectDrawRanges[RangeIndex].Start, SortedIndex));
            const uint32_t DrawDataIndex = static_cast<uint32_t>(OutData.DrawDatas.size() - 1);

            OutData.CommandTemplates.push_back(FIndirectDrawCommand::Make(
                ConstantBufferBase + SceneConstantBufferStride * SortedIndex,
                RuntimeDrawData.IndexStart, RuntimeDrawData.IndexCount, 0, DrawDataIndex));
            // This is a template, not a directly issued draw call; BuildClusterDagRunsAppend copies it into the output buffer based on GPU LOD selection results
            IndirectDrawRanges.back().Count += 1;
        }
    }

    OutData.RangeOffsets.reserve(IndirectDrawRanges.size());
    for (const FRenderer::FIndirectDrawRange& Range : IndirectDrawRanges)
    {
        OutData.RangeOffsets.push_back(Range.Start);
    }

    if (!ValidatePreparedRuntimeData(OutData))
    {
        IndirectDrawRanges.clear();
        VisibleRootCount = 0; 
        ClusterCount = 0;
        return false;
    }

    const uint32_t PreparedVisibleRootCount = static_cast<uint32_t>(OutData.RootGroups.size());
    const uint32_t PreparedClusterCount = static_cast<uint32_t>(OutData.Clusters.size());
    VisibleRootCount = PreparedVisibleRootCount;
    ClusterCount = PreparedClusterCount;
    StreamingPageCount = (std::max)(OutData.StreamingPageCount, 1u);
    StreamingPageSources = OutData.StreamingPageSources;
    return PreparedVisibleRootCount > 0 && PreparedClusterCount > 0;
}

bool FClusterDagRuntime::CreateRuntimeResources(FDeferredRenderer& Owner, FDX12Device* Device, const FPreparedData& Data)
{
    assert(!Data.Clusters.empty() && !Data.RootGroups.empty() && !Data.CommandTemplates.empty());

    RuntimeGroupCount = static_cast<uint32_t>(Data.Groups.size());
    RuntimeChildRefCount = static_cast<uint32_t>(Data.ChildRefs.size());
    StreamingPageCount = (std::max)(Data.StreamingPageCount, 1u);
    StreamingPageSources = Data.StreamingPageSources;
    RuntimeMaxTraversalLevels = 1;
    for (const FSceneGroupData& Group : Data.Groups)
    {
        RuntimeMaxTraversalLevels = (std::max)(RuntimeMaxTraversalLevels, Group.MipLevel + 2u);
    }
    for (const FSceneClusterData& Cluster : Data.Clusters)
    {
        RuntimeMaxTraversalLevels = (std::max)(RuntimeMaxTraversalLevels, Cluster.MipLevel + 2u);
    }

    const uint32_t Frames = Owner.GetFramesInFlight();
    const auto ClearAndResize = [&](auto& Container)
    {
        Container.clear();
        Container.resize(Frames);
    };

    ClearAndResize(QueueStateBuffers);
    ClearAndResize(GroupQueueBuffers);
    ClearAndResize(CandidateClusterQueueBuffers);
    ClearAndResize(VisitedGroupEpochBuffers);
    for (uint32_t BufferIndex = 0; BufferIndex < 2u; ++BufferIndex)
    {
        ClearAndResize(LevelSplitNodeCandidateBuffers[BufferIndex]);
        ClearAndResize(LevelSplitNodeArgsBuffers[BufferIndex]);
    }
    ClearAndResize(LevelSplitClusterArgsBuffers);
    ClearAndResize(SwRasterDispatchArgsBuffers);
    ClearAndResize(IndirectCommandBuffers);
    ClearAndResize(IndirectCommandTemplateBuffers);
    ClearAndResize(RunCountBuffers);
    ClearAndResize(VisibleEntryBuffers);
    ClearAndResize(VisibleEntryCounterBuffers);
    ClearAndResize(HwVisibleEntryIndexBuffers);
    ClearAndResize(SwVisibleEntryIndexBuffers);
    ClearAndResize(DrawDataVisibleEntryIndexBuffers);

    CreateBindlessBuffer(
        Device,
        L"ClusterDagGroupBuffer",
        CreateStructuredBufferDesc(Data.Groups),
        D3D12_RESOURCE_STATE_COMMON,
        GroupBuffer,
        true,
        false);
    CreateBindlessBuffer(
        Device,
        L"ClusterDagClusterBuffer",
        CreateStructuredBufferDesc(Data.Clusters),
        D3D12_RESOURCE_STATE_COMMON,
        ClusterBuffer,
        true,
        false);
    CreateBindlessBuffer(
        Device,
        L"ClusterDagChildRefBuffer",
        CreateStructuredBufferDesc(Data.ChildRefs),
        D3D12_RESOURCE_STATE_COMMON,
        ChildRefBuffer,
        true,
        false);
    CreateBindlessBuffer(
        Device,
        L"ClusterDagRootGroupBuffer",
        CreateStructuredBufferDesc(Data.RootGroups),
        D3D12_RESOURCE_STATE_COMMON,
        RootGroupBuffer,
        true,
        false);
    CreateBindlessBuffer(
        Device,
        L"ClusterDagDrawDataBuffer",
        CreateStructuredBufferDesc(Data.DrawDatas),
        D3D12_RESOURCE_STATE_COMMON,
        DrawDataBuffer,
        true,
        false);

    const uint64_t QueueStateBufferSize = ActiveTraversalMode == EClusterDAGTraversalMode::LevelSplitQueue
        ? GClusterDagLevelSplitQueueStateBufferSize
        : GClusterDagQueueStateBufferSize;
    const uint64_t LevelSplitNodeArgsBufferSize = GClusterDagLevelSplitNodeArgsBufferSize;
    const uint64_t LevelSplitClusterArgsBufferSize = GClusterDagLevelSplitClusterArgsBufferSize;
    const uint64_t SwRasterDispatchArgsBufferSize = GClusterDagSwRasterArgsBufferSize;
    const uint64_t IndirectCommandBufferSize = sizeof(FIndirectDrawCommand) * Data.CommandTemplates.size();
    const uint64_t RunCountBufferSize = sizeof(uint32_t) * IndirectDrawRanges.size();
    const uint64_t VisibleEntryCounterBufferSize = sizeof(uint32_t) * 3u;
    RuntimeCommandCount = static_cast<uint32_t>(Data.CommandTemplates.size());

    const auto CreatePerFrameBuffer = [&](uint32_t FrameIndex,
        const std::wstring& Name,
        const FRGBufferDesc& Desc,
        FBindlessBuffer& OutBuffer,
        bool bCreateSrv,
        bool bCreateUav)
    {
        CreateBindlessBuffer(
            Device,
            Name + L"_Frame" + std::to_wstring(FrameIndex),
            Desc,
            D3D12_RESOURCE_STATE_COMMON,
            OutBuffer,
            bCreateSrv,
            bCreateUav);
    };

    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagQueueState",
            CreateRawBufferDesc(QueueStateBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            QueueStateBuffers[FrameIndex],
            true,
            true);
        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagGroupQueue",
            CreateRWStructuredBufferDesc<uint32_t>(Data.Groups.size()),
            GroupQueueBuffers[FrameIndex],
            true,
            true);
        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagCandidateClusterQueue",
            CreateRWStructuredBufferDesc<FCandidateClusterEntry>(Data.Clusters.size()),
            CandidateClusterQueueBuffers[FrameIndex],
            true,
            true);
        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagVisitedGroupEpochs",
            CreateRWStructuredBufferDesc<uint32_t>(Data.Groups.size()),
            VisitedGroupEpochBuffers[FrameIndex],
            false,
            true);

        if (ActiveTraversalMode == EClusterDAGTraversalMode::LevelSplitQueue)
        {
            for (uint32_t BufferIndex = 0; BufferIndex < 2u; ++BufferIndex)
            {
                const std::wstring CandidateName = L"ClusterDagLevelSplitNodeCandidates" + std::to_wstring(BufferIndex);
                CreatePerFrameBuffer(
                    FrameIndex,
                    CandidateName,
                    CreateRWStructuredBufferDesc<uint32_t>(Data.Groups.size()),
                    LevelSplitNodeCandidateBuffers[BufferIndex][FrameIndex],
                    true,
                    true);

                const std::wstring ArgsName = L"ClusterDagLevelSplitNodeArgs" + std::to_wstring(BufferIndex);
                CreatePerFrameBuffer(
                    FrameIndex,
                    ArgsName,
                    CreateRawBufferDesc(LevelSplitNodeArgsBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                    LevelSplitNodeArgsBuffers[BufferIndex][FrameIndex],
                    true,
                    true);
            }

            CreatePerFrameBuffer(
                FrameIndex,
                L"ClusterDagLevelSplitClusterArgs",
                CreateRawBufferDesc(LevelSplitClusterArgsBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                LevelSplitClusterArgsBuffers[FrameIndex],
                true,
                true);
        }

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagRunCount",
            CreateRawBufferDesc(RunCountBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            RunCountBuffers[FrameIndex],
            false,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagVisibleEntries",
            CreateRWStructuredBufferDesc<FVisibleEntry>(Data.CommandTemplates.size()),
            VisibleEntryBuffers[FrameIndex],
            true,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagVisibleEntryCounters",
            CreateRawBufferDesc(VisibleEntryCounterBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            VisibleEntryCounterBuffers[FrameIndex],
            true,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagHWVisibleEntryIndices",
            CreateRWStructuredBufferDesc<uint32_t>(Data.CommandTemplates.size()),
            HwVisibleEntryIndexBuffers[FrameIndex],
            true,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagSWVisibleEntryIndices",
            CreateRWStructuredBufferDesc<uint32_t>(Data.CommandTemplates.size()),
            SwVisibleEntryIndexBuffers[FrameIndex],
            true,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagDrawDataVisibleEntryIndices",
            CreateRWStructuredBufferDesc<uint32_t>(Data.CommandTemplates.size()),
            DrawDataVisibleEntryIndexBuffers[FrameIndex],
            true,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagSwRasterDispatchArgs",
            CreateRawBufferDesc(SwRasterDispatchArgsBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            SwRasterDispatchArgsBuffers[FrameIndex],
            false,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagIndirectCommandBuffer",
            CreateRawBufferDesc(IndirectCommandBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            IndirectCommandBuffers[FrameIndex],
            false,
            true);

        CreatePerFrameBuffer(
            FrameIndex,
            L"ClusterDagIndirectTemplateBuffer",
            CreateRawBufferDesc(IndirectCommandBufferSize),
            IndirectCommandTemplateBuffers[FrameIndex],
            true,
            false);
    }

    return UploadRuntimeResources(Owner, Device, Data);
}

bool FClusterDagRuntime::UploadRuntimeResources(FDeferredRenderer& Owner, FDX12Device* Device, const FPreparedData& Data)
{
    const uint64_t SceneConstantBufferStride = Owner.GetSceneConstantBufferStride();
    const uint32_t Frames = Owner.GetFramesInFlight();
    const uint64_t IndirectCommandBufferSize = sizeof(FIndirectDrawCommand) * Data.CommandTemplates.size();

    FUploadBuffer GroupUpload;
    FUploadBuffer ClusterUpload;
    FUploadBuffer ChildRefUpload;
    FUploadBuffer RootGroupUpload;
    FUploadBuffer DrawDataUpload;
    std::vector<FUploadBuffer> IndirectTemplateUploads(Frames);

    CreateUploadBuffer(Device, L"ClusterDagGroupBufferUpload", sizeof(FSceneGroupData) * Data.Groups.size(), GroupUpload, Data.Groups.data());
    CreateUploadBuffer(Device, L"ClusterDagClusterBufferUpload", sizeof(FSceneClusterData) * Data.Clusters.size(), ClusterUpload, Data.Clusters.data());
    CreateUploadBuffer(Device, L"ClusterDagChildRefBufferUpload", sizeof(FRuntimeClusterChildRef) * Data.ChildRefs.size(), ChildRefUpload, Data.ChildRefs.data());
    CreateUploadBuffer(Device, L"ClusterDagRootGroupBufferUpload", sizeof(uint32_t) * Data.RootGroups.size(), RootGroupUpload, Data.RootGroups.data());
    CreateUploadBuffer(Device, L"ClusterDagDrawDataBufferUpload", sizeof(FClusterDrawData) * Data.DrawDatas.size(), DrawDataUpload, Data.DrawDatas.data());

    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        std::vector<FIndirectDrawCommand> FrameCommands = Data.CommandTemplates;
        const D3D12_GPU_VIRTUAL_ADDRESS ConstantBufferBase = Owner.GetClusterDagSceneConstantBufferAddress(FrameIndex);
        for (FIndirectDrawCommand& Command : FrameCommands)
        {
            const uint32_t DrawDataIndex = Command.DrawDataIndex;
            const uint32_t InstanceIndex =
                DrawDataIndex < Data.DrawDatas.size()
                ? Data.DrawDatas[DrawDataIndex].ModelIndex
                : 0u;
            Command.ConstantBufferAddress = ConstantBufferBase + SceneConstantBufferStride * InstanceIndex;
        }

        CreateUploadBuffer(
            Device,
            L"ClusterDagIndirectTemplateBufferUpload_Frame" + std::to_wstring(FrameIndex),
            IndirectCommandBufferSize,
            IndirectTemplateUploads[FrameIndex],
            FrameCommands.data());
    }

    ComPtr<ID3D12CommandAllocator> UploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> UploadList;
    HR_CHECK(Device->GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(UploadAllocator.GetAddressOf())));
    HR_CHECK(Device->GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, UploadAllocator.Get(), nullptr, IID_PPV_ARGS(UploadList.GetAddressOf())));
    UploadList->SetName(L"ClusterDagRuntimeUpload_CL");

    std::vector<D3D12_RESOURCE_BARRIER> PreCopyBarriers;
    auto AddCopyBarrier = [&](ID3D12Resource* Resource)
    {
        PreCopyBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
    };

    AddCopyBarrier(GroupBuffer.Get());
    AddCopyBarrier(ClusterBuffer.Get());
    AddCopyBarrier(ChildRefBuffer.Get());
    AddCopyBarrier(RootGroupBuffer.Get());
    AddCopyBarrier(DrawDataBuffer.Get());
    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        AddCopyBarrier(IndirectCommandTemplateBuffers[FrameIndex].Get());
    }

    UploadList->ResourceBarrier(static_cast<UINT>(PreCopyBarriers.size()), PreCopyBarriers.data());
    UploadList->CopyBufferRegion(GroupBuffer.Get(), 0, GroupUpload.Get(), 0, sizeof(FSceneGroupData) * Data.Groups.size());
    UploadList->CopyBufferRegion(ClusterBuffer.Get(), 0, ClusterUpload.Get(), 0, sizeof(FSceneClusterData) * Data.Clusters.size());
    UploadList->CopyBufferRegion(ChildRefBuffer.Get(), 0, ChildRefUpload.Get(), 0, sizeof(FRuntimeClusterChildRef) * Data.ChildRefs.size());
    UploadList->CopyBufferRegion(RootGroupBuffer.Get(), 0, RootGroupUpload.Get(), 0, sizeof(uint32_t) * Data.RootGroups.size());
    UploadList->CopyBufferRegion(DrawDataBuffer.Get(), 0, DrawDataUpload.Get(), 0, sizeof(FClusterDrawData) * Data.DrawDatas.size());
    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        UploadList->CopyBufferRegion(
            IndirectCommandTemplateBuffers[FrameIndex].Get(),
            0,
            IndirectTemplateUploads[FrameIndex].Get(),
            0,
            IndirectCommandBufferSize);
    }

    std::vector<D3D12_RESOURCE_BARRIER> PostCopyBarriers;
    auto AddSrvBarrier = [&](ID3D12Resource* Resource)
    {
        PostCopyBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(Resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    };

    AddSrvBarrier(GroupBuffer.Get());
    AddSrvBarrier(ClusterBuffer.Get());
    AddSrvBarrier(ChildRefBuffer.Get());
    AddSrvBarrier(RootGroupBuffer.Get());
    AddSrvBarrier(DrawDataBuffer.Get());
    for (uint32_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex)
    {
        AddSrvBarrier(IndirectCommandTemplateBuffers[FrameIndex].Get());
    }

    UploadList->ResourceBarrier(static_cast<UINT>(PostCopyBarriers.size()), PostCopyBarriers.data());
    HR_CHECK(UploadList->Close());
    ID3D12CommandList* Lists[] = { UploadList.Get() };
    Device->GetGraphicsQueue()->ExecuteCommandLists(1, Lists);
    Device->GetGraphicsQueue()->Flush();

    return true;
}

void FClusterDagRuntime::PopulateCullingConstants(FDeferredRenderer& Owner, const FCamera& Camera) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    DirectX::XMVECTOR Planes[6] = {};
    RendererUtils::BuildCameraFrustumPlanes(Camera, Planes);

    std::array<uint32_t, 60> Constants = {};
    for (uint32_t PlaneIndex = 0; PlaneIndex < 6; ++PlaneIndex)
    {
        DirectX::XMFLOAT4 Plane = {};
        DirectX::XMStoreFloat4(&Plane, Planes[PlaneIndex]);
        std::memcpy(Constants.data() + PlaneIndex * 4, &Plane, sizeof(DirectX::XMFLOAT4));
    }

    const DirectX::XMMATRIX ViewProjection = Camera.GetViewMatrix() * Camera.GetProjectionMatrix();
    DirectX::XMFLOAT4X4 ViewProjectionMatrix = {};
    DirectX::XMStoreFloat4x4(&ViewProjectionMatrix, ViewProjection);
    std::memcpy(Constants.data() + 24, &ViewProjectionMatrix, sizeof(DirectX::XMFLOAT4X4));
    Constants[40] = RuntimeCommandCount;
    Constants[41] = 0u;
    Constants[42] = 0u;
    Constants[43] = 0u;
    Constants[44] = 0u;
    Constants[45] = GpuDrivenCullingProvider.bClusterDagGpuDebugEnabled ? 1u : 0u;
    Constants[46] = static_cast<uint32_t>(IndirectDrawRanges.size());
    Constants[47] = static_cast<uint32_t>(FRenderer::ECullingMode::All);
    const DirectX::XMFLOAT3 CameraPosition = Camera.GetPosition();
    std::memcpy(Constants.data() + 48, &CameraPosition, sizeof(DirectX::XMFLOAT3));
    Constants[51] = 0u;
    std::memcpy(Constants.data() + 52, &GpuDrivenCullingProvider.ClusterDagTargetErrorPixels, sizeof(float));
    std::memcpy(Constants.data() + 53, &GpuDrivenCullingProvider.ViewportHeightPixels, sizeof(float));
    Constants[54] = GpuDrivenCullingProvider.ClusterDagVisibleRootCount;
    Constants[55] = GpuDrivenCullingProvider.bClusterDagForceMipEnabled ? 1u : 0u;
    Constants[56] = GpuDrivenCullingProvider.ClusterDagForceMipLevel;
    Constants[57] = GpuDrivenCullingProvider.bClusterDagForceMipSkipFrustumCull ? 1u : 0u;
    Constants[58] = bForceSoftwareRaster ? 1u : 0u;
    std::memcpy(Constants.data() + 59, &SwRasterThresholdPixels, sizeof(float));

    uint8_t* CullingConstantsMapped = GpuDrivenCullingProvider.CullingConstantBufferMapped;
    if (CullingConstantsMapped)
    {
        std::memcpy(CullingConstantsMapped, Constants.data(), sizeof(Constants));
    }
}

void FClusterDagRuntime::AddInitQueuePass(FDeferredPassContext& Context, const char* PassName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FClusterDagInitQueuePassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FClusterDagInitQueuePassData>(PassName, [this, &Context](FClusterDagInitQueuePassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = HasResources();
        Data.Camera = &Context.Camera;
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle QueueStateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_QueueState", MutableThis.QueueStateBuffers[FrameIndex]);
            const FRGBufferHandle GroupQueueHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_GroupQueue", MutableThis.GroupQueueBuffers[FrameIndex]);
            const FRGBufferHandle CandidateQueueHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_CandidateClusterQueue", MutableThis.CandidateClusterQueueBuffers[FrameIndex]);
            const FRGBufferHandle VisitedEpochHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisitedGroupEpoch", MutableThis.VisitedGroupEpochBuffers[FrameIndex]);
            const FRGBufferHandle RunCountHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_RunCounts", MutableThis.RunCountBuffers[FrameIndex]);
            const FRGBufferHandle VisibleEntryCounterHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisibleEntryCounters", MutableThis.VisibleEntryCounterBuffers[FrameIndex]);
            const FRGBufferHandle DrawDataVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_DrawDataVisibleEntryIndices", MutableThis.DrawDataVisibleEntryIndexBuffers[FrameIndex]);
            Builder.WriteBuffer(QueueStateHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(GroupQueueHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(CandidateQueueHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisitedEpochHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(RunCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisibleEntryCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(DrawDataVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.UavBarrier(QueueStateHandle);
            Builder.UavBarrier(GroupQueueHandle);
            Builder.UavBarrier(CandidateQueueHandle);
            Builder.UavBarrier(VisitedEpochHandle);
            Builder.UavBarrier(RunCountHandle);
            Builder.UavBarrier(VisibleEntryCounterHandle);
            Builder.UavBarrier(DrawDataVisibleEntryHandle);
            Builder.KeepAlive();
        }
    }, [this, &Owner, PassName](const FClusterDagInitQueuePassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchInitQueues(Owner, Cmd, *Data.Camera, PassName);
    });
}

void FClusterDagRuntime::AddPersistentCullPass(FDeferredPassContext& Context, const char* PassName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FClusterDagPersistentCullPassData
    {
        bool bEnabled = false;
    };

    Context.Graph.AddPass<FClusterDagPersistentCullPassData>(PassName, [this, &Owner, &Context](FClusterDagPersistentCullPassData& Data, FRGPassBuilder& Builder)
    {
        Data.bEnabled = HasResources();
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle QueueStateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_QueueState", MutableThis.QueueStateBuffers[FrameIndex]);
            const FRGBufferHandle GroupQueueHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_GroupQueue", MutableThis.GroupQueueBuffers[FrameIndex]);
            const FRGBufferHandle CandidateQueueHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_CandidateClusterQueue", MutableThis.CandidateClusterQueueBuffers[FrameIndex]);
            const FRGBufferHandle VisitedEpochHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisitedGroupEpoch", MutableThis.VisitedGroupEpochBuffers[FrameIndex]);
            const FRGBufferHandle IndirectHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_IndirectCommands", MutableThis.IndirectCommandBuffers[FrameIndex]);
            const FRGBufferHandle RunCountHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_RunCounts", MutableThis.RunCountBuffers[FrameIndex]);
            const FRGBufferHandle VisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisibleEntries", MutableThis.VisibleEntryBuffers[FrameIndex]);
            const FRGBufferHandle VisibleEntryCounterHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisibleEntryCounters", MutableThis.VisibleEntryCounterBuffers[FrameIndex]);
            const FRGBufferHandle HwVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_HWVisibleEntryIndices", MutableThis.HwVisibleEntryIndexBuffers[FrameIndex]);
            const FRGBufferHandle SwVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_SWVisibleEntryIndices", MutableThis.SwVisibleEntryIndexBuffers[FrameIndex]);
            const FRGBufferHandle DrawDataVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_DrawDataVisibleEntryIndices", MutableThis.DrawDataVisibleEntryIndexBuffers[FrameIndex]);
            Builder.WriteBuffer(QueueStateHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(GroupQueueHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(CandidateQueueHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisitedEpochHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(IndirectHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(RunCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisibleEntryCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(HwVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(SwVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(DrawDataVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.UavBarrier(QueueStateHandle);
            Builder.UavBarrier(GroupQueueHandle);
            Builder.UavBarrier(CandidateQueueHandle);
            Builder.UavBarrier(VisitedEpochHandle);
            Builder.UavBarrier(IndirectHandle);
            Builder.UavBarrier(RunCountHandle);
            Builder.UavBarrier(VisibleEntryHandle);
            Builder.UavBarrier(VisibleEntryCounterHandle);
            Builder.UavBarrier(HwVisibleEntryHandle);
            Builder.UavBarrier(SwVisibleEntryHandle);
            Builder.UavBarrier(DrawDataVisibleEntryHandle);
            if (FClusterDagStreamingManager* StreamingManager = Owner.GetClusterDagStreamingManager(); StreamingManager && StreamingManager->IsEnabled())
            {
                FRGBufferHandle PageTableHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_StreamingPageTablePersistent", StreamingManager->PageTableBuffer);
                FRGBufferHandle PageDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_StreamingPageDataPersistent", StreamingManager->PageDataBuffer);
                FRGBufferHandle FeedbackHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_StreamingFeedbackPersistent", StreamingManager->FeedbackBuffers[FrameIndex].Gpu);
                Builder.ReadBuffer(PageTableHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Builder.ReadBuffer(PageDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Builder.WriteBuffer(FeedbackHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Builder.UavBarrier(FeedbackHandle);
            }
            Builder.KeepAlive();
        }
    }, [this, &Owner, PassName](const FClusterDagPersistentCullPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchPersistentCull(Owner, Cmd, PassName);
    });
}

void FClusterDagRuntime::AddLevelSplitInitPass(FDeferredPassContext& Context, const char* PassName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FPassData
    {
        bool bEnabled = false;
        const FCamera* Camera = nullptr;
    };

    Context.Graph.AddPass<FPassData>(PassName, [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Level Split ClusterDAG");
        Data.bEnabled = HasResources();
        Data.Camera = &Context.Camera;
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle QueueStateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_QueueState", MutableThis.QueueStateBuffers[FrameIndex]);
            const FRGBufferHandle CandidateQueueHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_CandidateClusterQueue", MutableThis.CandidateClusterQueueBuffers[FrameIndex]);
            const FRGBufferHandle VisitedEpochHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisitedGroupEpoch", MutableThis.VisitedGroupEpochBuffers[FrameIndex]);
            const FRGBufferHandle NodeCandidate0Handle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitNodeCandidates0", MutableThis.LevelSplitNodeCandidateBuffers[0][FrameIndex]);
            const FRGBufferHandle NodeCandidate1Handle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitNodeCandidates1", MutableThis.LevelSplitNodeCandidateBuffers[1][FrameIndex]);
            const FRGBufferHandle NodeArgs0Handle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitNodeArgs0", MutableThis.LevelSplitNodeArgsBuffers[0][FrameIndex]);
            const FRGBufferHandle NodeArgs1Handle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitNodeArgs1", MutableThis.LevelSplitNodeArgsBuffers[1][FrameIndex]);
            const FRGBufferHandle RunCountHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_RunCounts", MutableThis.RunCountBuffers[FrameIndex]);
            const FRGBufferHandle VisibleEntryCounterHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisibleEntryCounters", MutableThis.VisibleEntryCounterBuffers[FrameIndex]);
            const FRGBufferHandle DrawDataVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_DrawDataVisibleEntryIndices", MutableThis.DrawDataVisibleEntryIndexBuffers[FrameIndex]);
            Builder.WriteBuffer(QueueStateHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(CandidateQueueHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisitedEpochHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(NodeCandidate0Handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(NodeCandidate1Handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(NodeArgs0Handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(NodeArgs1Handle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(RunCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisibleEntryCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(DrawDataVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.UavBarrier(QueueStateHandle);
            Builder.UavBarrier(CandidateQueueHandle);
            Builder.UavBarrier(VisitedEpochHandle);
            Builder.UavBarrier(NodeCandidate0Handle);
            Builder.UavBarrier(NodeCandidate1Handle);
            Builder.UavBarrier(NodeArgs0Handle);
            Builder.UavBarrier(NodeArgs1Handle);
            Builder.UavBarrier(RunCountHandle);
            Builder.UavBarrier(VisibleEntryCounterHandle);
            Builder.UavBarrier(DrawDataVisibleEntryHandle);
            Builder.KeepAlive();
        }
    }, [this, &Owner, PassName](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchLevelSplitInit(Owner, Cmd, *Data.Camera, PassName);
    });
}

void FClusterDagRuntime::AddLevelSplitPrepareNodePass(FDeferredPassContext& Context, const char* PassName, uint32_t Level) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FPassData
    {
        bool bEnabled = false;
    };

    Context.Graph.AddPass<FPassData>(PassName, [this, &Context, Level](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Level Split ClusterDAG");
        Data.bEnabled = HasResources();
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            const uint32_t CurrentBufferIndex = Level & 1u;
            const uint32_t NextBufferIndex = CurrentBufferIndex ^ 1u;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle CurrentNodeArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitCurrentNodeArgs", MutableThis.LevelSplitNodeArgsBuffers[CurrentBufferIndex][FrameIndex]);
            const FRGBufferHandle NextNodeArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitNextNodeArgs", MutableThis.LevelSplitNodeArgsBuffers[NextBufferIndex][FrameIndex]);
            Builder.WriteBuffer(CurrentNodeArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(NextNodeArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.UavBarrier(CurrentNodeArgsHandle);
            Builder.UavBarrier(NextNodeArgsHandle);
            Builder.KeepAlive();
        }
    }, [this, &Owner, PassName, Level](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchLevelSplitPrepareNode(Owner, Cmd, Level, PassName);
    });
}

void FClusterDagRuntime::AddLevelSplitNodeCullPass(FDeferredPassContext& Context, const char* PassName, uint32_t Level) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FPassData
    {
        bool bEnabled = false;
    };

    Context.Graph.AddPass<FPassData>(PassName, [this, &Context, Level](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Level Split ClusterDAG");
        Data.bEnabled = HasResources();
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            const uint32_t CurrentBufferIndex = Level & 1u;
            const uint32_t NextBufferIndex = CurrentBufferIndex ^ 1u;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle QueueStateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_QueueState", MutableThis.QueueStateBuffers[FrameIndex]);
            const FRGBufferHandle CandidateQueueHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_CandidateClusterQueue", MutableThis.CandidateClusterQueueBuffers[FrameIndex]);
            const FRGBufferHandle VisitedEpochHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisitedGroupEpoch", MutableThis.VisitedGroupEpochBuffers[FrameIndex]);
            const FRGBufferHandle CurrentNodeCandidateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitCurrentNodeCandidates", MutableThis.LevelSplitNodeCandidateBuffers[CurrentBufferIndex][FrameIndex]);
            const FRGBufferHandle NextNodeCandidateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitNextNodeCandidates", MutableThis.LevelSplitNodeCandidateBuffers[NextBufferIndex][FrameIndex]);
            const FRGBufferHandle CurrentNodeArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitCurrentNodeArgs", MutableThis.LevelSplitNodeArgsBuffers[CurrentBufferIndex][FrameIndex]);
            const FRGBufferHandle NextNodeArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitNextNodeArgs", MutableThis.LevelSplitNodeArgsBuffers[NextBufferIndex][FrameIndex]);
            Builder.ReadBuffer(CurrentNodeArgsHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            Builder.ReadBuffer(CurrentNodeCandidateHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteBuffer(NextNodeCandidateHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(NextNodeArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(QueueStateHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(CandidateQueueHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisitedEpochHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.UavBarrier(NextNodeCandidateHandle);
            Builder.UavBarrier(NextNodeArgsHandle);
            Builder.UavBarrier(QueueStateHandle);
            Builder.UavBarrier(CandidateQueueHandle);
            Builder.UavBarrier(VisitedEpochHandle);
            if (FClusterDagStreamingManager* StreamingManager = Context.Owner.GetClusterDagStreamingManager(); StreamingManager && StreamingManager->IsEnabled())
            {
                FRGBufferHandle PageTableHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_StreamingPageTableLevelNode", StreamingManager->PageTableBuffer);
                FRGBufferHandle PageDataHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_StreamingPageDataLevelNode", StreamingManager->PageDataBuffer);
                FRGBufferHandle FeedbackHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_StreamingFeedbackLevelNode", StreamingManager->FeedbackBuffers[FrameIndex].Gpu);
                Builder.ReadBuffer(PageTableHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Builder.ReadBuffer(PageDataHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Builder.WriteBuffer(FeedbackHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                Builder.UavBarrier(FeedbackHandle);
            }
            Builder.KeepAlive();
        }
    }, [this, &Owner, PassName, Level](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchLevelSplitNodeCull(Owner, Cmd, Level, PassName);
    });
}

void FClusterDagRuntime::AddLevelSplitPrepareClusterPass(FDeferredPassContext& Context, const char* PassName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FPassData
    {
        bool bEnabled = false;
    };

    Context.Graph.AddPass<FPassData>(PassName, [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Level Split ClusterDAG");
        Data.bEnabled = HasResources();
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle QueueStateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_QueueState", MutableThis.QueueStateBuffers[FrameIndex]);
            const FRGBufferHandle ClusterArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitClusterArgs", MutableThis.LevelSplitClusterArgsBuffers[FrameIndex]);
            Builder.WriteBuffer(QueueStateHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(ClusterArgsHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.UavBarrier(QueueStateHandle);
            Builder.UavBarrier(ClusterArgsHandle);
            Builder.KeepAlive();
        }
    }, [this, &Owner, PassName](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchLevelSplitPrepareCluster(Owner, Cmd, PassName);
    });
}

void FClusterDagRuntime::AddLevelSplitClusterCullPass(FDeferredPassContext& Context, const char* PassName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FPassData
    {
        bool bEnabled = false;
    };

    Context.Graph.AddPass<FPassData>(PassName, [this, &Context](FPassData& Data, FRGPassBuilder& Builder)
    {
        Builder.SetPixGroup("Level Split ClusterDAG");
        Data.bEnabled = HasResources();
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle QueueStateHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_QueueState", MutableThis.QueueStateBuffers[FrameIndex]);
            const FRGBufferHandle CandidateQueueHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_CandidateClusterQueue", MutableThis.CandidateClusterQueueBuffers[FrameIndex]);
            const FRGBufferHandle ClusterArgsHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_LevelSplitClusterArgs", MutableThis.LevelSplitClusterArgsBuffers[FrameIndex]);
            const FRGBufferHandle IndirectHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_IndirectCommands", MutableThis.IndirectCommandBuffers[FrameIndex]);
            const FRGBufferHandle RunCountHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_RunCounts", MutableThis.RunCountBuffers[FrameIndex]);
            const FRGBufferHandle VisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisibleEntries", MutableThis.VisibleEntryBuffers[FrameIndex]);
            const FRGBufferHandle VisibleEntryCounterHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_VisibleEntryCounters", MutableThis.VisibleEntryCounterBuffers[FrameIndex]);
            const FRGBufferHandle HwVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_HWVisibleEntryIndices", MutableThis.HwVisibleEntryIndexBuffers[FrameIndex]);
            const FRGBufferHandle SwVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_SWVisibleEntryIndices", MutableThis.SwVisibleEntryIndexBuffers[FrameIndex]);
            const FRGBufferHandle DrawDataVisibleEntryHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_DrawDataVisibleEntryIndices", MutableThis.DrawDataVisibleEntryIndexBuffers[FrameIndex]);
            Builder.ReadBuffer(ClusterArgsHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            Builder.ReadBuffer(CandidateQueueHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Builder.WriteBuffer(IndirectHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(RunCountHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(QueueStateHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(VisibleEntryCounterHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(HwVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(SwVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.WriteBuffer(DrawDataVisibleEntryHandle, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Builder.UavBarrier(IndirectHandle);
            Builder.UavBarrier(RunCountHandle);
            Builder.UavBarrier(QueueStateHandle);
            Builder.UavBarrier(VisibleEntryHandle);
            Builder.UavBarrier(VisibleEntryCounterHandle);
            Builder.UavBarrier(HwVisibleEntryHandle);
            Builder.UavBarrier(SwVisibleEntryHandle);
            Builder.UavBarrier(DrawDataVisibleEntryHandle);
            Builder.KeepAlive();
        }
    }, [this, &Owner, PassName](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        if (!Data.bEnabled)
        {
            return;
        }

        DispatchLevelSplitClusterCull(Owner, Cmd, PassName);
    });
}

void FClusterDagRuntime::AddFinalizeIndirectArgsPass(FDeferredPassContext& Context, const char* PassName, const char* PixGroupName) const
{
    FDeferredRenderer& Owner = Context.Owner;

    struct FPassData
    {
        bool bEnabled = false;
    };

    Context.Graph.AddPass<FPassData>(PassName, [this, &Owner, &Context, PixGroupName](FPassData& Data, FRGPassBuilder& Builder)
    {
        if (PixGroupName && PixGroupName[0] != '\0')
        {
            Builder.SetPixGroup(PixGroupName);
        }

        Data.bEnabled = HasResources();
        if (Data.bEnabled)
        {
            const uint32_t FrameIndex = Context.FrameIndex;
            FClusterDagRuntime& MutableThis = *const_cast<FClusterDagRuntime*>(this);
            const FRGBufferHandle IndirectHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_IndirectCommands", MutableThis.IndirectCommandBuffers[FrameIndex]);
            const FRGBufferHandle RunCountHandle = ImportBindlessBuffer(Context.Graph, "ClusterDAG_RunCounts", MutableThis.RunCountBuffers[FrameIndex]);
            Builder.ReadBuffer(IndirectHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            Builder.ReadBuffer(RunCountHandle, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            Builder.KeepAlive();
        }
    }, [](const FPassData& Data, FDX12CommandContext& Cmd)
    {
        (void)Data;
        (void)Cmd;
    });
}

void FClusterDagRuntime::DispatchInitQueues(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const FCamera& Camera, const char* PassName) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    const bool bUseCommittedQueueInit = ActiveTraversalMode == EClusterDAGTraversalMode::PersistentQueue;
    const uint32_t InitPipelineIndex = GetClusterDagInitPipelineIndex(GpuDrivenCullingProvider.bClusterDagFastShaderEnabled, bUseCommittedQueueInit);
    ID3D12PipelineState* InitPipelineState = InitQueuePipelines[InitPipelineIndex].Get();

    const uint32_t FrameIndex = Owner.GetFrameIndex();
    PopulateCullingConstants(Owner, Camera);

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    const uint32_t DebugStatsIndex = GpuDrivenCullingProvider.GpuDebugPrintStatsUavBindlessIndex;
    const uint32_t TraversalEpoch = ComputeTraversalEpoch(Owner);

    struct FClusterDagInitBindlessConstants
    {
        uint32_t RootGroupBufferIndex;
        uint32_t QueueStateBufferIndex;
        uint32_t GroupQueueBufferIndex;
        uint32_t CandidateClusterQueueBufferIndex;
        uint32_t VisitedGroupEpochBufferIndex;
        uint32_t RunCountBufferIndex;
        uint32_t RootGroupCount;
        uint32_t GroupQueueCapacity;
        uint32_t CandidateQueueCapacity;
        uint32_t GroupCount;
        uint32_t TraversalEpoch;
        uint32_t DebugPrintStatsIndex;
        uint32_t VisibleEntryCountersIndex;
        uint32_t DrawDataVisibleEntryIndicesIndex;
    };

    const FClusterDagInitBindlessConstants BindlessConstants =
    {
        RootGroupBuffer.SrvBindlessIndex,
        QueueStateBuffers[FrameIndex].UavBindlessIndex,
        GroupQueueBuffers[FrameIndex].UavBindlessIndex,
        CandidateClusterQueueBuffers[FrameIndex].UavBindlessIndex,
        VisitedGroupEpochBuffers[FrameIndex].UavBindlessIndex,
        RunCountBuffers[FrameIndex].UavBindlessIndex,
        GpuDrivenCullingProvider.ClusterDagVisibleRootCount,
        RuntimeGroupCount,
        GpuDrivenCullingProvider.ClusterDagClusterCount,
        RuntimeGroupCount,
        TraversalEpoch,
        DebugStatsIndex,
        VisibleEntryCounterBuffers[FrameIndex].UavBindlessIndex,
        DrawDataVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex
    };

    ID3D12DescriptorHeap* Heaps[] = { GpuDrivenCullingProvider.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(InitPipelineState);
    CommandList->SetComputeRootSignature(GpuDrivenCullingProvider.CullingRootSignature);
    CommandList->SetComputeRootConstantBufferView(0, GpuDrivenCullingProvider.CullingConstantBufferAddress);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(BindlessConstants) / sizeof(uint32_t), &BindlessConstants, 0);

    const uint32_t WorkCount = bUseCommittedQueueInit
        ? (std::max)((std::max)(GpuDrivenCullingProvider.ClusterDagVisibleRootCount, static_cast<uint32_t>(IndirectDrawRanges.size())), RuntimeCommandCount)
        : (std::max)(
            (std::max)((std::max)(GpuDrivenCullingProvider.ClusterDagVisibleRootCount, static_cast<uint32_t>(IndirectDrawRanges.size())), RuntimeGroupCount),
            (std::max)(GpuDrivenCullingProvider.ClusterDagClusterCount, RuntimeCommandCount));
    const uint32_t DispatchCount = (WorkCount + 63u) / 64u;
    CommandList->Dispatch((std::max)(1u, DispatchCount), 1, 1);
}

void FClusterDagRuntime::DispatchPersistentCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    const bool bClusterDagDebugEnabled = GpuDrivenCullingProvider.bClusterDagDebugEnabled;
    const bool bClusterDagGpuDebugEnabled = GpuDrivenCullingProvider.bClusterDagGpuDebugEnabled;
    const bool bClusterDagFastEnabled = GpuDrivenCullingProvider.bClusterDagFastShaderEnabled;
    const uint32_t PipelineIndex = GetClusterDagSelectPermutationIndex(ResolveClusterDagSelectPermutation(bClusterDagDebugEnabled, bClusterDagFastEnabled));
    ID3D12PipelineState* PersistentPipelineState = PersistentCullPipelines[PipelineIndex].Get();

    const uint32_t FrameIndex = Owner.GetFrameIndex();
    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    struct FClusterDagPersistentBindlessConstants
    {
        uint32_t GroupBufferIndex;
        uint32_t ClusterBufferIndex;
        uint32_t ChildRefBufferIndex;
        uint32_t DrawDataIndex;
        uint32_t CommandTemplatesIndex;
        uint32_t OutputCommandsIndex;
        uint32_t RunCountsIndex;
        uint32_t QueueStateBufferIndex;
        uint32_t GroupQueueBufferIndex;
        uint32_t CandidateClusterQueueBufferIndex;
        uint32_t VisitedGroupEpochBufferIndex;
        uint32_t GroupCount;
        uint32_t ClusterCount;
        uint32_t ChildRefCount;
        uint32_t DebugPrintStatsIndex;
        uint32_t DebugLineBufferIndex;
        uint32_t TraversalEpoch;
        uint32_t VisibleEntriesIndex;
        uint32_t VisibleEntryCountersIndex;
        uint32_t HwVisibleEntryIndicesIndex;
        uint32_t SwVisibleEntryIndicesIndex;
        uint32_t DrawDataVisibleEntryIndicesIndex;
        uint32_t PageTableBufferIndex;
        uint32_t PageDataBufferIndex;
        uint32_t StreamingRequestBufferIndex;
        uint32_t StreamingRequestCapacity;
        uint32_t StreamingResourceId;
        uint32_t PageSlotBytes;
    };

    const uint32_t TraversalEpoch = ComputeTraversalEpoch(Owner);
    const FClusterDagStreamingShaderBindings StreamingBindings = BuildClusterDagStreamingShaderBindings(Owner, FrameIndex);

    const FClusterDagPersistentBindlessConstants BindlessConstants =
    {
        GroupBuffer.SrvBindlessIndex,
        ClusterBuffer.SrvBindlessIndex,
        ChildRefBuffer.SrvBindlessIndex,
        DrawDataBuffer.SrvBindlessIndex,
        IndirectCommandTemplateBuffers[FrameIndex].SrvBindlessIndex,
        IndirectCommandBuffers[FrameIndex].UavBindlessIndex,
        RunCountBuffers[FrameIndex].UavBindlessIndex,
        QueueStateBuffers[FrameIndex].UavBindlessIndex,
        GroupQueueBuffers[FrameIndex].UavBindlessIndex,
        CandidateClusterQueueBuffers[FrameIndex].UavBindlessIndex,
        VisitedGroupEpochBuffers[FrameIndex].UavBindlessIndex,
        RuntimeGroupCount,
        GpuDrivenCullingProvider.ClusterDagClusterCount,
        RuntimeChildRefCount,
        GpuDrivenCullingProvider.GpuDebugPrintStatsUavBindlessIndex,
        GpuDrivenCullingProvider.GpuDebugLineBufferUavBindlessIndex,
        TraversalEpoch,
        VisibleEntryBuffers[FrameIndex].UavBindlessIndex,
        VisibleEntryCounterBuffers[FrameIndex].UavBindlessIndex,
        HwVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex,
        SwVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex,
        DrawDataVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex,
        StreamingBindings.PageTableBufferIndex,
        StreamingBindings.PageDataBufferIndex,
        StreamingBindings.StreamingRequestBufferIndex,
        StreamingBindings.StreamingRequestCapacity,
        StreamingBindings.StreamingResourceId,
        StreamingBindings.PageSlotBytes
    };

    ID3D12DescriptorHeap* Heaps[] = { GpuDrivenCullingProvider.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetPipelineState(PersistentPipelineState);
    CommandList->SetComputeRootSignature(GpuDrivenCullingProvider.CullingRootSignature);
    CommandList->SetComputeRootConstantBufferView(0, GpuDrivenCullingProvider.CullingConstantBufferAddress);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(BindlessConstants) / sizeof(uint32_t), &BindlessConstants, 0);

    const uint32_t WorkCount = (std::max)(RuntimeGroupCount, GpuDrivenCullingProvider.ClusterDagClusterCount);
    const uint32_t DispatchCount = (WorkCount + 63u) / 64u;
    CommandList->Dispatch((std::max)(1u, DispatchCount), 1, 1);
}

void FClusterDagRuntime::DispatchLevelSplitInit(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const FCamera& Camera, const char* PassName) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    const bool bClusterDagFastEnabled = GpuDrivenCullingProvider.bClusterDagFastShaderEnabled;
    const uint32_t FastPipelineIndex = bClusterDagFastEnabled ? 1u : 0u;
    ID3D12PipelineState* InitPipelineState = LevelSplitInitPipelines[FastPipelineIndex].Get();

    const uint32_t FrameIndex = Owner.GetFrameIndex();
    PopulateCullingConstants(Owner, Camera);

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    ID3D12DescriptorHeap* Heaps[] = { GpuDrivenCullingProvider.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(GpuDrivenCullingProvider.CullingRootSignature);
    CommandList->SetComputeRootConstantBufferView(0, GpuDrivenCullingProvider.CullingConstantBufferAddress);

    const uint32_t DebugStatsIndex = GpuDrivenCullingProvider.GpuDebugPrintStatsUavBindlessIndex;
    const uint32_t TraversalEpoch = ComputeTraversalEpoch(Owner);

    struct FClusterDagLevelSplitInitBindlessConstants
    {
        uint32_t RootGroupBufferIndex;
        uint32_t QueueStateBufferIndex;
        uint32_t NodeCandidateBuffer0Index;
        uint32_t NodeCandidateBuffer1Index;
        uint32_t NodeArgsBuffer0Index;
        uint32_t NodeArgsBuffer1Index;
        uint32_t CandidateClusterQueueBufferIndex;
        uint32_t VisitedGroupEpochBufferIndex;
        uint32_t RunCountBufferIndex;
        uint32_t RootGroupCount;
        uint32_t GroupCount;
        uint32_t CandidateQueueCapacity;
        uint32_t TraversalEpoch;
        uint32_t DebugPrintStatsIndex;
        uint32_t VisibleEntryCountersIndex;
        uint32_t DrawDataVisibleEntryIndicesIndex;
    };

    const FClusterDagLevelSplitInitBindlessConstants InitConstants =
    {
        RootGroupBuffer.SrvBindlessIndex,
        QueueStateBuffers[FrameIndex].UavBindlessIndex,
        LevelSplitNodeCandidateBuffers[0][FrameIndex].UavBindlessIndex,
        LevelSplitNodeCandidateBuffers[1][FrameIndex].UavBindlessIndex,
        LevelSplitNodeArgsBuffers[0][FrameIndex].UavBindlessIndex,
        LevelSplitNodeArgsBuffers[1][FrameIndex].UavBindlessIndex,
        CandidateClusterQueueBuffers[FrameIndex].UavBindlessIndex,
        VisitedGroupEpochBuffers[FrameIndex].UavBindlessIndex,
        RunCountBuffers[FrameIndex].UavBindlessIndex,
        GpuDrivenCullingProvider.ClusterDagVisibleRootCount,
        RuntimeGroupCount,
        GpuDrivenCullingProvider.ClusterDagClusterCount,
        TraversalEpoch,
        DebugStatsIndex,
        VisibleEntryCounterBuffers[FrameIndex].UavBindlessIndex,
        DrawDataVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex
    };

    CommandList->SetPipelineState(InitPipelineState);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(InitConstants) / sizeof(uint32_t), &InitConstants, 0);
    const uint32_t InitWorkCount = (std::max)(
        (std::max)(RuntimeGroupCount, GpuDrivenCullingProvider.ClusterDagClusterCount),
        (std::max)((std::max)(GpuDrivenCullingProvider.ClusterDagVisibleRootCount, static_cast<uint32_t>(IndirectDrawRanges.size())), RuntimeCommandCount));
    CommandList->Dispatch((std::max)(1u, (InitWorkCount + 63u) / 64u), 1, 1);
}

void FClusterDagRuntime::DispatchLevelSplitPrepareNode(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, uint32_t Level, const char* PassName) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    const bool bClusterDagFastEnabled = GpuDrivenCullingProvider.bClusterDagFastShaderEnabled;
    const uint32_t FastPipelineIndex = bClusterDagFastEnabled ? 1u : 0u;
    ID3D12PipelineState* PrepareNodePipelineState = LevelSplitPrepareNodePipelines[FastPipelineIndex].Get();

    const uint32_t FrameIndex = Owner.GetFrameIndex();
    const uint32_t CurrentBufferIndex = Level & 1u;
    const uint32_t NextBufferIndex = CurrentBufferIndex ^ 1u;

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    ID3D12DescriptorHeap* Heaps[] = { GpuDrivenCullingProvider.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(GpuDrivenCullingProvider.CullingRootSignature);
    CommandList->SetComputeRootConstantBufferView(0, GpuDrivenCullingProvider.CullingConstantBufferAddress);

    struct FClusterDagLevelSplitPrepareNodeBindlessConstants
    {
        uint32_t CurrentNodeArgsBufferIndex;
        uint32_t NextNodeArgsBufferIndex;
    };

    const FClusterDagLevelSplitPrepareNodeBindlessConstants PrepareNodeConstants =
    {
        LevelSplitNodeArgsBuffers[CurrentBufferIndex][FrameIndex].UavBindlessIndex,
        LevelSplitNodeArgsBuffers[NextBufferIndex][FrameIndex].UavBindlessIndex
    };

    CommandList->SetPipelineState(PrepareNodePipelineState);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(PrepareNodeConstants) / sizeof(uint32_t), &PrepareNodeConstants, 0);
    CommandList->Dispatch(1, 1, 1);
}

void FClusterDagRuntime::DispatchLevelSplitNodeCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, uint32_t Level, const char* PassName) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    const bool bClusterDagDebugEnabled = GpuDrivenCullingProvider.bClusterDagDebugEnabled;
    const bool bClusterDagFastEnabled = GpuDrivenCullingProvider.bClusterDagFastShaderEnabled;
    const uint32_t PermutationIndex = GetClusterDagSelectPermutationIndex(ResolveClusterDagSelectPermutation(bClusterDagDebugEnabled, bClusterDagFastEnabled));
    ID3D12PipelineState* NodeCullPipelineState = LevelSplitNodeCullPipelines[PermutationIndex].Get();

    const uint32_t FrameIndex = Owner.GetFrameIndex();
    const uint32_t CurrentBufferIndex = Level & 1u;
    const uint32_t NextBufferIndex = CurrentBufferIndex ^ 1u;
    ID3D12Resource* CurrentNodeArgsBuffer = LevelSplitNodeArgsBuffers[CurrentBufferIndex][FrameIndex].Get();

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    ID3D12DescriptorHeap* Heaps[] = { GpuDrivenCullingProvider.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(GpuDrivenCullingProvider.CullingRootSignature);
    CommandList->SetComputeRootConstantBufferView(0, GpuDrivenCullingProvider.CullingConstantBufferAddress);

    const uint32_t DebugStatsIndex = GpuDrivenCullingProvider.GpuDebugPrintStatsUavBindlessIndex;
    const uint32_t TraversalEpoch = ComputeTraversalEpoch(Owner);

    struct FClusterDagLevelSplitNodeCullBindlessConstants
    {
        uint32_t GroupBufferIndex;
        uint32_t ClusterBufferIndex;
        uint32_t ChildRefBufferIndex;
        uint32_t QueueStateBufferIndex;
        uint32_t CurrentNodeCandidateBufferIndex;
        uint32_t NextNodeCandidateBufferIndex;
        uint32_t NextNodeArgsBufferIndex;
        uint32_t CandidateClusterQueueBufferIndex;
        uint32_t VisitedGroupEpochBufferIndex;
        uint32_t GroupCount;
        uint32_t ClusterCount;
        uint32_t ChildRefCount;
        uint32_t DebugPrintStatsIndex;
        uint32_t DebugLineBufferIndex;
        uint32_t TraversalEpoch;
        uint32_t PageTableBufferIndex;
        uint32_t PageDataBufferIndex;
        uint32_t StreamingRequestBufferIndex;
        uint32_t StreamingRequestCapacity;
        uint32_t StreamingResourceId;
        uint32_t PageSlotBytes;
    };

    const FClusterDagStreamingShaderBindings StreamingBindings = BuildClusterDagStreamingShaderBindings(Owner, FrameIndex);
    const FClusterDagLevelSplitNodeCullBindlessConstants NodeCullConstants =
    {
        GroupBuffer.SrvBindlessIndex,
        ClusterBuffer.SrvBindlessIndex,
        ChildRefBuffer.SrvBindlessIndex,
        QueueStateBuffers[FrameIndex].UavBindlessIndex,
        LevelSplitNodeCandidateBuffers[CurrentBufferIndex][FrameIndex].SrvBindlessIndex,
        LevelSplitNodeCandidateBuffers[NextBufferIndex][FrameIndex].UavBindlessIndex,
        LevelSplitNodeArgsBuffers[NextBufferIndex][FrameIndex].UavBindlessIndex,
        CandidateClusterQueueBuffers[FrameIndex].UavBindlessIndex,
        VisitedGroupEpochBuffers[FrameIndex].UavBindlessIndex,
        RuntimeGroupCount,
        GpuDrivenCullingProvider.ClusterDagClusterCount,
        RuntimeChildRefCount,
        DebugStatsIndex,
        GpuDrivenCullingProvider.GpuDebugLineBufferUavBindlessIndex,
        TraversalEpoch,
        StreamingBindings.PageTableBufferIndex,
        StreamingBindings.PageDataBufferIndex,
        StreamingBindings.StreamingRequestBufferIndex,
        StreamingBindings.StreamingRequestCapacity,
        StreamingBindings.StreamingResourceId,
        StreamingBindings.PageSlotBytes
    };

    CommandList->SetPipelineState(NodeCullPipelineState);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(NodeCullConstants) / sizeof(uint32_t), &NodeCullConstants, 0);
    CommandList->ExecuteIndirect(DispatchCommandSignature.Get(), 1, CurrentNodeArgsBuffer, 0u, nullptr, 0u);
}

void FClusterDagRuntime::DispatchLevelSplitPrepareCluster(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    const bool bClusterDagFastEnabled = GpuDrivenCullingProvider.bClusterDagFastShaderEnabled;
    const uint32_t FastPipelineIndex = bClusterDagFastEnabled ? 1u : 0u;
    ID3D12PipelineState* PrepareClusterPipelineState = LevelSplitPrepareClusterPipelines[FastPipelineIndex].Get();

    const uint32_t FrameIndex = Owner.GetFrameIndex();
    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    ID3D12DescriptorHeap* Heaps[] = { GpuDrivenCullingProvider.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(GpuDrivenCullingProvider.CullingRootSignature);
    CommandList->SetComputeRootConstantBufferView(0, GpuDrivenCullingProvider.CullingConstantBufferAddress);

    struct FClusterDagLevelSplitPrepareClusterBindlessConstants
    {
        uint32_t QueueStateBufferIndex;
        uint32_t ClusterDispatchArgsBufferIndex;
    };

    const FClusterDagLevelSplitPrepareClusterBindlessConstants PrepareClusterConstants =
    {
        QueueStateBuffers[FrameIndex].UavBindlessIndex,
        LevelSplitClusterArgsBuffers[FrameIndex].UavBindlessIndex
    };

    CommandList->SetPipelineState(PrepareClusterPipelineState);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(PrepareClusterConstants) / sizeof(uint32_t), &PrepareClusterConstants, 0);
    CommandList->Dispatch(1, 1, 1);
}

void FClusterDagRuntime::DispatchLevelSplitClusterCull(FDeferredRenderer& Owner, FDX12CommandContext& CmdContext, const char* PassName) const
{
    const FRenderer::FGpuDrivenCullingProvider GpuDrivenCullingProvider = Owner.GetGpuDrivenCullingProvider();
    const bool bClusterDagDebugEnabled = GpuDrivenCullingProvider.bClusterDagDebugEnabled;
    const bool bClusterDagFastEnabled = GpuDrivenCullingProvider.bClusterDagFastShaderEnabled;
    const uint32_t PermutationIndex = GetClusterDagSelectPermutationIndex(ResolveClusterDagSelectPermutation(bClusterDagDebugEnabled, bClusterDagFastEnabled));
    ID3D12PipelineState* ClusterCullPipelineState = LevelSplitClusterCullPipelines[PermutationIndex].Get();

    const uint32_t FrameIndex = Owner.GetFrameIndex();
    ID3D12Resource* ClusterArgsBuffer = LevelSplitClusterArgsBuffers[FrameIndex].Get();

    ID3D12GraphicsCommandList* CommandList = CmdContext.GetCommandList();

    ID3D12DescriptorHeap* Heaps[] = { GpuDrivenCullingProvider.BindlessDescriptorHeap };
    CommandList->SetDescriptorHeaps(_countof(Heaps), Heaps);
    CommandList->SetComputeRootSignature(GpuDrivenCullingProvider.CullingRootSignature);
    CommandList->SetComputeRootConstantBufferView(0, GpuDrivenCullingProvider.CullingConstantBufferAddress);

    const uint32_t DebugStatsIndex = GpuDrivenCullingProvider.GpuDebugPrintStatsUavBindlessIndex;

    struct FClusterDagLevelSplitClusterCullBindlessConstants
    {
        uint32_t ClusterBufferIndex;
        uint32_t DrawDataIndex;
        uint32_t CommandTemplatesIndex;
        uint32_t OutputCommandsIndex;
        uint32_t RunCountsIndex;
        uint32_t QueueStateBufferIndex;
        uint32_t CandidateClusterQueueBufferIndex;
        uint32_t ClusterCount;
        uint32_t DebugPrintStatsIndex;
        uint32_t VisibleEntriesIndex;
        uint32_t VisibleEntryCountersIndex;
        uint32_t HwVisibleEntryIndicesIndex;
        uint32_t SwVisibleEntryIndicesIndex;
        uint32_t DrawDataVisibleEntryIndicesIndex;
        uint32_t PageDataBufferIndex;
    };

    const FClusterDagStreamingShaderBindings StreamingBindings = BuildClusterDagStreamingShaderBindings(Owner, FrameIndex);
    const FClusterDagLevelSplitClusterCullBindlessConstants ClusterCullConstants =
    {
        ClusterBuffer.SrvBindlessIndex,
        DrawDataBuffer.SrvBindlessIndex,
        IndirectCommandTemplateBuffers[FrameIndex].SrvBindlessIndex,
        IndirectCommandBuffers[FrameIndex].UavBindlessIndex,
        RunCountBuffers[FrameIndex].UavBindlessIndex,
        QueueStateBuffers[FrameIndex].UavBindlessIndex,
        CandidateClusterQueueBuffers[FrameIndex].SrvBindlessIndex,
        GpuDrivenCullingProvider.ClusterDagClusterCount,
        DebugStatsIndex,
        VisibleEntryBuffers[FrameIndex].UavBindlessIndex,
        VisibleEntryCounterBuffers[FrameIndex].UavBindlessIndex,
        HwVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex,
        SwVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex,
        DrawDataVisibleEntryIndexBuffers[FrameIndex].UavBindlessIndex,
        StreamingBindings.PageDataBufferIndex
    };

    CommandList->SetPipelineState(ClusterCullPipelineState);
    CommandList->SetComputeRoot32BitConstants(1, sizeof(ClusterCullConstants) / sizeof(uint32_t), &ClusterCullConstants, 0);
    CommandList->ExecuteIndirect(DispatchCommandSignature.Get(), 1, ClusterArgsBuffer, 0u, nullptr, 0u);
}

