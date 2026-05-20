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

static const uint kCommandStride = 32;

// IndirectArgs[0] = CONSTANT_BUFFER_VIEW   → 8 bytes (GPU VA)
// IndirectArgs[1] = CONSTANT (1개)         → 4 bytes ← DrawIndexStart (b2 register)
// IndirectArgs[2] = DRAW                   → 16 bytes (D3D12_DRAW_ARGUMENTS)

// 0	ConstantBufferAddress (8 bytes)
// 8	DrawIndexStart (b2 shader constant)
// 12	DrawArguments.VertexCountPerInstance
// 16	DrawArguments.InstanceCount
// 20	DrawArguments.StartVertexLocation
// 24	DrawArguments.StartInstanceLocation 

// Updated FIndirectDrawCommand layout:
// IndirectArgs[0] = CONSTANT_BUFFER_VIEW   8 bytes (GPU VA)
// IndirectArgs[1] = CONSTANT (2)           8 bytes, DrawIndexStart + DrawDataIndex (b2 register)
// IndirectArgs[2] = DRAW                   16 bytes (D3D12_DRAW_ARGUMENTS)
//
// 0  ConstantBufferAddress (8 bytes)
// 8  DrawIndexStart (b2 shader constant, dword 0)
// 12 DrawDataIndex (b2 shader constant, dword 1)
// 16 DrawArguments.VertexCountPerInstance
// 20 DrawArguments.InstanceCount
// 24 DrawArguments.StartVertexLocation
// 28 DrawArguments.StartInstanceLocation
static const uint kStartIndexOffset = 8;
static const uint kIndexCountOffset = 16;
static const uint kInstanceCountOffset = 20;

/*
struct FMeshletDrawData   // → shader에서 uint4로 읽힘
{
    uint32_t StartIndex;   // .x  인덱스 버퍼에서 이 meshlet이 시작하는 오프셋
    uint32_t IndexCount;   // .y  이 meshlet의 인덱스 개수
    uint32_t RangeIndex;   // .z  어느 draw range(재질/파이프라인 그룹)에 속하는지
    uint32_t GroupIndex;   // .w  그 range 안에서의 그룹 번호
};
*/
uint4 ReadMeshletDrawData(uint index, StructuredBuffer<uint4> MeshletDrawData)
{
    return MeshletDrawData[index];
}

bool IsSameRun(uint4 a, uint4 b)
{
    return a.z == b.z && a.w == b.w;  // RangeIndex + GroupIndex가 같으면 같은 draw로 병합 가능
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
void BuildMeshletRunsAppendCS(uint3 dispatchThreadId : SV_DispatchThreadID)
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

    uint runIndexCount = (runLast.x + runLast.y) - current.x;  // 런 전체 인덱스 수
    uint rangeIndex = current.z;
    uint runOffset = 0;
    InterlockedAdd(RunCounts[rangeIndex], 1, runOffset);

    uint outputIndex = RangeOffsets[rangeIndex] + runOffset;
    CopyTemplate(index, outputIndex, CommandTemplates, OutputCommands);

    uint baseOffset = outputIndex * kCommandStride;
    OutputCommands.Store(baseOffset + kIndexCountOffset, runIndexCount);
    OutputCommands.Store(baseOffset + kInstanceCountOffset, 1);
    OutputCommands.Store(baseOffset + kStartIndexOffset, current.x);  // 런 시작의 StartIndex
}
