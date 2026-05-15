#include "Course09_RadixSort.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

// Radix sort config matching Course09_RadixSort.hlsl.
static constexpr uint32_t kNRadix       = 8;
static constexpr uint32_t kBinSize      = 1u << kNRadix;   // 256
static constexpr uint32_t kRadixMask    = kBinSize - 1u;
static constexpr uint32_t kCountWGSize  = kBinSize;        // 256
static constexpr uint32_t kScanWGSize   = kBinSize;        // 256
static constexpr uint32_t kSortWGSize   = 64;
static constexpr uint32_t kSortPerWI    = 12;

namespace
{
    // Count kernel: histogram per work-group.
    void DispatchCount(
        CourseRunner&        Runner,
        ID3D12RootSignature* RS,
        ID3D12PipelineState* PSO,
        const GpuBuffer&     Src,
        const GpuBuffer&     Hist,
        uint32_t             N,
        uint32_t             StartBit,
        uint32_t             NGroups,
        uint32_t             NItemsPerGroup)
    {
        struct Const { uint32_t StartBit, N, NItemsPerGroup, NGroupsExecuted; };
        Const C = { StartBit, N, NItemsPerGroup, NGroups };

        CourseRunner::DispatchDesc D;
        D.RootSig        = RS;
        D.PSO            = PSO;
        D.GroupsX        = NGroups;
        D.Constants      = &C;
        D.ConstantDwords = 4;
        D.Uavs[0] = Src.VA();
        D.Uavs[1] = Hist.VA();
        D.NumUavs = 2;
        D.Label   = L"CountCS";
        Runner.Dispatch(D);
    }

    // Scan kernel: exclusive prefix over per-bin histograms.
    void DispatchScan(
        CourseRunner&        Runner,
        ID3D12RootSignature* RS,
        ID3D12PipelineState* PSO,
        const GpuBuffer&     Count,
        const GpuBuffer&     HistOut,
        const GpuBuffer&     PartialSum,
        const GpuBuffer&     IsReady,
        uint32_t             NGroups)
    {
        struct Const { uint32_t StartBit, N, NItemsPerGroup, NGroupsExecuted; };
        Const C = { 0, 0, 0, NGroups };

        CourseRunner::DispatchDesc D;
        D.RootSig        = RS;
        D.PSO            = PSO;
        D.GroupsX        = NGroups;
        D.Constants      = &C;
        D.ConstantDwords = 4;
        D.Uavs[0] = Count.VA();
        D.Uavs[1] = HistOut.VA();
        D.Uavs[2] = PartialSum.VA();
        D.Uavs[3] = IsReady.VA();
        D.NumUavs = 4;
        D.Label   = L"ParallelExclusiveScanCS";
        Runner.Dispatch(D);
    }

    // Sort kernel: scatter.
    void DispatchSort(
        CourseRunner&        Runner,
        ID3D12RootSignature* RS,
        ID3D12PipelineState* PSO,
        const GpuBuffer&     SrcKey,
        const GpuBuffer&     DstKey,
        const GpuBuffer&     Hist,
        uint32_t             N,
        uint32_t             StartBit,
        uint32_t             NGroups,
        uint32_t             NItemsPerWI)
    {
        struct Const { uint32_t StartBit, N, NItemsPerGroup, NGroupsExecuted; };
        Const C = { StartBit, N, NItemsPerWI * kSortWGSize, NGroups };

        CourseRunner::DispatchDesc D;
        D.RootSig        = RS;
        D.PSO            = PSO;
        D.GroupsX        = NGroups;
        D.Constants      = &C;
        D.ConstantDwords = 4;
        D.Uavs[0] = SrcKey.VA();
        D.Uavs[1] = DstKey.VA();
        D.Uavs[2] = Hist.VA();
        D.NumUavs = 3;
        D.Label   = L"SortCS";
        Runner.Dispatch(D);
    }
}

