static const float kRestirPi = 3.14159265f;

void BuildOrthonormalBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = (abs(normal.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

float3 SampleHemisphereCosine(float2 Xi, float3 normal)
{
    const float phi = 6.2831853f * Xi.x;
    const float cosTheta = sqrt(1.0f - Xi.y);
    const float sinTheta = sqrt(Xi.y);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);

    const float3 sample = tangent * (cos(phi) * sinTheta)
        + bitangent * (sin(phi) * sinTheta)
        + normal * cosTheta;
    return normalize(sample);
}
