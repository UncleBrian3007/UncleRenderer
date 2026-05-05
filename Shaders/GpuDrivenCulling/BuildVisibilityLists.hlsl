#include "../CullingConstants.hlsl"

cbuffer VisibilityListConstants : register(b1)
{
    uint VisibilityIndex;
    uint VisibleListIndex;
    uint InvisibleListIndex;
    uint VisibleCountIndex;
    uint InvisibleCountIndex;
    uint ListPadding0;
};

[numthreads(64, 1, 1)]
void BuildVisibilityListsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= IndirectCommandCount)
        return;

    StructuredBuffer<uint> Visibility = ResourceDescriptorHeap[VisibilityIndex];
    uint visible = Visibility[index];

    if (visible != 0)
    {
        RWStructuredBuffer<uint> VisibleList = ResourceDescriptorHeap[VisibleListIndex];
        RWByteAddressBuffer VisibleCount = ResourceDescriptorHeap[VisibleCountIndex];
        uint writeIndex = 0;
        VisibleCount.InterlockedAdd(0, 1, writeIndex);
        VisibleList[writeIndex] = index;
    }
    else
    {
        RWStructuredBuffer<uint> InvisibleList = ResourceDescriptorHeap[InvisibleListIndex];
        RWByteAddressBuffer InvisibleCount = ResourceDescriptorHeap[InvisibleCountIndex];
        uint writeIndex = 0;
        InvisibleCount.InterlockedAdd(0, 1, writeIndex);
        InvisibleList[writeIndex] = index;
    }
}
