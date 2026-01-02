cbuffer AutoExposureConstants : register(b0)
{
    float2 InputSize;
    float DeltaTime;
    float AdaptationSpeedUp;
    float AdaptationSpeedDown;
    uint UseHistory;
    float AutoExposureKey;
    float AutoExposureMin;
    float AutoExposureMax;
};

Texture2D HDRScene : register(t0);
Texture2D PrevLogAverageLuminance : register(t1);
RWTexture2D<float> LogAverageLuminance : register(u0);
SamplerState SceneSampler : register(s0);

groupshared float SharedLog[256];

[numthreads(16, 16, 1)]
void CSMain(uint3 DispatchThreadId : SV_DispatchThreadID, uint3 GroupThreadId : SV_GroupThreadID)
{
    float2 groupCoord = float2(GroupThreadId.xy) + 0.5f;
    float2 samplePos = groupCoord * (InputSize / 16.0f);
    float2 uv = samplePos / max(InputSize, 1.0f);
    float mipLevel = max(0.0f, log2(max(InputSize.x, InputSize.y)) - 4.0f);

    float3 color = HDRScene.SampleLevel(SceneSampler, uv, mipLevel).rgb;
    const float3 LuminanceWeights = float3(0.2126f, 0.7152f, 0.0722f);
    float luminance = dot(max(color, 0.0f), LuminanceWeights);

    uint index = GroupThreadId.x + GroupThreadId.y * 16;
    SharedLog[index] = log2(max(luminance, 1e-4f));

    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (index < stride)
        {
            SharedLog[index] += SharedLog[index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0)
    {
        float logAverageEv = SharedLog[0] / 256.0f;
        float keyEv = log2(max(AutoExposureKey, 1e-4f));
        float targetExposureEv = keyEv - logAverageEv;
        float minEv = log2(max(AutoExposureMin, 1e-4f));
        float maxEv = log2(max(AutoExposureMax, 1e-4f));
        targetExposureEv = clamp(targetExposureEv, minEv, maxEv);

        float adapted = targetExposureEv;
        if (UseHistory != 0)
        {
            float previousLog = PrevLogAverageLuminance.Load(int3(0, 0, 0)).r;
            float speed = targetExposureEv > previousLog ? AdaptationSpeedUp : AdaptationSpeedDown;
            float alpha = 1.0f - exp(-DeltaTime * speed);
            adapted = lerp(previousLog, targetExposureEv, saturate(alpha));
        }
        LogAverageLuminance[uint2(0, 0)] = adapted;
    }
}
