#include "../ClusterDag/ClusterDagCommon.hlsl"
#include "GpuDebugPrintCommon.hlsl"
#include "GpuDebugLineCommon.hlsl"

cbuffer DebugPrintStatsBindlessConstants : register(b0)
{
    uint StatsBufferIndex;
    uint DebugPrintBufferIndex;
    uint DebugLineBufferIndex;
};

uint DebugPrintPackChars(uint c0, uint c1, uint c2, uint c3)
{
	return (c0 & 0xFFu) | ((c1 & 0xFFu) << 8) | ((c2 & 0xFFu) << 16) | ((c3 & 0xFFu) << 24);
}

uint DebugPrintUnpackChar(uint packed, uint index)
{
	return (packed >> (index * 8)) & 0xFFu;
}

void PrintChar(uint2 position, uint code, uint color)
{
	RWByteAddressBuffer DebugPrintBuffer = ResourceDescriptorHeap[DebugPrintBufferIndex];
	uint index = 0;
    DebugPrintBuffer.InterlockedAdd(0, 1, index);
	if (index >= kDebugPrintMaxEntries)
	{
		return;
	}

	uint offset = kDebugPrintHeaderSize + index * kDebugPrintEntryStride;
    DebugPrintBuffer.Store(offset + 0, position.x);
    DebugPrintBuffer.Store(offset + 4, position.y);
    DebugPrintBuffer.Store(offset + 8, code);
    DebugPrintBuffer.Store(offset + 12, color);
}

void PrintString(uint2 position, uint color, uint length, uint packed0, uint packed1)
{
	uint2 cursor = position;
	for (uint i = 0; i < length; ++i)
	{
		uint packed = i < 4 ? packed0 : packed1;
		uint code = DebugPrintUnpackChar(packed, i % 4);
		if (code == 0)
		{
			return;
		}

		PrintChar(cursor, code, color);
		cursor.x += kDebugPrintDefaultAdvance;
	}
}

void PrintLabel(uint2 position, uint color, uint c0, uint c1, uint c2, uint c3, uint c4, uint c5, uint c6, uint c7)
{
    uint packed0 = DebugPrintPackChars(c0, c1, c2, c3);
    uint packed1 = DebugPrintPackChars(c4, c5, c6, c7);
    PrintString(position, color, 8u, packed0, packed1);
}

void PrintUInt(uint2 position, uint value, uint color)
{
    uint2 cursor = position;
    uint divisor = 10000u;
    bool started = false;
    for (uint i = 0; i < 5; ++i)
    {
        uint digit = value / divisor;
        value -= digit * divisor;
        divisor = max(1u, divisor / 10u);

        if (digit != 0 || started || i == 4)
        {
            started = true;
            PrintChar(cursor, 48u + digit, color);
            cursor.x += kDebugPrintDefaultAdvance;
        }
    }
}

void PrintMipBucketLabel(uint2 position, uint color, uint bucket)
{
    if (bucket < kClusterDagVisibleMipHistogramOverflowBucket)
    {
        PrintLabel(position, color, 'M', 'I', 'P', 48u + bucket, ' ', ' ', ' ', ' ');
    }
    else
    {
        PrintLabel(position, color, 'M', 'I', 'P', '5', '+', ' ', ' ', ' ');
    }
}

