#include "Time.h"

FTime::FTime()
{
    LastFrameTime = std::chrono::high_resolution_clock::now();
}

void FTime::Tick()
{
    auto Current = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> Delta = Current - LastFrameTime;
    LastFrameTime = Current;

    DeltaTime = Delta.count();
    FPS = DeltaTime > 0.0 ? 1.0 / DeltaTime : 0.0;
}
