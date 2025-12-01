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

    void SetUp(const FFloat3& InUp) { Up = InUp; }
    const FFloat3& GetUp() const { return Up; }

    void SetPerspective(float InFovYRadians, float InAspectRatio, float InNearClip, float InFarClip);

    void SetFovY(float InFovYRadians) { FovY = InFovYRadians; }
    float GetFovY() const { return FovY; }
    float GetAspectRatio() const { return AspectRatio; }
    float GetNearClip() const { return NearClip; }
    float GetFarClip() const { return FarClip; }

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
