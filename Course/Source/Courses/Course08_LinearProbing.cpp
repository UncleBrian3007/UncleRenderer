#include "Course08_LinearProbing.h"
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>

namespace
{
    // splitmix64 on CPU to replicate the GPU sequence.
    uint64_t splitmix64_cpu(uint64_t& state)
    {
        state += 0x9e3779b97f4a7c15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // CPU reference: collect inserted keys for a given thread.
    void CpuInsertedKeys(uint32_t Tid, uint32_t Upper, uint32_t ItemsPerThread, std::unordered_set<int>& Keys)
    {
        uint64_t rng = (uint64_t)Tid;
        for (uint32_t i = 0; i < ItemsPerThread; ++i)
        {
            uint64_t r = splitmix64_cpu(rng);
            Keys.insert((int)(r % Upper));
        }
    }
}

void Course08_Run(CourseRunner& Runner)
{
    printf("\n=== Course 08: Linear Probing Hash Table ===\n");

    std::vector<uint8_t> InsLPBC, InsBLPBC, FindLPBC, FindBLPBC;
    const wchar_t* Path = L"Course/Shaders/Course08_LinearProbing.hlsl";
    if (!Runner.CompileCS(Path, L"InsertLPCS",  InsLPBC)  ||
        !Runner.CompileCS(Path, L"InsertBLPCS", InsBLPBC) ||
        !Runner.CompileCS(Path, L"FindLPCS",    FindLPBC) ||
        !Runner.CompileCS(Path, L"FindBLPCS",   FindBLPBC))
    {
        fprintf(stderr, "[Course08] Shader compilation failed.\n");
        return;
    }

    // 2 UAVs: HashTable + Counter.
    auto RS         = Runner.CreateRootSignature(2, 4);
    auto InsLPPSO   = Runner.CreateComputePSO(RS.Get(), InsLPBC);
    auto InsBLPPSO  = Runner.CreateComputePSO(RS.Get(), InsBLPBC);
    auto FindLPPSO  = Runner.CreateComputePSO(RS.Get(), FindLPBC);
    auto FindBLPPSO = Runner.CreateComputePSO(RS.Get(), FindBLPBC);

    static constexpr uint32_t kBlockSize      = 256;
    static constexpr uint32_t kThreadCount    = 1024;
    static constexpr uint32_t kItemsPerThread = 8;
    static constexpr uint32_t kUpper          = 512;
    static constexpr uint32_t kTableSize      = kUpper * 4;  // 4x load-factor headroom.

    auto RunTest = [&](const wchar_t* Label,
                       ID3D12PipelineState* InsPSO,
                       ID3D12PipelineState* FindPSO)
    {
        uint32_t Groups = (kThreadCount + kBlockSize - 1u) / kBlockSize;

        GpuBuffer TableBuf   = Runner.CreateBuffer((uint64_t)kTableSize * sizeof(int), L"HT_Table");
        GpuBuffer CounterBuf = Runner.CreateBuffer(sizeof(int), L"HT_Counter");

        // Init table to -1 (EMPTY).
        std::vector<int> InitTable(kTableSize, -1);
        Runner.Upload(TableBuf, InitTable.data(), (uint64_t)kTableSize * sizeof(int));
        Runner.ZeroBuffer(CounterBuf, sizeof(int));

        struct Constants { uint32_t TableSize, Upper, ItemsPerThread, Pad; };
        Constants C = { kTableSize, kUpper, kItemsPerThread, 0 };

        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS.Get();
        Desc.PSO            = InsPSO;
        Desc.GroupsX        = Groups;
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0] = TableBuf.VA();
        Desc.Uavs[1] = CounterBuf.VA();
        Desc.NumUavs = 2;
        std::wstring InsLabel  = std::wstring(L"Insert") + Label;
        std::wstring FindLabel = std::wstring(L"Find")   + Label;
        Desc.Label = InsLabel.c_str();
        Runner.Dispatch(Desc);
        Runner.Flush();

        Desc.PSO   = FindPSO;
        Desc.Label = FindLabel.c_str();
        Runner.ZeroBuffer(CounterBuf, sizeof(int));
        Runner.Dispatch(Desc);
        Runner.Flush();

        GpuBuffer RB = Runner.CreateReadbackBuffer(sizeof(int), L"HT_CountRB");
        Runner.Readback(RB, CounterBuf, sizeof(int));
        const int* Found = reinterpret_cast<const int*>(Runner.MapReadback(RB));
        int GpuFound = *Found;
        Runner.UnmapReadback(RB);

        // CPU reference: count found items.
        std::unordered_set<int> InsertedKeys;
        for (uint32_t t = 0; t < kThreadCount; ++t)
            CpuInsertedKeys(t, kUpper, kItemsPerThread, InsertedKeys);

        // Sequence-match finds: same RNG, so all should be found.
        // Different-sequence finds: check against inserted set.
        int ExpectedFound = 0;
        for (uint32_t t = 0; t < kThreadCount; ++t)
        {
            uint64_t rng = (uint64_t)t;
            for (uint32_t i = 0; i < kItemsPerThread; ++i)
            {
                uint64_t r = splitmix64_cpu(rng);
                int key = (int)(r % kUpper);
                if (InsertedKeys.count(key)) ++ExpectedFound;
            }
        }
        for (uint32_t t = 0; t < kThreadCount; ++t)
        {
            uint64_t rng = (uint64_t)(t ^ 0x12345u);
            for (uint32_t i = 0; i < kItemsPerThread; ++i)
            {
                uint64_t r = splitmix64_cpu(rng);
                int key = (int)(r % kUpper);
                if (InsertedKeys.count(key)) ++ExpectedFound;
            }
        }

        printf("  %-8ls  found=%d  expected=%d  %s\n",
               Label, GpuFound, ExpectedFound, GpuFound == ExpectedFound ? "OK" : "FAIL");
    };

    RunTest(L"LP",  InsLPPSO.Get(),  FindLPPSO.Get());
    RunTest(L"BLP", InsBLPPSO.Get(), FindBLPPSO.Get());

    printf("=== Course 08 done ===\n");
}
