// Linear-probing hash table with concurrent GPU insertions and lookups.
//
// Root signature layout:
//   Param 0 : Root Constants  b0  (4 dwords: TableSize, Upper, ItemsPerThread, pad)
//   Param 1 : Root UAV        u0  HashTable  (RWStructuredBuffer<int>, -1 = empty)
//   Param 2 : Root UAV        u1  Counter    (found count for find kernels)

cbuffer Constants : register(b0)
{
    uint TableSize;
    uint Upper;           // Random values in [0, Upper).
    uint ItemsPerThread;
    uint Pad0;
};

RWStructuredBuffer<int> HashTable : register(u0);
RWStructuredBuffer<int> Counter   : register(u1);

static const uint BlockSize = 256;
static const int  EMPTY     = -1;

// ============================================================
// splitmix64 PRNG (mirrors Orochi splitmix64 struct).
// ============================================================
uint64_t splitmix64(inout uint64_t state)
{
    state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// ============================================================
// LP (Linear Probing) insert.
// ============================================================
void LP_insert(int key)
{
    uint slot = ((uint)key) % TableSize;
    for (uint i = 0; i < TableSize; ++i)
    {
        uint s = (slot + i) % TableSize;
        int  prev;
        InterlockedCompareExchange(HashTable[s], EMPTY, key, prev);
        if (prev == EMPTY || prev == key) return;
    }
}

bool LP_find(int key)
{
    uint slot = ((uint)key) % TableSize;
    for (uint i = 0; i < TableSize; ++i)
    {
        uint s = (slot + i) % TableSize;
        int  v = HashTable[s];
        if (v == key)   return true;
        if (v == EMPTY) return false;
    }
    return false;
}

// ============================================================
// BLP (Bit-Lock Linear Probing) – uses the MSB of each slot as a lock bit.
// This avoids the ABA problem when two threads insert the same value.
// ============================================================
static const int LOCK_BIT = 1 << 30;

void BLP_insert(int key)
{
    uint slot = ((uint)key) % TableSize;
    for (uint i = 0; i < TableSize; ++i)
    {
        uint s = (slot + i) % TableSize;

        // Try to lock an empty slot.
        int prev;
        InterlockedCompareExchange(HashTable[s], EMPTY, LOCK_BIT | key, prev);
        if (prev == EMPTY)
        {
            HashTable[s] = key;  // Write real value and clear lock bit.
            return;
        }
        if ((prev & ~LOCK_BIT) == key) return;  // Already inserted.
    }
}

bool BLP_find(int key)
{
    uint slot = ((uint)key) % TableSize;
    for (uint i = 0; i < TableSize; ++i)
    {
        uint s = (slot + i) % TableSize;
        int  v = HashTable[s] & ~LOCK_BIT;  // Strip lock bit when reading.
        if (v == key)   return true;
        if (v == EMPTY) return false;
    }
    return false;
}

// ============================================================
// Kernels
// ============================================================
[numthreads(BlockSize, 1, 1)]
void InsertLPCS(uint3 dtid : SV_DispatchThreadID)
{
    uint tid = dtid.x;
    uint64_t rng = (uint64_t)tid;
    for (uint i = 0; i < ItemsPerThread; ++i)
    {
        uint64_t r = splitmix64(rng);
        int key = (int)(r % Upper);
        LP_insert(key);
    }
}

[numthreads(BlockSize, 1, 1)]
void InsertBLPCS(uint3 dtid : SV_DispatchThreadID)
{
    uint tid = dtid.x;
    uint64_t rng = (uint64_t)tid;
    for (uint i = 0; i < ItemsPerThread; ++i)
    {
        uint64_t r = splitmix64(rng);
        int key = (int)(r % Upper);
        BLP_insert(key);
    }
}

[numthreads(BlockSize, 1, 1)]
void FindLPCS(uint3 dtid : SV_DispatchThreadID)
{
    uint tid = dtid.x;
    int found = 0;

    // Search for same sequence as insert.
    {
        uint64_t rng = (uint64_t)tid;
        for (uint i = 0; i < ItemsPerThread; ++i)
        {
            uint64_t r = splitmix64(rng);
            int key = (int)(r % Upper);
            if (LP_find(key)) ++found;
        }
    }
    // Search for a different sequence.
    {
        uint64_t rng = (uint64_t)(tid ^ 0x12345u);
        for (uint i = 0; i < ItemsPerThread; ++i)
        {
            uint64_t r = splitmix64(rng);
            int key = (int)(r % Upper);
            if (LP_find(key)) ++found;
        }
    }

    InterlockedAdd(Counter[0], found);
}

[numthreads(BlockSize, 1, 1)]
void FindBLPCS(uint3 dtid : SV_DispatchThreadID)
{
    uint tid = dtid.x;
    int found = 0;

    {
        uint64_t rng = (uint64_t)tid;
        for (uint i = 0; i < ItemsPerThread; ++i)
        {
            uint64_t r = splitmix64(rng);
            int key = (int)(r % Upper);
            if (BLP_find(key)) ++found;
        }
    }
    {
        uint64_t rng = (uint64_t)(tid ^ 0x12345u);
        for (uint i = 0; i < ItemsPerThread; ++i)
        {
            uint64_t r = splitmix64(rng);
            int key = (int)(r % Upper);
            if (BLP_find(key)) ++found;
        }
    }

    InterlockedAdd(Counter[0], found);
}
