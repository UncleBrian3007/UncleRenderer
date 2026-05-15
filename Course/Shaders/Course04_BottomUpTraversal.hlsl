#include "Common.hlsli"

// Root signature layout:
//   Param 0 : Root Constants  b0  (4 dwords: LeafCount, pad x3)
//   Param 1 : Root UAV        u0  Nodes    (RWStructuredBuffer<Node>)
//   Param 2 : Root UAV        u1  Leaves   (RWStructuredBuffer<Leaf>)
//   Param 3 : Root UAV        u2  Sums     (RWStructuredBuffer<int>)
//   Param 4 : Root UAV        u3  Counters (RWStructuredBuffer<int>, atomic counters per internal node)

cbuffer Constants : register(b0)
{
    uint LeafCount;
    uint Pad0, Pad1, Pad2;
};

RWStructuredBuffer<Node> Nodes    : register(u0);
RWStructuredBuffer<Leaf> Leaves   : register(u1);
RWStructuredBuffer<int>  Sums     : register(u2);
RWStructuredBuffer<int>  Counters : register(u3);

static const uint BlockSize = 1024;

// ============================================================
// Bottom-up tree traversal with atomic "second-arrival" gate.
//
// Each thread starts at a leaf and walks upward.
// The first thread to reach a node atomically increments its counter
// and sees 0 (old value) -> it exits; both children haven't finished yet.
// The second thread sees 1 -> it proceeds to compute and propagate the sum.
//
// Mirrors BottomUpTraversalKernel.
// ============================================================
[numthreads(BlockSize, 1, 1)]
void BottomUpTraversalCS(uint3 dtid : SV_DispatchThreadID)
{
    int index = (int)dtid.x;
    if (index >= (int)LeafCount) return;

    // Start at the leaf's parent.
    Leaf leaf = Leaves[index];
    index = leaf.ParentAddr;

    // Walk up until we reach the root sentinel (-1) or we are the first to arrive.
    while (index >= 0)
    {
        int prev;
        InterlockedAdd(Counters[index], 1, prev);
        if (prev == 0) break;   // First arrival: partner hasn't finished yet.

        // Second arrival: both children are done. Read their results.
        DeviceMemoryBarrier();  // Ensure all writes from children are visible.

        Node node = Nodes[index];

        int leftAddr  = NodeGetLeftAddr(node);
        int rightAddr = NodeGetRightAddr(node);

        int sum = 0;
        sum += NodeIsLeftLeaf(node)  ? Leaves[leftAddr].Value  : Sums[leftAddr];
        sum += NodeIsRightLeaf(node) ? Leaves[rightAddr].Value : Sums[rightAddr];

        Sums[index] = sum;
        DeviceMemoryBarrier();  // Ensure our write is visible before parent reads it.

        index = node.ParentAddr;
    }
}
