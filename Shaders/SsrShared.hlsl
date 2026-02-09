#include "SceneConstants.hlsl"

struct RayItem
{
    float3 OriginVS;
    float TMin;
    float3 DirVS;
    float TMax;
    uint2 PixelCoord;
    float Roughness;
    float Padding;
};

struct FHZBSample
{
    float2 TileDepth;
    uint Mip;
};

struct FTraceResult
{
    bool bHit;
    float3 Color;
    float Weight;
};

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

float2 ClampUvToMip(float2 uv, uint mipWidth, uint mipHeight)
{
    float2 uvClamped = saturate(uv);
    float2 coordFloat = min(uvClamped * float2(mipWidth, mipHeight), float2(mipWidth - 1, mipHeight - 1));
    return coordFloat;
}

FHZBSample SampleHZBDevice(float2 uv, float2 prevUv, Texture2D<float2> HZBTexture, uint hzbWidth, uint hzbHeight, uint hzbMipCount, uint hzbAvailable)
{
    FHZBSample Sample = { float2(0.0f, 0.0f), 0u };

#if HZB_ENABLED
    if (hzbAvailable == 0 || hzbWidth == 0 || hzbHeight == 0 || hzbMipCount == 0)
    {
        return Sample;
    }

    float2 uvPixels = abs(uv - prevUv) * float2(hzbWidth, hzbHeight);
    float maxPixels = max(uvPixels.x, uvPixels.y);
    Sample.Mip = (uint)clamp(floor(log2(max(maxPixels, 1.0f))), 0.0f, (float)(hzbMipCount - 1));
    uint mipWidth = max(1u, hzbWidth >> Sample.Mip);
    uint mipHeight = max(1u, hzbHeight >> Sample.Mip);
    uint2 coord = (uint2)ClampUvToMip(uv, mipWidth, mipHeight);
    Sample.TileDepth = HZBTexture.Load(int3(coord, Sample.Mip)).xy;
#endif
    return Sample;
}

FTraceResult TraceSw(
    float3 viewPos,
    float3 rayDir,
    float2 startUv,
    uint maxSteps,
    float stride,
    float maxDistance,
    float thickness,
    Texture2D LinearDepth,
    Texture2D SceneColor,
    SamplerState pointSampler,
    SamplerState linearSampler,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    uint hzbMipCount,
    uint hzbAvailable)
{
    FTraceResult Result;
    Result.bHit = false;
    Result.Color = 0.0f;
    Result.Weight = 0.0f;

    float3 rayPos = viewPos;
    float t = 0.0f;
    float tPrev = 0.0f;
    float2 prevUv = startUv;
    FHZBSample hzbSample = { float2(0.0f, 0.0f), 0u };
    uint nextStepScale = 1u;

    [loop]
    for (uint stepIndex = 0; stepIndex < maxSteps; ++stepIndex)
    {
        const uint stepScale = nextStepScale;
        tPrev = t;
        t += stride * stepScale;
        rayPos = viewPos + rayDir * t;

        if (t > maxDistance)
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

#if HZB_ENABLED
        const bool bUvValid = all(uv >= 0.0f) && all(uv <= 1.0f)
            && all(prevUv >= 0.0f) && all(prevUv <= 1.0f);
        if (bUvValid)
        {
            hzbSample = SampleHZBDevice(uv, prevUv, HZBTexture, hzbWidth, hzbHeight, hzbMipCount, hzbAvailable);
        }

        const uint cappedMip = (hzbMipCount > 1u) ? (uint)min(hzbSample.Mip, hzbMipCount - 2) : 0u;
        const uint stepScaleCandidate = min(1u << cappedMip, 8u);
        const float rayD = clip.z / clip.w;
        const float dFar = hzbSample.TileDepth.x;
        const float dNear = hzbSample.TileDepth.y;
        const float eps = 0;

        if (!bUvValid || hzbAvailable == 0)
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
#else
        nextStepScale = 1u;
#endif

        float sceneViewZ = LinearDepth.SampleLevel(pointSampler, uv, 0).r;
        float depthDelta = rayPos.z - sceneViewZ;

        if (depthDelta >= 0.0f)
        {
#if SSR_REFINE_ENABLED
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

                float midDepth = LinearDepth.SampleLevel(pointSampler, midUv, 0).r;
                float midDelta = midPos.z - midDepth;
                if (midDelta >= 0.0f && midDelta <= thickness)
                {
                    float fade = 1.0f - saturate(tMid / maxDistance);
                    Result.Color = SceneColor.SampleLevel(linearSampler, midUv, 0).rgb;
                    Result.Weight = fade;
                    Result.bHit = true;
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
#else
            if (depthDelta <= thickness)
            {
                float fade = 1.0f - saturate(t / maxDistance);
                Result.Color = SceneColor.SampleLevel(linearSampler, uv, 0).rgb;
                Result.Weight = fade;
                Result.bHit = true;
                break;
            }
#endif
        }

        prevUv = uv;
    }

    return Result;
}
