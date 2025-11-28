#pragma once

#include "../Math/MathTypes.h"

class FTransform
{
public:
    void SetPosition(const FFloat3& InPosition) { Position = InPosition; }
    void SetScale(const FFloat3& InScale) { Scale = InScale; }
    void SetRotation(const FQuaternion& InRotation) { Rotation = InRotation; }

    const FFloat3& GetPosition() const { return Position; }
    const FFloat3& GetScale() const { return Scale; }
    const FQuaternion& GetRotation() const { return Rotation; }

private:
    FFloat3 Position{0.0f, 0.0f, 0.0f};
    FFloat3 Scale{1.0f, 1.0f, 1.0f};
    FQuaternion Rotation{};
};
