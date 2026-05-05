#include "../SceneConstants.hlsl"
#include "../Common.hlsli"

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

struct FTraceResult
{
    bool bHit;
    float3 Color;
    float Weight;
};

struct FTraceHitResult
{
    bool bHit;
    float2 HitUv;
    float HitT;
    float HitViewZ;
    float Weight;
};

struct FHZBTraceTileState
{
    float TileExitAlpha;
    float TileClosestDepth;
    float ResumeAlpha;
    bool bCandidateTile;
};

float2 ClampUvToMip(float2 uv, uint mipWidth, uint mipHeight)
{
    float2 uvClamped = saturate(uv);
    float2 coordFloat = min(uvClamped * float2(mipWidth, mipHeight), float2(mipWidth - 1, mipHeight - 1));
    return coordFloat;
}

float3 ProjectViewPositionToScreenUVZ(float3 viewPos)
{
    const float4 clip = mul(float4(viewPos, 1.0f), Projection);
    const float invW = rcp(max(clip.w, 1e-6f));
    float2 uv = clip.xy * invW;
    uv = uv * float2(0.5f, -0.5f) + 0.5f.xx;
    return float3(uv, clip.z * invW);
}

float ComputeTraceRayEndDistance(float3 viewPos, float3 rayDir, float maxDistance)
{
    if (rayDir.z < 0.0f)
    {
        const float zPlaneDistance = -0.99f * viewPos.z / min(rayDir.z, -1e-6f);
        return min(zPlaneDistance, maxDistance);
    }

    return maxDistance;
}

float2 LineBoxIntersect(float2 rayOrigin, float2 rayEnd, float2 boxMin, float2 boxMax)
{
    const float2 invRayDir = rcp(rayEnd - rayOrigin);
    const float2 firstPlaneIntersections = (boxMin - rayOrigin) * invRayDir;
    const float2 secondPlaneIntersections = (boxMax - rayOrigin) * invRayDir;
    const float2 closestPlaneIntersections = min(firstPlaneIntersections, secondPlaneIntersections);
    const float2 furthestPlaneIntersections = max(firstPlaneIntersections, secondPlaneIntersections);

    float2 boxIntersections;
    boxIntersections.x = max(closestPlaneIntersections.x, closestPlaneIntersections.y);
    boxIntersections.y = min(furthestPlaneIntersections.x, furthestPlaneIntersections.y);
    return saturate(boxIntersections);
}

float SampleClosestHZBDevice(float2 uv, Texture2D<float2> HZBTexture, uint hzbWidth, uint hzbHeight, uint mipLevel)
{
    const uint mipWidth = max(1u, hzbWidth >> mipLevel);
    const uint mipHeight = max(1u, hzbHeight >> mipLevel);
    const uint2 coord = (uint2)ClampUvToMip(uv, mipWidth, mipHeight);
    return HZBTexture.Load(int3(coord, mipLevel)).y;
}

uint EstimateHZBSegmentMip(float2 uv0, float2 uv1, uint hzbWidth, uint hzbHeight, uint hzbMipCount)
{
    const float2 segmentPixels = abs(uv1 - uv0) * float2(hzbWidth, hzbHeight);
    const float maxSpan = max(max(segmentPixels.x, segmentPixels.y), 1.0f);
    return (uint)clamp(floor(log2(maxSpan)), 0.0f, (float)(hzbMipCount - 1u));
}

uint2 ComputeHZBTileCoord(
    float2 uv,
    float2 rayDirectionScreenUV,
    uint mipWidth,
    uint mipHeight)
{
    const float2 resolution = float2(mipWidth, mipHeight);
    const float kTileBoundaryBias = 1e-4f;
    const float2 tileBias = float2(
        rayDirectionScreenUV.x < 0.0f ? -kTileBoundaryBias : 0.0f,
        rayDirectionScreenUV.y < 0.0f ? -kTileBoundaryBias : 0.0f);
    float2 coordFloat = saturate(uv) * resolution + tileBias;
    coordFloat = clamp(coordFloat, 0.0f.xx, resolution - 1.0f.xx);
    return (uint2)coordFloat;
}

