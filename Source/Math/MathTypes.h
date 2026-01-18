#pragma once

#include <DirectXMath.h>
#include <cstdint>

using FFloat2 = DirectX::XMFLOAT2;
using FFloat3 = DirectX::XMFLOAT3;
using FFloat4 = DirectX::XMFLOAT4;
using FMatrix = DirectX::XMMATRIX;
using FQuaternion = DirectX::XMVECTOR;

struct FUInt4
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    uint32_t w = 0;
};