[numthreads(1, 1, 1)]
void GpuDebugPrintStatsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ByteAddressBuffer StatsBuffer = ResourceDescriptorHeap[StatsBufferIndex];
    uint frustum = StatsBuffer.Load(0);
    uint occlusion = StatsBuffer.Load(4);
    uint cone = StatsBuffer.Load(8);
    uint merged = StatsBuffer.Load(4 * kDebugPrintStatsMergedIndex);
    uint lateVisible = StatsBuffer.Load(4 * kDebugPrintStatsLateVisibleIndex);
    uint clusterDagVisible = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagVisibleIndex);
    uint clusterDagLeafVisible = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagLeafVisibleIndex);
    uint clusterDagLeafFrustumCulled = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagLeafFrustumCulledIndex);
    uint clusterDagStackOverflow = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagStackOverflowIndex);
    uint clusterDagExpandedOverflow = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagExpandedOverflowIndex);
    uint clusterDagIterationOverflow = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagIterationOverflowIndex);
    uint clusterDagPersistentOverflow = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagPersistentOverflowIndex);
    uint clusterDagPersistentGroupCommitSpin = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagPersistentGroupCommitSpinIndex);
    uint clusterDagPersistentCandidateCommitSpin = StatsBuffer.Load(4 * kDebugPrintStatsClusterDagPersistentCandidateCommitSpinIndex);
    uint clusterDagNonLeafVisible = StatsBuffer.Load(4 * kClusterDagNonLeafVisibleStatIndex);
    uint clusterDagNonLeafFrustumCulled = StatsBuffer.Load(4 * kClusterDagNonLeafFrustumCulledStatIndex);
    uint clusterDagVisibleMipHistogram[kClusterDagVisibleMipHistogramBucketCount];
    [unroll]
    for (uint loadBucket = 0u; loadBucket < kClusterDagVisibleMipHistogramBucketCount; ++loadBucket)
    {
        clusterDagVisibleMipHistogram[loadBucket] = StatsBuffer.Load(4u * (kClusterDagVisibleMipHistogramBaseStatIndex + loadBucket));
    }

    const uint textColor = 0xffffffffu;
    uint2 pos = uint2(8, 20);
    PrintLabel(pos, textColor, 'F', 'R', 'U', 'S', 'T', 'U', 'M', ' ');
    PrintUInt(uint2(8 + 8 * 8, 20), frustum, textColor);

    pos = uint2(8, 36);
    PrintLabel(pos, textColor, 'O', 'C', 'C', 'L', 'U', 'D', 'E', ' ');
    PrintUInt(uint2(8 + 8 * 8, 36), occlusion, textColor);

    pos = uint2(8, 52);
    PrintLabel(pos, textColor, 'C', 'O', 'N', 'E', 'C', 'U', 'L', ' ');
    PrintUInt(uint2(8 + 8 * 8, 52), cone, textColor);

    pos = uint2(8, 68);
    PrintLabel(pos, textColor, 'M', 'E', 'R', 'G', 'E', 'D', ' ', ' ');
    PrintUInt(uint2(8 + 8 * 8, 68), merged, textColor);

    pos = uint2(8, 84);
    PrintLabel(pos, textColor, 'L', 'A', 'T', 'E', 'V', 'I', 'S', ' ');
    PrintUInt(uint2(8 + 8 * 8, 84), lateVisible, textColor);

    pos = uint2(8, 100);
    PrintLabel(pos, textColor, 'C', 'L', 'U', 'S', 'T', 'E', 'R', ' ');
    PrintUInt(uint2(8 + 8 * 8, 100), clusterDagVisible, textColor);

    pos = uint2(8, 116);
    PrintLabel(pos, textColor, 'L', 'E', 'A', 'F', ' ', ' ', ' ', ' ');
    PrintUInt(uint2(8 + 8 * 8, 116), clusterDagLeafVisible, textColor);

    pos = uint2(8, 132);
    PrintLabel(pos, textColor, 'N', 'L', 'V', 'I', 'S', ' ', ' ', ' ');
    PrintUInt(uint2(8 + 8 * 8, 132), clusterDagNonLeafVisible, textColor);

    pos = uint2(8, 148);
    PrintLabel(pos, textColor, 'L', 'E', 'A', 'F', 'C', 'U', 'L', ' ');
    PrintUInt(uint2(8 + 8 * 8, 148), clusterDagLeafFrustumCulled, textColor);

    pos = uint2(8, 164);
    PrintLabel(pos, textColor, 'N', 'L', 'C', 'U', 'L', ' ', ' ', ' ');
    PrintUInt(uint2(8 + 8 * 8, 164), clusterDagNonLeafFrustumCulled, textColor);

    pos = uint2(8, 180);
    PrintLabel(pos, textColor, 'S', 'T', 'K', 'D', 'R', 'O', 'P', ' ');
    PrintUInt(uint2(8 + 8 * 8, 180), clusterDagStackOverflow, textColor);

    pos = uint2(8, 196);
    PrintLabel(pos, textColor, 'G', 'R', 'P', 'D', 'R', 'O', 'P', ' ');
    PrintUInt(uint2(8 + 8 * 8, 196), clusterDagExpandedOverflow, textColor);

    pos = uint2(8, 212);
    PrintLabel(pos, textColor, 'I', 'T', 'E', 'R', 'O', 'V', 'F', ' ');
    PrintUInt(uint2(8 + 8 * 8, 212), clusterDagIterationOverflow, textColor);

    pos = uint2(8, 228);
    PrintLabel(pos, textColor, 'Q', 'O', 'V', 'E', 'R', 'F', ' ', ' ');
    PrintUInt(uint2(8 + 8 * 8, 228), clusterDagPersistentOverflow, textColor);

    pos = uint2(8, 244);
    PrintLabel(pos, textColor, 'Q', 'G', 'S', 'P', 'I', 'N', ' ', ' ');
    PrintUInt(uint2(8 + 8 * 8, 244), clusterDagPersistentGroupCommitSpin, textColor);

    pos = uint2(8, 260);
    PrintLabel(pos, textColor, 'Q', 'C', 'S', 'P', 'I', 'N', ' ', ' ');
    PrintUInt(uint2(8 + 8 * 8, 260), clusterDagPersistentCandidateCommitSpin, textColor);

    uint histogramStartY = 276u;

    if (DebugLineBufferIndex != 0xffffffffu)
    {
        ByteAddressBuffer DebugLineBuffer = ResourceDescriptorHeap[DebugLineBufferIndex];
        const uint lineCount = DebugLineBuffer.Load(kDebugLineHeaderLineCountOffset);
        const uint droppedCount = DebugLineBuffer.Load(kDebugLineHeaderDroppedCountOffset);

        pos = uint2(8, 276);
        PrintLabel(pos, textColor, 'L', 'I', 'N', 'E', 'S', ' ', ' ', ' ');
        PrintUInt(uint2(8 + 8 * 8, 276), lineCount, textColor);

        pos = uint2(8, 292);
        PrintLabel(pos, textColor, 'D', 'R', 'O', 'P', 'P', 'E', 'D', ' ');
        PrintUInt(uint2(8 + 8 * 8, 292), droppedCount, textColor);

        histogramStartY = 308u;
    }

    [unroll]
    for (uint drawBucket = 0u; drawBucket < kClusterDagVisibleMipHistogramBucketCount; ++drawBucket)
    {
        pos = uint2(8, histogramStartY + drawBucket * 16u);
        PrintMipBucketLabel(pos, textColor, drawBucket);
        PrintUInt(uint2(8 + 8 * 8, histogramStartY + drawBucket * 16u), clusterDagVisibleMipHistogram[drawBucket], textColor);
    }
}
