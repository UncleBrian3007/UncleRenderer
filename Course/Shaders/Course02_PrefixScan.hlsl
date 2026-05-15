#include "WaveCommon.hlsli"

// Root signature layout:
//   Param 0 : Root Constants  b0  (4 dwords: InputSize, pad x3)
//   Param 1 : Root UAV        u0  Input
//   Param 2 : Root UAV        u1  Output
//
// NOTE: The device-wide spinlock scan (ScanDevice from the Orochi course) is
//       implemented separately in Course06 (Phase 4) because it relies on
//       forward-progress guarantees that D3D12 does not formally provide.
//       The kernels here demonstrate intra-block scans only.

cbuffer Constants : register(b0)
{
    uint InputSize;
    uint Pad0, Pad1, Pad2;
};

RWStructuredBuffer<int> Input  : register(u0);
RWStructuredBuffer<int> Output : register(u1);

static const uint BlockSize = 1024;

// ============================================================
// Hillis-Steele inclusive prefix scan (block-wise).
// Work: O(n log n). Step: O(log n). Simple, work-inefficient.
// ============================================================
groupshared int gs_HS[BlockSize];

[numthreads(BlockSize, 1, 1)]
void ScanBlock_HillisSteeleCS(uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    int  val   = (index < InputSize) ? Input[index] : 0;

    gs_HS[tid.x] = val;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 1; stride < BlockSize; stride <<= 1)
    {
        // Read neighbour before writing (double-buffer via two syncs).
        int add = (tid.x >= stride) ? gs_HS[tid.x - stride] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_HS[tid.x] += add;
        GroupMemoryBarrierWithGroupSync();
    }

    if (index < InputSize) Output[index] = gs_HS[tid.x];
}

// ============================================================
// Blelloch exclusive prefix scan (block-wise), converted to inclusive.
// Work: O(n). Step: O(2 log n). Work-efficient.
// ============================================================
groupshared int gs_BL[BlockSize];

[numthreads(BlockSize, 1, 1)]
void ScanBlock_BlellochCS(uint3 tid : SV_GroupThreadID, uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    int  val   = (index < InputSize) ? Input[index] : 0;

    gs_BL[tid.x] = val;
    GroupMemoryBarrierWithGroupSync();

    // --- Up-sweep (reduce phase) ---
    uint active = BlockSize >> 1;
    for (uint stride = 1; stride < BlockSize; stride <<= 1)
    {
        if (tid.x < active)
        {
            uint left  = stride * (2u * tid.x + 1u) - 1u;
            uint right = stride * (2u * tid.x + 2u) - 1u;
            gs_BL[right] += gs_BL[left];
        }
        active >>= 1;
        GroupMemoryBarrierWithGroupSync();
    }

    // Set identity element at root.
    if (tid.x == 0) gs_BL[BlockSize - 1] = 0;
    GroupMemoryBarrierWithGroupSync();

    // --- Down-sweep phase ---
    active = 1;
    for (uint d = BlockSize >> 1; d >= 1; d >>= 1)
    {
        if (tid.x < active)
        {
            uint left  = d * (2u * tid.x + 1u) - 1u;
            uint right = d * (2u * tid.x + 2u) - 1u;
            int tmp = gs_BL[right];
            gs_BL[right] += gs_BL[left];
            gs_BL[left]   = tmp;
        }
        active <<= 1;
        GroupMemoryBarrierWithGroupSync();
    }

    // gs_BL holds exclusive scan; add original value to get inclusive.
    if (index < InputSize) Output[index] = gs_BL[tid.x] + val;
}
