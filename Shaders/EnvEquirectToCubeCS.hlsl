#include "EnvCubemapBasis.hlsli"

cbuffer EnvBuildConstants : register(b0)
{
    uint InputSrvIndex;
    uint OutputUavIndex;
    uint Width;
    uint Height;
    uint SourceMip;
    uint FaceCount;
    uint SamplerIndex;
    uint Padding1;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height || dispatchThreadId.z >= FaceCount)
    {
        return;
    }

    Texture2D<float4> EquirectTexture = ResourceDescriptorHeap[InputSrvIndex];
    RWTexture2DArray<float3> OutputCube = ResourceDescriptorHeap[OutputUavIndex];

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(Width, Height);
    float3 dir = GetCubeDirection(dispatchThreadId.z, uv);
    float2 envUv = DirectionToEquirectUV(dir);
    SamplerState LinearSampler = SamplerDescriptorHeap[SamplerIndex];
    float3 color = EquirectTexture.SampleLevel(LinearSampler, envUv, 0.0f).rgb;

    OutputCube[uint3(dispatchThreadId.xy, dispatchThreadId.z)] = color;
}
