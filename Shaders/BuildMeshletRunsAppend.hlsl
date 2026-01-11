#include "CullingConstants.hlsl"

StructuredBuffer<uint> VisibleMeshlets : register(t0);
StructuredBuffer<uint4> MeshletDrawData : register(t1);
StructuredBuffer<uint> RangeOffsets : register(t2);
ByteAddressBuffer CommandTemplates : register(t3);
RWByteAddressBuffer OutputCommands : register(u0);
RWStructuredBuffer<uint> RunCounts : register(u1);

static const uint kCommandStride = 128;
static const uint kIndexCountOffset = 104;
static const uint kInstanceCountOffset = 108;
static const uint kStartIndexOffset = 112;

uint4 ReadMeshletDrawData(uint index)
{
    return MeshletDrawData[index];
}

bool IsSameRun(uint4 a, uint4 b)
{
    return a.z == b.z && a.w == b.w;
}

void CopyTemplate(uint srcIndex, uint dstIndex)
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

    if (VisibleMeshlets[index] == 0)
        return;

    uint4 current = ReadMeshletDrawData(index);
    bool isStart = true;
    if (index > 0)
    {
        if (VisibleMeshlets[index - 1] != 0)
        {
            uint4 previous = ReadMeshletDrawData(index - 1);
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

        uint4 nextData = ReadMeshletDrawData(runEnd + 1);
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
    CopyTemplate(index, outputIndex);

    uint baseOffset = outputIndex * kCommandStride;
    OutputCommands.Store(baseOffset + kIndexCountOffset, runIndexCount);
    OutputCommands.Store(baseOffset + kInstanceCountOffset, 1);
    OutputCommands.Store(baseOffset + kStartIndexOffset, current.x);
}
