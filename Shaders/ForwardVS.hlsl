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
};

VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    Output.Position = float4(Input.Position, 1.0);
    Output.Normal = Input.Normal;
    Output.UV = Input.UV;
    return Output;
}
