#ifndef GPU_DEBUG_LINE_COMMON_HLSL
#define GPU_DEBUG_LINE_COMMON_HLSL

static const uint kDebugLineHeaderLineCountOffset = 0;
static const uint kDebugLineHeaderDroppedCountOffset = 4;
static const uint kDebugLineHeaderSize = 8;
static const uint kDebugLineEntryStride = 32;
static const uint kDebugLineMaxEntries = 8192;

bool DebugDrawLineEnabled()
{
    return true;
}

void DebugDrawLine(uint DebugLineBufferIndex, float3 P0, float3 P1, uint PackedColor)
{
    if (!DebugDrawLineEnabled() || DebugLineBufferIndex == 0xffffffffu)
    {
        return;
    }

    RWByteAddressBuffer DebugLineBuffer = ResourceDescriptorHeap[DebugLineBufferIndex];
    uint lineIndex = 0;
    DebugLineBuffer.InterlockedAdd(kDebugLineHeaderLineCountOffset, 1, lineIndex);
    if (lineIndex >= kDebugLineMaxEntries)
    {
        DebugLineBuffer.InterlockedAdd(kDebugLineHeaderDroppedCountOffset, 1);
        return;
    }

    const uint baseOffset = kDebugLineHeaderSize + lineIndex * kDebugLineEntryStride;
    DebugLineBuffer.Store3(baseOffset + 0, asuint(P0));
    DebugLineBuffer.Store(baseOffset + 12, 0u);
    DebugLineBuffer.Store3(baseOffset + 16, asuint(P1));
    DebugLineBuffer.Store(baseOffset + 28, PackedColor);
}

#endif
