cbuffer MeshletRunBindlessConstants : register(b1)
{
    uint VisibleMeshletsIndex;
    uint MeshletDrawDataIndex;
    uint RangeOffsetsIndex;
    uint CommandTemplatesIndex;
    uint OutputCommandsIndex;
    uint RunCountsIndex;
    uint IndirectCommandCount;
    uint RangeCount;
};

[numthreads(64, 1, 1)]
void ClearMeshletRunCountsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> RunCounts = ResourceDescriptorHeap[RunCountsIndex];
    uint index = dispatchThreadId.x;
    if (index >= RangeCount)
        return;

    RunCounts[index] = 0;
}
