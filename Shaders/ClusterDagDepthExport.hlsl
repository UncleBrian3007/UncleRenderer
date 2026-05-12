#include "ClusterDag/ClusterDagCommon.hlsl"

cbuffer ClusterDagDepthExportConstants : register(b0)
{
    uint Visibility64TextureIndex;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

VSOutput ClusterDagDepthExportVS(uint vertexId : SV_VertexID)
{
    VSOutput Output;
    Output.UV = float2((vertexId << 1) & 2, vertexId & 2);
    Output.Position = float4(Output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return Output;
}

float ClusterDagDepthExportPS(VSOutput Input) : SV_Depth
{
    Texture2D<uint2> Visibility64Texture = ResourceDescriptorHeap[Visibility64TextureIndex];
    const uint2 visibility = Visibility64Texture.Load(int3(int2(Input.Position.xy), 0));
    if (visibility.x == 0u)
    {
        clip(-1.0f);
    }

    return asfloat(visibility.y);
}