void Course09_Run(CourseRunner& Runner)
{
    printf("\n=== Course 09: Radix Sort (8-bit, 3-pass GPU) ===\n");

    std::vector<uint8_t> CountBC, ScanBC, SortBC;
    const wchar_t* Path = L"Course/Shaders/Course09_RadixSort.hlsl";
    if (!Runner.CompileCS(Path, L"CountCS",                  CountBC) ||
        !Runner.CompileCS(Path, L"ParallelExclusiveScanCS",  ScanBC)  ||
        !Runner.CompileCS(Path, L"SortCS",                   SortBC))
    {
        fprintf(stderr, "[Course09] Shader compilation failed.\n");
        return;
    }

    // Count RS: 2 UAVs (Src, Hist).
    auto CountRS  = Runner.CreateRootSignature(2, 4);
    auto CountPSO = Runner.CreateComputePSO(CountRS.Get(), CountBC);

    // Scan RS: 4 UAVs (Count, Hist, PartialSum, IsReady).
    auto ScanRS   = Runner.CreateRootSignature(4, 4);
    auto ScanPSO  = Runner.CreateComputePSO(ScanRS.Get(),  ScanBC);

    // Sort RS: 3 UAVs (SrcKey, DstKey, Hist).
    auto SortRS   = Runner.CreateRootSignature(3, 4);
    auto SortPSO  = Runner.CreateComputePSO(SortRS.Get(),  SortBC);

    std::mt19937 Rng(42);
    std::uniform_int_distribution<int> Dist(0, INT32_MAX);

    for (uint32_t N : { 1024u, 65536u, 1048576u })
    {
        std::vector<int> HostData(N);
        for (int& V : HostData) V = Dist(Rng);

        std::vector<int> CpuSorted = HostData;
        std::sort(CpuSorted.begin(), CpuSorted.end());

        GpuBuffer BufA = Runner.CreateBuffer((uint64_t)N * sizeof(int), L"RS_BufA");
        GpuBuffer BufB = Runner.CreateBuffer((uint64_t)N * sizeof(int), L"RS_BufB");
        Runner.Upload(BufA, HostData.data(), (uint64_t)N * sizeof(int));

        // Radix sort: process 8 bits per pass (4 passes for 32-bit keys).
        uint32_t NItemsPerGroup = (N + kSortWGSize - 1u) / kSortWGSize;
        uint32_t NGroups        = (N + NItemsPerGroup * kSortWGSize - 1u) / (NItemsPerGroup * kSortWGSize);
        NGroups = std::max(NGroups, 1u);

        uint64_t HistBytes = (uint64_t)kBinSize * NGroups * sizeof(int);
        GpuBuffer HistBuf    = Runner.CreateBuffer(HistBytes,            L"RS_Hist");
        GpuBuffer PartialSum = Runner.CreateBuffer((uint64_t)NGroups * sizeof(int), L"RS_PS");
        GpuBuffer IsReady    = Runner.CreateBuffer((uint64_t)NGroups * sizeof(uint32_t), L"RS_Ready");

        GpuBuffer* Src = &BufA;
        GpuBuffer* Dst = &BufB;

        for (uint32_t Pass = 0; Pass < 4; ++Pass)
        {
            uint32_t StartBit = Pass * 8u;

            Runner.ZeroBuffer(HistBuf,    HistBytes);
            Runner.ZeroBuffer(PartialSum, (uint64_t)NGroups * sizeof(int));
            Runner.ZeroBuffer(IsReady,    (uint64_t)NGroups * sizeof(uint32_t));

            DispatchCount(Runner, CountRS.Get(), CountPSO.Get(), *Src, HistBuf, N, StartBit, NGroups, NItemsPerGroup * kSortWGSize);
            Runner.Flush();

            DispatchScan(Runner, ScanRS.Get(), ScanPSO.Get(), HistBuf, HistBuf, PartialSum, IsReady, NGroups);
            Runner.Flush();

            DispatchSort(Runner, SortRS.Get(), SortPSO.Get(), *Src, *Dst, HistBuf, N, StartBit, NGroups, NItemsPerGroup);
            Runner.Flush();

            std::swap(Src, Dst);
        }

        // Readback sorted result.
        GpuBuffer RB = Runner.CreateReadbackBuffer((uint64_t)N * sizeof(int), L"RS_RB");
        Runner.Readback(RB, *Src, (uint64_t)N * sizeof(int));
        const int* GpuResult = reinterpret_cast<const int*>(Runner.MapReadback(RB));

        bool Ok = true;
        for (uint32_t i = 0; i < N; ++i)
        {
            if (GpuResult[i] != CpuSorted[i]) { Ok = false; break; }
        }
        Runner.UnmapReadback(RB);

        printf("  N=%8u  %s\n", N, Ok ? "OK" : "FAIL");
    }

    printf("=== Course 09 done ===\n");
}
