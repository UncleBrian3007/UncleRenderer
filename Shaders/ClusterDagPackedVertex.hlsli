#ifndef CLUSTER_DAG_PACKED_VERTEX_HLSLI
#define CLUSTER_DAG_PACKED_VERTEX_HLSLI

#include "OctahedralEncoding.hlsli"

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

float3 DecodeClusterDagPackedPosition(ClusterDagPackedPosition packedPosition)
{
    const uint qx = packedPosition.XY & 0xFFFFu;
    const uint qy = packedPosition.XY >> 16u;
    const uint qz = packedPosition.Z & 0xFFFFu;
    return ClusterDagPackedPositionOffset.xyz
        + float3(qx, qy, qz) * ClusterDagPackedPositionScale.xyz;
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

#endif
