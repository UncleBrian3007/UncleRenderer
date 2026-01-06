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

float FastSqrt(float x)
{
    return asfloat(0x1fbd1df5 + (asint(x) >> 1));
}

float FastACos(float inX)
{
    const float pi = 3.141593f;
    const float halfPi = 1.570796f;
    float x = abs(inX);
    float res = -0.156583f * x + halfPi;
    res *= FastSqrt(1.0f - x);
    return (inX >= 0.0f) ? res : pi - res;
}

float ComputeGtao(float2 uv, float3 viewPos, float3 viewNormal)
{
    if (GtaoIntensity <= 0.0f || GtaoRadius <= 0.0f)
    {
        return 1.0f;
    }

    uint viewportWidth;
    uint viewportHeight;
    LinearDepthTexture.GetDimensions(viewportWidth, viewportHeight);
    float2 viewportSize = float2((float)viewportWidth, (float)viewportHeight);
    float2 pixelSize = rcp(max(viewportSize, 1.0f.xx));

    float3 viewVec = normalize(-viewPos);

#if GTAO_USE_JITTER
    float2 jitter = TaaJitter * 0.5f + 0.5f;
#else
    float2 jitter = float2(0.5f, 0.5f);
#endif

    const float pi = 3.14159265f;
    const float halfPi = 1.57079633f;

    uint directionCount = max(GtaoDirectionCount, 1u);
    uint stepCount = max(GtaoStepCount, 1u);
    float invDirections = 1.0f / (float)directionCount;
    float invSteps = 1.0f / (float)stepCount;

    float effectRadius = GtaoRadius;
    float falloffRange = effectRadius;
    float falloffFrom = effectRadius * 0.5f;
    float falloffMul = -1.0f / max(falloffRange, 1e-4f);
    float falloffAdd = falloffFrom / max(falloffRange, 1e-4f) + 1.0f;

    float2 pixelDirRBViewspaceSizeAtCenterZ = float2(
        (2.0f * pixelSize.x * viewPos.z) / Projection._11,
        (2.0f * pixelSize.y * viewPos.z) / Projection._22);

    float screenspaceRadius = effectRadius / max(abs(pixelDirRBViewspaceSizeAtCenterZ.x), 1e-4f);
    if (screenspaceRadius < 0.5f)
    {
        return 1.0f;
    }

    float visibility = 0.0f;
    float thinOccluderCompensation = saturate(GtaoThickness);
    float noiseSlice = jitter.x;
    float noiseSample = jitter.y;

    [loop]
    for (uint dirIndex = 0; dirIndex < directionCount; ++dirIndex)
    {
        float sliceK = (dirIndex + noiseSlice) * invDirections;
        float phi = sliceK * pi;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        float2 omega = float2(cosPhi, -sinPhi) * screenspaceRadius;

        float3 directionVec = float3(cosPhi, sinPhi, 0.0f);
        float3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec);
        float3 axisVec = normalize(cross(orthoDirectionVec, viewVec));
        float3 projectedNormalVec = viewNormal - axisVec * dot(viewNormal, axisVec);
        float projectedNormalVecLength = max(length(projectedNormalVec), 1e-4f);
        float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));
        float cosNorm = saturate(dot(projectedNormalVec, viewVec) / projectedNormalVecLength);
        float n = signNorm * FastACos(cosNorm);
        float lowHorizonCos0 = cos(n + halfPi);
        float lowHorizonCos1 = cos(n - halfPi);
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;

        [loop]
        for (uint stepIndex = 0; stepIndex < stepCount; ++stepIndex)
        {
            float stepBaseNoise = ((float)dirIndex + (float)stepIndex * (float)stepCount) * 0.6180339887f;
            float stepNoise = frac(noiseSample + stepBaseNoise);
            float s = (stepIndex + stepNoise) * invSteps;
            s = pow(s, 0.9f);

            float2 sampleOffset = round(s * omega) * pixelSize;

            float2 sampleUv0 = uv + sampleOffset;
            float2 sampleUv1 = uv - sampleOffset;
            if (any(sampleUv0 < 0.0f) || any(sampleUv0 > 1.0f) || any(sampleUv1 < 0.0f) || any(sampleUv1 > 1.0f))
            {
                continue;
            }

            float sampleViewZ0 = LinearDepthTexture.SampleLevel(GBufferSampler, sampleUv0, 0).r;
            float sampleViewZ1 = LinearDepthTexture.SampleLevel(GBufferSampler, sampleUv1, 0).r;
            float3 samplePos0 = ReconstructViewPosition(sampleUv0, sampleViewZ0);
            float3 samplePos1 = ReconstructViewPosition(sampleUv1, sampleViewZ1);

            float3 sampleDelta0 = samplePos0 - viewPos;
            float3 sampleDelta1 = samplePos1 - viewPos;
            float sampleDist0 = max(length(sampleDelta0), 1e-4f);
            float sampleDist1 = max(length(sampleDelta1), 1e-4f);

            float3 sampleHorizonVec0 = sampleDelta0 / sampleDist0;
            float3 sampleHorizonVec1 = sampleDelta1 / sampleDist1;

            float falloffBase0 = length(float3(sampleDelta0.x, sampleDelta0.y, sampleDelta0.z * (1.0f + thinOccluderCompensation)));
            float falloffBase1 = length(float3(sampleDelta1.x, sampleDelta1.y, sampleDelta1.z * (1.0f + thinOccluderCompensation)));
            float weight0 = saturate(falloffBase0 * falloffMul + falloffAdd);
            float weight1 = saturate(falloffBase1 * falloffMul + falloffAdd);

            float shc0 = dot(sampleHorizonVec0, viewVec);
            float shc1 = dot(sampleHorizonVec1, viewVec);
            shc0 = lerp(lowHorizonCos0, shc0, weight0);
            shc1 = lerp(lowHorizonCos1, shc1, weight1);

            horizonCos0 = max(horizonCos0, shc0);
            horizonCos1 = max(horizonCos1, shc1);
        }

        float h0 = -FastACos(horizonCos1);
        float h1 = FastACos(horizonCos0);
        float iarc0 = (cosNorm + 2.0f * h0 * sin(n) - cos(2.0f * h0 - n)) * 0.25f;
        float iarc1 = (cosNorm + 2.0f * h1 * sin(n) - cos(2.0f * h1 - n)) * 0.25f;
        float localVisibility = projectedNormalVecLength * (iarc0 + iarc1);
        visibility += localVisibility;
    }

    visibility /= max((float)directionCount, 1.0f);
    visibility = max(visibility, 0.03f);
    float ao = saturate(1.0f - (1.0f - visibility) * GtaoIntensity);
    ao = pow(ao, max(GtaoPower, 1e-3f));
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
