#include "Course02_PrefixScan.h"
#include <cstdio>
#include <random>
#include <vector>
#include <chrono>

namespace
{
    // CPU reference: inclusive prefix sum.
    void CpuScan(const std::vector<int>& In, std::vector<int>& Out)
    {
        Out.resize(In.size());
        int Running = 0;
        for (size_t i = 0; i < In.size(); ++i)
        {
            Running += In[i];
            Out[i] = Running;
        }
    }

    bool RunScan(
        CourseRunner&        Runner,
        const wchar_t*       Label,
        ID3D12RootSignature* RS,
        ID3D12PipelineState* PSO,
        const std::vector<int>& HostData,
        const std::vector<int>& Expected)
    {
        static constexpr uint32_t kBlockSize = 1024;
        uint32_t Size = (uint32_t)HostData.size();

        GpuBuffer InputBuf  = Runner.CreateBuffer((uint64_t)Size * sizeof(int), L"ScanInput");
        GpuBuffer OutputBuf = Runner.CreateBuffer((uint64_t)Size * sizeof(int), L"ScanOutput");
        Runner.Upload(InputBuf, HostData.data(), (uint64_t)Size * sizeof(int));
        Runner.ZeroBuffer(OutputBuf, (uint64_t)Size * sizeof(int));

        struct Constants { uint32_t Size, P0, P1, P2; };
        Constants C = { Size, 0, 0, 0 };

        // Note: block scan only works correctly within a single block (Size <= 1024).
        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS;
        Desc.PSO            = PSO;
        Desc.GroupsX        = 1;  // Single block – intra-block demo only.
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0]        = InputBuf.VA();
        Desc.Uavs[1]        = OutputBuf.VA();
        Desc.NumUavs        = 2;
        Desc.Label          = Label;
        Runner.Dispatch(Desc);
        Runner.Flush();

        GpuBuffer RB = Runner.CreateReadbackBuffer((uint64_t)Size * sizeof(int), L"ScanRB");
        Runner.Readback(RB, OutputBuf, (uint64_t)Size * sizeof(int));
        const int* Result = reinterpret_cast<const int*>(Runner.MapReadback(RB));

        bool AllOk = true;
        for (uint32_t i = 0; i < Size; ++i)
        {
            if (Result[i] != Expected[i])
            {
                fprintf(stderr, "  %ls FAIL at [%u]: got %d expected %d\n", Label, i, Result[i], Expected[i]);
                AllOk = false;
                break;
            }
        }
        Runner.UnmapReadback(RB);

        printf("  %-28ls size=%4u  %s\n", Label, Size, AllOk ? "OK" : "FAIL");
        return AllOk;
    }
}

void Course02_Run(CourseRunner& Runner)
{
    printf("\n=== Course 02: Prefix Scan (intra-block) ===\n");
    printf("  Note: device-wide spinlock scan is in Course 06 (Phase 4 / hazardous).\n");

    std::vector<uint8_t> HSBC, BLBC;
    if (!Runner.CompileCS(L"Course/Shaders/Course02_PrefixScan.hlsl", L"ScanBlock_HillisSteeleCS", HSBC) ||
        !Runner.CompileCS(L"Course/Shaders/Course02_PrefixScan.hlsl", L"ScanBlock_BlellochCS",     BLBC))
    {
        fprintf(stderr, "[Course02] Shader compilation failed.\n");
        return;
    }

    auto RS    = Runner.CreateRootSignature(2, 4);
    auto HSPSO = Runner.CreateComputePSO(RS.Get(), HSBC);
    auto BLPSO = Runner.CreateComputePSO(RS.Get(), BLBC);

    std::mt19937 Rng(7);
    std::uniform_int_distribution<int> Dist(0, 8);

    for (uint32_t Size : { 32u, 128u, 512u, 1024u })
    {
        std::vector<int> Data(Size);
        for (int& V : Data) V = Dist(Rng);
        std::vector<int> Ref;
        CpuScan(Data, Ref);

        RunScan(Runner, L"HillisSteele", RS.Get(), HSPSO.Get(), Data, Ref);
        RunScan(Runner, L"Blelloch",     RS.Get(), BLPSO.Get(), Data, Ref);
    }

    printf("=== Course 02 done ===\n");
}
