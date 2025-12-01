#pragma once

#include <d3d12.h>

#if __has_include(<pix3.h>)
#define WITH_PIX_EVENTS 1
#include <pix3.h>
#else
#define WITH_PIX_EVENTS 0
#endif

class FScopedPixEvent
{
public:
    FScopedPixEvent(ID3D12GraphicsCommandList* InCommandList, const wchar_t* EventName)
        : CommandList(InCommandList)
    {
#if WITH_PIX_EVENTS
        if (CommandList)
        {
            PIXBeginEvent(CommandList, PIX_COLOR_DEFAULT, EventName);
        }
#endif
    }

    ~FScopedPixEvent()
    {
#if WITH_PIX_EVENTS
        if (CommandList)
        {
            PIXEndEvent(CommandList);
        }
#endif
    }

    FScopedPixEvent(const FScopedPixEvent&) = delete;
    FScopedPixEvent& operator=(const FScopedPixEvent&) = delete;

private:
    ID3D12GraphicsCommandList* CommandList;
};

inline void PixSetMarker(ID3D12GraphicsCommandList* CommandList, const wchar_t* EventName)
{
#if WITH_PIX_EVENTS
    if (CommandList)
    {
        PIXSetMarker(CommandList, PIX_COLOR_DEFAULT, EventName);
    }
#else
    (void)CommandList;
    (void)EventName;
#endif
}

