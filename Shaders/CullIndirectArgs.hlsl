cbuffer CullingConstants : register(b0)
{
    float4 FrustumPlanes[6];
    uint ModelCount;
};

StructuredBuffer<float4> ModelBounds : register(t0);
RWByteAddressBuffer IndirectArgs : register(u0);

static const uint kCommandStride = 64;
static const uint kInstanceCountOffset = 44;

bool IsSphereVisible(float3 center, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        float4 plane = FrustumPlanes[i];
        if (dot(plane.xyz, center) + plane.w < -radius)
        {
            return false;
        }
    }
    return true;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= ModelCount)
    {
        return;
    }

    float4 bounds = ModelBounds[index];
    bool visible = IsSphereVisible(bounds.xyz, bounds.w);

    uint baseOffset = index * kCommandStride + kInstanceCountOffset;
    IndirectArgs.Store(baseOffset, visible ? 1u : 0u);
}
