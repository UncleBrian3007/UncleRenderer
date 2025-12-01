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

    void SetPerspective(float InFovYRadians, float InAspectRatio, float InNearClip, float InFarClip);

    FMatrix GetViewMatrix() const;
    FMatrix GetProjectionMatrix() const;

private:
    FFloat3 Position;
    FFloat3 Forward;
    FFloat3 Up;

    float FovY;
    float AspectRatio;
    float NearClip;
    float FarClip;
};
