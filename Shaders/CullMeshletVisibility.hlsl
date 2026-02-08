#include "CullingConstants.hlsl"
#include "CullMeshletCommon.hlsl"

cbuffer CullingBindlessConstants : register(b1)
{
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
};


[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= IndirectCommandCount)
        return;

    StructuredBuffer<float4> ModelBounds = ResourceDescriptorHeap[ModelBoundsIndex];
    StructuredBuffer<float4> MeshletConeAxisCutoff = ResourceDescriptorHeap[MeshletConeAxisIndex];
    RWStructuredBuffer<uint> VisibleMeshlets = ResourceDescriptorHeap[VisibleMeshletsIndex];
    StructuredBuffer<uint> VisibilityInput = ResourceDescriptorHeap[VisibilityInputIndex];

    if (CullingMode == 1)
    {
        if (VisibilityInput[index] == 0)
        {
            VisibleMeshlets[index] = 0;
            return;
        }
    }
    else if (CullingMode == 2)
    {
        if (VisibleMeshlets[index] != 0)
        {
            VisibleMeshlets[index] = 1;
            return;
        }
    }

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
