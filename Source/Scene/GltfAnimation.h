#pragma once

#include "GltfLoader.h"

void InitializeGltfAnimationPose(const FGltfScene& Scene, FGltfAnimationPose& OutPose);
void UpdateGltfAnimationPose(const FGltfScene& Scene, float TimeSeconds, FGltfAnimationPose& InOutPose);
