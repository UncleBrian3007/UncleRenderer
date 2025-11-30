#pragma once

#include <cstdint>
#include <chrono>

class FTime
{
public:
    FTime();

    void Tick();

    double GetDeltaTimeSeconds() const { return DeltaTime; }
    double GetFPS() const { return FPS; }

private:
    std::chrono::high_resolution_clock::time_point LastFrameTime;
    double DeltaTime = 0.0;
    double FPS = 0.0;
};
