#ifndef RAY_TRACING_COMMON_HLSL
#define RAY_TRACING_COMMON_HLSL

#include "Common.hlsli"

float3 TangentToWorld(float3 v, float3 N)
{
    const float3 B = normalize(GetPerpendicularVector(N));
    const float3 T = normalize(cross(B, N));
    return T * v.x + B * v.y + N * v.z;
}

float3 WorldToTangent(float3 w, float3 N)
{
    const float3 B = normalize(GetPerpendicularVector(N));
    const float3 T = normalize(cross(B, N));
    return float3(dot(w, T), dot(w, B), dot(w, N));
}

float3 EvaluateSky(float3 direction)
{
    TextureCube EnvironmentMap = ResourceDescriptorHeap[EnvironmentCubeBindlessIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return EnvironmentMap.SampleLevel(LinearSampler, direction, 0).rgb;
}

// "Texture Level-of-Detail Strategies for Real-Time Ray Tracing"
struct FRayCone
{
    float Width;
    float SpreadAngle;
};

FRayCone RayConeFromCamera(uint RenderHeight)
{
    FRayCone Cone;
    Cone.Width = 0.0f;
    Cone.SpreadAngle = atan(2.0f * rcp(max(Projection._22, 1e-6f)) / max((float)RenderHeight, 1.0f));
    return Cone;
}

void RayConePropagate(inout FRayCone Cone, float SurfaceSpreadAngle, float HitT)
{
    Cone.Width += Cone.SpreadAngle * max(HitT, 0.0f);
    Cone.SpreadAngle += SurfaceSpreadAngle;
}

float ComputeRayConeTextureLod(Texture2D<float4> Texture, float3 RayDirection, float3 SurfaceNormal, FRayCone Cone, float TriangleLodConstant = 0.0f)
{
    uint TextureWidth = 1u;
    uint TextureHeight = 1u;
    uint MipCount = 1u;
    Texture.GetDimensions(0, TextureWidth, TextureHeight, MipCount);

    const float NdotD = max(1e-4f, abs(dot(normalize(RayDirection), normalize(SurfaceNormal))));
    const float ConeTerm = max(1e-6f, Cone.Width / NdotD);
    const float TexelTerm = max(1.0f, (float)TextureWidth * (float)TextureHeight);
    const float Lod = TriangleLodConstant + 0.5f * log2(TexelTerm) + log2(ConeTerm);
    return clamp(Lod, 0.0f, max(0.0f, (float)MipCount - 1.0f));
}

// Structure to hold per-instance geometry buffer indices
struct FInstanceData
{
    uint PositionBufferIndex;
    uint NormalBufferIndex;
    uint UVBufferIndex;
    uint IndexBufferIndex;
    uint TangentBufferIndex;
    uint BaseColorTextureIndex;
    uint NormalTextureIndex;
    uint MetallicRoughnessTextureIndex;
    uint Flags;
    uint EmissiveTextureIndex;
    uint Padding0;
    uint Padding1;
    float4 EmissiveFactor;
    float4 BaseColorFactorAndAlpha;
    float4 MetallicRoughnessAlphaCutoff;
    row_major float4x4 WorldInverseTranspose;
};

static const uint INSTANCE_FLAG_DOUBLE_SIDED = 1u;

bool IsDoubleSided(FInstanceData instData)
{
    return (instData.Flags & INSTANCE_FLAG_DOUBLE_SIDED) != 0u;
}

// Fetch instance data for a hit
FInstanceData GetInstanceData(uint instanceID)
{
    StructuredBuffer<FInstanceData> InstanceDataBuffer = ResourceDescriptorHeap[InstanceDataBufferIndex];
    return InstanceDataBuffer[instanceID];
}

// Fetch triangle indices
uint3 GetTriangleIndices(uint instanceID, uint primitiveIndex)
{
    FInstanceData instData = GetInstanceData(instanceID);
    StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[instData.IndexBufferIndex];

    uint baseIndex = primitiveIndex * 3;
    uint3 indices;
    indices.x = IndexBuffer[baseIndex + 0];
    indices.y = IndexBuffer[baseIndex + 1];
    indices.z = IndexBuffer[baseIndex + 2];
    return indices;
}

// Interpolate vertex attribute using barycentric coordinates
float3 InterpolateFloat3(float3 v0, float3 v1, float3 v2, float2 barycentrics)
{
    return v0 * (1.0f - barycentrics.x - barycentrics.y) + v1 * barycentrics.x + v2 * barycentrics.y;
}

float2 InterpolateFloat2(float2 v0, float2 v1, float2 v2, float2 barycentrics)
{
    return v0 * (1.0f - barycentrics.x - barycentrics.y) + v1 * barycentrics.x + v2 * barycentrics.y;
}

// Get interpolated UV at hit point
float2 GetInterpolatedUV(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    FInstanceData instData = GetInstanceData(instanceID);
    uint3 indices = GetTriangleIndices(instanceID, primitiveIndex);

    StructuredBuffer<float2> UVBuffer = ResourceDescriptorHeap[instData.UVBufferIndex];

    float2 uv0 = UVBuffer[indices.x];
    float2 uv1 = UVBuffer[indices.y];
    float2 uv2 = UVBuffer[indices.z];

    return InterpolateFloat2(uv0, uv1, uv2, barycentrics);
}

float4 GetInterpolatedTangent(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    FInstanceData instData = GetInstanceData(instanceID);
    uint3 indices = GetTriangleIndices(instanceID, primitiveIndex);

    StructuredBuffer<float4> TangentBuffer = ResourceDescriptorHeap[instData.TangentBufferIndex];

    float4 t0 = TangentBuffer[indices.x];
    float4 t1 = TangentBuffer[indices.y];
    float4 t2 = TangentBuffer[indices.z];

    float3 tangent = InterpolateFloat3(t0.xyz, t1.xyz, t2.xyz, barycentrics);
    float handedness = t0.w * (1.0f - barycentrics.x - barycentrics.y) + t1.w * barycentrics.x + t2.w * barycentrics.y;

    return float4(tangent, handedness);
}

// Get interpolated normal at hit point (transforms from object space to world space)
float3 GetInterpolatedNormal(uint instanceID, uint primitiveIndex, float2 barycentrics, float mipLod)
{
    FInstanceData instData = GetInstanceData(instanceID);
    uint3 indices = GetTriangleIndices(instanceID, primitiveIndex);

    StructuredBuffer<float3> NormalBuffer = ResourceDescriptorHeap[instData.NormalBufferIndex];

    float3 n0 = NormalBuffer[indices.x];
    float3 n1 = NormalBuffer[indices.y];
    float3 n2 = NormalBuffer[indices.z];

    // Interpolate object-space normal
    float3 objectNormal = InterpolateFloat3(n0, n1, n2, barycentrics);

    // Transform to world space using inverse transpose
    float3 worldNormal = mul((float3x3)instData.WorldInverseTranspose, objectNormal);

    worldNormal = normalize(worldNormal);

    if (instData.NormalTextureIndex == 0xFFFFFFFF)
    {
        return worldNormal;
    }

    float4 tangentPacked = GetInterpolatedTangent(instanceID, primitiveIndex, barycentrics);
    float3 worldTangent = normalize(mul((float3x3)instData.WorldInverseTranspose, tangentPacked.xyz));
    worldTangent = normalize(worldTangent - worldNormal * dot(worldNormal, worldTangent));
    float3 worldBitangent = cross(worldNormal, worldTangent) * tangentPacked.w;

    float2 uv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
    Texture2D<float4> NormalTexture = ResourceDescriptorHeap[instData.NormalTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    float3 tangentNormal = NormalTexture.SampleLevel(LinearSampler, uv, mipLod).xyz * 2.0f - 1.0f;

    float3x3 TBN = float3x3(worldTangent, worldBitangent, worldNormal);
    return normalize(mul(tangentNormal, TBN));
}

float3 GetInterpolatedNormal(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    return GetInterpolatedNormal(instanceID, primitiveIndex, barycentrics, 0.0f);
}

float3 GetInterpolatedNormalRayCone(uint instanceID, uint primitiveIndex, float2 barycentrics, float3 rayDirection, float3 surfaceNormal, FRayCone cone, float triangleLodConstant = 0.0f)
{
    FInstanceData instData = GetInstanceData(instanceID);
    if (instData.NormalTextureIndex == 0xFFFFFFFF)
    {
        return GetInterpolatedNormal(instanceID, primitiveIndex, barycentrics, 0.0f);
    }

    Texture2D<float4> NormalTexture = ResourceDescriptorHeap[instData.NormalTextureIndex];
    const float mipLod = ComputeRayConeTextureLod(NormalTexture, rayDirection, surfaceNormal, cone, triangleLodConstant);
    return GetInterpolatedNormal(instanceID, primitiveIndex, barycentrics, mipLod);
}

float3 SampleAlbedo(uint instanceID, float2 uv, float mipLod);
float3 SampleEmissive(uint instanceID, float2 uv, float mipLod);
float SampleOpacity(uint instanceID, uint primitiveIndex, float2 barycentrics, float mipLod);
float2 SampleMetallicRoughness(uint instanceID, float2 uv, float mipLod);

// Sample albedo from texture
float3 SampleAlbedo(uint instanceID, float2 uv)
{
    return SampleAlbedo(instanceID, uv, 0.0f);
}

float3 SampleAlbedo(uint instanceID, float2 uv, float mipLod)
{
    FInstanceData instData = GetInstanceData(instanceID);
    float3 baseColorFactor = instData.BaseColorFactorAndAlpha.rgb;

    if (instData.BaseColorTextureIndex == 0xFFFFFFFF)
    {
        return baseColorFactor;
    }

    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[instData.BaseColorTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return baseColorFactor * BaseColorTexture.SampleLevel(LinearSampler, uv, mipLod).rgb;
}

float3 SampleAlbedoRayCone(uint instanceID, float2 uv, float3 rayDirection, float3 surfaceNormal, FRayCone cone, float triangleLodConstant = 0.0f)
{
    FInstanceData instData = GetInstanceData(instanceID);
    if (instData.BaseColorTextureIndex == 0xFFFFFFFF)
    {
        return SampleAlbedo(instanceID, uv, 0.0f);
    }

    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[instData.BaseColorTextureIndex];
    const float mipLod = ComputeRayConeTextureLod(BaseColorTexture, rayDirection, surfaceNormal, cone, triangleLodConstant);
    return SampleAlbedo(instanceID, uv, mipLod);
}


float3 SampleEmissive(uint instanceID, float2 uv)
{
    return SampleEmissive(instanceID, uv, 0.0f);
}

float3 SampleEmissive(uint instanceID, float2 uv, float mipLod)
{
    FInstanceData instData = GetInstanceData(instanceID);
    float3 emissiveFactor = max(instData.EmissiveFactor.rgb, 0.0f.xxx);

    if (instData.EmissiveTextureIndex == 0xFFFFFFFF)
    {
        return emissiveFactor;
    }

    Texture2D<float4> EmissiveTexture = ResourceDescriptorHeap[instData.EmissiveTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return emissiveFactor * EmissiveTexture.SampleLevel(LinearSampler, uv, mipLod).rgb;
}

float3 SampleEmissiveRayCone(uint instanceID, float2 uv, float3 rayDirection, float3 surfaceNormal, FRayCone cone, float triangleLodConstant = 0.0f)
{
    FInstanceData instData = GetInstanceData(instanceID);
    if (instData.EmissiveTextureIndex == 0xFFFFFFFF)
    {
        return SampleEmissive(instanceID, uv, 0.0f);
    }

    Texture2D<float4> EmissiveTexture = ResourceDescriptorHeap[instData.EmissiveTextureIndex];
    const float mipLod = ComputeRayConeTextureLod(EmissiveTexture, rayDirection, surfaceNormal, cone, triangleLodConstant);
    return SampleEmissive(instanceID, uv, mipLod);
}

float SampleOpacity(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    return SampleOpacity(instanceID, primitiveIndex, barycentrics, 0.0f);
}

float SampleOpacity(uint instanceID, uint primitiveIndex, float2 barycentrics, float mipLod)
{
    FInstanceData instData = GetInstanceData(instanceID);
    float alpha = instData.BaseColorFactorAndAlpha.a;

    if (instData.BaseColorTextureIndex == 0xFFFFFFFF)
    {
        return alpha;
    }

    float2 uv = GetInterpolatedUV(instanceID, primitiveIndex, barycentrics);
    Texture2D<float4> BaseColorTexture = ResourceDescriptorHeap[instData.BaseColorTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    return alpha * BaseColorTexture.SampleLevel(LinearSampler, uv, mipLod).a;
}

bool AlphaTest(uint instanceID, uint primitiveIndex, float2 barycentrics)
{
    if (AlphaMode == 0u)
    {
        return true;
    }

    if (AlphaMode == 2u)
    {
        return false;
    }

    FInstanceData instData = GetInstanceData(instanceID);
    float alpha = SampleOpacity(instanceID, primitiveIndex, barycentrics);
    return alpha >= instData.MetallicRoughnessAlphaCutoff.z;
}

// Sample metallic and roughness from texture
float2 SampleMetallicRoughness(uint instanceID, float2 uv)
{
    return SampleMetallicRoughness(instanceID, uv, 0.0f);
}

float2 SampleMetallicRoughness(uint instanceID, float2 uv, float mipLod)
{
    FInstanceData instData = GetInstanceData(instanceID);
    float metallic = instData.MetallicRoughnessAlphaCutoff.x;
    float roughness = instData.MetallicRoughnessAlphaCutoff.y;

    if (instData.MetallicRoughnessTextureIndex == 0xFFFFFFFF)
    {
        return float2(saturate(metallic), max(roughness, 0.03f));
    }

    Texture2D<float4> MetallicRoughnessTexture = ResourceDescriptorHeap[instData.MetallicRoughnessTextureIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[LinearClampSamplerIndex];
    float2 mr = MetallicRoughnessTexture.SampleLevel(LinearSampler, uv, mipLod).bg; // Blue=metallic, Green=roughness

    // Clamp to valid ranges
    metallic = saturate(metallic * mr.x);  // [0, 1]
    roughness = max(roughness * mr.y, 0.03f);  // Minimum roughness to avoid numerical issues

    return float2(metallic, roughness);
}

float2 SampleMetallicRoughnessRayCone(uint instanceID, float2 uv, float3 rayDirection, float3 surfaceNormal, FRayCone cone, float triangleLodConstant = 0.0f)
{
    FInstanceData instData = GetInstanceData(instanceID);
    if (instData.MetallicRoughnessTextureIndex == 0xFFFFFFFF)
    {
        return SampleMetallicRoughness(instanceID, uv, 0.0f);
    }

    Texture2D<float4> MetallicRoughnessTexture = ResourceDescriptorHeap[instData.MetallicRoughnessTextureIndex];
    const float mipLod = ComputeRayConeTextureLod(MetallicRoughnessTexture, rayDirection, surfaceNormal, cone, triangleLodConstant);
    return SampleMetallicRoughness(instanceID, uv, mipLod);
}

#endif