float LoadHZBTileClosestDepth(
    uint2 tileCoord,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    uint mipLevel)
{
    const uint mipWidth = max(1u, hzbWidth >> mipLevel);
    const uint mipHeight = max(1u, hzbHeight >> mipLevel);
    const uint2 clampedCoord = min(tileCoord, uint2(mipWidth - 1u, mipHeight - 1u));
    return HZBTexture.Load(int3(clampedCoord, mipLevel)).y;
}

float ComputeHZBTileExitAlpha(
    float2 uv,
    float2 rayDirectionScreenUV,
    uint2 tileCoord,
    float2 texelSize,
    float currentAlpha,
    float alphaLimit,
    float alphaEpsilon)
{
    float alphaX = 1e30f;
    if (rayDirectionScreenUV.x > 1e-6f)
    {
        const float boundaryX = ((float)tileCoord.x + 1.0f) * texelSize.x;
        alphaX = currentAlpha + (boundaryX - uv.x) / rayDirectionScreenUV.x;
    }
    else if (rayDirectionScreenUV.x < -1e-6f)
    {
        const float boundaryX = (float)tileCoord.x * texelSize.x;
        alphaX = currentAlpha + (boundaryX - uv.x) / rayDirectionScreenUV.x;
    }

    float alphaY = 1e30f;
    if (rayDirectionScreenUV.y > 1e-6f)
    {
        const float boundaryY = ((float)tileCoord.y + 1.0f) * texelSize.y;
        alphaY = currentAlpha + (boundaryY - uv.y) / rayDirectionScreenUV.y;
    }
    else if (rayDirectionScreenUV.y < -1e-6f)
    {
        const float boundaryY = (float)tileCoord.y * texelSize.y;
        alphaY = currentAlpha + (boundaryY - uv.y) / rayDirectionScreenUV.y;
    }

    float alphaExit = min(alphaLimit, min(alphaX, alphaY));
    if (alphaExit <= currentAlpha + alphaEpsilon * 0.5f)
    {
        alphaExit = min(alphaLimit, currentAlpha + alphaEpsilon);
    }

    return alphaExit;
}

float AdvanceTraceAlpha(float alpha, float alphaLimit, float alphaEpsilon)
{
    return min(alphaLimit, alpha + alphaEpsilon);
}

bool SegmentMayIntersectHZBTile(
    float3 rayStartScreenUV,
    float3 rayDirectionScreenUV,
    float alphaStart,
    float alphaEnd,
    float tileMaxDepth)
{
    const float startDepth = rayStartScreenUV.z + alphaStart * rayDirectionScreenUV.z;
    const float endDepth = rayStartScreenUV.z + alphaEnd * rayDirectionScreenUV.z;
    const float segmentFarthestDepth = min(startDepth, endDepth);
    return segmentFarthestDepth <= tileMaxDepth;
}

float ComputeTraceThicknessThreshold(float thickness, float sceneViewZ, bool bRelativeThickness)
{
    return bRelativeThickness ? (thickness * max(abs(sceneViewZ), 1e-5f)) : thickness;
}

float EvaluateTraceDepthDelta(
    float3 rayScreenUV,
    Texture2D<float> LinearDepth,
    SamplerState pointSampler,
    out float sceneViewZ)
{
    sceneViewZ = LinearDepth.SampleLevel(pointSampler, saturate(rayScreenUV.xy), 0).r;
    if (sceneViewZ <= 0.0f)
    {
        return -1e6f;
    }

    const float rayViewZ = ReconstructViewZ(rayScreenUV.z, Projection);
    return rayViewZ - sceneViewZ;
}

