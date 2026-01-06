#include "SceneConstants.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

Texture2D GBufferA : register(t0);
Texture2D LinearDepthTexture : register(t1);
SamplerState GBufferSampler : register(s0);

VSOutput VSMain(uint VertexId : SV_VertexID)
{
    float2 Positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0, 1.0);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

float3 ReconstructViewPosition(float2 uv, float viewZ)
{
    float2 ndc = float2(uv * 2.0f - 1.0f);
    float viewX = ndc.x * viewZ / Projection._11;
    float viewY = -ndc.y * viewZ / Projection._22;
    return float3(viewX, viewY, viewZ);
}

float ComputeGtao(float2 uv, float3 viewPos, float3 viewNormal)
{
    if (GtaoIntensity <= 0.0f || GtaoRadius <= 0.0f)
    {
        return 1.0f;
    }

    float3 up = abs(viewNormal.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(up, viewNormal));
    float3 bitangent = cross(viewNormal, tangent);

#if GTAO_USE_JITTER
    float2 jitter = TaaJitter * 0.5f + 0.5f;
#else
    float2 jitter = float2(0.5f, 0.5f);
#endif
    float baseAngle = jitter.x * 6.2831853f;

    uint directionCount = max(GtaoDirectionCount, 1u);
    uint stepCount = max(GtaoStepCount, 1u);
    float invDirections = 1.0f / directionCount;
    float invSteps = 1.0f / stepCount;

    float occlusion = 0.0f;

    [loop]
    for (uint dirIndex = 0; dirIndex < directionCount; ++dirIndex)
    {
        float angle = baseAngle + (dirIndex + jitter.y) * 6.2831853f * invDirections;
        float2 dir = float2(cos(angle), sin(angle));
        float3 sampleDir = dir.x * tangent + dir.y * bitangent;

        [loop]
        for (uint stepIndex = 1; stepIndex <= stepCount; ++stepIndex)
        {
            float stepScale = stepIndex * invSteps;
            float radius = GtaoRadius * stepScale;
            float3 samplePos = viewPos + sampleDir * radius;

            float4 clip = mul(float4(samplePos, 1.0f), Projection);
            float2 sampleUv = clip.xy / clip.w * 0.5f + 0.5f;
            sampleUv.y = 1.0f - sampleUv.y;
            if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
            {
                continue;
            }

            float sampleViewZ = LinearDepthTexture.SampleLevel(GBufferSampler, sampleUv, 0).r;
            float dz = samplePos.z - sampleViewZ;
            float weight = saturate(1.0f - stepScale);
            occlusion += (dz > GtaoThickness) ? weight : 0.0f;
        }
    }

    float sampleCount = directionCount * stepCount;
    float ao = 1.0f - (occlusion / max(sampleCount, 1.0f)) * GtaoIntensity;
    ao = pow(saturate(ao), GtaoPower);
    return ao;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float4 normalEncoded = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 normal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float viewZ = LinearDepthTexture.Sample(GBufferSampler, Input.UV).r;
    float3 viewPos = ReconstructViewPosition(Input.UV, viewZ);
    float ao = ComputeGtao(Input.UV, viewPos, normal);
    return float4(ao, ao, ao, 1.0f);
}
