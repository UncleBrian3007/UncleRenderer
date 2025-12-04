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
    float3 CameraPosition;
    float Padding2;
};

Texture2D GridTexture : register(t0);
SamplerState GridSampler : register(s0);

float4 PSMain(VSOutput Input) : SV_Target
{
    float3 albedo = GridTexture.Sample(GridSampler, Input.UV).rgb * BaseColor;
    float3 n = normalize(Input.Normal);
    float3 v = normalize(CameraPosition - Input.WorldPos);
    float3 l = normalize(-LightDirection);
    float3 h = normalize(v + l);

    float metallic = 0.0f;
    float roughness = 0.5f;
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    float NdotL = saturate(dot(n, l));
    float NdotV = saturate(dot(n, v));
    float NdotH = saturate(dot(n, h));
    float VdotH = saturate(dot(v, h));

    const float PI = 3.14159265f;
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = (NdotH * NdotH) * (alpha2 - 1.0f) + 1.0f;
    float D = alpha2 / max(PI * denom * denom, 1e-4f);

    float k = (roughness + 1.0f);
    k = (k * k) / 8.0f;
    float Gv = NdotV / (NdotV * (1.0f - k) + k);
    float Gl = NdotL / (NdotL * (1.0f - k) + k);
    float G = Gv * Gl;

    float3 F = F0 + (1.0f - F0) * pow(1.0f - VdotH, 5.0f);

    float3 specular = (D * G * F) / max(4.0f * NdotL * NdotV, 1e-4f);
    float3 kd = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kd * albedo / PI;

    float3 ambient = albedo * 0.03f;
    float3 color = (diffuse + specular) * NdotL + ambient;
    return float4(color, 1.0);
}
