#include "Common.hlsli"

// Root signature layout:
//   Param 0 : Root Constants  b0  (4 dwords: Size, pad x3)
//   Param 1 : Root UAV        u0  Input     (sorted int array)
//   Param 2 : Root UAV        u1  TaskQueue (int per node slot; 1=pending, 0=done)
//   Param 3 : Root UAV        u2  Counters  ([0]=internal node count, [1]=leaf count)
//   Param 4 : Root UAV        u3  Nodes
//   (Leaves are a sub-region of Nodes buffer for simplicity; see C++ host code)

cbuffer Constants : register(b0)
{
    uint Size;
    uint Pad0, Pad1, Pad2;
};

RWStructuredBuffer<int>  Input     : register(u0);
RWStructuredBuffer<int>  TaskQueue : register(u1);
RWStructuredBuffer<int>  Counters  : register(u2);  // [0]=nodeAlloc [1]=leafCount
RWStructuredBuffer<Node> Nodes     : register(u3);
RWStructuredBuffer<Leaf> Leaves    : register(u4);

static const uint BlockSize = 1024;

// ============================================================
// Waterfall-scheme parallel binary-tree builder.
//
// All threads spin until all leaves have been generated.
// Each thread picks up a task (a pending node) from the queue,
// processes it (splits into children), and writes new tasks.
//
// WaveActiveAllTrue(done) replaces __all(done): the wave exits only
// when every active lane has finished its work.
//
// Mirrors BuildTree in the Orochi course.
// ============================================================
[numthreads(BlockSize, 1, 1)]
void BuildTreeCS(uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    bool done  = false;

    // Spin until all leaves have been emitted.
    while (true)
    {
        // Relaxed read of the global leaf counter.
        int leafCount;
        InterlockedAdd(Counters[1], 0, leafCount);
        if ((uint)leafCount >= Size) break;

        DeviceMemoryBarrier();

        if (index >= Size - 1u)
        {
            // Threads beyond the internal-node range still participate in the
            // WaveActiveAllTrue check so they must loop; they do no real work.
            if (WaveActiveAllTrue(done)) break;
            continue;
        }

        int task = TaskQueue[index];
        if (task != 0 && !done)
        {
            Node node = Nodes[index];
            int l = node.LeftIndex;
            int r = node.RightIndex;
            int m = (l + r) / 2;
            node.Pivot = Input[m];

            // Count how many children are internal nodes (vs leaves).
            uint internalCount = 0;
            if (m - l > 1) ++internalCount;
            if (r - m > 1) ++internalCount;

            int childOffset;
            InterlockedAdd(Counters[0], (int)internalCount, childOffset);

            // Left child
            if (m - l > 1)
            {
                int childIdx = childOffset++;
                node.LeftIndex = childIdx;
                Node child;
                child.LeftIndex  = l;
                child.RightIndex = m;
                child.ParentAddr = (int)index;
                child.Pivot      = 0;
                Nodes[childIdx]    = child;
                DeviceMemoryBarrier();
                TaskQueue[childIdx] = 1;
            }
            else
            {
                node.LeftIndex = ~l;
                Leaf lf;
                lf.Value      = Input[l];
                lf.ParentAddr = (int)index;
                Leaves[l] = lf;
            }

            // Right child
            if (r - m > 1)
            {
                int childIdx = childOffset;
                node.RightIndex = childIdx;
                Node child;
                child.LeftIndex  = m;
                child.RightIndex = r;
                child.ParentAddr = (int)index;
                child.Pivot      = 0;
                Nodes[childIdx]    = child;
                DeviceMemoryBarrier();
                TaskQueue[childIdx] = 1;
            }
            else
            {
                node.RightIndex = ~m;
                Leaf lf;
                lf.Value      = Input[m];
                lf.ParentAddr = (int)index;
                Leaves[m] = lf;
            }

            Nodes[index] = node;
            InterlockedAdd(Counters[1], (int)(2u - internalCount), childOffset);
            done = true;
        }

        // Exit only when the entire wave is done.
        if (WaveActiveAllTrue(done)) break;
    }
}
