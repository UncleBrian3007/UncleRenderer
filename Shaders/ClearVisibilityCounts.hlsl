#include "CullingConstants.hlsl"

cbuffer ClearCountsConstants : register(b1)
{
    uint Count0Index;
    uint Count1Index;
    uint ClearPadding0;
    uint ClearPadding1;
    uint ClearPadding2;
    uint ClearPadding3;
};

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWByteAddressBuffer Count0 = ResourceDescriptorHeap[Count0Index];
    RWByteAddressBuffer Count1 = ResourceDescriptorHeap[Count1Index];
    Count0.Store(0, 0);
    Count1.Store(0, 0);
}
