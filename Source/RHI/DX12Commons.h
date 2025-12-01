#pragma once
#include <Windows.h>
#include <d3d12.h>

#ifndef D3D12_SDK_PATH_W
#define D3D12_SDK_PATH_W L".\\D3D12\\"
#endif

#ifndef D3D12SDKVersion
#define D3D12SDKVersion D3D12_SDK_VERSION
#endif
#ifndef D3D12SDKPath
#define D3D12SDKPath D3D12_SDK_PATH_W
#endif

#include <dxgi1_6.h>
#include <wrl.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

using uint8  = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int8   = int8_t;
using int16  = int16_t;
using int32  = int32_t;
using int64  = int64_t;

#define SAFE_RELEASE(P) if (P) { P->Release(); P = nullptr; }
#define HR_CHECK(x) { HRESULT hr__ = (x); if (FAILED(hr__)) { __debugbreak(); } }
