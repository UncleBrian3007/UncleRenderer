// ⚠ PHASE 4 – HAZARDOUS PATTERN ⚠
//
// This shader uses a global barrier implemented via a spin-lock counter.
// D3D12 / Vulkan do not guarantee forward progress between thread groups
// (the driver may serialize groups on some hardware or in some configurations).
// If any group stalls indefinitely the GPU will time-out (TDR / device removal).
//
// Safe production alternative: split into two separate dispatches:
//   Pass A: reset histogram
//   Pass B: accumulate histogram
//
// This file exists for educational purposes only.

// Root signature layout:
//   Param 0 : Root Constants  b0  (4 dwords: InputSize, Bins, Threads, pad)
//   Param 1 : Root UAV        u0  Input
//   Param 2 : Root UAV        u1  Output (histogram bins)
//   Param 3 : Root UAV        u2  Counter (global wave-arrival counter)

cbuffer Constants : register(b0)
{
    uint InputSize;
    uint Bins;
    uint Threads;     // Total number of threads launched (gridDim * blockDim).
    uint Pad0;
};

RWStructuredBuffer<int> Input   : register(u0);
RWStructuredBuffer<int> Output  : register(u1);  // Histogram output.
RWStructuredBuffer<int> Counter : register(u2);  // Global sync counter.

static const uint BlockSize = 64;

// ============================================================
// Persistent-thread histogram with global barrier.
//
// Phase A: all threads reset their portion of the output array.
// Global barrier: spin until all threads have completed Phase A.
// Phase B: all threads accumulate their portion of the input.
//
// Mirrors HistogramKernel in the Orochi course.
// ============================================================
[numthreads(BlockSize, 1, 1)]
void HistogramPersistentCS(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    uint totalGroups = (Threads + BlockSize - 1u) / BlockSize;
    uint warpsPerGroup = (BlockSize + WaveGetLaneCount() - 1u) / WaveGetLaneCount();
    uint totalWaves    = totalGroups * warpsPerGroup;

    // --- Phase A: Zero the histogram ---
    for (uint i = dtid.x; i < Bins; i += Threads)
        Output[i] = 0;

    // Global barrier: each wave's first lane increments the counter,
    // then spins until all waves have checked in.
    if (WaveIsFirstLane())
    {
        InterlockedAdd(Counter[0], 1);
        int arrived;
        do { InterlockedAdd(Counter[0], 0, arrived); } while ((uint)arrived < totalWaves);
    }
    // Synchronise all threads in the group before proceeding.
    GroupMemoryBarrierWithGroupSync();

    DeviceMemoryBarrier();  // Ensure zeroes are globally visible.

    // --- Phase B: Accumulate the histogram ---
    for (uint i = dtid.x; i < InputSize; i += Threads)
    {
        int val = Input[i];
        InterlockedAdd(Output[val], 1);
    }
}
