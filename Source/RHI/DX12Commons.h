#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dx12.h>
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

void ReportDxFailure(ID3D12Device* Device, HRESULT Hr, const wchar_t* Where, IUnknown* RelatedObject = nullptr);

#define HR_CHECK(x) do { HRESULT hr__ = (x); if (FAILED(hr__)) { ReportDxFailure(nullptr, hr__, L#x, nullptr); } } while (0)
#define HR_CHECK_DX(DevicePtr, Expr, WhereString) do { HRESULT hr__ = (Expr); if (FAILED(hr__)) { ReportDxFailure((DevicePtr), hr__, (WhereString), nullptr); } } while (0)
#define HR_CHECK_DX_OBJ(DevicePtr, ObjPtr, Expr, WhereString) do { HRESULT hr__ = (Expr); if (FAILED(hr__)) { ReportDxFailure((DevicePtr), hr__, (WhereString), (ObjPtr)); } } while (0)
