#ifndef CULL_MESHLET_COMMON_HLSL
#define CULL_MESHLET_COMMON_HLSL

#include "GpuDebug/GpuDebugPrintCommon.hlsl"
#include "HzbOcclusionCommon.hlsl"

bool IsConeVisible_SphereExpanded(
    float3 center,
    float radius,
    uint index,
    StructuredBuffer<float4> MeshletConeAxisCutoff)
{
    float4 axisCutoff = MeshletConeAxisCutoff[index];

    if (axisCutoff.w < 0.0f)
        return true;

    float3 axis = axisCutoff.xyz;

    float3 view = center - CameraPosition;

    float distSq = dot(view, view);
    if (distSq <= 1e-8f)
        return true;

    float dist = sqrt(distSq);

    bool coneCulled =
        dot(view, -axis) >= axisCutoff.w * dist + radius;

    return !coneCulled;
}

void EvaluateMeshletVisibility(
    uint index,
    StructuredBuffer<float4> ModelBounds,
    StructuredBuffer<float4> MeshletConeAxisCutoff,
    Texture2D<float2> HZBTexture,
    out bool visible,
    out bool frustumVisible,
    out bool coneVisible,
    out bool occluded)
{
    float4 sphere = ModelBounds[index];
    float3 center = sphere.xyz;
    float radius = sphere.w;

    frustumVisible = IsSphereVisible(center, radius);
    coneVisible = true;
    visible = frustumVisible;

    if (visible)
    {
        const float coneCutoff = MeshletConeAxisCutoff[index].w;
        if (coneCutoff >= 0.0f)
        {
            coneVisible = IsConeVisible_SphereExpanded(center, radius, index, MeshletConeAxisCutoff);
            visible = coneVisible;
        }
    }

    occluded = false;
    if (visible && HZBEnabled != 0)
    {
        occluded = IsSphereOccludedByHZB(center, radius, ViewProjection, HZBTexture, HZBWidth, HZBHeight, HZBMipCount);
        visible = !occluded;
    }
}

void RecordMeshletDebug(
    bool visible,
    bool frustumVisible,
    bool coneVisible,
    bool occluded,
    RWByteAddressBuffer DebugPrintStats)
{
    if (DebugPrintEnabled == 0 || visible)
    {
        return;
    }

    if (!frustumVisible)
    {
        DebugPrintStats.InterlockedAdd(0, 1);
    }
    else if (!coneVisible)
    {
        DebugPrintStats.InterlockedAdd(8, 1);
    }
    else if (occluded)
    {
        DebugPrintStats.InterlockedAdd(4 * kDebugPrintStatsOccludedIndex, 1);
    }
}

#endif
