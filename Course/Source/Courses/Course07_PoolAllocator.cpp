#include "Course07_PoolAllocator.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <stack>
#include <vector>

struct GpuNode07 { int LeftIndex, RightIndex, ParentAddr, Pivot; };
struct GpuLeaf07 { int Value, ParentAddr; };

namespace
{
    struct Tree07
    {
        std::vector<GpuNode07> nodes;
        std::vector<GpuLeaf07> leaves;
    };

    Tree07 BuildTree07(std::vector<int> input)
    {
        std::sort(input.begin(), input.end());
        int N = (int)input.size();
        Tree07 T;
        T.nodes.resize(N);
        T.leaves.resize(N);

        std::stack<int> stk;
        T.nodes[0] = {0, N, -1, 0};
        int nc = 1;
        stk.push(0);

        while (!stk.empty())
        {
            int i = stk.top(); stk.pop();
            auto& nd = T.nodes[i];
            int l = nd.LeftIndex, r = nd.RightIndex, m = (l + r) / 2;
            nd.Pivot = input[m];
            if (m - l > 1) { int c = nc++; nd.LeftIndex = c; T.nodes[c] = {l,m,i,0}; stk.push(c); }
            else            { nd.LeftIndex = ~l; T.leaves[l] = {input[l], i}; }
            if (r - m > 1) { int c = nc++; nd.RightIndex = c; T.nodes[c] = {m,r,i,0}; stk.push(c); }
            else            { nd.RightIndex = ~m; T.leaves[m] = {input[m], i}; }
        }
        return T;
    }

    int IsLeaf(int idx) { return idx < 0; }
    int GetAddr(int idx) { return idx < 0 ? ~idx : idx; }

    // CPU reference: count how many leaves equal query in the tree.
    int CpuCount(const Tree07& T, int Query)
    {
        int Count = 0;
        std::stack<int> stk;
        stk.push(0);
        while (!stk.empty())
        {
            int idx = stk.top(); stk.pop();
            if (!IsLeaf(idx))
            {
                const auto& nd = T.nodes[GetAddr(idx)];
                if (nd.Pivot <= Query) stk.push(nd.RightIndex);
                if (nd.Pivot >= Query) stk.push(nd.LeftIndex);
            }
            else
            {
                if (T.leaves[GetAddr(idx)].Value == Query) ++Count;
            }
        }
        return Count;
    }
}

void Course07_Run(CourseRunner& Runner)
{
    printf("\n=== Course 07: Pool Allocator (warp-level stack pool) ===\n");

    std::vector<uint8_t> BC;
    if (!Runner.CompileCS(L"Course/Shaders/Course07_PoolAllocator.hlsl", L"CountCS", BC))
    {
        fprintf(stderr, "[Course07] Shader compilation failed.\n");
        return;
    }

    // RS: 4 constants + 6 UAVs (Nodes, Leaves, Queries, Counts, StackBuf, Locks).
    auto RS  = Runner.CreateRootSignature(6, 4);
    auto PSO = Runner.CreateComputePSO(RS.Get(), BC);

    std::mt19937 Rng(17);
    std::uniform_int_distribution<int> ValDist(0, 31);

    static constexpr uint32_t kWaveSize   = 32;   // Assume NVIDIA warp size; safe lower bound.
    static constexpr uint32_t kStackSize  = 64;   // Max BST depth.
    static constexpr uint32_t kBlockSize  = 1024;

    for (uint32_t N : { 32u, 256u, 1024u })
    {
        std::vector<int> Input(N);
        for (int& V : Input) V = ValDist(Rng);

        Tree07 T = BuildTree07(Input);

        uint32_t NodeCount = (uint32_t)T.nodes.size();

        // Queries = all leaf values.
        std::vector<int> Queries;
        for (const auto& L : T.leaves) Queries.push_back(L.Value);
        uint32_t QueryCount = (uint32_t)Queries.size();

        // CPU reference counts.
        std::vector<int> CpuCounts(QueryCount);
        for (uint32_t i = 0; i < QueryCount; ++i)
            CpuCounts[i] = CpuCount(T, Queries[i]);

        // Pool dimensions: one slot per warp, generous pool.
        uint32_t TotalWaves  = (QueryCount + kWaveSize - 1u) / kWaveSize;
        uint32_t StackCount  = TotalWaves * 4u * kWaveSize;  // 4x more slots than needed.
        uint32_t LockCount   = StackCount / kWaveSize;

        GpuBuffer NodeBuf    = Runner.CreateBuffer(NodeCount  * sizeof(GpuNode07), L"PA_Nodes");
        GpuBuffer LeafBuf    = Runner.CreateBuffer(NodeCount  * sizeof(GpuLeaf07), L"PA_Leaves");
        GpuBuffer QueryBuf   = Runner.CreateBuffer(QueryCount * sizeof(int),       L"PA_Queries");
        GpuBuffer CountBuf   = Runner.CreateBuffer(QueryCount * sizeof(int),       L"PA_Counts");
        GpuBuffer StackBuf   = Runner.CreateBuffer((uint64_t)StackCount * kStackSize * sizeof(int), L"PA_Stack");
        GpuBuffer LocksBuf   = Runner.CreateBuffer(LockCount * sizeof(int),        L"PA_Locks");

        Runner.Upload(NodeBuf,  T.nodes.data(),  NodeCount  * sizeof(GpuNode07));
        Runner.Upload(LeafBuf,  T.leaves.data(), NodeCount  * sizeof(GpuLeaf07));
        Runner.Upload(QueryBuf, Queries.data(),  QueryCount * sizeof(int));
        Runner.ZeroBuffer(CountBuf, QueryCount * sizeof(int));
        Runner.ZeroBuffer(StackBuf, (uint64_t)StackCount * kStackSize * sizeof(int));
        Runner.ZeroBuffer(LocksBuf, LockCount * sizeof(int));

        struct Constants { uint32_t QueryCount, StackSize, StackCount, Pad; };
        Constants C = { QueryCount, kStackSize, StackCount, 0 };

        uint32_t Groups = (QueryCount + kBlockSize - 1u) / kBlockSize;

        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS.Get();
        Desc.PSO            = PSO.Get();
        Desc.GroupsX        = Groups;
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0] = NodeBuf.VA();
        Desc.Uavs[1] = LeafBuf.VA();
        Desc.Uavs[2] = QueryBuf.VA();
        Desc.Uavs[3] = CountBuf.VA();
        Desc.Uavs[4] = StackBuf.VA();
        Desc.Uavs[5] = LocksBuf.VA();
        Desc.NumUavs = 6;
        Desc.Label   = L"CountCS";
        Runner.Dispatch(Desc);
        Runner.Flush();

        GpuBuffer RB = Runner.CreateReadbackBuffer(QueryCount * sizeof(int), L"PA_CountsRB");
        Runner.Readback(RB, CountBuf, QueryCount * sizeof(int));
        const int* GpuCounts = reinterpret_cast<const int*>(Runner.MapReadback(RB));

        bool Ok = true;
        for (uint32_t i = 0; i < QueryCount; ++i)
        {
            if (GpuCounts[i] != CpuCounts[i]) { Ok = false; break; }
        }
        Runner.UnmapReadback(RB);

        printf("  N=%4u  queries=%4u  %s\n", N, QueryCount, Ok ? "OK" : "FAIL");
    }

    printf("=== Course 07 done ===\n");
}
