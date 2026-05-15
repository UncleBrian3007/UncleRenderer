#pragma once

// Wave-level inclusive prefix sum (addition).
// Each lane i returns sum of Input[0..i].
template<typename T>
T WaveScanInclusiveSum(T val)
{
    uint laneCount = WaveGetLaneCount();
    for (uint i = 1; i < laneCount; i <<= 1)
    {
        T tmp = WaveReadLaneAt(val, WaveGetLaneIndex() - i);
        if (WaveGetLaneIndex() >= i) val += tmp;
    }
    return val;
}

// Wave-level exclusive prefix sum.
// Each lane i returns sum of Input[0..i-1]. Lane 0 returns 0.
template<typename T>
T WaveScanExclusiveSum(T val)
{
    return WaveScanInclusiveSum(val) - val;
}

// Wave-level reduction (sum of all active lanes).
// Equivalent to WaveActiveSum but explicit for learning purposes.
template<typename T>
T WaveReduceSum(T val)
{
    uint laneCount = WaveGetLaneCount();
    for (uint i = 1; i < laneCount; i <<= 1)
        val += WaveReadLaneAt(val, WaveGetLaneIndex() ^ i);
    return val;
}