bool ResolveTraceHitFullResDDA(
    float3 viewPos,
    float3 rayDir,
    float3 rayStartScreenUV,
    float3 rayEndScreenUV,
    float alphaStart,
    float alphaEnd,
    float thickness,
    bool bRelativeThickness,
    float maxDistance,
    Texture2D<float> LinearDepth,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    out FTraceHitResult Result)
{
    Result.bHit = false;
    Result.HitUv = 0.0f.xx;
    Result.HitT = 0.0f;
    Result.HitViewZ = 0.0f;
    Result.Weight = 0.0f;

    if (alphaEnd <= alphaStart)
    {
        return false;
    }

    uint depthWidth = 0u;
    uint depthHeight = 0u;
#if SSR_HZB_FULL_RES_DEPTH_ENABLED
    depthWidth = hzbWidth;
    depthHeight = hzbHeight;
#else
    LinearDepth.GetDimensions(depthWidth, depthHeight);
#endif
    if (depthWidth == 0u || depthHeight == 0u)
    {
        return false;
    }

    const float2 fullResTexelSize = rcp(float2(depthWidth, depthHeight));
    const float2 intervalPixelSpan = abs((rayEndScreenUV.xy - rayStartScreenUV.xy) * (alphaEnd - alphaStart))
        * float2(depthWidth, depthHeight);
    const uint maxTraversalIterations = min(
        512u,
        max(8u, (uint)ceil(intervalPixelSpan.x + intervalPixelSpan.y) + 4u));
    const float alphaEpsilon = max(
        1e-5f,
        0.25f / max(
            max(abs(rayEndScreenUV.x - rayStartScreenUV.x) * depthWidth, abs(rayEndScreenUV.y - rayStartScreenUV.y) * depthHeight),
            1.0f));
    const float3 rayDirectionScreenUV = rayEndScreenUV - rayStartScreenUV;
    float currentAlpha = alphaStart;

    [loop]
    for (uint iteration = 0u; iteration < maxTraversalIterations && currentAlpha < alphaEnd; ++iteration)
    {
        const float2 currentUv = rayStartScreenUV.xy + currentAlpha * rayDirectionScreenUV.xy;
        const uint2 pixelCoord = ComputeHZBTileCoord(
            currentUv,
            rayDirectionScreenUV.xy,
            depthWidth,
            depthHeight);
        const float pixelExitAlpha = ComputeHZBTileExitAlpha(
            currentUv,
            rayDirectionScreenUV.xy,
            pixelCoord,
            fullResTexelSize,
            currentAlpha,
            alphaEnd,
            alphaEpsilon);
#if SSR_HZB_FULL_RES_DEPTH_ENABLED
        const float sceneDeviceZ = LoadHZBTileClosestDepth(
            pixelCoord,
            HZBTexture,
            hzbWidth,
            hzbHeight,
            0u);
        const float sceneViewZ = ReconstructViewZ(sceneDeviceZ, Projection);
#else
        const float sceneViewZ = LinearDepth.Load(int3(pixelCoord, 0)).r;
#endif
        if (sceneViewZ > 0.0f)
        {
            const float rayViewStart = ReconstructViewZ(rayStartScreenUV.z + currentAlpha * rayDirectionScreenUV.z, Projection);
            const float rayViewEnd = ReconstructViewZ(rayStartScreenUV.z + pixelExitAlpha * rayDirectionScreenUV.z, Projection);
            const float rayViewNearest = min(rayViewStart, rayViewEnd);
            const float rayViewFarthest = max(rayViewStart, rayViewEnd);
            const float thicknessThreshold = ComputeTraceThicknessThreshold(thickness, sceneViewZ, bRelativeThickness);

            if (rayViewFarthest >= sceneViewZ && rayViewNearest <= sceneViewZ + thicknessThreshold)
            {
                float refineStart = currentAlpha;
                float refineEnd = pixelExitAlpha;
                float3 finalScreenUV = rayStartScreenUV + refineEnd * rayDirectionScreenUV;
                float finalSceneViewZ = sceneViewZ;
                float finalDepthDelta = rayViewEnd - sceneViewZ;

                [unroll]
                for (uint refineStep = 0u; refineStep < 5u; ++refineStep)
                {
                    const float refineMid = lerp(refineStart, refineEnd, 0.5f);
                    const float3 refineScreenUV = rayStartScreenUV + refineMid * rayDirectionScreenUV;
                    const float refineRayViewZ = ReconstructViewZ(refineScreenUV.z, Projection);
                    const float refineDepthDelta = refineRayViewZ - sceneViewZ;

                    if (refineDepthDelta >= 0.0f)
                    {
                        refineEnd = refineMid;
                        finalScreenUV = refineScreenUV;
                        finalDepthDelta = refineDepthDelta;
                    }
                    else
                    {
                        refineStart = refineMid;
                    }
                }

                if (finalDepthDelta <= thicknessThreshold)
                {
                    const float2 hitUv = saturate(finalScreenUV.xy);
                    const float3 hitViewPos = ReconstructViewPosition(hitUv, finalSceneViewZ, Projection);
                    const float hitT = max(dot(hitViewPos - viewPos, rayDir), 0.0f);
                    if (hitT > 1e-3f)
                    {
                        Result.bHit = true;
                        Result.HitUv = hitUv;
                        Result.HitT = hitT;
                        Result.HitViewZ = finalSceneViewZ;
                        Result.Weight = 1.0f - saturate(hitT / max(maxDistance, 1e-3f));
                        return true;
                    }
                }
            }
        }

        currentAlpha = AdvanceTraceAlpha(pixelExitAlpha, alphaEnd, alphaEpsilon);
    }

    return false;
}

