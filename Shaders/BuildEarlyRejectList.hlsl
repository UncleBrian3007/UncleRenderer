#include "CullingConstants.hlsl"

cbuffer EarlyRejectConstants : register(b1)
{
    uint VisibilityIndex;
    uint RejectListIndex;
    uint RejectCountIndex;
    uint RejectPadding0;
    uint RejectPadding1;
    uint RejectPadding2;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= IndirectCommandCount)
        return;

    StructuredBuffer<uint> Visibility = ResourceDescriptorHeap[VisibilityIndex];
    if (Visibility[index] != 0)
        return;

    RWStructuredBuffer<uint> RejectList = ResourceDescriptorHeap[RejectListIndex];
    RWByteAddressBuffer RejectCount = ResourceDescriptorHeap[RejectCountIndex];
    uint writeIndex = 0;
    RejectCount.InterlockedAdd(0, 1, writeIndex);
    RejectList[writeIndex] = index;
}
