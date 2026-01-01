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
    float WhitePoint;
    float Gamma;
    float AutoExposureKey;
    float AutoExposureMin;
    float AutoExposureMax;
};

Texture2D HDRScene : register(t0);
Texture2D LogAverageLuminance : register(t1);
SamplerState SceneSampler : register(s0);

float LpmCurve(float luminance, float hdrMax)
{
    const float a = 1.6f;
    const float d = 0.977f;
    const float midIn = 0.18f;
    const float midOut = 0.22f;
    const float b = pow(midIn, a) / midOut;

    float lumaNorm = luminance / max(hdrMax, 1e-4f);
    float mapped = pow(lumaNorm / (lumaNorm + b), d);
    return mapped * hdrMax;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float3 hdrColor = HDRScene.Sample(SceneSampler, Input.UV).rgb;
    float3 mappedColor = max(hdrColor, 0.0f);
    float exposure = Exposure;

    if (EnableAutoExposure != 0)
    {
        float exposureEv = LogAverageLuminance.Load(int3(0, 0, 0)).r;
        exposure *= exp2(exposureEv);
    }

    if (EnableTonemap != 0)
    {
        mappedColor *= exposure;

        const float3 LuminanceWeights = float3(0.2126f, 0.7152f, 0.0722f);
        float luminance = dot(mappedColor, LuminanceWeights);
        float ldrLuminance = LpmCurve(luminance, WhitePoint);

        float scale = ldrLuminance / max(luminance, 1e-4f);
        mappedColor *= scale;
        mappedColor = saturate(mappedColor);
    }
    else
    {
        mappedColor = saturate(mappedColor);
    }

    mappedColor = pow(mappedColor, 1.0f / max(Gamma, 1e-3f));
    return float4(mappedColor, 1.0f);
}
