#include "CullingConstants.hlsl"

cbuffer MeshletRunBindlessConstants : register(b1)
{
    uint VisibleMeshletsIndex;
    uint MeshletDrawDataIndex;
    uint RangeOffsetsIndex;
    uint CommandTemplatesIndex;
    uint OutputCommandsIndex;
    uint RunCountsIndex;
};

static const uint kCommandStride = 32;
static const uint kIndexCountOffset = 8;
static const uint kInstanceCountOffset = 12;
static const uint kStartIndexOffset = 16;

uint4 ReadMeshletDrawData(uint index, StructuredBuffer<uint4> MeshletDrawData)
{
    return MeshletDrawData[index];
}

bool IsSameRun(uint4 a, uint4 b)
{
    return a.z == b.z && a.w == b.w;
}

void CopyTemplate(uint srcIndex, uint dstIndex, ByteAddressBuffer CommandTemplates, RWByteAddressBuffer OutputCommands)
{
    uint srcBase = srcIndex * kCommandStride;
    uint dstBase = dstIndex * kCommandStride;
    [unroll]
    for (uint i = 0; i < kCommandStride / 16; ++i)
    {
        uint4 values = CommandTemplates.Load4(srcBase + i * 16);
        OutputCommands.Store4(dstBase + i * 16, values);
    }
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= IndirectCommandCount)
        return;

    StructuredBuffer<uint> VisibleMeshlets = ResourceDescriptorHeap[VisibleMeshletsIndex];
    StructuredBuffer<uint4> MeshletDrawData = ResourceDescriptorHeap[MeshletDrawDataIndex];
    StructuredBuffer<uint> RangeOffsets = ResourceDescriptorHeap[RangeOffsetsIndex];
    ByteAddressBuffer CommandTemplates = ResourceDescriptorHeap[CommandTemplatesIndex];
    RWByteAddressBuffer OutputCommands = ResourceDescriptorHeap[OutputCommandsIndex];
    RWStructuredBuffer<uint> RunCounts = ResourceDescriptorHeap[RunCountsIndex];

    if (VisibleMeshlets[index] == 0)
        return;

    uint4 current = ReadMeshletDrawData(index, MeshletDrawData);
    bool isStart = true;
    if (index > 0)
    {
        if (VisibleMeshlets[index - 1] != 0)
        {
            uint4 previous = ReadMeshletDrawData(index - 1, MeshletDrawData);
            if (IsSameRun(current, previous))
            {
                isStart = false;
            }
        }
    }

    if (!isStart)
        return;

    uint runEnd = index;
    uint4 runLast = current;
    while (runEnd + 1 < IndirectCommandCount)
    {
        if (VisibleMeshlets[runEnd + 1] == 0)
        {
            break;
        }

        uint4 nextData = ReadMeshletDrawData(runEnd + 1, MeshletDrawData);
        if (!IsSameRun(current, nextData))
        {
            break;
        }

        runEnd += 1;
        runLast = nextData;
    }

    uint runIndexCount = (runLast.x + runLast.y) - current.x;
    uint rangeIndex = current.z;
    uint runOffset = 0;
    InterlockedAdd(RunCounts[rangeIndex], 1, runOffset);

    uint outputIndex = RangeOffsets[rangeIndex] + runOffset;
    CopyTemplate(index, outputIndex, CommandTemplates, OutputCommands);

    uint baseOffset = outputIndex * kCommandStride;
    OutputCommands.Store(baseOffset + kIndexCountOffset, runIndexCount);
    OutputCommands.Store(baseOffset + kInstanceCountOffset, 1);
    OutputCommands.Store(baseOffset + kStartIndexOffset, current.x);
}