bool ResolveTraceHitHZBInterval(
    float3 viewPos,
    float3 rayDir,
    float3 rayStartScreenUV,
    float3 rayEndScreenUV,
    float alphaStart,
    float alphaEnd,
    float tileDeviceDepth,
    float thickness,
    bool bRelativeThickness,
    float maxDistance,
    out FTraceHitResult Result)
{
    Result.bHit = false;
    Result.HitUv = 0.0f.xx;
    Result.HitT = 0.0f;
    Result.HitViewZ = 0.0f;
    Result.Weight = 0.0f;

    if (alphaEnd <= alphaStart)
    {
        return false;
    }

    const float sceneViewZ = ReconstructViewZ(tileDeviceDepth, Projection);
    if (sceneViewZ <= 0.0f)
    {
        return false;
    }

    const float3 rayDirectionScreenUV = rayEndScreenUV - rayStartScreenUV;
    const float rayViewStart = ReconstructViewZ(rayStartScreenUV.z + alphaStart * rayDirectionScreenUV.z, Projection);
    const float rayViewEnd = ReconstructViewZ(rayStartScreenUV.z + alphaEnd * rayDirectionScreenUV.z, Projection);
    const float rayViewNearest = min(rayViewStart, rayViewEnd);
    const float rayViewFarthest = max(rayViewStart, rayViewEnd);
    const float thicknessThreshold = ComputeTraceThicknessThreshold(thickness, sceneViewZ, bRelativeThickness);

    if (rayViewFarthest < sceneViewZ || rayViewNearest > sceneViewZ + thicknessThreshold)
    {
        return false;
    }

    float refineStart = alphaStart;
    float refineEnd = alphaEnd;
    float3 finalScreenUV = rayStartScreenUV + refineEnd * rayDirectionScreenUV;
    float finalDepthDelta = ReconstructViewZ(finalScreenUV.z, Projection) - sceneViewZ;

    [unroll]
    for (uint refineStep = 0u; refineStep < 5u; ++refineStep)
    {
        const float refineMid = lerp(refineStart, refineEnd, 0.5f);
        const float3 refineScreenUV = rayStartScreenUV + refineMid * rayDirectionScreenUV;
        const float refineDepthDelta = ReconstructViewZ(refineScreenUV.z, Projection) - sceneViewZ;

        if (refineDepthDelta >= 0.0f)
        {
            refineEnd = refineMid;
            finalScreenUV = refineScreenUV;
            finalDepthDelta = refineDepthDelta;
        }
        else
        {
            refineStart = refineMid;
        }
    }

    if (finalDepthDelta > thicknessThreshold)
    {
        return false;
    }

    const float2 hitUv = saturate(finalScreenUV.xy);
    const float3 hitViewPos = ReconstructViewPosition(hitUv, sceneViewZ, Projection);
    const float hitT = max(dot(hitViewPos - viewPos, rayDir), 0.0f);
    if (hitT <= 1e-3f)
    {
        return false;
    }

    Result.bHit = true;
    Result.HitUv = hitUv;
    Result.HitT = hitT;
    Result.HitViewZ = sceneViewZ;
    Result.Weight = 1.0f - saturate(hitT / max(maxDistance, 1e-3f));
    return true;
}

