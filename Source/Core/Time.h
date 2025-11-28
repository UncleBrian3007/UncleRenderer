#pragma once

#include <cstdint>
#include <chrono>

class FTime
{
public:
    FTime();

    // 프레임 시간을 갱신하고 DeltaTime/FPS 값을 계산합니다.
    void Tick();

    double GetDeltaTimeSeconds() const { return DeltaTime; }
    double GetFPS() const { return FPS; }

private:
    std::chrono::high_resolution_clock::time_point LastFrameTime;
    double DeltaTime = 0.0;
    double FPS = 0.0;
};
