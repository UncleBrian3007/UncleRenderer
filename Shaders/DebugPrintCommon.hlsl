#ifndef DEBUG_PRINT_COMMON_HLSL
#define DEBUG_PRINT_COMMON_HLSL

static const uint kDebugPrintHeaderSize = 4;
static const uint kDebugPrintEntryStride = 16;
static const uint kDebugPrintMaxEntries = 4096;
static const uint kDebugPrintDefaultAdvance = 8;

uint DebugPrintPackChars(uint c0, uint c1, uint c2, uint c3)
{
    return (c0 & 0xFFu) | ((c1 & 0xFFu) << 8) | ((c2 & 0xFFu) << 16) | ((c3 & 0xFFu) << 24);
}

uint DebugPrintUnpackChar(uint packed, uint index)
{
    return (packed >> (index * 8)) & 0xFFu;
}

#if !defined(DEBUG_PRINT_READ_ONLY)
void PrintChar(uint2 position, uint code, uint color)
{
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
#endif

#endif