FHZBTraceTileState BuildHZBTraceTileState(
    float3 rayStartScreenUV,
    float3 rayDirectionScreenUV,
    float currentAlpha,
    float currentLimit,
    float alphaEpsilon,
    uint currentMip,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight)
{
    const uint mipWidth = max(1u, hzbWidth >> currentMip);
    const uint mipHeight = max(1u, hzbHeight >> currentMip);
    const float2 texelSize = rcp(float2(mipWidth, mipHeight));
    const float2 currentUv = rayStartScreenUV.xy + currentAlpha * rayDirectionScreenUV.xy;
    const uint2 tileCoord = ComputeHZBTileCoord(
        currentUv,
        rayDirectionScreenUV.xy,
        mipWidth,
        mipHeight);

    FHZBTraceTileState TileState;
    TileState.TileExitAlpha = ComputeHZBTileExitAlpha(
        currentUv,
        rayDirectionScreenUV.xy,
        tileCoord,
        texelSize,
        currentAlpha,
        currentLimit,
        alphaEpsilon);
    TileState.TileClosestDepth = LoadHZBTileClosestDepth(
        tileCoord,
        HZBTexture,
        hzbWidth,
        hzbHeight,
        currentMip);
    TileState.bCandidateTile = SegmentMayIntersectHZBTile(
        rayStartScreenUV,
        rayDirectionScreenUV,
        currentAlpha,
        TileState.TileExitAlpha,
        TileState.TileClosestDepth);
    TileState.ResumeAlpha = AdvanceTraceAlpha(TileState.TileExitAlpha, currentLimit, alphaEpsilon);
    return TileState;
}

bool ResolveTraceHitWithFullResDDA(
    float3 viewPos,
    float3 rayDir,
    float3 rayStartScreenUV,
    float3 rayEndScreenUV,
    float alphaStart,
    float alphaEnd,
    float tileDeviceDepth,
    float thickness,
    bool bRelativeThickness,
    float maxDistance,
    Texture2D<float> LinearDepth,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    out FTraceHitResult Result)
{
#if SSR_HZB_FULL_RES_DEPTH_ENABLED
    return ResolveTraceHitFullResDDA(
        viewPos,
        rayDir,
        rayStartScreenUV,
        rayEndScreenUV,
        alphaStart,
        alphaEnd,
        thickness,
        bRelativeThickness,
        maxDistance,
        LinearDepth,
        HZBTexture,
        hzbWidth,
        hzbHeight,
        Result);
#else
    return ResolveTraceHitHZBInterval(
        viewPos,
        rayDir,
        rayStartScreenUV,
        rayEndScreenUV,
        alphaStart,
        alphaEnd,
        tileDeviceDepth,
        thickness,
        bRelativeThickness,
        maxDistance,
        Result);
#endif
}

bool ResolveTraceHitFromDescendedInterval(
    float3 viewPos,
    float3 rayDir,
    float3 rayStartScreenUV,
    float3 rayEndScreenUV,
    float alphaStart,
    float alphaEnd,
    uint startMip,
    float thickness,
    bool bRelativeThickness,
    float maxDistance,
    float alphaEpsilon,
    Texture2D<float> LinearDepth,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    out FTraceHitResult Result);

bool ResolveTraceHitForCandidateInterval(
    float3 viewPos,
    float3 rayDir,
    float3 rayStartScreenUV,
    float3 rayEndScreenUV,
    float alphaStart,
    float alphaEnd,
    uint currentMip,
    float tileClosestDepth,
    float thickness,
    bool bRelativeThickness,
    float maxDistance,
    float alphaEpsilon,
    Texture2D<float> LinearDepth,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    out FTraceHitResult Result)
{
    if (currentMip > 0u)
    {
        return ResolveTraceHitFromDescendedInterval(
            viewPos,
            rayDir,
            rayStartScreenUV,
            rayEndScreenUV,
            alphaStart,
            alphaEnd,
            currentMip,
            thickness,
            bRelativeThickness,
            maxDistance,
            alphaEpsilon,
            LinearDepth,
            HZBTexture,
            hzbWidth,
            hzbHeight,
            Result);
    }

    return ResolveTraceHitWithFullResDDA(
        viewPos,
        rayDir,
        rayStartScreenUV,
        rayEndScreenUV,
        alphaStart,
        alphaEnd,
        tileClosestDepth,
        thickness,
        bRelativeThickness,
        maxDistance,
        LinearDepth,
        HZBTexture,
        hzbWidth,
        hzbHeight,
        Result);
}

