ByteAddressBuffer StatsBuffer : register(t0);
RWByteAddressBuffer DebugPrintBuffer : register(u0);

#include "DebugPrintCommon.hlsl"

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
    uint frustum = StatsBuffer.Load(0);
    uint occlusion = StatsBuffer.Load(4);

    const uint textColor = 0xffffffffu;
    uint2 pos = uint2(8, 20);
    PrintLabel(pos, textColor, 'F', 'R', 'U', 'S', 'T', 'U', 'M', ' ');
    PrintUInt(uint2(8 + 8 * 8, 20), frustum, textColor);

    pos = uint2(8, 36);
    PrintLabel(pos, textColor, 'O', 'C', 'C', 'L', 'U', 'D', 'E', ' ');
    PrintUInt(uint2(8 + 8 * 8, 36), occlusion, textColor);
}
