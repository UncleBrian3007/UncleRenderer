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

cbuffer TonemapBindlessConstants : register(b1)
{
    uint HDRSceneIndex;
    uint LogAverageLuminanceIndex;
};

SamplerState SceneSampler : register(s0);

// Based on https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/pbrNeutral.glsl
    Texture2D HDRScene = ResourceDescriptorHeap[HDRSceneIndex];
    Texture2D LogAverageLuminance = ResourceDescriptorHeap[LogAverageLuminanceIndex];
// 원본 색상(Hue)과 채도(Saturation)를 최대한 유지하면서 밝은 하이라이트 부분만 자연스럽게 압축
float3 PBRNeutralToneMapping(float3 color)
{
    const float startCompression = 0.8f - 0.04f;
    const float desaturation = 0.15f;

	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f; // x - 6.25x^2 (x 가 0.08 보다 작아짐에 따라 0.04 부터 부드럽게 0 으로 감소) 
    color -= offset; // 확실한 Black 으로 눌러주는 효과, 회색이 되지 않게

    // 화면이 충분히 어둡다고 판단하면 톤매핑을 적용하지 않음, 중간 톤의 색상이 매우 정확하게 유지됨
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) // 0.76
    {
        return color;
    }

    // 하이라이트 압축
    const float d = 1.0f - startCompression;
	float newPeak = 1.0f - d * d / (peak + d - startCompression); // peak(0.76 ~ 100) 가 아무리 커도 newPeak (0.76 ~ 0.999) 사이의 값으로 수렴
    // (분모에 peak가 있어서 peak 무한대로 커지면 d^2/(peak..) 는 0에 가까워짐)

    color *= newPeak / max(peak, 1e-4f); // 색상 변경 없이 스케일링으로만 처리

    // 고휘도 탈색, 압축된 정도(peak-newPeak)가 클수록 색을 무채색(흰색/회색)으로 섞어줌
    // 빛이 너무 강해지면 눈이나 카메라는 색상을 잃고 하얗게 보이는 현상 시뮬레이션
    float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
    return lerp(color, newPeak * float3(1.0f, 1.0f, 1.0f), g);
}

float4 PSMain(VSOutput Input) : SV_Target
{
	Texture2D<float4> HDRScene = ResourceDescriptorHeap[HDRSceneIndex];
	Texture2D<float> LogAverageLuminance = ResourceDescriptorHeap[LogAverageLuminanceIndex];

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
