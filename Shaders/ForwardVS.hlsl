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
    float3 CameraPosition;
    float Padding2;
};

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
