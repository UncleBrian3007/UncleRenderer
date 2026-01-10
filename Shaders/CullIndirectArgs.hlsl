cbuffer CullingConstants : register(b0)
{
    float4 FrustumPlanes[6];
    float4x4 ViewProjection;
    uint IndirectCommandCount;
    uint HZBEnabled;
    uint HZBMipCount;
    uint HZBWidth;
    uint HZBHeight;
    uint DebugPrintEnabled;
    uint Padding0;
    uint Padding1;
    float3 CameraPosition;
    float Padding2;
};

StructuredBuffer<float4> ModelBounds : register(t0);
Texture2D<float> HZBTexture : register(t1);
StructuredBuffer<float4> MeshletConeAxisCutoff : register(t2);
StructuredBuffer<float4> MeshletConeApex : register(t3);
RWByteAddressBuffer IndirectArgs : register(u0);
RWByteAddressBuffer DebugPrintBuffer : register(u1);
RWByteAddressBuffer DebugPrintStats : register(u2);

#include "DebugPrintCommon.hlsl"

static const uint kCommandStride = 64;
static const uint kInstanceCountOffset = 44;

bool IsSphereVisible(float3 center, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = FrustumPlanes[i];
        float distance = dot(plane.xyz, center) + plane.w;
        if (distance < -radius)
        {
            return false;
        }
    }
    return true;
}

bool IsConeVisible_SphereExpanded(
    float3 center,
    float radius,
    uint index)
{
	float4 axisCutoff = MeshletConeAxisCutoff[index];

	if (axisCutoff.w < 0.0f)
		return true;

	float3 axis = axisCutoff.xyz;

    // camera → center
	float3 view = center - CameraPosition;

	float distSq = dot(view, view);
	if (distSq <= 1e-8f)
		return true;

	float dist = sqrt(distSq);

    // 완전히 cone 뒤에 들어갔는가?
    // dot(view, -axis) >= cos(theta) * |view| + radius
	bool coneCulled =
        dot(view, -axis) >= axisCutoff.w * dist + radius;

	return !coneCulled;
}

float4 ProjectToClip(float3 position)
{
    return mul(float4(position, 1.0f), ViewProjection);
}

bool IsOccluded(float3 center, float radius)
{
    float3 boundsMin = center - radius;
    float3 boundsMax = center + radius;

    float3 corners[8] =
    {
        float3(boundsMin.x, boundsMin.y, boundsMin.z),
        float3(boundsMax.x, boundsMin.y, boundsMin.z),
        float3(boundsMin.x, boundsMax.y, boundsMin.z),
        float3(boundsMax.x, boundsMax.y, boundsMin.z),
        float3(boundsMin.x, boundsMin.y, boundsMax.z),
        float3(boundsMax.x, boundsMin.y, boundsMax.z),
        float3(boundsMin.x, boundsMax.y, boundsMax.z),
        float3(boundsMax.x, boundsMax.y, boundsMax.z)
    };

    float2 minUv = float2(1.0f, 1.0f);
    float2 maxUv = float2(0.0f, 0.0f);
    float maxDepth = 0.0f;

    bool anyBehind = false;

    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        float4 clip = ProjectToClip(corners[i]);
        if (clip.w <= 0.0f)
        {
            anyBehind = true;
            break;
        }

        float3 ndc = clip.xyz / clip.w;
        float2 uv;
        uv.x = ndc.x * 0.5f + 0.5f;
        uv.y = 1 - (ndc.y * 0.5f + 0.5f);

        minUv = min(minUv, uv);
        maxUv = max(maxUv, uv);
        maxDepth = max(maxDepth, ndc.z);
    }

    if (anyBehind)
    {
        return false;
    }

    if (maxUv.x < 0.0f || maxUv.y < 0.0f || minUv.x > 1.0f || minUv.y > 1.0f)
    {
        return false;
    }

    minUv = saturate(minUv);
    maxUv = saturate(maxUv);

    float2 extent = maxUv - minUv;
    float2 pixelSize = extent * float2(HZBWidth, HZBHeight);
    float maxDim = max(pixelSize.x, pixelSize.y);
    uint mipLevel = 0;
    if (maxDim > 1.0f)
    {
        mipLevel = (uint)clamp(floor(log2(maxDim)), 0.0f, (float)(HZBMipCount - 1));
    }

    uint mipWidth = max(1u, HZBWidth >> mipLevel);
    uint mipHeight = max(1u, HZBHeight >> mipLevel);

    uint2 minCoord = uint2(minUv * float2(mipWidth, mipHeight));
    uint2 maxCoord = uint2(maxUv * float2(mipWidth, mipHeight));
    minCoord = min(minCoord, uint2(mipWidth - 1, mipHeight - 1));
    maxCoord = min(maxCoord, uint2(mipWidth - 1, mipHeight - 1));

    float hzbDepth = 1.0f;
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(minCoord, mipLevel)));
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(maxCoord.x, minCoord.y, mipLevel)));
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(minCoord.x, maxCoord.y, mipLevel)));
    hzbDepth = min(hzbDepth, HZBTexture.Load(int3(maxCoord, mipLevel)));

    return maxDepth < hzbDepth;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint index = dispatchThreadId.x;
	if (index >= IndirectCommandCount)
		return;

	float4 sphere = ModelBounds[index];
	float3 center = sphere.xyz;
	float radius = sphere.w;

	bool frustumVisible = IsSphereVisible(center, radius);
	bool coneVisible = true;
	bool visible = frustumVisible;

	if (visible)
	{
		coneVisible = IsConeVisible_SphereExpanded(center, radius, index);
		visible = coneVisible;
	}

	bool occluded = false;
	if (visible && HZBEnabled != 0)
	{
		occluded = IsOccluded(center, radius);
		visible = !occluded;
	}

	uint baseOffset = index * kCommandStride + kInstanceCountOffset;
	IndirectArgs.Store(baseOffset, visible ? 1u : 0u);

	if (DebugPrintEnabled != 0 && !visible)
	{
		if (!frustumVisible)
		{
			DebugPrintStats.InterlockedAdd(0, 1);
		}
		else if (!coneVisible)
		{
			DebugPrintStats.InterlockedAdd(8, 1);
		}
		else if (occluded)
		{
			DebugPrintStats.InterlockedAdd(4, 1);
		}
	}
}
