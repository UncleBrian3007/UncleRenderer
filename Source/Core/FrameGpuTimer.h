#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <vector>

class FFrameGpuTimer
{
public:
    // Lazy-initializes resources if needed, then reads back the previous frame's timing.
    // CompletedFenceValue is the queue's current completed fence value (for readback sync).
    // Must be called once per frame before recording begins.
    void BeginFrame(ID3D12Device* Device, ID3D12CommandQueue* Queue,
                    uint64_t CompletedFenceValue,
                    uint32_t BackBufferIndex, uint32_t BackBufferCount);

    // Records the frame-start timestamp. Call immediately after BeginFrame barriers.
    void RecordBeginTimestamp(ID3D12GraphicsCommandList* CommandList, uint32_t BackBufferIndex);

    // Records the frame-end timestamp and resolves query data to the readback buffer.
    // Call just before CloseAndExecute.
    void RecordEndTimestamp(ID3D12GraphicsCommandList* CommandList, uint32_t BackBufferIndex);

    // Stores the fence value for the given back-buffer slot so BeginFrame can
    // determine when the readback data is safe to read.
    void OnFenceSignaled(uint32_t BackBufferIndex, uint64_t FenceValue);

    bool IsReady() const { return QueryHeap && Readback; }

private:
    void EnsureResources(ID3D12Device* Device, uint32_t BackBufferCount);
    void ReadbackPreviousFrame(uint64_t CompletedFenceValue, uint32_t BackBufferIndex);

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> QueryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  Readback;
    std::vector<uint64_t>                   FenceValues;
    uint64_t                                Frequency = 0;
};
