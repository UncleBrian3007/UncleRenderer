#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

#define SAFE_RELEASE(P) if (P) { P->Release(); P = nullptr; }
#define HR_CHECK(x) { HRESULT hr__ = (x); if (FAILED(hr__)) { __debugbreak(); } }
