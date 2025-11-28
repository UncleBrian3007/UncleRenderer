#pragma once

#include <string>
#include "../Math/MathTypes.h"

class FMaterial
{
public:
    void SetName(const std::string& InName) { Name = InName; }
    const std::string& GetName() const { return Name; }

    void SetBaseColor(const FFloat3& InColor) { BaseColor = InColor; }
    const FFloat3& GetBaseColor() const { return BaseColor; }

private:
    std::string Name;
    FFloat3 BaseColor{1.0f, 1.0f, 1.0f};
};
