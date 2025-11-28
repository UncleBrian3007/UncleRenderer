#pragma once

#include "../Math/MathTypes.h"

class FCamera
{
public:
    FCamera();

    void SetPosition(const FFloat3& InPosition) { Position = InPosition; }
    const FFloat3& GetPosition() const { return Position; }

    void SetForward(const FFloat3& InForward) { Forward = InForward; }
    const FFloat3& GetForward() const { return Forward; }

private:
    FFloat3 Position;
    FFloat3 Forward;
};
