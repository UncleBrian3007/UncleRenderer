struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 View;
    row_major float4x4 Projection;
    float3 BaseColor;
    float Padding0;
    float3 LightDirection;
    float Padding1;
};

Texture2D GBufferA : register(t0);
Texture2D GBufferB : register(t1);
Texture2D GBufferC : register(t2);
SamplerState GBufferSampler : register(s0);

VSOutput VSMain(uint VertexId : SV_VertexID)
{
    float2 Positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };

    VSOutput Output;
    Output.Position = float4(Positions[VertexId], 0.0, 1.0);
    Output.UV = float2(Positions[VertexId].x * 0.5f + 0.5f, -Positions[VertexId].y * 0.5f + 0.5f);
    return Output;
}

float4 PSMain(VSOutput Input) : SV_Target
{
    float3 normal = normalize(GBufferA.Sample(GBufferSampler, Input.UV).xyz);
    float4 smr = GBufferB.Sample(GBufferSampler, Input.UV);
    float3 albedo = GBufferC.Sample(GBufferSampler, Input.UV).rgb;

    float3 lightDir = normalize(-LightDirection);
    float diffuse = saturate(dot(normal, lightDir));
    float3 ambient = albedo * 0.05;

    float3 lit = albedo * diffuse + ambient + smr.x.xxx * 0.1;
    return float4(lit, 1.0);
}
