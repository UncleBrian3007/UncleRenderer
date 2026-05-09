#ifndef SHADER_COMMON_HLSLI
#define SHADER_COMMON_HLSLI

// "Efficient Construction of Perpendicular Vectors Without Branching"
float3 GetPerpendicularVector(float3 u)
{
    float3 a = abs(u);
    uint xm = ((a.x - a.y) < 0 && (a.x - a.z) < 0) ? 1 : 0;
    uint ym = (a.y - a.z) < 0 ? (1 ^ xm) : 0;
    uint zm = 1 ^ (xm | ym);
    return cross(u, float3(xm, ym, zm));
}

float2 ApplyTextureTransform(float2 uv, float4 offsetScale, float2 rotation)
{
    float2 scaled = uv * offsetScale.zw;
    float2 rotated = float2(
        scaled.x * rotation.x - scaled.y * rotation.y,
        scaled.x * rotation.y + scaled.y * rotation.x);
    return rotated + offsetScale.xy;
}

float ReconstructViewZ(float depth, row_major float4x4 projection)
{
    return projection._43 / max(depth, 1e-6f);
}

float ResolveLinearDepthFromDeviceDepth(float DeviceDepth, row_major float4x4 projection)
{
    return projection._43 / max(DeviceDepth, 1e-6f);
}

float ResolveDeviceDepthFromLinearDepth(float LinearDepth, row_major float4x4 projection)
{
    return projection._43 / max(LinearDepth, 1e-6f);
}

uint Hash32(uint value)
{
    value ^= value >> 17;
    value *= 0xed5ad4bbu;
    value ^= value >> 11;
    value *= 0xac4c1b51u;
    value ^= value >> 15;
    value *= 0x31848babu;
    value ^= value >> 14;
    return value;
}

float3 ReconstructViewPosition(float2 uv, float viewZ, row_major float4x4 projection)
{
    float2 ndc = float2(uv * 2.0f - 1.0f);
    float viewX = ndc.x * viewZ / projection._11;
    float viewY = -ndc.y * viewZ / projection._22;
    return float3(viewX, viewY, viewZ);
}

float3 ReconstructViewPositionFromDepth(float2 uv, float depth, row_major float4x4 projection)
{
    float viewZ = ReconstructViewZ(depth, projection);
    return ReconstructViewPosition(uv, viewZ, projection);
}

float3 ReconstructWorldPosition(float2 uv, float depth, row_major float4x4 viewProjectionInverse)
{
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clip = float4(ndc, depth, 1.0f);
    float4 worldPosition = mul(clip, viewProjectionInverse);
    worldPosition.xyz /= max(worldPosition.w, 1e-6f);
    return worldPosition.xyz;
}

float3 ReconstructWorldPosition(uint2 pixel, float depth, uint2 renderSize, row_major float4x4 viewProjectionInverse)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(renderSize);
    return ReconstructWorldPosition(uv, depth, viewProjectionInverse);
}

float3 SampleGGX(float2 randVal, float alpha)
{
    float phi = 6.28318530718f * randVal.x;
    float cosTheta = sqrt((1.0f - randVal.y) / (1.0f + (alpha * alpha - 1.0f) * randVal.y));
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 TangentToWorld(float3 v, float3 N)
{
    const float3 Nn = normalize(N);
    const float3 B = normalize(GetPerpendicularVector(Nn));
    const float3 T = normalize(cross(B, Nn));
    return T * v.x + B * v.y + Nn * v.z;
}

float3 WorldToTangent(float3 w, float3 N)
{
    const float3 Nn = normalize(N);
    const float3 B = normalize(GetPerpendicularVector(Nn));
    const float3 T = normalize(cross(B, Nn));
    return float3(dot(w, T), dot(w, B), dot(w, Nn));
}

float4 UnpackColor(uint packedColor)
{
    const float r = (packedColor & 0xFFu) / 255.0f;
    const float g = ((packedColor >> 8) & 0xFFu) / 255.0f;
    const float b = ((packedColor >> 16) & 0xFFu) / 255.0f;
    const float a = ((packedColor >> 24) & 0xFFu) / 255.0f;
    return float4(r, g, b, a);
}

float3 DecodeTangentNormalRG(float2 tangentNormalRG)
{
    float tangentNormalZ = sqrt(saturate(1.0f - dot(tangentNormalRG, tangentNormalRG)));
    return float3(tangentNormalRG, tangentNormalZ);
}

float3 ComputeWorldNormal(float3 vertexNormal, float4 tangentFrame, float3 tangentNormal)
{
    float3 normal = normalize(vertexNormal);
    float3 tangent = normalize(tangentFrame.xyz - normal * dot(normal, tangentFrame.xyz));
    float3 bitangent = normalize(cross(normal, tangent)) * tangentFrame.w;

    const float tangentEpsilon = 1e-5f;
    float tangentNormalLength = length(tangentNormal);
    float3 safeTangentNormal = tangentNormalLength < tangentEpsilon ? float3(0.0f, 0.0f, 1.0f) : tangentNormal;

    float3x3 TBN = float3x3(tangent, bitangent, normal);
    return normalize(mul(safeTangentNormal, TBN));
}

bool BadFloat4(float4 v)
{
    return any(isnan(v)) || any(isinf(v));
}

bool BadFloat3(float3 Value)
{
    return any(isnan(Value)) || any(isinf(Value));
}

float Random01(uint2 pixel, uint salt)
{
    uint seed = Hash32(pixel.x + 0x9e3779b9u);
    seed = Hash32(seed + pixel.y);
    seed = Hash32(seed + salt * 1664525u);
    return (seed & 0x00ffffffu) / 16777216.0f;
}

void BuildOrthonormalBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    bitangent = normalize(GetPerpendicularVector(normal));
    tangent = cross(bitangent, normal);
}

// PDF = cos / PI = NdotL / PI
float3 SampleHemisphereCosine(float2 Xi, float3 normal)
{
    const float phi = 6.2831853f * Xi.y;
    const float cosTheta = sqrt(1.0f - Xi.x);
    const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);

    const float3 sample = tangent * (cos(phi) * sinTheta)
        + bitangent * (sin(phi) * sinTheta)
        + normal * cosTheta;
    return normalize(sample);
}

// PDF = 1 / (2 * PI)
float3 SampleHemisphereUniform(float2 Xi, float3 normal)
{
    const float phi = 6.2831853f * Xi.y;
    const float cosTheta = 1.0f - Xi.x;
    const float sinTheta = sqrt(1.0f - cosTheta * cosTheta);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);

    const float3 sample = tangent * (cos(phi) * sinTheta)
        + bitangent * (sin(phi) * sinTheta)
        + normal * cosTheta;
    return normalize(sample);
}

#endif
