cbuffer CullingConstants : register(b0)
{
    float4 FrustumPlanes[6];
    uint DebugPrintEnabled;
    float3 CameraPosition;
};

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
