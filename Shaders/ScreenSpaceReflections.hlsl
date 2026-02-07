#include "SceneConstants.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

cbuffer SSRConstants : register(b1)
{
    uint2 OutputSize;
    uint MaxSteps;
    float Thickness;
    float MaxDistance;
    float Stride;
    float RoughnessCutoff;
    float Intensity;
    uint UseHistory;
};

cbuffer SSRBindlessConstants : register(b2)
{
    uint GBufferAIndex;
    uint GBufferBIndex;
    uint LinearDepthIndex;
    uint SceneColorIndex;
    uint GBufferPointSamplerIndex;
    uint SceneColorLinearSamplerIndex;
};

VSOutput VSMain(uint VertexId : SV_VertexID)
{
    float2 Positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0f, 1.0f);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

float ReconstructViewZ(float depth)
{
    return Projection._43 / max(depth, 1e-6f);
}

float3 ReconstructViewPosition(float2 uv, float viewZ)
{
    float2 ndc = float2(uv * 2.0f - 1.0f);
    float viewX = ndc.x * viewZ / Projection._11;
    float viewY = -ndc.y * viewZ / Projection._22;
    return float3(viewX, viewY, viewZ);
}

float4 PSMain(VSOutput Input) : SV_Target
{
    if (UseHistory == 0)
    {
        return 0.0f;
    }

    Texture2D GBufferA = ResourceDescriptorHeap[GBufferAIndex];
    Texture2D GBufferB = ResourceDescriptorHeap[GBufferBIndex];
    Texture2D LinearDepth = ResourceDescriptorHeap[LinearDepthIndex];
    Texture2D SceneColor = ResourceDescriptorHeap[SceneColorIndex];
    SamplerState GBufferPointSampler = SamplerDescriptorHeap[GBufferPointSamplerIndex];
    SamplerState SceneColorLinearSampler = SamplerDescriptorHeap[SceneColorLinearSamplerIndex];

    float4 normalEncoded = GBufferA.Sample(GBufferPointSampler, Input.UV);
    float3 worldNormal = normalize(normalEncoded.xyz * 2.0f - 1.0f);
    float4 smr = GBufferB.Sample(GBufferPointSampler, Input.UV);
    float roughness = smr.z;

    if (roughness > RoughnessCutoff)
    {
        return 0.0f;
    }

    float viewZ = LinearDepth.SampleLevel(GBufferPointSampler, Input.UV, 0).r;
    if (viewZ <= 0.0f)
    {
        return 0.0f;
    }

    float3 viewPos = ReconstructViewPosition(Input.UV, viewZ);
    float3 viewDir = normalize(-viewPos);
    float3 viewNormal = normalize(mul(worldNormal, (float3x3)View));
    float3 rayDir = normalize(reflect(-viewDir, viewNormal));

    float3 rayPos = viewPos;
    float hitWeight = 0.0f;
    float3 hitColor = 0.0f;

    [loop]
    for (uint stepIndex = 0; stepIndex < MaxSteps; ++stepIndex)
    {
        rayPos += rayDir * Stride;

        float travel = length(rayPos - viewPos);
        if (travel > MaxDistance)
        {
            break;
        }

        float4 clip = mul(float4(rayPos, 1.0f), Projection);
        if (clip.w <= 0.0f)
        {
            break;
        }

        float2 uv = clip.xy / clip.w;
        uv = uv * 0.5f + 0.5f;
        uv.y = 1.0f - uv.y;

        if (any(uv < 0.0f) || any(uv > 1.0f))
        {
            break;
        }

        float sceneDepth = LinearDepth.SampleLevel(GBufferPointSampler, uv, 0).r;
        float depthDelta = rayPos.z - sceneDepth;
        float thickness = Thickness * (1.0f / max(abs(rayDir.z), 1e-3f));
        if (depthDelta >= 0.0f && depthDelta <= thickness)
        {
            float fade = 1.0f - saturate(travel / MaxDistance);
            hitColor = SceneColor.SampleLevel(SceneColorLinearSampler, uv, 0).rgb;
            hitWeight = fade;
            break;
        }
    }

    return float4(hitColor * Intensity, hitWeight);
}
