struct VSOutput
{
    float4 Position : SV_Position;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
};

float4 PSMain(VSOutput Input) : SV_Target
{
    float lighting = saturate(dot(normalize(Input.Normal), float3(0.0, 1.0, 0.0)));
    return float4(lighting.xxx, 1.0);
}
