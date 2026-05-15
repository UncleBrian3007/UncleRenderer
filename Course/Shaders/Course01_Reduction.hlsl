#include "WaveCommon.hlsli"

// Root signature layout (shared by all Course01 kernels):
//   Param 0 : Root Constants  b0  (4 dwords)
//   Param 1 : Root UAV        u0  Input  (RWStructuredBuffer<int>)
//   Param 2 : Root UAV        u1  Output (RWStructuredBuffer<int>, Output[0] accumulates via InterlockedAdd)

cbuffer Constants : register(b0)
{
    uint InputSize;
    uint Pad0, Pad1, Pad2;
};

RWStructuredBuffer<int> Input  : register(u0);
RWStructuredBuffer<int> Output : register(u1);

static const uint BlockSize = 1024;

// ============================================================
// Kernel 1: Block-wise reduction via groupshared memory tree.
// Mirrors ReduceBlockKernel in the Orochi course.
// ============================================================
groupshared int gs_Block[BlockSize];

[numthreads(BlockSize, 1, 1)]
void ReduceBlockCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    int  val   = (index < InputSize) ? Input[index] : 0;

    gs_Block[tid.x] = val;
    GroupMemoryBarrierWithGroupSync();

    // Log2(BlockSize) rounds; each thread combines with its XOR partner.
    for (uint stride = 1; stride < BlockSize; stride <<= 1)
    {
        uint other = tid.x ^ stride;
        if (tid.x < other)
            gs_Block[tid.x] += gs_Block[other];
        GroupMemoryBarrierWithGroupSync();
    }

    // Thread 0 holds the block sum; atomically accumulate into global output.
    if (tid.x == 0)
        InterlockedAdd(Output[0], gs_Block[0]);
}

// ============================================================
// Kernel 2: Wave-wise reduction via WaveActiveSum.
// Mirrors ReduceWarpKernel in the Orochi course.
// ============================================================
[numthreads(BlockSize, 1, 1)]
void ReduceWaveCS(uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    int  val   = (index < InputSize) ? Input[index] : 0;

    // Sum across all lanes in the wave.
    int waveSum = WaveActiveSum(val);

    // Only the first lane in each wave contributes to avoid double-counting.
    if (WaveIsFirstLane())
        InterlockedAdd(Output[0], waveSum);
}
