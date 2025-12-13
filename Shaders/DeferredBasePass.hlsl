struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float4 Tangent  : TANGENT;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD0;
    float3 WorldPos : TEXCOORD1;
    float4 Tangent  : TEXCOORD2;
};

cbuffer SceneConstants : register(b0)
{
    row_major float4x4 World;
    row_major float4x4 View;
    row_major float4x4 Projection;
    float3 BaseColor;
    float LightIntensity;
    float3 LightDirection;
    float Padding1;
    float3 CameraPosition;
    float Padding2;
};

Texture2D AlbedoTexture : register(t0);
Texture2D MetallicRoughnessTexture : register(t1);
Texture2D NormalTexture : register(t2);
SamplerState AlbedoSampler : register(s0);

VSOutput VSMain(VSInput Input)
{
    VSOutput Output;
    float4 WorldPos = mul(float4(Input.Position, 1.0), World);
    float4 ViewPos = mul(WorldPos, View);
    Output.Position = mul(ViewPos, Projection);
    Output.Normal = mul(Input.Normal, (float3x3)World);
    Output.UV = Input.UV;
    Output.WorldPos = WorldPos.xyz;
    Output.Tangent = float4(normalize(mul(Input.Tangent.xyz, (float3x3)World)), Input.Tangent.w);
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

    float3 vertexNormal = normalize(Input.Normal);

    float3 tangent = Input.Tangent.xyz;
    const float tangentEpsilon = 1e-5f;
    bool bHasValidTangent = length(tangent) > tangentEpsilon;

    float3 bitangent = float3(0.0f, 0.0f, 0.0f);
    if (!bHasValidTangent)
    {
        float3 ddxPos = ddx(Input.WorldPos);
        float3 ddyPos = ddy(Input.WorldPos);
        float2 ddxUV = ddx(Input.UV);
        float2 ddyUV = ddy(Input.UV);

        tangent = ddxPos * ddyUV.y - ddyPos * ddxUV.y;
        float3 rawBitangent = ddyPos * ddxUV.x - ddxPos * ddyUV.x;
        bHasValidTangent = length(tangent) > tangentEpsilon && length(rawBitangent) > tangentEpsilon;

        tangent = bHasValidTangent ? normalize(tangent - vertexNormal * dot(vertexNormal, tangent)) : float3(0.0f, 0.0f, 0.0f);
        float handedness = dot(cross(vertexNormal, tangent), rawBitangent) < 0.0f ? -1.0f : 1.0f;
        bitangent = bHasValidTangent ? normalize(cross(vertexNormal, tangent)) * handedness : float3(0.0f, 0.0f, 0.0f);
    }
    else
    {
        tangent = normalize(tangent - vertexNormal * dot(vertexNormal, tangent));
        bitangent = normalize(cross(vertexNormal, tangent)) * Input.Tangent.w;
    }

    float3 tangentNormal = NormalTexture.Sample(AlbedoSampler, Input.UV).rgb * 2.0f - 1.0f;
    float tangentNormalLength = length(tangentNormal);
    tangentNormal = tangentNormalLength < tangentEpsilon ? float3(0.0f, 0.0f, 1.0f) : tangentNormal;

    float3x3 TBN = float3x3(tangent, bitangent, vertexNormal);
    float3 worldNormal = bHasValidTangent ? mul(tangentNormal, TBN) : vertexNormal;

    float3 viewNormal = normalize(mul(normalize(worldNormal), (float3x3)View));
    float3 albedo = AlbedoTexture.Sample(AlbedoSampler, Input.UV).rgb * BaseColor;

    float viewDepth = -mul(float4(Input.WorldPos, 1.0), View).z;
    Output.GBufferA = float4(viewNormal, viewDepth);

    const float specular = 0.04f;
    float2 metallicRoughness = MetallicRoughnessTexture.Sample(AlbedoSampler, Input.UV).bg;
    Output.GBufferB = float4(specular, metallicRoughness.x, metallicRoughness.y, 1.0);

    Output.GBufferC = float4(albedo, 1.0);
    return Output;
}
