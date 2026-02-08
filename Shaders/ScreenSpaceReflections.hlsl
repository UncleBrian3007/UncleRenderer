#include "SceneConstants.hlsl"

#ifndef HZB_ENABLED
#define HZB_ENABLED 1
#endif

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

cbuffer SSRConstants : register(b1)
{
    uint2 OutputSize;
    uint MaxSteps;
    float Thickness;
    float MaxDistance;
    float Stride;
    float RoughnessCutoff;
    float Intensity;
    uint UseHistory;
    uint HZBWidth;
    uint HZBHeight;
    uint HZBMipCount;
    uint HZBAvailable;
};

cbuffer SSRBindlessConstants : register(b2)
{
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint LinearDepthIndex;
    uint SceneColorIndex;
    uint HZBIndex;
    uint GBufferPointSamplerIndex;
    uint SceneColorLinearSamplerIndex;
};

VSOutput VSMain(uint VertexId : SV_VertexID)
{
    float2 Positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0f, 1.0f);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

float ReconstructViewZ(float depth)
{
    return Projection._43 / max(depth, 1e-6f);
}

float3 ReconstructViewPosition(float2 uv, float viewZ)
{
    float2 ndc = float2(uv * 2.0f - 1.0f);
    float viewX = ndc.x * viewZ / Projection._11;
    float viewY = -ndc.y * viewZ / Projection._22;
    return float3(viewX, viewY, viewZ);
}

struct FDepthSample
{
    float ViewZ;
    uint Mip;
};

struct FHZBSample
{
    float2 TileDepth;
    uint Mip;
};

float2 ClampUvToMip(float2 uv, uint mipWidth, uint mipHeight)
{
    float2 uvClamped = saturate(uv);
    float2 coordFloat = min(uvClamped * float2(mipWidth, mipHeight), float2(mipWidth - 1, mipHeight - 1));
    return coordFloat;
}

