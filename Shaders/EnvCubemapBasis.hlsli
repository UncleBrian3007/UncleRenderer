#ifndef ENV_CUBEMAP_BASIS_HLSLI
#define ENV_CUBEMAP_BASIS_HLSLI

static const float ENV_PI = 3.14159265359f;

static const float3 views[6] =
{
    float3( 1.0f,  0.0f,  0.0f),
    float3(-1.0f,  0.0f,  0.0f),
    float3( 0.0f,  1.0f,  0.0f),
    float3( 0.0f, -1.0f,  0.0f),
    float3( 0.0f,  0.0f,  1.0f),
    float3( 0.0f,  0.0f, -1.0f)
};

static const float3 ups[6] =
{
    float3( 0.0f, -1.0f,  0.0f),
    float3( 0.0f, -1.0f,  0.0f),
    float3( 0.0f,  0.0f,  1.0f),
    float3( 0.0f,  0.0f, -1.0f),
    float3( 0.0f, -1.0f,  0.0f),
    float3( 0.0f, -1.0f,  0.0f)
};

static const float3 rights[6] =
{
    float3( 0.0f,  0.0f, -1.0f),
    float3( 0.0f,  0.0f,  1.0f),
    float3( 1.0f,  0.0f,  0.0f),
    float3( 1.0f,  0.0f,  0.0f),
    float3( 1.0f,  0.0f,  0.0f),
    float3(-1.0f,  0.0f,  0.0f)
};

float3 GetCubeDirection(uint face, float2 uv)
{
    float2 xy = uv * 2.0f - 1.0f;
    return normalize(ups[face] * xy.y + rights[face] * xy.x + views[face]);
}

float2 DirectionToEquirectUV(float3 dir)
{
    float3 V = -dir;
    float u = (atan2(V.x, V.z) / ENV_PI + 1.0f) * 0.5f;
    float v = 1.0f - acos(clamp(V.y, -1.0f, 1.0f)) / ENV_PI;
    return float2(u, v);
}

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float2 Hammersley(uint index, uint count)
{
    return float2((float)index / (float)count, RadicalInverseVdC(index));
}

float3 TangentToWorld(float3 tangentSample, float3 normal)
{
    float3 up = abs(normal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return tangent * tangentSample.x + bitangent * tangentSample.y + normal * tangentSample.z;
}

float3 SampleGGX(float2 randVal, float alpha)
{
    float phi = 2.0f * ENV_PI * randVal.x;
    float cosTheta = sqrt((1.0f - randVal.y) / (1.0f + (alpha * alpha - 1.0f) * randVal.y));
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

#endif
