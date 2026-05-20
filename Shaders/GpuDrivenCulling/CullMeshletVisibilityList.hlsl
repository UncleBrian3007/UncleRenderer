#include "../CullingConstants.hlsl"

cbuffer CullingBindlessConstants : register(b1)
{
    row_major float4x4 ViewProjection;
    uint ModelBoundsIndex;
    uint HZBTextureIndex;
    uint MeshletConeAxisIndex;
    uint MeshletConeApexIndex;
    uint VisibleMeshletsIndex;
    uint VisibilityInputIndex;
    uint CullingListIndex;
    uint CullingListCountIndex;
    uint DebugPrintBufferIndex;
    uint DebugPrintStatsIndex;
    uint IndirectCommandCount;
    uint HZBEnabled;
    uint HZBMipCount;
    uint HZBWidth;
    uint HZBHeight;
    uint CullingMode;
};

#include "../CullMeshletCommon.hlsl"

[numthreads(64, 1, 1)]
void CullMeshletVisibilityListCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ByteAddressBuffer CullingListCount = ResourceDescriptorHeap[CullingListCountIndex];
    const uint listCount = CullingListCount.Load(0);
    if (dispatchThreadId.x >= listCount)
        return;

    StructuredBuffer<uint> CullingList = ResourceDescriptorHeap[CullingListIndex];
    const uint index = CullingList[dispatchThreadId.x];
    if (index >= IndirectCommandCount)
        return;

    StructuredBuffer<float4> ModelBounds = ResourceDescriptorHeap[ModelBoundsIndex];
    StructuredBuffer<float4> MeshletConeAxisCutoff = ResourceDescriptorHeap[MeshletConeAxisIndex];
    RWStructuredBuffer<uint> VisibleMeshlets = ResourceDescriptorHeap[VisibleMeshletsIndex];

    Texture2D<float2> HZBTexture = ResourceDescriptorHeap[HZBTextureIndex];
    bool visible;
    bool frustumVisible;
    bool coneVisible;
    bool occluded;
    EvaluateMeshletVisibility(
        index,
        ModelBounds,
        MeshletConeAxisCutoff,
        HZBTexture,
        visible,
        frustumVisible,
        coneVisible,
        occluded);

    VisibleMeshlets[index] = visible ? 1u : 0u;

    if (DebugPrintEnabled != 0 && !visible)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[DebugPrintStatsIndex];
        RecordMeshletDebug(
            visible,
            frustumVisible,
            coneVisible,
            occluded,
            DebugPrintStats);
    }
    else if (DebugPrintEnabled != 0 && visible && CullingMode == 2)
    {
        RWByteAddressBuffer DebugPrintStats = ResourceDescriptorHeap[DebugPrintStatsIndex];
        DebugPrintStats.InterlockedAdd(16, 1);
    }
}
