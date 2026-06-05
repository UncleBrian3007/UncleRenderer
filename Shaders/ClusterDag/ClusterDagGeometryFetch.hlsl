#ifndef CLUSTER_DAG_GEOMETRY_FETCH_HLSL
#define CLUSTER_DAG_GEOMETRY_FETCH_HLSL

#include "ClusterDagCommon.hlsl"
#include "../OctahedralEncoding.hlsli"
#include "../SceneConstantsFields.hlsli"

struct ClusterDagResolveSceneData
{
    SCENE_CONSTANTS_FIELDS
};

struct ClusterDagPackedPosition
{
    uint XY;
    uint Z;
};

int SignExtend10(uint value)
{
    return (value & 0x200u) != 0u
        ? int(value | 0xFFFFFC00u)
        : int(value & 0x3FFu);
}

float DecodeSnorm10(uint value)
{
    return max((float)SignExtend10(value) / 511.0f, -1.0f);
}

float3 DecodeClusterDagPackedPosition(
    ClusterDagPackedPosition packedPosition,
    ClusterDagResolveSceneData sceneData)
{
    const uint qx = packedPosition.XY & 0xFFFFu;
    const uint qy = packedPosition.XY >> 16u;
    const uint qz = packedPosition.Z & 0xFFFFu;
    return sceneData.ClusterDagPackedPositionOffset.xyz
        + float3(qx, qy, qz) * sceneData.ClusterDagPackedPositionScale.xyz;
}

float2 DecodeClusterDagPackedUV(uint packedUV)
{
    return float2(
        f16tof32(packedUV & 0xFFFFu),
        f16tof32(packedUV >> 16u));
}

float4 DecodeClusterDagPackedTangent(uint packedTangent)
{
    const float3 tangent = normalize(float3(
        DecodeSnorm10(packedTangent & 0x3FFu),
        DecodeSnorm10((packedTangent >> 10u) & 0x3FFu),
        DecodeSnorm10((packedTangent >> 20u) & 0x3FFu)));
    const float tangentSign = ((packedTangent >> 30u) & 0x1u) != 0u ? -1.0f : 1.0f;
    return float4(tangent, tangentSign);
}

float4 DecodeClusterDagPackedColor(uint packedColor)
{
    const float inv255 = 1.0f / 255.0f;
    return float4(
        (packedColor & 0xFFu) * inv255,
        ((packedColor >> 8u) & 0xFFu) * inv255,
        ((packedColor >> 16u) & 0xFFu) * inv255,
        ((packedColor >> 24u) & 0xFFu) * inv255);
}

float3 BuildClusterDagFallbackTangentDirection(float3 normal)
{
    const float3 up = abs(normal.z) < 0.999f
        ? float3(0.0f, 0.0f, 1.0f)
        : float3(0.0f, 1.0f, 0.0f);
    return normalize(cross(up, normal));
}

float4 BuildClusterDagFallbackTangent(float3 normal)
{
    return float4(BuildClusterDagFallbackTangentDirection(normal), 1.0f);
}

bool LoadClusterDagTriangleIndices(
    ClusterDagResolveSceneData sceneData,
    ClusterDagVisibleEntry visibleEntry,
    ClusterDagDrawData drawData,
    uint primitiveId,
    ByteAddressBuffer PageData,
    out uint index0,
    out uint index1,
    out uint index2)
{
    index0 = 0u;
    index1 = 0u;
    index2 = 0u;

    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.ExtraBindlessIndices.y)];
    const uint baseIndex = drawData.StartIndex + primitiveId * 3u;
    if (HasClusterDagPageData(visibleEntry))
    {
        const bool loaded0 = LoadClusterDagPagedVertexIndex(visibleEntry, drawData, primitiveId, 0u, PageData, index0);
        const bool loaded1 = LoadClusterDagPagedVertexIndex(visibleEntry, drawData, primitiveId, 1u, PageData, index1);
        const bool loaded2 = LoadClusterDagPagedVertexIndex(visibleEntry, drawData, primitiveId, 2u, PageData, index2);
        return loaded0 && loaded1 && loaded2;
    }

    index0 = IndexBuffer[baseIndex + 0u];
    index1 = IndexBuffer[baseIndex + 1u];
    index2 = IndexBuffer[baseIndex + 2u];
    return true;
}

float3 LoadClusterDagPosition(ClusterDagResolveSceneData sceneData, ClusterDagVisibleEntry visibleEntry, uint vertexIndex, ByteAddressBuffer PageData)
{
    StructuredBuffer<float3> PositionBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.x)];
    StructuredBuffer<ClusterDagPackedPosition> PackedPositionBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.x)];
    uint packedXy = 0u;
    uint packedZ = 0u;
    if (LoadClusterDagPagedPackedPositionWords(visibleEntry, vertexIndex, PageData, packedXy, packedZ))
    {
        ClusterDagPackedPosition packedPosition;
        packedPosition.XY = packedXy;
        packedPosition.Z = packedZ;
        return DecodeClusterDagPackedPosition(packedPosition, sceneData);
    }

    if (HasClusterDagPageData(visibleEntry))
    {
        return 0.0f.xxx;
    }

    return sceneData.ClusterDagVertexPackingMode != 0u
        ? DecodeClusterDagPackedPosition(PackedPositionBuffer[vertexIndex], sceneData)
        : PositionBuffer[vertexIndex];
}

