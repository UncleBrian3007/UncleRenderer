#include "SceneConstants.hlsl"

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV       : TEXCOORD0;
};

cbuffer LinearDepthBindlessConstants : register(b1)
{
    uint DepthBufferIndex;
};
SamplerState DepthSampler : register(s0);

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

float PSMain(VSOutput Input) : SV_Target
{
    Texture2D<float> DepthBuffer = ResourceDescriptorHeap[DepthBufferIndex];
    float depth = DepthBuffer.Sample(DepthSampler, Input.UV).r;
    float viewZ = Projection._43 / max(depth, 1e-6f);
    return viewZ;
}
