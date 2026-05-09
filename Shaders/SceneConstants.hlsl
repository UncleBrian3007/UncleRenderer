#ifndef SCENE_CONSTANTS_HLSL
#define SCENE_CONSTANTS_HLSL

#include "SceneConstantsFields.hlsli"

cbuffer SceneConstants : register(b0)
{
    SCENE_CONSTANTS_FIELDS
};

#endif
