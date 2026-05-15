// Root signature layout:
//   Param 0 : Root Constants  b0  (4 dwords: InputSize, pad x3)
//   Param 1 : Root UAV        u0  Input
//   Param 2 : Root UAV        u1  Output  (compacted result)
//   Param 3 : Root UAV        u2  Counter (atomic enqueue index)

cbuffer Constants : register(b0)
{
    uint InputSize;
    uint Pad0, Pad1, Pad2;
};

RWStructuredBuffer<int> Input   : register(u0);
RWStructuredBuffer<int> Output  : register(u1);
RWStructuredBuffer<int> Counter : register(u2);

static const uint BlockSize = 1024;

// ============================================================
// Kernel 1: Naive enqueue - one atomic per active thread.
// Very inefficient: atomic serialisation is per-thread.
// Mirrors EnqueueNaiveKernel.
// ============================================================
[numthreads(BlockSize, 1, 1)]
void EnqueueNaiveCS(uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    if (index >= InputSize) return;

    int  val     = Input[index];
    bool enqueue = (val & 1) != 0;   // Enqueue odd values.

    if (enqueue)
    {
        int slot;
        InterlockedAdd(Counter[0], 1, slot);
        Output[slot] = val;
    }
}

// ============================================================
// Kernel 2: Wave-efficient enqueue via WavePrefixSum.
// One atomic per wave instead of per thread.
// Mirrors EnqueueKernel (two-items-per-thread variant simplified to one).
// ============================================================
[numthreads(BlockSize, 1, 1)]
void EnqueueWaveCS(uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    int  val   = (index < InputSize) ? Input[index] : 0;
    bool enqueue = (index < InputSize) && ((val & 1) != 0);

    // Exclusive prefix count of enqueued items within the wave.
    uint waveScan  = WavePrefixCountBits(enqueue);
    // Total enqueued in this wave.
    uint waveTotal = WaveActiveCountBits(enqueue);

    // Last lane performs the single wave-level atomic to claim a block of slots.
    uint waveOffset = 0;
    if (WaveGetLaneIndex() == WaveGetLaneCount() - 1)
        InterlockedAdd(Counter[0], waveTotal, waveOffset);

    // Broadcast the wave base offset to all lanes.
    waveOffset = WaveReadLaneAt(waveOffset, WaveGetLaneCount() - 1);

    if (enqueue)
        Output[waveOffset + waveScan] = val;
}

// ============================================================
// Kernel 3: Binary enqueue using WaveActiveBallot + countbits.
// Equivalent to __ballot / __popcll in CUDA.
// Mirrors EnqueueBinaryKernel.
// ============================================================
[numthreads(BlockSize, 1, 1)]
void EnqueueBinaryCS(uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    int  val   = (index < InputSize) ? Input[index] : 0;
    bool enqueue = (index < InputSize) && ((val & 1) != 0);

    // ballot returns a uint4 bitmask of which lanes have enqueue==true.
    uint4 ballot  = WaveActiveBallot(enqueue);
    uint  laneIdx = WaveGetLaneIndex();

    // Build a mask of bits with lane index < laneIdx and count them
    // to get the exclusive prefix within the wave.
    // We only handle up to 64 lanes (ballot.xy covers 64 bits).
    uint lowBits  = (laneIdx < 32u) ? ((1u << laneIdx) - 1u) : 0xFFFFFFFFu;
    uint highBits = (laneIdx >= 32u) ? ((1u << (laneIdx - 32u)) - 1u) : 0u;
    uint waveScan = countbits(ballot.x & lowBits) + countbits(ballot.y & highBits);

    uint waveTotal = countbits(ballot.x) + countbits(ballot.y);

    uint waveOffset = 0;
    if (WaveGetLaneIndex() == WaveGetLaneCount() - 1)
        InterlockedAdd(Counter[0], waveTotal, waveOffset);
    waveOffset = WaveReadLaneAt(waveOffset, WaveGetLaneCount() - 1);

    if (enqueue)
        Output[waveOffset + waveScan] = val;
}

// ============================================================
// Kernel 4: Complement enqueue.
// Odd values => front of output, even values => back.
// Uses the complementary property: rank_back = laneIdx - rank_front.
// Mirrors EnqueueComplementKernel.
// ============================================================
RWStructuredBuffer<int> Counter2 : register(u3);  // Second counter for back-fill.

[numthreads(BlockSize, 1, 1)]
void EnqueueComplementCS(uint3 dtid : SV_DispatchThreadID)
{
    uint index = dtid.x;
    if (index >= InputSize) return;

    int  val     = Input[index];
    bool enqueue = (val & 1) != 0;

    uint4 ballot  = WaveActiveBallot(enqueue);
    uint  laneIdx = WaveGetLaneIndex();

    uint lowBits  = (laneIdx < 32u) ? ((1u << laneIdx) - 1u) : 0xFFFFFFFFu;
    uint highBits = (laneIdx >= 32u) ? ((1u << (laneIdx - 32u)) - 1u) : 0u;
    uint waveScan      = countbits(ballot.x & lowBits)     + countbits(ballot.y & highBits);
    uint waveTotal     = countbits(ballot.x)               + countbits(ballot.y);
    uint complWaveScan = laneIdx - waveScan;
    uint complTotal    = WaveGetLaneCount() - waveTotal;

    // Front offset for 'true' elements.
    uint waveOffset = 0;
    if (WaveGetLaneIndex() == WaveGetLaneCount() - 1)
        InterlockedAdd(Counter[0], waveTotal, waveOffset);
    waveOffset = WaveReadLaneAt(waveOffset, WaveGetLaneCount() - 1);

    // Back offset for 'false' elements (fills from the end).
    uint complWaveOffset = 0;
    if (WaveGetLaneIndex() == WaveGetLaneCount() - 1)
        InterlockedAdd(Counter2[0], complTotal, complWaveOffset);
    complWaveOffset = WaveReadLaneAt(complWaveOffset, WaveGetLaneCount() - 1);

    if (enqueue)
        Output[waveOffset + waveScan] = val;
    else
        Output[InputSize - 1u - (complWaveOffset + complWaveScan)] = val;
}
