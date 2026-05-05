#include "SceneConstants.hlsl"
#include "OctahedralEncoding.hlsli"

cbuffer ExtractHalfDepthNormalConstants : register(b1)
{
    uint DepthIndex;
    uint GBufferAIndex;
    uint OutputUavIndex;
    uint FrameIndex;
}

[numthreads(8, 8, 1)]
void ExtractHalfDepthNormalCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    Texture2D<float> DepthTexture = ResourceDescriptorHeap[DepthIndex];
    Texture2D<float4> GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    RWTexture2D<uint2> OutHalfDepthNormal = ResourceDescriptorHeap[OutputUavIndex];

    uint FullWidth = 0;
    uint FullHeight = 0;
    DepthTexture.GetDimensions(FullWidth, FullHeight);

    const uint2 HalfPos = DispatchThreadId.xy;
    const uint2 HalfSize = (uint2(FullWidth, FullHeight) + 1u) / 2u;
    if (HalfPos.x >= HalfSize.x || HalfPos.y >= HalfSize.y)
    {
        return;
    }

    static const uint2 Offsets[4] =
    {
        uint2(1u, 1u),
        uint2(1u, 0u),
        uint2(0u, 0u),
        uint2(0u, 1u)
    };

    const uint2 FullPos = min(HalfPos * 2u + Offsets[FrameIndex & 3u], uint2(FullWidth - 1u, FullHeight - 1u));
    const float Depth = DepthTexture[FullPos];
    const float3 Normal = normalize(GBufferA[FullPos].xyz * 2.0f - 1.0f);
    OutHalfDepthNormal[HalfPos] = uint2(asuint(Depth), EncodeOctahedral16x2(Normal));
}
