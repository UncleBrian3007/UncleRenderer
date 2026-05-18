#include "Course05_WaterfallScheme.h"

#include <cstdio>
#include <random>
#include <stack>
#include <vector>
#include <algorithm>

// Reuse the same Node/Leaf structs & BuildTree from Course04.
// Since including .cpp would cause duplicate symbols, we just redeclare the
// minimal CPU structures here.

// (Node / Leaf / BuildTree are defined in the anonymous namespace of
// Course04_BottomUpTraversal.cpp, so they won't conflict.)

namespace
{
    struct CpuNode { int l, r, parent, pivot; };
    struct CpuLeaf { int value, parent; };

    struct CpuTree { std::vector<CpuNode> nodes; std::vector<CpuLeaf> leaves; };

    CpuTree BuildCpuTree(std::vector<int> input)
    {
        std::sort(input.begin(), input.end());
        int N = (int)input.size();
        CpuTree T;
        T.nodes.resize(N);
        T.leaves.resize(N);

        std::stack<int> stk;
        T.nodes[0] = { 0, N, -1, 0 };
        int nc = 1;
        stk.push(0);

        while (!stk.empty())
        {
            int i = stk.top(); stk.pop();
            auto& nd = T.nodes[i];
            int l = nd.l, r = nd.r, m = (l + r) / 2;
            nd.pivot = input[m];
            if (m - l > 1)
            {
                int c = nc++;
                nd.l = c;
                T.nodes[c] = { l, m, i, 0 };
                stk.push(c);
            }
            else
            {
                nd.l = ~l;
                T.leaves[l] = { input[l], i };
            }
            if (r - m > 1)
            {
                int c = nc++;
                nd.r = c;
                T.nodes[c] = { m, r, i, 0 };
                stk.push(c);
            }
            else
            {
                nd.r = ~m;
                T.leaves[m] = { input[m], i };
            }
        }
        return T;
    }
}

// GPU-side structs must match Common.hlsli.
struct GpuNode05 { int LeftIndex, RightIndex, ParentAddr, Pivot; };
struct GpuLeaf05 { int Value, ParentAddr; };

void Course05_Run(CourseRunner& Runner)
{
    printf("\n=== Course 05: Waterfall Scheme (Parallel Tree Build) ===\n");

    std::vector<uint8_t> BC;
    if (!Runner.CompileCS(L"Course/Shaders/Course05_WaterfallScheme.hlsl", L"BuildTreeCS", BC))
    {
        fprintf(stderr, "[Course05] Shader compilation failed.\n");
        return;
    }

    // RS: 4 constants + 5 UAVs (Input, TaskQueue, Counters, Nodes, Leaves).
    auto RS  = Runner.CreateRootSignature(5, 4);
    auto PSO = Runner.CreateComputePSO(RS.Get(), BC);

    std::mt19937 Rng(13);
    std::uniform_int_distribution<int> Dist(0, 1000);

    // Waterfall scheme requires all threads in one threadgroup to spin together.
    // N must not exceed kBlockSize (1024); multi-group dispatch breaks coherency.
    for (uint32_t N : { 8u, 64u, 256u, 1024u })
    {
        std::vector<int> Input(N);
        for (int& V : Input) V = Dist(Rng);

        // CPU reference tree to validate pivot/structure.
        auto CRef = BuildCpuTree(Input);
        int  CRefLeafSum = 0;
        for (auto& L : CRef.leaves) CRefLeafSum += L.value;

        // Sort input before uploading (BST builder expects sorted data).
        std::vector<int> Sorted = Input;
        std::sort(Sorted.begin(), Sorted.end());

        GpuBuffer InputBuf = Runner.CreateBuffer(N * sizeof(int),  L"WFInput");
        GpuBuffer TaskBuf  = Runner.CreateBuffer(N * sizeof(int),  L"WFTask");
        GpuBuffer CntBuf   = Runner.CreateBuffer(2 * sizeof(int),  L"WFCounters");
        GpuBuffer NodeBuf  = Runner.CreateBuffer(N * sizeof(GpuNode05), L"WFNodes");
        GpuBuffer LeafBuf  = Runner.CreateBuffer(N * sizeof(GpuLeaf05), L"WFLeaves");

        Runner.Upload(InputBuf, Sorted.data(), N * sizeof(int));
        Runner.ZeroBuffer(TaskBuf,  N * sizeof(int));
        Runner.ZeroBuffer(CntBuf,   2 * sizeof(int));
        Runner.ZeroBuffer(NodeBuf,  N * sizeof(GpuNode05));
        Runner.ZeroBuffer(LeafBuf,  N * sizeof(GpuLeaf05));

        // Counters[0]=1 (root occupies slot 0), Counters[1]=0 (no leaves yet).
        int InitCounters[2] = { 1, 0 };
        Runner.Upload(CntBuf, InitCounters, sizeof(InitCounters));

        // task[0]=1 kicks off the root node.
        std::vector<int> TaskInit(N, 0); TaskInit[0] = 1;
        Runner.Upload(TaskBuf, TaskInit.data(), N * sizeof(int));

        // Root node covers the full sorted range [0, N).
        GpuNode05 RootNode = { 0, (int)N, -1, 0 };
        Runner.Upload(NodeBuf, &RootNode, sizeof(GpuNode05));

        struct Constants { uint32_t Size, P0, P1, P2; };
        Constants C = { N, 0, 0, 0 };

        static constexpr uint32_t kBlockSize = 1024;
        uint32_t Groups = 1;  // must be 1: waterfall is a single-group algorithm

        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS.Get();
        Desc.PSO            = PSO.Get();
        Desc.GroupsX        = Groups;
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0] = InputBuf.VA();
        Desc.Uavs[1] = TaskBuf.VA();
        Desc.Uavs[2] = CntBuf.VA();
        Desc.Uavs[3] = NodeBuf.VA();
        Desc.Uavs[4] = LeafBuf.VA();
        Desc.NumUavs = 5;
        Desc.Label   = L"BuildTreeCS";
        Runner.Dispatch(Desc);
        Runner.Flush();

        // Validate by reading back leaves and summing.
        GpuBuffer LeavesRB = Runner.CreateReadbackBuffer(N * sizeof(GpuLeaf05), L"LeavesRB");
        Runner.Readback(LeavesRB, LeafBuf, N * sizeof(GpuLeaf05));
        const GpuLeaf05* Lf = reinterpret_cast<const GpuLeaf05*>(Runner.MapReadback(LeavesRB));
        int GpuSum = 0;
        for (uint32_t i = 0; i < N; ++i) GpuSum += Lf[i].Value;
        Runner.UnmapReadback(LeavesRB);

        printf("  N=%5u  GpuLeafSum=%d  CpuLeafSum=%d  %s\n",
               N, GpuSum, CRefLeafSum, GpuSum == CRefLeafSum ? "OK" : "FAIL");
    }

    printf("=== Course 05 done ===\n");
}
