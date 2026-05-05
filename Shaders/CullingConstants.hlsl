cbuffer CullingConstants : register(b0)
{
    float4 FrustumPlanes[6];
    row_major float4x4 ViewProjection;
    uint IndirectCommandCount;
    uint HZBEnabled;
    uint HZBMipCount;
    uint HZBWidth;
    uint HZBHeight;
    uint DebugPrintEnabled;
    uint RangeCount;
    uint CullingMode;
    float3 CameraPosition;
    float Padding1;
    float ClusterDAGTargetErrorPixels;
    float ViewportHeightPixels;
    uint ClusterDagRootCount;
    uint ClusterDAGForceMipEnabled;
    uint ClusterDAGForceMipLevel;
    uint ClusterDAGForceMipSkipFrustumCull;
    uint Padding2;
    uint Padding3;
};
