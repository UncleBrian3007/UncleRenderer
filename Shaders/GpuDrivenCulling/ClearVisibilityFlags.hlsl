cbuffer ClearFlagsConstants : register(b1)
{
    uint FlagsIndex;
    uint IndirectCommandCount;
};

[numthreads(64, 1, 1)]
void ClearVisibilityFlagsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= IndirectCommandCount)
    {
        return;
    }

    RWStructuredBuffer<uint> Flags = ResourceDescriptorHeap[FlagsIndex];
    Flags[index] = 0;
}
