cbuffer SsrBuildIndirectArgsConstants : register(b0)
{
    uint ThreadGroupSizeX;
    uint MaxRayCount;
};

cbuffer SsrBuildIndirectArgsBindlessConstants : register(b1)
{
    uint RayCounterIndex;
    uint IndirectArgsIndex;
};

[numthreads(1, 1, 1)]
void SsrBuildIndirectArgsCS(uint3 DispatchThreadId : SV_DispatchThreadID)
{
    if (RayCounterIndex == 0xFFFFFFFFu || IndirectArgsIndex == 0xFFFFFFFFu)
    {
        return;
    }

    ByteAddressBuffer RayCounter = ResourceDescriptorHeap[RayCounterIndex];
    RWByteAddressBuffer IndirectArgs = ResourceDescriptorHeap[IndirectArgsIndex];

    const uint rayCount = min(MaxRayCount, RayCounter.Load(0));
    const uint groupCountX = (rayCount + ThreadGroupSizeX - 1u) / ThreadGroupSizeX;

    IndirectArgs.Store(0, groupCountX);
    IndirectArgs.Store(4, 1u);
    IndirectArgs.Store(8, 1u);
}
