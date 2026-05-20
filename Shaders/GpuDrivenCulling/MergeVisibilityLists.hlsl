cbuffer MergeListConstants : register(b1)
{
    uint ListAIndex;
    uint ListBIndex;
    uint CountAIndex;
    uint CountBIndex;
    uint OutputListIndex;
    uint OutputCountIndex;
    uint FlagsIndex;
};

[numthreads(64, 1, 1)]
void MergeVisibilityListsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ByteAddressBuffer CountA = ResourceDescriptorHeap[CountAIndex];
    ByteAddressBuffer CountB = ResourceDescriptorHeap[CountBIndex];
    const uint countA = CountA.Load(0);
    const uint countB = CountB.Load(0);
    const uint totalCount = countA + countB;

    const uint index = dispatchThreadId.x;
    if (index >= totalCount)
        return;

    StructuredBuffer<uint> ListA = ResourceDescriptorHeap[ListAIndex];
    StructuredBuffer<uint> ListB = ResourceDescriptorHeap[ListBIndex];
    RWStructuredBuffer<uint> OutputList = ResourceDescriptorHeap[OutputListIndex];
    RWStructuredBuffer<uint> Flags = ResourceDescriptorHeap[FlagsIndex];

    uint value = 0;
    bool isDuplicate = true;
    if (index < countA)
    {
        value = ListA[index];
        uint previous = 0;
        // compareValue, exchangeValue, originalValue
        InterlockedCompareExchange(Flags[value], 0, 1, previous);
        isDuplicate = previous != 0;
    }
    else
    {
        const uint listIndex = index - countA;
        value = ListB[listIndex];
        uint previous = 0;
        InterlockedCompareExchange(Flags[value], 0, 1, previous);
        isDuplicate = previous != 0;
    }

    if (!isDuplicate)
    {
        RWByteAddressBuffer OutputCount = ResourceDescriptorHeap[OutputCountIndex];
        uint writeIndex = 0;
        OutputCount.InterlockedAdd(0, 1, writeIndex);
        OutputList[writeIndex] = value;

    }

}
