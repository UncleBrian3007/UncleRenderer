struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
};

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

VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    float4 WorldPos = mul(float4(Input.Position, 1.0), World);
    float4 ViewPos = mul(WorldPos, View);
    Output.Position = mul(ViewPos, Projection);
    Output.Normal = mul(Input.Normal, (float3x3)World);
    Output.UV = Input.UV;
    Output.WorldPos = WorldPos.xyz;
    return Output;
}

struct PSOutput
{
    float4 GBufferA : SV_Target0; // Normal
    float4 GBufferB : SV_Target1; // Specular/Metallic/Roughness
    float4 GBufferC : SV_Target2; // BaseColor
};

PSOutput PSMain(VSOutput Input)
{
    PSOutput Output;

    float3 normal = normalize(Input.Normal);
    float3 albedo = GridTexture.Sample(GridSampler, Input.UV).rgb * BaseColor;

    Output.GBufferA = float4(normalize(normal), 1.0);

    const float specular = 0.04f;
    const float metallic = 0.0f;
    const float roughness = 0.5f;
    Output.GBufferB = float4(specular, metallic, roughness, 1.0);

    Output.GBufferC = float4(albedo, 1.0);
    return Output;
}
