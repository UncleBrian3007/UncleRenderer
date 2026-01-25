#include "SceneConstants.hlsl"

RaytracingAccelerationStructure Scene : register(t0);
cbuffer RayTracingBindlessConstants : register(b1)
{
    uint DepthTextureIndex;
    uint GBufferAIndex;
    uint GBufferCIndex;
    uint OutputTextureIndex;
    uint DispatchWidth;
    uint DispatchHeight;
    uint FrameIndex;
};

float3 ReconstructWorldPosition(uint2 pixel, float depth, uint2 dispatchDim)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(dispatchDim);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 clip = float4(ndc, depth, 1.0f);
    float4 worldPosition = mul(clip, ViewProjectionInverse);
    worldPosition.xyz /= worldPosition.w;
    return worldPosition.xyz;
}

uint Hash(uint value)
{
    value ^= value >> 17;
    value *= 0xed5ad4bb;
    value ^= value >> 11;
    value *= 0xac4c1b51;
    value ^= value >> 15;
    value *= 0x31848bab;
    value ^= value >> 14;
    return value;
}

float Random01(uint2 pixel, uint salt)
{
    uint seed = Hash(pixel.x + 0x9e3779b9u);
    seed = Hash(seed + pixel.y);
    seed = Hash(seed + salt * 1664525u);
    return (seed & 0x00ffffffu) / 16777216.0f;
}

float3 HashToColor(uint value)
{
    uint h = Hash(value);
    float r = ((h >> 0) & 0xff) / 255.0f;
    float g = ((h >> 8) & 0xff) / 255.0f;
    float b = ((h >> 16) & 0xff) / 255.0f;
    return float3(r, g, b);
}

float3 SampleHemisphereCosine(float2 rand, float3 normal)
{
    float phi = 6.2831853f * rand.x;
    float cosTheta = sqrt(1.0f - rand.y);
    float sinTheta = sqrt(rand.y);

    float3 tangent = normalize(abs(normal.z) < 0.999f ? cross(float3(0.0f, 0.0f, 1.0f), normal) : cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);

    float3 sample = tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta) + normal * cosTheta;
    return normalize(sample);
}

float3 EvaluateSky(float3 direction)
{
    float t = saturate(direction.y * 0.5f + 0.5f);
    return lerp(float3(0.5f, 0.6f, 0.8f), float3(0.1f, 0.2f, 0.4f), 1.0f - t);
}

static const uint RayQueryThreadGroupSize = 8;
static const uint PathRayFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
    | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
    | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES;

[numthreads(RayQueryThreadGroupSize, RayQueryThreadGroupSize, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (DispatchThreadId.x >= DispatchWidth || DispatchThreadId.y >= DispatchHeight)
    {
        return;
    }

    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthTextureIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float4> GBufferC = ResourceDescriptorHeap[GBufferCIndex];
    RWTexture2D<float4> PathOutput = ResourceDescriptorHeap[OutputTextureIndex];

    const uint2 DispatchIndex = DispatchThreadId.xy;
    const uint2 DispatchDim = uint2(DispatchWidth, DispatchHeight);
    const float Depth = DepthTexture.Load(int3(DispatchIndex, 0));
    if (Depth >= 1.0f)
    {
        float3 worldPos = ReconstructWorldPosition(DispatchIndex, 1.0f, DispatchDim);
        float3 worldDir = normalize(worldPos - CameraPosition);
        PathOutput[DispatchIndex] = float4(EvaluateSky(worldDir), 1.0f);
        return;
    }

    float3 worldPosition = ReconstructWorldPosition(DispatchIndex, Depth, DispatchDim);
    float4 normalEncoded = GBufferA.Load(int3(DispatchIndex, 0));
    float3 normalView = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float3 worldNormal = normalize(mul(normalView, (float3x3)ViewInverse));
    if (any(isnan(worldNormal)) || all(worldNormal == 0.0f))
    {
        worldNormal = float3(0.0f, 1.0f, 0.0f);
    }

    float3 albedo = GBufferC.Load(int3(DispatchIndex, 0)).rgb;

    float2 rand = float2(Random01(DispatchIndex, FrameIndex * 2u + 1u), Random01(DispatchIndex, FrameIndex * 2u + 2u));
    float3 bounceDir = SampleHemisphereCosine(rand, worldNormal);

    RayDesc Ray;
    Ray.Origin = worldPosition + worldNormal * 0.01f;
    Ray.Direction = bounceDir;
    Ray.TMin = 0.001f;
    Ray.TMax = 1.0f;

    RayQuery<PathRayFlags> RayQuery;
    RayQuery.TraceRayInline(Scene, PathRayFlags, 0xFF, Ray);
    while (RayQuery.Proceed())
    {
    }

	float visibility = RayQuery.CommittedStatus() == COMMITTED_NOTHING ? 3.0 : 0.0;

	float3 radiance = float3(visibility, visibility, visibility);

    PathOutput[DispatchIndex] = float4(radiance, 1.0f);
}