float3 LoadClusterDagNormal(ClusterDagResolveSceneData sceneData, ClusterDagVisibleEntry visibleEntry, uint vertexIndex, ByteAddressBuffer PageData)
{
    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.y)];
    StructuredBuffer<uint> PackedNormalBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.y)];
    uint packedNormal = 0u;
    if (LoadClusterDagPagedPackedScalar(visibleEntry, vertexIndex, kClusterDagGpuPageHeaderPackedNormalByteOffsetOffset, kClusterDagGpuPageHeaderPackedNormalCountOffset, PageData, packedNormal))
    {
        return DecodeOctahedral16x2(packedNormal);
    }

    if (HasClusterDagPageData(visibleEntry))
    {
        return float3(0.0f, 0.0f, 1.0f);
    }

    return sceneData.ClusterDagVertexPackingMode != 0u
        ? DecodeOctahedral16x2(PackedNormalBuffer[vertexIndex])
        : NormalBuffer[vertexIndex];
}

float2 LoadClusterDagUv(ClusterDagResolveSceneData sceneData, ClusterDagVisibleEntry visibleEntry, uint vertexIndex, ByteAddressBuffer PageData)
{
    if (sceneData.ClusterDagVertexPackingMode != 0u)
    {
        uint packedUv = 0u;
        if (LoadClusterDagPagedPackedScalar(visibleEntry, vertexIndex, kClusterDagGpuPageHeaderPackedUvByteOffsetOffset, kClusterDagGpuPageHeaderPackedUvCountOffset, PageData, packedUv))
        {
            return DecodeClusterDagPackedUV(packedUv);
        }

        if (HasClusterDagPageData(visibleEntry))
        {
            return sceneData.ClusterDagPackedConstantUV.xy;
        }

        if (sceneData.VertexBufferBindlessIndices.z != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedTexCoordBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.z)];
            return DecodeClusterDagPackedUV(PackedTexCoordBuffer[vertexIndex]);
        }

        return sceneData.ClusterDagPackedConstantUV.xy;
    }

    StructuredBuffer<float2> TexCoordBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.z)];
    return TexCoordBuffer[vertexIndex];
}

float4 LoadClusterDagTangent(ClusterDagResolveSceneData sceneData, ClusterDagVisibleEntry visibleEntry, uint vertexIndex, float3 normal, ByteAddressBuffer PageData)
{
    if (sceneData.ClusterDagVertexPackingMode != 0u)
    {
        uint packedTangent = 0u;
        if (LoadClusterDagPagedPackedScalar(visibleEntry, vertexIndex, kClusterDagGpuPageHeaderPackedTangentByteOffsetOffset, kClusterDagGpuPageHeaderPackedTangentCountOffset, PageData, packedTangent))
        {
            return DecodeClusterDagPackedTangent(packedTangent);
        }

        if (HasClusterDagPageData(visibleEntry))
        {
            return BuildClusterDagFallbackTangent(normal);
        }

        if (sceneData.VertexBufferBindlessIndices.w != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedTangentBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.w)];
            return DecodeClusterDagPackedTangent(PackedTangentBuffer[vertexIndex]);
        }

        return BuildClusterDagFallbackTangent(normal);
    }

    StructuredBuffer<float4> TangentBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.VertexBufferBindlessIndices.w)];
    return TangentBuffer[vertexIndex];
}

float4 LoadClusterDagColor(ClusterDagResolveSceneData sceneData, ClusterDagVisibleEntry visibleEntry, uint vertexIndex, ByteAddressBuffer PageData)
{
    if (sceneData.ClusterDagVertexPackingMode != 0u)
    {
        uint packedColor = 0u;
        if (LoadClusterDagPagedPackedScalar(visibleEntry, vertexIndex, kClusterDagGpuPageHeaderPackedColorByteOffsetOffset, kClusterDagGpuPageHeaderPackedColorCountOffset, PageData, packedColor))
        {
            return DecodeClusterDagPackedColor(packedColor);
        }

        if (HasClusterDagPageData(visibleEntry))
        {
            return sceneData.ClusterDagPackedConstantColor;
        }

        if (sceneData.ExtraBindlessIndices.x != 0xffffffffu)
        {
            StructuredBuffer<uint> PackedColorBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.ExtraBindlessIndices.x)];
            return DecodeClusterDagPackedColor(PackedColorBuffer[vertexIndex]);
        }

        return sceneData.ClusterDagPackedConstantColor;
    }

    StructuredBuffer<float4> ColorBuffer = ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.ExtraBindlessIndices.x)];
    return ColorBuffer[vertexIndex];
}

#endif
