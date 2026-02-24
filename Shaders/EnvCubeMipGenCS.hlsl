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

    TextureCube<float3> InputCube = ResourceDescriptorHeap[InputSrvIndex];
    RWTexture2DArray<float3> OutputCube = ResourceDescriptorHeap[OutputUavIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[SamplerIndex];

    uint2 srcBase = dispatchThreadId.xy * 2u;
    float3 accum = 0.0f;
    [unroll]
    for (uint y = 0; y < 2; ++y)
    {
        [unroll]
        for (uint x = 0; x < 2; ++x)
        {
            uint2 srcCoord = srcBase + uint2(x, y);
            float2 srcUv = (float2(srcCoord) + 0.5f) / float2(max(1u, Width * 2u), max(1u, Height * 2u));
            float3 dir;
            if (dispatchThreadId.z == 0) dir = normalize(float3( 1.0f, 1.0f - srcUv.y * 2.0f, -(srcUv.x * 2.0f - 1.0f)));
            else if (dispatchThreadId.z == 1) dir = normalize(float3(-1.0f, 1.0f - srcUv.y * 2.0f,  (srcUv.x * 2.0f - 1.0f)));
            else if (dispatchThreadId.z == 2) dir = normalize(float3( srcUv.x * 2.0f - 1.0f,  1.0f, -(1.0f - srcUv.y * 2.0f)));
            else if (dispatchThreadId.z == 3) dir = normalize(float3( srcUv.x * 2.0f - 1.0f, -1.0f,  (1.0f - srcUv.y * 2.0f)));
            else if (dispatchThreadId.z == 4) dir = normalize(float3( srcUv.x * 2.0f - 1.0f, 1.0f - srcUv.y * 2.0f,  1.0f));
            else dir = normalize(float3(-(srcUv.x * 2.0f - 1.0f), 1.0f - srcUv.y * 2.0f, -1.0f));
            accum += InputCube.SampleLevel(LinearSampler, dir, SourceMip);
        }
    }

    OutputCube[uint3(dispatchThreadId.xy, dispatchThreadId.z)] = accum * 0.25f;
}
