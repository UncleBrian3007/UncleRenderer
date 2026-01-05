struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

VSOutput VSMain(uint VertexId : SV_VertexID)
{
    float2 Positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0, 1.0);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

cbuffer TonemapParams : register(b0)
{
    uint EnableTonemap;
    uint EnableAutoExposure;
    float Exposure;
    float Gamma;
};

Texture2D HDRScene : register(t0);
Texture2D LogAverageLuminance : register(t1);
SamplerState SceneSampler : register(s0);

float3 PBRNeutralToneMapping(float3 color)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
    {
        return color;
    }

    const float d = 1.0f - startCompression;
    float newPeak = 1.0f - d * d / (peak + d - startCompression);
    color *= newPeak / max(peak, 1e-4f);

    float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return lerp(color, newPeak * float3(1.0f, 1.0f, 1.0f), g);
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float3 hdrColor = HDRScene.Sample(SceneSampler, Input.UV).rgb;
    float finalExposure = Exposure;

    if (EnableAutoExposure != 0)
    {
        float exposureEv = LogAverageLuminance.Load(int3(0, 0, 0)).r;
        finalExposure *= exp2(exposureEv);
	}

	float3 color = hdrColor * finalExposure;
    
    if (EnableTonemap != 0)
    {
        color = PBRNeutralToneMapping(color);
    }

	color = saturate(color);

    color = pow(color, 1.0f / max(Gamma, 1e-3f));
    return float4(color, 1.0f);
}
