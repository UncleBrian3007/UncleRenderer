#include "Course03_Enqueue.h"
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>

namespace
{
    // CPU reference: collect odd values into front, even into back.
    void CpuEnqueue(const std::vector<int>& In, std::vector<int>& Front, std::vector<int>& Back)
    {
        for (int V : In)
        {
            if (V & 1) Front.push_back(V);
            else        Back.push_back(V);
        }
    }

    struct EnqueueResult
    {
        std::vector<int> Output;
        int              Count = 0;
    };

    EnqueueResult RunEnqueue(
        CourseRunner&        Runner,
        const wchar_t*       Label,
        ID3D12RootSignature* RS,
        ID3D12PipelineState* PSO,
        const std::vector<int>& HostData,
        bool                 HasCounter2 = false)
    {
        uint32_t Size = (uint32_t)HostData.size();

        GpuBuffer InputBuf   = Runner.CreateBuffer((uint64_t)Size * sizeof(int), L"EnqInput");
        GpuBuffer OutputBuf  = Runner.CreateBuffer((uint64_t)Size * sizeof(int), L"EnqOutput");
        GpuBuffer CounterBuf = Runner.CreateBuffer(sizeof(int), L"EnqCounter");
        GpuBuffer Counter2Buf= Runner.CreateBuffer(sizeof(int), L"EnqCounter2");

        Runner.Upload(InputBuf, HostData.data(), (uint64_t)Size * sizeof(int));
        Runner.ZeroBuffer(OutputBuf,   (uint64_t)Size * sizeof(int));
        Runner.ZeroBuffer(CounterBuf,  sizeof(int));
        Runner.ZeroBuffer(Counter2Buf, sizeof(int));

        struct Constants { uint32_t Size, P0, P1, P2; };
        Constants C = { Size, 0, 0, 0 };

        static constexpr uint32_t kBlockSize = 256;
        uint32_t Groups = (Size + kBlockSize - 1u) / kBlockSize;

        CourseRunner::DispatchDesc Desc;
        Desc.RootSig        = RS;
        Desc.PSO            = PSO;
        Desc.GroupsX        = Groups;
        Desc.Constants      = &C;
        Desc.ConstantDwords = 4;
        Desc.Uavs[0]        = InputBuf.VA();
        Desc.Uavs[1]        = OutputBuf.VA();
        Desc.Uavs[2]        = CounterBuf.VA();
        Desc.Uavs[3]        = Counter2Buf.VA();
        Desc.NumUavs        = HasCounter2 ? 4u : 3u;
        Desc.Label          = Label;
        Runner.Dispatch(Desc);
        Runner.Flush();

        // Read counter
        GpuBuffer CntRB = Runner.CreateReadbackBuffer(sizeof(int), L"EnqCounterRB");
        Runner.Readback(CntRB, CounterBuf, sizeof(int));
        const int* CntPtr = reinterpret_cast<const int*>(Runner.MapReadback(CntRB));
        int Count = *CntPtr;
        Runner.UnmapReadback(CntRB);

        // Read output
        GpuBuffer OutRB = Runner.CreateReadbackBuffer((uint64_t)Size * sizeof(int), L"EnqOutputRB");
        Runner.Readback(OutRB, OutputBuf, (uint64_t)Size * sizeof(int));
        const int* OutPtr = reinterpret_cast<const int*>(Runner.MapReadback(OutRB));
        std::vector<int> Output(OutPtr, OutPtr + Size);
        Runner.UnmapReadback(OutRB);

        return { Output, Count };
    }

    bool Validate(
        const wchar_t*       Label,
        const EnqueueResult& Result,
        const std::vector<int>& ExpFront,
        uint32_t             Size)
    {
        bool CountOk = (Result.Count == (int)ExpFront.size());

        // The GPU output may be in arbitrary order within the front section;
        // sort both and compare.
        std::vector<int> GpuFront(Result.Output.begin(), Result.Output.begin() + Result.Count);
        std::vector<int> CpuFront = ExpFront;
        std::sort(GpuFront.begin(), GpuFront.end());
        std::sort(CpuFront.begin(), CpuFront.end());
        bool ValuesOk = (GpuFront == CpuFront);

        printf("  %-24ls  count=%d/%d  values=%s\n",
               Label, Result.Count, (int)ExpFront.size(),
               (CountOk && ValuesOk) ? "OK" : "FAIL");
        return CountOk && ValuesOk;
    }
}

void Course03_Run(CourseRunner& Runner)
{
    printf("\n=== Course 03: Enqueue (Stream Compaction) ===\n");

    std::vector<uint8_t> NaiveBC, WaveBC, BinaryBC, ComplBC;
    const wchar_t* Path = L"Course/Shaders/Course03_Enqueue.hlsl";
    if (!Runner.CompileCS(Path, L"EnqueueNaiveCS",      NaiveBC)  ||
        !Runner.CompileCS(Path, L"EnqueueWaveCS",       WaveBC)   ||
        !Runner.CompileCS(Path, L"EnqueueBinaryCS",     BinaryBC) ||
        !Runner.CompileCS(Path, L"EnqueueComplementCS", ComplBC))
    {
        fprintf(stderr, "[Course03] Shader compilation failed.\n");
        return;
    }

    // 4 UAVs (input, output, counter, counter2 for complement kernel).
    auto RS        = Runner.CreateRootSignature(4, 4);
    auto NaivePSO  = Runner.CreateComputePSO(RS.Get(), NaiveBC);
    auto WavePSO   = Runner.CreateComputePSO(RS.Get(), WaveBC);
    auto BinaryPSO = Runner.CreateComputePSO(RS.Get(), BinaryBC);
    auto ComplPSO  = Runner.CreateComputePSO(RS.Get(), ComplBC);

    std::mt19937 Rng(3);
    std::uniform_int_distribution<int> Dist(0, 31);

    for (uint32_t Size : { 1024u, 16384u, 1048576u })
    {
        std::vector<int> Data(Size);
        for (int& V : Data) V = Dist(Rng);

        std::vector<int> ExpFront, ExpBack;
        CpuEnqueue(Data, ExpFront, ExpBack);

        auto R1 = RunEnqueue(Runner, L"EnqueueNaive",      RS.Get(), NaivePSO.Get(),  Data, false);
        auto R2 = RunEnqueue(Runner, L"EnqueueWave",       RS.Get(), WavePSO.Get(),   Data, false);
        auto R3 = RunEnqueue(Runner, L"EnqueueBinary",     RS.Get(), BinaryPSO.Get(), Data, false);
        auto R4 = RunEnqueue(Runner, L"EnqueueComplement", RS.Get(), ComplPSO.Get(),  Data, true);

        printf("  -- size=%u --\n", Size);
        Validate(L"Naive",      R1, ExpFront, Size);
        Validate(L"Wave",       R2, ExpFront, Size);
        Validate(L"Binary",     R3, ExpFront, Size);
        Validate(L"Complement", R4, ExpFront, Size);
    }

    printf("=== Course 03 done ===\n");
}
