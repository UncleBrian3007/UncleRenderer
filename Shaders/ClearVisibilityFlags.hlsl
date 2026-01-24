#include "CullingConstants.hlsl"

cbuffer ClearFlagsConstants : register(b1)
{
    uint FlagsIndex;
    uint FlagsPadding0;
    uint FlagsPadding1;
    uint FlagsPadding2;
    uint FlagsPadding3;
    uint FlagsPadding4;
    uint FlagsPadding5;
    uint FlagsPadding6;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= IndirectCommandCount)
    {
        return;
    }

    RWStructuredBuffer<uint> Flags = ResourceDescriptorHeap[FlagsIndex];
    Flags[index] = 0;
}
