#define A_GPU
#define A_HLSL
#define SPD_LINEAR_SAMPLER

#include "../ffx_a.h"

cbuffer EnvSpdConstants : register(b0)
{
    uint InputSrvIndex;
    uint AtomicCounterUavIndex;
    uint SourceWidth;
    uint SourceHeight;
    uint MipCount;
    uint NumWorkGroups;
    uint FaceCount;
    uint SamplerIndex;
    uint4 OutputUavIndices[12];
}

groupshared AF4 SpdIntermediate[16][16];
groupshared AU1 SpdCounter;

float3 ComputeCubeDirection(uint Face, float2 Uv)
{
    float U = Uv.x * 2.0f - 1.0f;
    float V = 1.0f - Uv.y * 2.0f;

    if (Face == 0u) return float3(1.0f, V, -U);
    if (Face == 1u) return float3(-1.0f, V, U);
    if (Face == 2u) return float3(U, 1.0f, -V);
    if (Face == 3u) return float3(U, -1.0f, V);
    if (Face == 4u) return float3(U, V, 1.0f);
    return float3(-U, V, -1.0f);
}

AF4 SpdLoadSourceImage(ASU2 Texel, AU1 Slice)
{
    TextureCube<float4> InputCube = ResourceDescriptorHeap[InputSrvIndex];
    SamplerState LinearSampler = SamplerDescriptorHeap[SamplerIndex];
    float2 InvSourceSize = 1.0f / float2(max(1u, SourceWidth), max(1u, SourceHeight));
    float2 Uv = float2(Texel) * InvSourceSize + InvSourceSize;
    float3 Direction = ComputeCubeDirection(Slice, Uv);
    return AF4(InputCube.SampleLevel(LinearSampler, Direction, 0.0f));
}

AF4 SpdLoad(ASU2 Texel, AU1 Slice)
{
    globallycoherent RWTexture2DArray<float4> Mip5 = ResourceDescriptorHeap[OutputUavIndices[5].x];
    return AF4(Mip5[uint3(uint2(Texel), Slice)]);
}

void SpdStore(ASU2 Pixel, AF4 Value, AU1 Mip, AU1 Slice)
{
    if (Mip == 5u)
    {
        globallycoherent RWTexture2DArray<float4> Mip5 = ResourceDescriptorHeap[OutputUavIndices[5].x];
        Mip5[uint3(uint2(Pixel), Slice)] = float4(Value.xyz, 1.0f);
        return;
    }

    RWTexture2DArray<float4> OutputMip = ResourceDescriptorHeap[OutputUavIndices[Mip].x];
    OutputMip[uint3(uint2(Pixel), Slice)] = float4(Value.xyz, 1.0f);
}

void SpdIncreaseAtomicCounter(AU1 Slice)
{
    globallycoherent RWByteAddressBuffer SpdAtomic = ResourceDescriptorHeap[AtomicCounterUavIndex];
    SpdAtomic.InterlockedAdd(Slice * 4u, 1u, SpdCounter);
}

AU1 SpdGetAtomicCounter()
{
    return SpdCounter;
}

void SpdResetAtomicCounter(AU1 Slice)
{
    globallycoherent RWByteAddressBuffer SpdAtomic = ResourceDescriptorHeap[AtomicCounterUavIndex];
    SpdAtomic.Store(Slice * 4u, 0u);
}

AF4 SpdLoadIntermediate(AU1 X, AU1 Y)
{
    return SpdIntermediate[X][Y];
}

void SpdStoreIntermediate(AU1 X, AU1 Y, AF4 Value)
{
    SpdIntermediate[X][Y] = Value;
}

AF4 SpdReduce4(AF4 V0, AF4 V1, AF4 V2, AF4 V3)
{
    return (V0 + V1 + V2 + V3) * 0.25f;
}

#include "../ffx_spd.h"

[numthreads(256, 1, 1)]
void EnvCubeMipGenCS(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    if (WorkGroupId.z >= FaceCount || MipCount == 0u)
    {
        return;
    }

    SpdDownsample(AU2(WorkGroupId.xy), AU1(LocalThreadIndex), AU1(MipCount), AU1(NumWorkGroups), AU1(WorkGroupId.z));
}
