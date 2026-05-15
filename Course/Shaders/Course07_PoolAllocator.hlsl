#include "Common.hlsli"

// Root signature layout:
//   Param 0 : Root Constants  b0  (4 dwords: QueryCount, StackSize, StackCount, pad)
//   Param 1 : Root UAV        u0  Nodes
//   Param 2 : Root UAV        u1  Leaves
//   Param 3 : Root UAV        u2  Queries
//   Param 4 : Root UAV        u3  Counts   (output: per-query match count)
//   Param 5 : Root UAV        u4  StackBuf (pool-allocated warp stacks)
//   Param 6 : Root UAV        u5  Locks    (one int per pool slot; 0=free, 1=held)

cbuffer Constants : register(b0)
{
    uint QueryCount;
    uint StackSize;
    uint StackCount;
    uint Pad0;
};

RWStructuredBuffer<Node> Nodes    : register(u0);
RWStructuredBuffer<Leaf> Leaves   : register(u1);
RWStructuredBuffer<int>  Queries  : register(u2);
RWStructuredBuffer<int>  Counts   : register(u3);
RWStructuredBuffer<int>  StackBuf : register(u4);
RWStructuredBuffer<int>  Locks    : register(u5);

static const uint BlockSize = 1024;

// ============================================================
// Wave-level pool allocator (ported from Orochi's Stack class).
//
// The pool has StackCount slots, each wide enough for one wave.
// A wave spins, trying to claim a slot via InterlockedCompareExchange.
// On release, InterlockedExchange resets the lock to 0.
//
// Stack layout: StackBuf[slot * waveSize * StackSize + laneIdx + depth * waveSize]
// (SoA so that adjacent lanes access adjacent DRAM locations.)
// ============================================================

// ============================================================
// Each thread traverses the BST for its query value,
// counting leaves that match. Uses the wave-level pool stack.
// Mirrors CountKernel in the Orochi course.
// ============================================================
groupshared uint gs_WarpIndex;  // Global wave index for this block.

[numthreads(BlockSize, 1, 1)]
void CountCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    // Each wave computes its own global index.
    uint waveSize      = WaveGetLaneCount();
    uint wavesPerGroup = (BlockSize + waveSize - 1u) / waveSize;
    uint waveInGroup   = tid.x / waveSize;
    uint warpGlobal    = gid.x * wavesPerGroup + waveInGroup;

    uint index = dtid.x;
    if (index >= QueryCount) return;

    int query = Queries[index];
    int count = 0;

    // Acquire a pool slot for this wave's stack.
    uint stackBase;
    uint activeSlots = StackCount / waveSize;
    uint candidate   = warpGlobal % activeSlots;
    uint laneIdx     = WaveGetLaneIndex();

    uint warpHash = 0xFFFFFFFF;
    while (warpHash == 0xFFFFFFFF)
    {
        if (laneIdx == 0)
        {
            int prev;
            InterlockedCompareExchange(Locks[candidate], 0, 1, prev);
            if (prev == 0) warpHash = candidate;
        }
        candidate = (candidate + 1u) % activeSlots;
        warpHash  = WaveReadLaneAt(warpHash, 0);
    }
    stackBase = warpHash * waveSize * StackSize;

    // -- BST traversal with the per-wave stack --
    int  stackTop  = 0;
    int  stackBase_ = (int)stackBase;

    // Push root (index 0).
    StackBuf[stackBase_ + 0 * (int)waveSize + (int)laneIdx] = 0;
    stackTop = 1;

    while (stackTop > 0)
    {
        --stackTop;
        int nodeIndex = StackBuf[stackBase_ + stackTop * (int)waveSize + (int)laneIdx];

        if (!IsLeaf(nodeIndex))
        {
            Node node = Nodes[GetNodeAddr(nodeIndex)];
            if (node.Pivot <= query)
                StackBuf[stackBase_ + stackTop++ * (int)waveSize + (int)laneIdx] = node.RightIndex;
            if (node.Pivot >= query)
                StackBuf[stackBase_ + stackTop++ * (int)waveSize + (int)laneIdx] = node.LeftIndex;
        }
        else
        {
            Leaf lf = Leaves[GetNodeAddr(nodeIndex)];
            if (lf.Value == query) ++count;
        }
    }

    // Release pool slot.
    if (laneIdx == 0)
    {
        int dummy;
        InterlockedExchange(Locks[warpHash], 0, dummy);
    }

    Counts[index] = count;
}
