#include "CullingConstants.hlsl"

RWStructuredBuffer<uint> RunCounts : register(u1);

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= RangeCount)
        return;

    RunCounts[index] = 0;
}
