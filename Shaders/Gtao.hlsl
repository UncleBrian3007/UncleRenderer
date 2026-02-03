#include "SceneConstants.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

cbuffer GtaoBindlessConstants : register(b1)
{
    uint GBufferAIndex;
    uint LinearDepthIndex;
    uint HilbertLutIndex;
};

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


// Based on https://www.shadertoy.com/view/3tB3z3 (R2 sequence)
#define XE_HILBERT_LEVEL 6u
#define XE_HILBERT_WIDTH (1u << XE_HILBERT_LEVEL)

float2 SpatioTemporalNoise(Texture2D<uint> HilbertLut, uint2 pixCoord, uint temporalIndex)
{
    uint2 wrappedCoord = pixCoord & (XE_HILBERT_WIDTH - 1u);
    uint index = HilbertLut.Load(uint3(wrappedCoord, 0)).x;
    index += 288u * (temporalIndex & (XE_HILBERT_WIDTH - 1u));
    return frac(0.5f + index * float2(0.75487766624669276005f, 0.56984029099805326591f));
}

// Based on https://github.com/GameTechDev/XeGTAO
float ComputeGtao(Texture2D<float> LinearDepthTexture, Texture2D<uint> HilbertLut, float2 uv, uint2 pixCoord, float3 viewPos, float3 viewNormal)
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
    float2 jitter = SpatioTemporalNoise(HilbertLut, pixCoord, GtaoTemporalIndex);
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
        float2 omega = float2(cosPhi, -sinPhi) * screenspaceRadius; // 현재 direction 에서의 최대 Screen-space 반경

		float3 directionVec = float3(cosPhi, sinPhi, 0.0f); // Screen-space 기준 AO 샘플 방향 
        float3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec); // 샘플 방향을 시선에 수직한 평면으로 투영, 화면에서의 실제 샘플 진행 방향
        float3 axisVec = normalize(cross(orthoDirectionVec, viewVec)); // 현재 AO direction 슬라이스의 회전 축
        float3 projectedNormalVec = viewNormal - axisVec * dot(viewNormal, axisVec); // 법선을 슬라이스 평면으로 투영 
        float projectedNormalVecLength = max(length(projectedNormalVec), 1e-4f);
        float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec)); // 슬라이스에 투영된 법선이 어느쪽인지 판단 (왼/오른쪽)
		float cosNorm = saturate(dot(projectedNormalVec, viewVec) / projectedNormalVecLength); // 슬라이스에 투영된 법선과 시선의 코사인, Horizon 에 누우면 0, 수직이면 1
        float n = signNorm * FastACos(cosNorm); // Horizon 에 누우면 Acos(0) 이라서 1.57 혹은 -1.57(반대편), 수직이면 0
        float lowHorizonCos0 = cos(n + halfPi); // 아무 Horizon 이 없을 때의 코사인 값
        float lowHorizonCos1 = cos(n - halfPi); // 법선만으로 결졍된 최소 가려짐 각도
        float horizonCos0 = lowHorizonCos0;
        float horizonCos1 = lowHorizonCos1;

        // 기본 horizon(법선 기반) 에 실제 Geometry 샘플들이 얼마나 하늘을 가리는지를 누적
        [loop]
        for (uint stepIndex = 0; stepIndex < stepCount; ++stepIndex)
        {
            float stepBaseNoise = ((float)dirIndex + (float)stepIndex * (float)stepCount) * 0.6180339887f;
            float stepNoise = frac(noiseSample + stepBaseNoise);
            float s = (stepIndex + stepNoise) * invSteps;
			s = pow(s, 2.0f); // 근거리 샘플 밀도 증가, AO 는 가까운 Occluder 가 더 중요, Importance Sampling

			float2 sampleOffset = round(s * omega) * pixelSize; // (s * 최대 반경), round는 픽셀 단위 샘플링을 위해, subpixel jitter로 인한 temporal instability 방지 

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

            float3 sampleHorizonVec0 = sampleDelta0 / sampleDist0; // 현재 샘플이 어느 각도로 하늘을 가리는지를 나타내는 정규화된 벡터
            float3 sampleHorizonVec1 = sampleDelta1 / sampleDist1;

            // 거리 기반 falloff 적용, 멀리 있는 occluder 는 덜 중요, 또한 얇은 오클루더 보정도 적용
            float falloffBase0 = length(float3(sampleDelta0.x, sampleDelta0.y, sampleDelta0.z * (1.0f + thinOccluderCompensation)));
            float falloffBase1 = length(float3(sampleDelta1.x, sampleDelta1.y, sampleDelta1.z * (1.0f + thinOccluderCompensation)));
            float weight0 = saturate(falloffBase0 * falloffMul + falloffAdd);
            float weight1 = saturate(falloffBase1 * falloffMul + falloffAdd);

            float shc0 = dot(sampleHorizonVec0, viewVec); // 샘플이 얼마나 view 반구를 가리는지
            float shc1 = dot(sampleHorizonVec1, viewVec);
            shc0 = lerp(lowHorizonCos0, shc0, weight0); // 가까우면 샘플을 신뢰, 멀면 법선 기반 최소 가림값으로 보정
            shc1 = lerp(lowHorizonCos1, shc1, weight1);

			horizonCos0 = max(horizonCos0, shc0); // 가장 가리는 Occluder 가 지배
            horizonCos1 = max(horizonCos1, shc1);
        }

		float h0 = -FastACos(horizonCos1); // Horizon 각도 계산, 적분은 각도 공간에서 수행해야함
		float h1 = FastACos(horizonCos0); // h0 에 음수를 붙인 이유는 좌측 각도 범위를 [-Pi/2, 0], 우측 각도 범위를 [0, Pi/2] 로 나누기 위해서
        float iarc0 = (cosNorm + 2.0f * h0 * sin(n) - cos(2.0f * h0 - n)) * 0.25f; // ∫ cos(θ - n) dθ, θ는 시선과 법선이 이루는 각도
        float iarc1 = (cosNorm + 2.0f * h1 * sin(n) - cos(2.0f * h1 - n)) * 0.25f; // cosNorm : 법선 방향 기본 기여, 2*h*sin(n) : 법선 기울기에 따른 가시 반구 면적, cos(2*h - n) : horizon에 의해 잘린 영역 보정, 0.25 : 반구 정규화 상수
        float localVisibility = projectedNormalVecLength * (iarc0 + iarc1); // 법선이 slice에 잘 드러나 있으면 → 기여 ↑, 법선이 slice와 거의 수직이면 → 기여 ↓
        visibility += localVisibility;
    }

    visibility /= max((float)directionCount, 1.0f);
	visibility = max(visibility, 0.03f); // 최소한의 환경광 보장
    float ao = saturate(1.0f - (1.0f - visibility) * GtaoIntensity);
    ao = pow(ao, max(GtaoPower, 1e-3f));
    return ao;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D<float> LinearDepthTexture = ResourceDescriptorHeap[LinearDepthIndex];
    Texture2D<uint> HilbertLut = ResourceDescriptorHeap[HilbertLutIndex];
    float4 normalEncoded = GBufferA.Sample(GBufferSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float3 viewNormal = normalize(mul(worldNormal, (float3x3)View));
    float viewZ = LinearDepthTexture.Sample(GBufferSampler, Input.UV).r;
    float3 viewPos = ReconstructViewPosition(Input.UV, viewZ);
    uint2 pixCoord = uint2(Input.Position.xy);
    float ao = ComputeGtao(LinearDepthTexture, HilbertLut, Input.UV, pixCoord, viewPos, viewNormal);
    return float4(ao, ao, ao, 1.0f);
}