bool ResolveTraceHitFromDescendedInterval(
    float3 viewPos,
    float3 rayDir,
    float3 rayStartScreenUV,
    float3 rayEndScreenUV,
    float alphaStart,
    float alphaEnd,
    uint startMip,
    float thickness,
    bool bRelativeThickness,
    float maxDistance,
    float alphaEpsilon,
    Texture2D<float> LinearDepth,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    out FTraceHitResult Result)
{
    Result.bHit = false;
    Result.HitUv = 0.0f.xx;
    Result.HitT = 0.0f;
    Result.HitViewZ = 0.0f;
    Result.Weight = 0.0f;

    if (alphaEnd <= alphaStart)
    {
        return false;
    }

    const uint maxStackDepth = 24u;
    const uint maxTraversalIterations = max(32u, (startMip + 1u) * 24u);
    uint mipStack[24];
    float alphaStack[24];
    float limitStack[24];
    uint stackSize = 0u;
    uint currentMip = startMip;
    float currentAlpha = alphaStart;
    float currentLimit = alphaEnd;
    const float3 rayDirectionScreenUV = rayEndScreenUV - rayStartScreenUV;

    [loop]
    for (uint iteration = 0u; iteration < maxTraversalIterations; ++iteration)
    {
        if (currentAlpha >= currentLimit - alphaEpsilon)
        {
            if (stackSize == 0u)
            {
                break;
            }

            --stackSize;
            currentMip = mipStack[stackSize];
            currentAlpha = alphaStack[stackSize];
            currentLimit = limitStack[stackSize];
            continue;
        }

        const FHZBTraceTileState TileState = BuildHZBTraceTileState(
            rayStartScreenUV,
            rayDirectionScreenUV,
            currentAlpha,
            currentLimit,
            alphaEpsilon,
            currentMip,
            HZBTexture,
            hzbWidth,
            hzbHeight);

        if (!TileState.bCandidateTile)
        {
            currentAlpha = TileState.ResumeAlpha;
            continue;
        }

        if (currentMip > 0u)
        {
            if (TileState.ResumeAlpha < currentLimit - alphaEpsilon)
            {
                if (stackSize < maxStackDepth)
                {
                    mipStack[stackSize] = currentMip;
                    alphaStack[stackSize] = TileState.ResumeAlpha;
                    limitStack[stackSize] = currentLimit;
                    ++stackSize;
                }
                else
                {
                    if (ResolveTraceHitWithFullResDDA(
                            viewPos,
                            rayDir,
                            rayStartScreenUV,
                            rayEndScreenUV,
                            currentAlpha,
                            TileState.TileExitAlpha,
                            TileState.TileClosestDepth,
                            thickness,
                            bRelativeThickness,
                            maxDistance,
                            LinearDepth,
                            HZBTexture,
                            hzbWidth,
                            hzbHeight,
                            Result))
                    {
                        return true;
                    }

                    currentAlpha = TileState.ResumeAlpha;
                    continue;
                }
            }

            --currentMip;
            currentLimit = TileState.TileExitAlpha;
            continue;
        }

        if (ResolveTraceHitWithFullResDDA(
                viewPos,
                rayDir,
                rayStartScreenUV,
                rayEndScreenUV,
                currentAlpha,
                TileState.TileExitAlpha,
                TileState.TileClosestDepth,
                thickness,
                bRelativeThickness,
                maxDistance,
                LinearDepth,
                HZBTexture,
                hzbWidth,
                hzbHeight,
                Result))
        {
            return true;
        }

        currentAlpha = TileState.ResumeAlpha;
    }

    return false;
}

FTraceHitResult TraceSwHitLinear(
    float3 viewPos,
    float3 rayDir,
    float2 startUv,
    uint maxSteps,
    float stride,
    float maxDistance,
    float thickness,
    bool bRelativeThickness,
    Texture2D<float> LinearDepth,
    SamplerState pointSampler,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    uint hzbMipCount,
    uint hzbAvailable)
{
    FTraceHitResult Result;
    Result.bHit = false;
    Result.HitUv = 0.0f.xx;
    Result.HitT = 0.0f;
    Result.HitViewZ = 0.0f;
    Result.Weight = 0.0f;

    float3 rayPos = viewPos;
    float t = 0.0f;
    float tPrev = 0.0f;

    [loop]
    for (uint stepIndex = 0; stepIndex < maxSteps; ++stepIndex)
    {
        tPrev = t;
        t += stride;
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
                    Result.HitUv = midUv;
                    Result.HitT = tMid;
                    Result.HitViewZ = midDepth;
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
                Result.HitUv = uv;
                Result.HitT = t;
                Result.HitViewZ = sceneViewZ;
                Result.Weight = fade;
                Result.bHit = true;
                break;
            }
#endif
        }

    }

    return Result;
}

