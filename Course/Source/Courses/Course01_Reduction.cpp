#include "Course01_Reduction.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
    // -----------------------------------------------------------------
    // CPU reference: sum all elements.
    // -----------------------------------------------------------------
    int64_t CpuReduce(const std::vector<int>& Data)
    {
        int64_t Sum = 0;
        for (int V : Data) Sum += V;
        return Sum;
    }

    // -----------------------------------------------------------------
    // Helper: build and dispatch a reduction kernel, return the sum.
    // -----------------------------------------------------------------
    int RunReduction(
        CourseRunner&         Runner,
        const std::wstring&   KernelEntry,
        ID3D12RootSignature*  RS,
        ID3D12PipelineState*  PSO,
        const GpuBuffer&      InputBuf,
        uint32_t              Size)
    {
        static constexpr uint32_t kBlockSize = 1024;

        // Output buffer: single int, zeroed before each run.
        GpuBuffer OutputBuf = Runner.CreateBuffer(sizeof(int), L"ReduceOutput");
        Runner.ZeroBuffer(OutputBuf, sizeof(int));

        // Dispatch
        struct Constants { uint32_t Size, P0, P1, P2; };
        const Constants C = { Size, 0, 0, 0 };

        const uint32_t Groups = (Size + kBlockSize - 1u) / kBlockSize;

        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS;
        Desc.PSO            = PSO;
        Desc.GroupsX        = Groups;
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0]        = InputBuf.VA();
        Desc.Uavs[1]        = OutputBuf.VA();
        Desc.NumUavs        = 2;
        Desc.Label          = KernelEntry.c_str();
        Runner.Dispatch(Desc);
        Runner.Flush();

        // Readback
        GpuBuffer RB = Runner.CreateReadbackBuffer(sizeof(int), L"ReduceReadback");
        Runner.Readback(RB, OutputBuf, sizeof(int));
        const int* Ptr = reinterpret_cast<const int*>(Runner.MapReadback(RB));
        int Result = *Ptr;
        Runner.UnmapReadback(RB);
        return Result;
    }

    void TestSize(
        CourseRunner&        Runner,
        ID3D12RootSignature* RS,
        ID3D12PipelineState* BlockPSO,
        ID3D12PipelineState* WavePSO,
        uint32_t             Size)
    {
        // Fill with a fast deterministic pattern in [0,15].
        // Avoids mt19937 + uniform_int_distribution overhead (~5x faster for large N).
        std::vector<int> HostData(Size);
        for (uint32_t i = 0; i < Size; ++i)
            HostData[i] = (int)(i & 0xF);

        const int64_t Expected = CpuReduce(HostData);

        // Upload to GPU.
        GpuBuffer InputBuf = Runner.CreateBuffer((uint64_t)Size * sizeof(int), L"ReduceInput");
        Runner.Upload(InputBuf, HostData.data(), (uint64_t)Size * sizeof(int));

        // ----- ReduceBlockCS -----
        auto T0 = std::chrono::high_resolution_clock::now();
        int BlockResult = RunReduction(Runner, L"ReduceBlockCS", RS, BlockPSO, InputBuf, Size);
        auto T1 = std::chrono::high_resolution_clock::now();
        double BlockMs = std::chrono::duration<double, std::milli>(T1 - T0).count();

        bool BlockOk = (BlockResult == (int)Expected);
        printf("  ReduceBlock : size=%7u  result=%d  expected=%lld  %s  (%.2f ms)\n",
               Size, BlockResult, Expected, BlockOk ? "OK" : "FAIL", BlockMs);

        // ----- ReduceWaveCS -----
        auto T2 = std::chrono::high_resolution_clock::now();
        int WaveResult = RunReduction(Runner, L"ReduceWaveCS", RS, WavePSO, InputBuf, Size);
        auto T3 = std::chrono::high_resolution_clock::now();
        double WaveMs = std::chrono::duration<double, std::milli>(T3 - T2).count();

        bool WaveOk = (WaveResult == (int)Expected);
        printf("  ReduceWave  : size=%7u  result=%d  expected=%lld  %s  (%.2f ms)\n",
               Size, WaveResult, Expected, WaveOk ? "OK" : "FAIL", WaveMs);
    }
}

// -----------------------------------------------------------------
// Entry point called from Main.cpp
// -----------------------------------------------------------------
void Course01_Run(CourseRunner& Runner)
{
    printf("\n=== Course 01: Reduction ===\n");

    // Compile shaders.
    std::vector<uint8_t> BlockBC, WaveBC;
    if (!Runner.CompileCS(L"Course/Shaders/Course01_Reduction.hlsl", L"ReduceBlockCS", BlockBC) ||
        !Runner.CompileCS(L"Course/Shaders/Course01_Reduction.hlsl", L"ReduceWaveCS",  WaveBC))
    {
        fprintf(stderr, "[Course01] Shader compilation failed.\n");
        return;
    }

    // Create root signature: 2 UAVs + 4 constant dwords.
    auto RS       = Runner.CreateRootSignature(2, 4);
    auto BlockPSO = Runner.CreateComputePSO(RS.Get(), BlockBC);
    auto WavePSO  = Runner.CreateComputePSO(RS.Get(), WaveBC);

    // Test various sizes.
    for (uint32_t Size : { 16u * 1000u, 16u * 10000u, 16u * 100000u, 16u * 1000000u })
        TestSize(Runner, RS.Get(), BlockPSO.Get(), WavePSO.Get(), Size);

    printf("=== Course 01 done ===\n");
}
