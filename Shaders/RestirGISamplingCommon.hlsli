#include "Common.hlsli"

static const float kRestirPi = 3.14159265f;
static const float kRestirTwoPi = 6.2831853f;

void BuildOrthonormalBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    bitangent = normalize(GetPerpendicularVector(normal));
    tangent = cross(bitangent, normal);
}

// PDF = cos / PI = NdotL / PI
float3 SampleHemisphereCosine(float2 Xi, float3 normal)
{
    const float phi = kRestirTwoPi * Xi.y;
    const float cosTheta = sqrt(1.0 - Xi.x);
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

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
    const float phi = kRestirTwoPi * Xi.y;
    const float cosTheta = 1.0 - Xi.x;
    const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);

    const float3 sample = tangent * (cos(phi) * sinTheta)
        + bitangent * (sin(phi) * sinTheta)
        + normal * cosTheta;
    return normalize(sample);
}
