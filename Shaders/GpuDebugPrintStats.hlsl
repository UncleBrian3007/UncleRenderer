#include "GpuDebugPrintCommon.hlsl"

cbuffer DebugPrintStatsBindlessConstants : register(b0)
{
    uint StatsBufferIndex;
    uint DebugPrintBufferIndex;
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

[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    ByteAddressBuffer StatsBuffer = ResourceDescriptorHeap[StatsBufferIndex];
    uint frustum = StatsBuffer.Load(0);
    uint occlusion = StatsBuffer.Load(4);
    uint cone = StatsBuffer.Load(8);

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
}