FTraceHitResult TraceSwHitHZB(
    float3 viewPos,
    float3 rayDir,
    float2 startUv,
    uint maxSteps,
    float stride,
    float maxDistance,
    float thickness,
    bool bRelativeThickness,
    Texture2D<float> LinearDepth,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    uint hzbMipCount,
    uint hzbAvailable)
{
    FTraceHitResult Result;
    Result.bHit = false;
    Result.HitUv = 0.0f.xx;
    Result.HitT = 0.0f;
    Result.HitViewZ = 0.0f;
    Result.Weight = 0.0f;

    if (hzbAvailable == 0u || hzbWidth == 0u || hzbHeight == 0u || hzbMipCount == 0u)
    {
        return Result;
    }

    const float maxTraceDistance = ComputeTraceRayEndDistance(viewPos, rayDir, maxDistance);
    if (maxTraceDistance <= 0.0f)
    {
        return Result;
    }

    const float4 startClip = mul(float4(viewPos, 1.0f), Projection);
    if (startClip.w <= 0.0f)
    {
        return Result;
    }

    const float3 rayStartScreenUV = float3(startUv, startClip.z / startClip.w);
    const float3 rayEndViewPos = viewPos + rayDir * maxTraceDistance;
    const float4 rayEndClip = mul(float4(rayEndViewPos, 1.0f), Projection);
    if (rayEndClip.w <= 0.0f)
    {
        return Result;
    }

    float3 rayEndScreenUV = ProjectViewPositionToScreenUVZ(rayEndViewPos);
    const float2 screenEdgeIntersections = LineBoxIntersect(
        rayStartScreenUV.xy,
        rayEndScreenUV.xy,
        0.0f.xx,
        1.0f.xx);
    if (screenEdgeIntersections.y <= screenEdgeIntersections.x)
    {
        return Result;
    }

    rayEndScreenUV = rayStartScreenUV + (rayEndScreenUV - rayStartScreenUV) * screenEdgeIntersections.y;
    const float3 rayDirectionScreenUV = rayEndScreenUV - rayStartScreenUV;
    if (max(abs(rayDirectionScreenUV.x), abs(rayDirectionScreenUV.y)) < 1e-6f)
    {
        return Result;
    }

    const uint initialMip = EstimateHZBSegmentMip(
        rayStartScreenUV.xy,
        rayEndScreenUV.xy,
        hzbWidth,
        hzbHeight,
        hzbMipCount);
    const float alphaEpsilon = max(
        1e-5f,
        0.25f / max(
            max(abs(rayDirectionScreenUV.x) * hzbWidth, abs(rayDirectionScreenUV.y) * hzbHeight),
            1.0f));
    const uint maxTraversalIterations = max(maxSteps * 4u, max(hzbMipCount * 8u, 32u));
    const uint maxStackDepth = 16u;
    uint mipStack[16];
    float alphaStack[16];
    float limitStack[16];
    uint stackSize = 0u;
    uint currentMip = initialMip;
    float currentAlpha = 0.0f;
    float currentLimit = 1.0f;

    [loop]
    for (uint iteration = 0u; iteration < maxTraversalIterations; ++iteration)
    {
        if (currentAlpha >= currentLimit - alphaEpsilon)
        {
            if (stackSize == 0u)
            {
                break;
            }

            --stackSize;
            currentMip = mipStack[stackSize];
            currentAlpha = alphaStack[stackSize];
            currentLimit = limitStack[stackSize];
            continue;
        }

        const FHZBTraceTileState TileState = BuildHZBTraceTileState(
            rayStartScreenUV,
            rayDirectionScreenUV,
            currentAlpha,
            currentLimit,
            alphaEpsilon,
            currentMip,
            HZBTexture,
            hzbWidth,
            hzbHeight);

        if (!TileState.bCandidateTile)
        {
            currentAlpha = TileState.ResumeAlpha;
            continue;
        }

        if (currentMip > 0u)
        {
            if (TileState.ResumeAlpha < currentLimit - alphaEpsilon)
            {
                if (stackSize < maxStackDepth)
                {
                    mipStack[stackSize] = currentMip;
                    alphaStack[stackSize] = TileState.ResumeAlpha;
                    limitStack[stackSize] = currentLimit;
                    ++stackSize;
                }
                else
                {
                    FTraceHitResult FallbackResult;
                    if (ResolveTraceHitForCandidateInterval(
                            viewPos,
                            rayDir,
                            rayStartScreenUV,
                            rayEndScreenUV,
                            currentAlpha,
                            TileState.TileExitAlpha,
                            currentMip,
                            TileState.TileClosestDepth,
                            thickness,
                            bRelativeThickness,
                            maxDistance,
                            alphaEpsilon,
                            LinearDepth,
                            HZBTexture,
                            hzbWidth,
                            hzbHeight,
                            FallbackResult))
                    {
                        return FallbackResult;
                    }

                    currentAlpha = TileState.ResumeAlpha;
                    continue;
                }
            }

            --currentMip;
            currentLimit = TileState.TileExitAlpha;
            continue;
        }

        FTraceHitResult CandidateResult;
        if (ResolveTraceHitForCandidateInterval(
                viewPos,
                rayDir,
                rayStartScreenUV,
                rayEndScreenUV,
                currentAlpha,
                TileState.TileExitAlpha,
                currentMip,
                TileState.TileClosestDepth,
                thickness,
                bRelativeThickness,
                maxDistance,
                alphaEpsilon,
                LinearDepth,
                HZBTexture,
                hzbWidth,
                hzbHeight,
                CandidateResult))
        {
            return CandidateResult;
        }

        currentAlpha = TileState.ResumeAlpha;
    }

    return Result;
}

