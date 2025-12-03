struct VSOutput
{
    float4 Position : SV_Position;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
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

Texture2D GridTexture : register(t0);
SamplerState GridSampler : register(s0);

float4 PSMain(VSOutput Input) : SV_Target
{
    float3 n = normalize(Input.Normal);
    float3 l = normalize(-LightDirection);
    float diffuse = saturate(dot(n, l));

    float3 albedo = GridTexture.Sample(GridSampler, Input.UV).rgb * BaseColor;
    float3 ambient = albedo * 0.1;
    float3 lit = albedo * diffuse + ambient;
    return float4(lit, 1.0);
}
