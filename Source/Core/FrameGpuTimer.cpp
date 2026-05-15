#include "FrameGpuTimer.h"

#include "../Render/RenderGraph.h"

#include <d3dx12.h>

void FFrameGpuTimer::BeginFrame(
    ID3D12Device* Device,
    ID3D12CommandQueue* Queue,
    uint64_t CompletedFenceValue,
    uint32_t BackBufferIndex,
    uint32_t BackBufferCount)
{
    EnsureResources(Device, BackBufferCount);

    if (Frequency == 0 && Queue)
    {
        Queue->GetTimestampFrequency(&Frequency);
    }

    ReadbackPreviousFrame(CompletedFenceValue, BackBufferIndex);
}

void FFrameGpuTimer::RecordBeginTimestamp(ID3D12GraphicsCommandList* CommandList, uint32_t BackBufferIndex)
{
    if (!CommandList || !IsReady())
    {
        return;
    }

    CommandList->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, BackBufferIndex * 2);
}

void FFrameGpuTimer::RecordEndTimestamp(ID3D12GraphicsCommandList* CommandList, uint32_t BackBufferIndex)
{
    if (!CommandList || !IsReady())
    {
        return;
    }

    const uint32_t QueryIndex = BackBufferIndex * 2;
    CommandList->EndQuery(QueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, QueryIndex + 1);
    CommandList->ResolveQueryData(
        QueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        QueryIndex,
        2,
        Readback.Get(),
        static_cast<UINT64>(QueryIndex) * sizeof(uint64_t));
}

void FFrameGpuTimer::OnFenceSignaled(uint32_t BackBufferIndex, uint64_t FenceValue)
{
    if (BackBufferIndex < static_cast<uint32_t>(FenceValues.size()))
    {
        FenceValues[BackBufferIndex] = FenceValue;
    }
}

void FFrameGpuTimer::EnsureResources(ID3D12Device* Device, uint32_t BackBufferCount)
{
    if (!Device || (IsReady() && FenceValues.size() == BackBufferCount))
    {
        return;
    }

    FenceValues.assign(BackBufferCount, 0);
    Frequency = 0;

    D3D12_QUERY_HEAP_DESC HeapDesc = {};
    HeapDesc.Count = BackBufferCount * 2;
    HeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    HeapDesc.NodeMask = 0;
    Device->CreateQueryHeap(&HeapDesc, IID_PPV_ARGS(QueryHeap.ReleaseAndGetAddressOf()));

    const UINT64 ReadbackSize = static_cast<UINT64>(BackBufferCount * 2) * sizeof(uint64_t);
    CD3DX12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE_READBACK);
    CD3DX12_RESOURCE_DESC BufferDesc = CD3DX12_RESOURCE_DESC::Buffer(ReadbackSize);
    Device->CreateCommittedResource(
        &HeapProps,
        D3D12_HEAP_FLAG_NONE,
        &BufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(Readback.ReleaseAndGetAddressOf()));
}

void FFrameGpuTimer::ReadbackPreviousFrame(uint64_t CompletedFenceValue, uint32_t BackBufferIndex)
{
    if (!IsReady() || Frequency == 0 || BackBufferIndex >= static_cast<uint32_t>(FenceValues.size()))
    {
        return;
    }

    const uint64_t FenceValue = FenceValues[BackBufferIndex];
    if (FenceValue == 0 || CompletedFenceValue < FenceValue)
    {
        return;
    }

    const UINT64 Offset = static_cast<UINT64>(BackBufferIndex * 2) * sizeof(uint64_t);
    D3D12_RANGE ReadRange = { Offset, Offset + sizeof(uint64_t) * 2 };
    uint64_t* TimestampData = nullptr;
    if (FAILED(Readback->Map(0, &ReadRange, reinterpret_cast<void**>(&TimestampData))) || !TimestampData)
    {
        return;
    }

    const uint64_t Start = TimestampData[BackBufferIndex * 2];
    const uint64_t End   = TimestampData[BackBufferIndex * 2 + 1];
    if (End > Start)
    {
        const double Milliseconds = static_cast<double>(End - Start) / static_cast<double>(Frequency) * 1000.0;
        FRenderGraph::AddExternalGpuTimingSample("Frame", Milliseconds);
    }

    Readback->Unmap(0, nullptr);
    FenceValues[BackBufferIndex] = 0;
}
