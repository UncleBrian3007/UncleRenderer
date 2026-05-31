#pragma once

#include "GltfLoader.h"

void InitializeGltfAnimationPose(const FGltfAnimationRuntime& Scene, FGltfAnimationPose& OutPose);
void UpdateGltfAnimationPose(const FGltfAnimationRuntime& Scene, float TimeSeconds, FGltfAnimationPose& InOutPose);
