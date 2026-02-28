#pragma once

#include <d3d12.h>

#if __has_include(<pix3.h>)
#define WITH_PIX_EVENTS 1
#include <pix3.h>
#else
#define WITH_PIX_EVENTS 0
#endif

inline bool GPixEventsEnabled = true;
inline bool GModelPixEventsEnabled = true;

class FScopedPixEvent
{
public:
    FScopedPixEvent(ID3D12GraphicsCommandList* InCommandList, const wchar_t* EventName, bool bEnabled = true)
        : CommandList(InCommandList)
    {
#if WITH_PIX_EVENTS
        bActive = CommandList && bEnabled && GPixEventsEnabled;
        if (bActive)
        {
            PIXBeginEvent(CommandList, PIX_COLOR_DEFAULT, EventName);
        }
#endif
    }

    ~FScopedPixEvent()
    {
#if WITH_PIX_EVENTS
        if (bActive)
        {
            PIXEndEvent(CommandList);
        }
#endif
    }

    FScopedPixEvent(const FScopedPixEvent&) = delete;
    FScopedPixEvent& operator=(const FScopedPixEvent&) = delete;

private:
    ID3D12GraphicsCommandList* CommandList;
    bool bActive = false;
};

inline void SetPixEventsEnabled(bool bEnabled)
{
    GPixEventsEnabled = bEnabled;
}

inline void SetModelPixEventsEnabled(bool bEnabled)
{
    GModelPixEventsEnabled = bEnabled;
}

inline bool AreModelPixEventsEnabled()
{
    return GModelPixEventsEnabled;
}

inline bool BeginPixGpuCapture(bool bShouldCapture = true)
{
#if WITH_PIX_EVENTS
    if (!bShouldCapture)
    {
        return false;
    }

    return SUCCEEDED(PIXBeginCapture(PIX_CAPTURE_GPU, nullptr));
#else
    (void)bShouldCapture;
    return false;
#endif
}

inline bool BeginPixGpuCaptureOnce(bool bShouldCapture = true)
{
#if WITH_PIX_EVENTS
    static bool bCaptured = false;
    if (!bShouldCapture || bCaptured)
    {
        return false;
    }

    if (!BeginPixGpuCapture(true))
    {
        return false;
    }

    bCaptured = true;
    return true;
#else
    (void)bShouldCapture;
    return false;
#endif
}

inline void EndPixGpuCapture(bool bCaptureActive)
{
#if WITH_PIX_EVENTS
    if (!bCaptureActive)
    {
        return;
    }

    PIXEndCapture(FALSE);
#else
    (void)bCaptureActive;
#endif
}
