#include "PBRCommon.hlsl"
#include "EnvCubemapBasis.hlsli"

cbuffer EnvBuildConstants : register(b0)
{
    uint InputSrvIndex;
    uint OutputUavIndex;
    uint Width;
    uint Height;
    uint MipIndex;
    uint MipCount;
    uint SampleCount;
    uint SamplerIndex;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= Width || dispatchThreadId.y >= Height || dispatchThreadId.z >= 6)
    {
        return;
    }

    TextureCube<float3> InputCube = ResourceDescriptorHeap[InputSrvIndex];
    RWTexture2DArray<float3> OutputCube = ResourceDescriptorHeap[OutputUavIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[SamplerIndex];

    float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float2(Width, Height);
    float3 N = GetCubeDirection(dispatchThreadId.z, uv);
    float3 V = N;

    float roughness = (MipCount > 1) ? (float)MipIndex / (float)(MipCount - 1) : 0.0f;
    float alpha = max(roughness * roughness, 1e-4f);

    float3 prefiltered = 0.0f;
    float totalWeight = 0.0f;

    const float eps = 1e-4f;
    const uint safeSampleCount = max(1u, SampleCount);
    const float sampleCountF = (float)safeSampleCount;
    const float baseResolution = max(1.0f, (float)(Width << MipIndex));
    const float omegaP = (4.0f * ENV_PI) / (6.0f * baseResolution * baseResolution);
    const float mipBias = 1.0f;

    [loop]
    for (uint i = 0; i < safeSampleCount; ++i)
    {
        float2 xi = Hammersley(i, safeSampleCount);
        float3 Ht = SampleGGX(xi, alpha);
        float3 H = TangentToWorld(Ht, N);
        float3 L = normalize(2.0f * dot(V, H) * H - V);

        float NdotL = saturate(dot(N, L));
        if (NdotL > 0.0f)
        {
            float NdotH = saturate(dot(N, H));
            float LdotH = saturate(dot(L, H));
            float D = D_GGX(NdotH, alpha);
            float pdf = max(D * NdotH / max(4.0f * LdotH, eps), eps);
            float omegaS = 1.0f / (sampleCountF * pdf);
            float sourceMip = max(0.5f * log2(omegaS / omegaP) + mipBias, 0.0f);
            sourceMip = min(sourceMip, (float)(MipCount - 1));

            prefiltered += InputCube.SampleLevel(LinearSampler, L, sourceMip).rgb * NdotL;
            totalWeight += NdotL;
        }
    }

    prefiltered /= max(totalWeight, eps);
    OutputCube[uint3(dispatchThreadId.xy, dispatchThreadId.z)] = prefiltered;
}
