#include "../SceneConstants.hlsl"

#define A_GPU
#define A_HLSL
#include "../ffx_a.h"

cbuffer RestirGiSpdConstants : register(b0)
{
    uint InputSrvIndex;
    uint AtomicCounterUavIndex;
    uint OutputUavMip0;
    uint OutputUavMip1;
    uint OutputUavMip2;
    uint OutputUavMip3;
    uint Mips;
    uint NumWorkGroups;
    uint2 WorkGroupOffset;
};

groupshared AF4 SpdIntermediate[16][16];
groupshared AU1 SpdCounter;

AF4 MakeWeightedDepth(float Depth)
{
    return AF4(Depth, (Depth > 0.0f) ? 1.0f : 0.0f, 0.0f, 0.0f);
}

AF4 SpdLoadSourceImage(ASU2 Texel, AU1 Slice)
{
    Texture2D<float> InputDepth = ResourceDescriptorHeap[InputSrvIndex];
    return MakeWeightedDepth(InputDepth[uint2(Texel)]);
}

AF4 SpdLoad(ASU2 Texel, AU1 Slice)
{
    globallycoherent RWTexture2D<float> Mip3 = ResourceDescriptorHeap[OutputUavMip3];
    return MakeWeightedDepth(Mip3[uint2(Texel)]);
}

void SpdStore(ASU2 Pixel, AF4 Value, AU1 Mip, AU1 Slice)
{
    uint TargetIndex = OutputUavMip0;
    if (Mip == 1u) { TargetIndex = OutputUavMip1; }
    else if (Mip == 2u) { TargetIndex = OutputUavMip2; }
    else if (Mip >= 3u) { TargetIndex = OutputUavMip3; }

    RWTexture2D<float> OutputMip = ResourceDescriptorHeap[TargetIndex];
    OutputMip[uint2(Pixel)] = Value.x;
}

void SpdIncreaseAtomicCounter(AU1 Slice)
{
    globallycoherent RWByteAddressBuffer SpdAtomic = ResourceDescriptorHeap[AtomicCounterUavIndex];
    SpdAtomic.InterlockedAdd(0u, 1u, SpdCounter);
}

AU1 SpdGetAtomicCounter()
{
    return SpdCounter;
}

void SpdResetAtomicCounter(AU1 Slice)
{
    globallycoherent RWByteAddressBuffer SpdAtomic = ResourceDescriptorHeap[AtomicCounterUavIndex];
    SpdAtomic.Store(0u, 0u);
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
    const float ValidWeightSum = V0.y + V1.y + V2.y + V3.y;
    const float ValidAverageDepth =
        (V0.x * V0.y + V1.x * V1.y + V2.x * V2.y + V3.x * V3.y) / max(ValidWeightSum, 1e-5f);
    return AF4(ValidAverageDepth, ValidWeightSum * 0.25f, 0.0f, 0.0f);
}

#include "../ffx_spd.h"

[numthreads(256, 1, 1)]
void CSGenerateLinearDepthMipsSpd(uint3 WorkGroupId : SV_GroupID, uint LocalThreadIndex : SV_GroupIndex)
{
    if (Mips == 0u)
    {
        return;
    }

    SpdDownsample(AU2(WorkGroupId.xy), AU1(LocalThreadIndex), AU1(Mips), AU1(NumWorkGroups), 0u, AU2(WorkGroupOffset));
}
