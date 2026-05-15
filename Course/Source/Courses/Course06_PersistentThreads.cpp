#include "Course06_PersistentThreads.h"
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>

// ⚠ Phase 4 – Hazardous pattern.
// The persistent-thread histogram uses a global spin-lock barrier.
// D3D12 provides no forward-progress guarantee between thread groups,
// so this may deadlock / TDR on some drivers or hardware configurations.
// Safe alternative: two separate dispatches (zero-pass + accumulate-pass).

namespace
{
    void CpuHistogram(const std::vector<int>& Data, uint32_t Bins, std::vector<int>& Out)
    {
        Out.assign(Bins, 0);
        for (int V : Data) Out[V % Bins]++;
    }
}

void Course06_Run(CourseRunner& Runner)
{
    printf("\n=== Course 06: Persistent Threads (global barrier histogram) ===\n");
    printf("  ⚠ This pattern may cause GPU TDR on some hardware (Phase 4 – hazardous).\n");
    printf("  Safe alternative: dispatch 2 passes (zero + accumulate) separately.\n");

    std::vector<uint8_t> BC;
    if (!Runner.CompileCS(L"Course/Shaders/Course06_PersistentThreads.hlsl", L"HistogramPersistentCS", BC))
    {
        fprintf(stderr, "[Course06] Shader compilation failed.\n");
        return;
    }

    static constexpr uint32_t kBlockSize = 64;
    static constexpr uint32_t kBins      = 256;

    auto RS  = Runner.CreateRootSignature(3, 4);
    auto PSO = Runner.CreateComputePSO(RS.Get(), BC);

    std::mt19937 Rng(55);
    std::uniform_int_distribution<int> Dist(0, (int)kBins - 1);

    for (uint32_t N : { 4096u, 65536u })
    {
        std::vector<int> Data(N);
        for (int& V : Data) V = Dist(Rng);

        std::vector<int> CpuHist;
        CpuHistogram(Data, kBins, CpuHist);

        uint32_t Groups  = (N + kBlockSize - 1u) / kBlockSize;
        uint32_t Threads = Groups * kBlockSize;

        GpuBuffer InputBuf  = Runner.CreateBuffer((uint64_t)N * sizeof(int),    L"HistInput");
        GpuBuffer OutputBuf = Runner.CreateBuffer((uint64_t)kBins * sizeof(int), L"HistOutput");
        GpuBuffer CntBuf    = Runner.CreateBuffer(sizeof(int),                   L"HistCounter");

        Runner.Upload(InputBuf, Data.data(), (uint64_t)N * sizeof(int));
        Runner.ZeroBuffer(OutputBuf, (uint64_t)kBins * sizeof(int));
        Runner.ZeroBuffer(CntBuf, sizeof(int));

        struct Constants { uint32_t InputSize, Bins, Threads, Pad; };
        Constants C = { N, kBins, Threads, 0 };

        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS.Get();
        Desc.PSO            = PSO.Get();
        Desc.GroupsX        = Groups;
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0] = InputBuf.VA();
        Desc.Uavs[1] = OutputBuf.VA();
        Desc.Uavs[2] = CntBuf.VA();
        Desc.NumUavs = 3;
        Desc.Label   = L"HistogramPersistentCS";
        Runner.Dispatch(Desc);
        Runner.Flush();

        GpuBuffer RB = Runner.CreateReadbackBuffer((uint64_t)kBins * sizeof(int), L"HistRB");
        Runner.Readback(RB, OutputBuf, (uint64_t)kBins * sizeof(int));
        const int* GpuHist = reinterpret_cast<const int*>(Runner.MapReadback(RB));

        bool Ok = true;
        for (uint32_t b = 0; b < kBins; ++b)
        {
            if (GpuHist[b] != CpuHist[b]) { Ok = false; break; }
        }
        Runner.UnmapReadback(RB);

        printf("  N=%6u  bins=%u  %s\n", N, kBins, Ok ? "OK" : "FAIL");
    }

    printf("=== Course 06 done ===\n");
}
