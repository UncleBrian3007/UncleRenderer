#pragma once

#include <cstdint>

enum class EDeferredGBufferSlot : uint32_t
{
    A = 0,
    B,
    C,
    D,
    Max
};

inline constexpr uint32_t kDeferredGBufferCount = static_cast<uint32_t>(EDeferredGBufferSlot::Max);