// prevUv is used only for mip selection from uv delta.
FHZBSample SampleHZBDevice(float2 uv, float2 prevUv, Texture2D<float2> HZBTexture)
{
    FHZBSample Sample = { float2(0.0f, 0.0f), 0u };

#if HZB_ENABLED
    if (HZBAvailable == 0 || HZBWidth == 0 || HZBHeight == 0 || HZBMipCount == 0)
    {
        return Sample;
    }

    float2 uvPixels = abs(uv - prevUv) * float2(HZBWidth, HZBHeight);
    float maxPixels = max(uvPixels.x, uvPixels.y);
    Sample.Mip = (uint)clamp(floor(log2(max(maxPixels, 1.0f))), 0.0f, (float)(HZBMipCount - 1));
    uint mipWidth = max(1u, HZBWidth >> Sample.Mip);
    uint mipHeight = max(1u, HZBHeight >> Sample.Mip);
    uint2 coord = (uint2)ClampUvToMip(uv, mipWidth, mipHeight);
    Sample.TileDepth = HZBTexture.Load(int3(coord, Sample.Mip)).xy;
#endif
    return Sample;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    if (UseHistory == 0)
    {
        return 0.0f;
    }

    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D LinearDepth = ResourceDescriptorHeap[LinearDepthIndex];
    Texture2D SceneColor = ResourceDescriptorHeap[SceneColorIndex];
    Texture2D<float2> HZBTexture = ResourceDescriptorHeap[HZBIndex];
    SamplerState GBufferPointSampler = SamplerDescriptorHeap[GBufferPointSamplerIndex];
    SamplerState SceneColorLinearSampler = SamplerDescriptorHeap[SceneColorLinearSamplerIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferPointSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferPointSampler, Input.UV);
    float roughness = smr.z;

    if (roughness > RoughnessCutoff)
    {
        return 0.0f;
    }

    float viewZ = LinearDepth.SampleLevel(GBufferPointSampler, Input.UV, 0).r;
    if (viewZ <= 0.0f)
    {
        return 0.0f;
    }

    float3 viewPos = ReconstructViewPosition(Input.UV, viewZ);
    float3 viewDir = normalize(-viewPos);
    float3 viewNormal = normalize(mul(worldNormal, (float3x3)View));
    float3 rayDir = normalize(reflect(-viewDir, viewNormal));
    float thickness = Thickness * rcp(max(abs(rayDir.z), 1e-3f));

    float3 rayPos = viewPos;
    float t = 0.0f;
    float tPrev = 0.0f;
    float hitWeight = 0.0f;
    float3 hitColor = 0.0f;
    float2 prevUv = Input.UV;
    FHZBSample hzbSample = { float2(0.0f, 0.0f), 0u };
    uint nextStepScale = 1u;

    [loop]
    for (uint stepIndex = 0; stepIndex < MaxSteps; ++stepIndex)
    {
        const uint stepScale = nextStepScale;
        tPrev = t;
        t += Stride * stepScale;
        rayPos = viewPos + rayDir * t;

        if (t > MaxDistance)
        {
            break;
        }

        float4 clip = mul(float4(rayPos, 1.0f), Projection);
        if (clip.w <= 0.0f)
        {
            break;
        }

        float2 uv = clip.xy / clip.w;
        uv = uv * 0.5f + 0.5f;
        uv.y = 1.0f - uv.y;

        if (any(uv < 0.0f) || any(uv > 1.0f))
        {
            break;
        }

        const bool bUvValid = all(uv >= 0.0f) && all(uv <= 1.0f)
            && all(prevUv >= 0.0f) && all(prevUv <= 1.0f);
        if (bUvValid)
        {
            hzbSample = SampleHZBDevice(uv, prevUv, HZBTexture);
        }

        const uint cappedMip = (HZBMipCount > 1u) ? (uint)min(hzbSample.Mip, HZBMipCount - 2) : 0u;
        const uint stepScaleCandidate = min(1u << cappedMip, 8u);
        const float rayD = clip.z / clip.w;
        const float dFar = hzbSample.TileDepth.x;
        const float dNear = hzbSample.TileDepth.y;
        const float eps = 0;

        if (!bUvValid || HZBAvailable == 0)
        {
            nextStepScale = 1u;
        }
        else if (rayD > dNear + eps)
        {
            nextStepScale = stepScaleCandidate;
        }
        else if (rayD < dFar - eps)
        {
            nextStepScale = 1u;
        }
        else
        {
            nextStepScale = min(stepScaleCandidate, 2u);
        }

        float sceneViewZ = LinearDepth.SampleLevel(GBufferPointSampler, uv, 0).r;
        float depthDelta = rayPos.z - sceneViewZ;

        if (depthDelta >= 0.0f)
        {
            float tRefineStart = tPrev;
            float tRefineEnd = t;
            bool bRefinedHit = false;
            [unroll]
            for (uint refineStep = 0; refineStep < 3; ++refineStep)
            {
                float tMid = lerp(tRefineStart, tRefineEnd, 0.5f);
                float3 midPos = viewPos + rayDir * tMid;
                float4 midClip = mul(float4(midPos, 1.0f), Projection);
                if (midClip.w <= 0.0f)
                {
                    break;
                }

                float2 midUv = midClip.xy / midClip.w;
                midUv = midUv * 0.5f + 0.5f;
                midUv.y = 1.0f - midUv.y;
                if (any(midUv < 0.0f) || any(midUv > 1.0f))
                {
                    break;
                }

                float midDepth = LinearDepth.SampleLevel(GBufferPointSampler, midUv, 0).r;
                float midDelta = midPos.z - midDepth;
                if (midDelta >= 0.0f && midDelta <= thickness)
                {
                    float fade = 1.0f - saturate(tMid / MaxDistance);
                    hitColor = SceneColor.SampleLevel(SceneColorLinearSampler, midUv, 0).rgb;
                    hitWeight = fade;
                    bRefinedHit = true;
                    break;
                }

                if (midDelta > 0.0f)
                {
                    tRefineEnd = tMid;
                }
                else
                {
                    tRefineStart = tMid;
                }
            }

            if (bRefinedHit)
            {
                break;
            }
        }

        prevUv = uv;
    }

    return float4(hitColor * Intensity, hitWeight);
}
