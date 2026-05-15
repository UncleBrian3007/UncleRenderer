#include "Course04_BottomUpTraversal.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <stack>
#include <vector>

// Matches the GPU-side structs in Common.hlsli.
struct Node
{
    int LeftIndex;
    int RightIndex;
    int ParentAddr;
    int Pivot;
};

struct Leaf
{
    int Value;
    int ParentAddr;
};

namespace
{
    // CPU binary-search-tree builder (mirrors Orochi's TreeBuilder).
    struct Tree
    {
        std::vector<Node> Nodes;
        std::vector<Leaf> Leaves;
    };

    Tree BuildTree(std::vector<int> Input)
    {
        std::sort(Input.begin(), Input.end());
        uint32_t N = (uint32_t)Input.size();

        Tree T;
        T.Nodes.resize(N);
        T.Leaves.resize(N);

        Node Root = { 0, (int)N, -1, 0 };
        T.Nodes[0] = Root;
        int NodeCount = 1;

        std::stack<int> Stack;
        Stack.push(0);

        while (!Stack.empty())
        {
            int Idx = Stack.top(); 
            Stack.pop();
            Node& Nd = T.Nodes[Idx];
            int l = Nd.LeftIndex, r = Nd.RightIndex, m = (l + r) / 2;
            Nd.Pivot = Input[m];

            if (m - l > 1)
            {
                int c = NodeCount++;
                Nd.LeftIndex  = c;
                T.Nodes[c]    = { l, m, Idx, 0 };
                Stack.push(c);
            }
            else
            {
                Nd.LeftIndex = ~l;
                T.Leaves[l]  = { Input[l], Idx };
            }

            if (r - m > 1)
            {
                int c = NodeCount++;
                Nd.RightIndex = c;
                T.Nodes[c]    = { m, r, Idx, 0 };
                Stack.push(c);
            }
            else
            {
                Nd.RightIndex = ~m;
                T.Leaves[m]   = { Input[m], Idx };
            }
        }
        return T;
    }

    // CPU reference: sum of all leaf values.
    int CpuLeafSum(const Tree& T)
    {
        int Sum = 0;
        for (const Leaf& L : T.Leaves) Sum += L.Value;
        return Sum;
    }
}

void Course04_Run(CourseRunner& Runner)
{
    printf("\n=== Course 04: Bottom-Up Tree Traversal ===\n");

    std::vector<uint8_t> BC;
    if (!Runner.CompileCS(L"Course/Shaders/Course04_BottomUpTraversal.hlsl", L"BottomUpTraversalCS", BC))
    {
        fprintf(stderr, "[Course04] Shader compilation failed.\n");
        return;
    }

    // RS: 4 dwords constants + 4 UAVs (Nodes, Leaves, Sums, Counters).
    auto RS  = Runner.CreateRootSignature(4, 4);
    auto PSO = Runner.CreateComputePSO(RS.Get(), BC);

    std::mt19937 Rng(99);
    std::uniform_int_distribution<int> Dist(0, 100);

    for (uint32_t N : { 16u, 64u, 1024u, 8192u })
    {
        std::vector<int> Input(N);
        for (int& V : Input) V = Dist(Rng);

        Tree T   = BuildTree(Input);
        int  Ref = CpuLeafSum(T);

        uint32_t NodeCount = (uint32_t)T.Nodes.size();
        uint32_t LeafCount = (uint32_t)T.Leaves.size();

        GpuBuffer NodeBuf  = Runner.CreateBuffer(NodeCount * sizeof(Node), L"Nodes");
        GpuBuffer LeafBuf  = Runner.CreateBuffer(LeafCount * sizeof(Leaf), L"Leaves");
        GpuBuffer SumBuf   = Runner.CreateBuffer(NodeCount * sizeof(int),  L"Sums");
        GpuBuffer CntBuf   = Runner.CreateBuffer(NodeCount * sizeof(int),  L"Counters");

        Runner.Upload(NodeBuf, T.Nodes.data(),  NodeCount * sizeof(Node));
        Runner.Upload(LeafBuf, T.Leaves.data(), LeafCount * sizeof(Leaf));
        Runner.ZeroBuffer(SumBuf, NodeCount * sizeof(int));
        Runner.ZeroBuffer(CntBuf, NodeCount * sizeof(int));

        struct Constants { uint32_t LeafCount, P0, P1, P2; };
        Constants C = { LeafCount, 0, 0, 0 };

        static constexpr uint32_t kBlockSize = 1024;
        uint32_t Groups = (LeafCount + kBlockSize - 1u) / kBlockSize;

        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS.Get();
        Desc.PSO            = PSO.Get();
        Desc.GroupsX        = Groups;
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0] = NodeBuf.VA();
        Desc.Uavs[1] = LeafBuf.VA();
        Desc.Uavs[2] = SumBuf.VA();
        Desc.Uavs[3] = CntBuf.VA();
        Desc.NumUavs = 4;
        Desc.Label   = L"BottomUpTraversalCS";
        Runner.Dispatch(Desc);
        Runner.Flush();

        // Root sum is stored at Sums[0].
        GpuBuffer RB = Runner.CreateReadbackBuffer(sizeof(int), L"SumRB");
        Runner.Readback(RB, SumBuf, sizeof(int));
        const int* Ptr = reinterpret_cast<const int*>(Runner.MapReadback(RB));
        int GpuSum = *Ptr;
        Runner.UnmapReadback(RB);

        printf("  N=%5u  GPU=%d  CPU=%d  %s\n", N, GpuSum, Ref, GpuSum == Ref ? "OK" : "FAIL");
    }

    printf("=== Course 04 done ===\n");
}