FTraceHitResult TraceSwHit(
    float3 viewPos,
    float3 rayDir,
    float2 startUv,
    uint maxSteps,
    float stride,
    float maxDistance,
    float thickness,
    bool bRelativeThickness,
    Texture2D<float> LinearDepth,
    SamplerState pointSampler,
    Texture2D<float2> HZBTexture,
    uint hzbWidth,
    uint hzbHeight,
    uint hzbMipCount,
    uint hzbAvailable)
{
#if HZB_ENABLED
    return TraceSwHitHZB(
        viewPos,
        rayDir,
        startUv,
        maxSteps,
        stride,
        maxDistance,
        thickness,
        bRelativeThickness,
        LinearDepth,
        HZBTexture,
        hzbWidth,
        hzbHeight,
        hzbMipCount,
        hzbAvailable);
#else
    return TraceSwHitLinear(
        viewPos,
        rayDir,
        startUv,
        maxSteps,
        stride,
        maxDistance,
        thickness,
        bRelativeThickness,
        LinearDepth,
        pointSampler,
        HZBTexture,
        hzbWidth,
        hzbHeight,
        hzbMipCount,
        hzbAvailable);
#endif
}

FTraceResult TraceSw(
    float3 viewPos,
    float3 rayDir,
    float2 startUv,
    uint maxSteps,
    float stride,
    float maxDistance,
    float thickness,
    Texture2D<float> LinearDepth,
    Texture2D<float4> SceneColor,
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
    Result.Color = 0.0f.xxx;
    Result.Weight = 0.0f;

    const FTraceHitResult HitResult = TraceSwHit(
        viewPos,
        rayDir,
        startUv,
        maxSteps,
        stride,
        maxDistance,
        thickness,
        false,
        LinearDepth,
        pointSampler,
        HZBTexture,
        hzbWidth,
        hzbHeight,
        hzbMipCount,
        hzbAvailable);

    Result.bHit = HitResult.bHit;
    Result.Weight = HitResult.Weight;
    if (HitResult.bHit)
    {
        Result.Color = SceneColor.SampleLevel(linearSampler, HitResult.HitUv, 0).rgb;
    }

    return Result;
}
