#pragma once

#include "DX12Commons.h"

#include <atomic>

extern std::atomic<bool> GDeviceRemoved;
extern std::atomic<bool> GDredDumped;

bool IsDeviceRemovedHr(HRESULT Hr);
void ConfigureDredSettingsBeforeDeviceCreation();
void HandleDeviceRemoved(ID3D12Device* Device, HRESULT Hr, const wchar_t* Where);
void DumpDRED(ID3D12Device* Device);
void ReportDxFailure(ID3D12Device* Device, HRESULT Hr, const wchar_t* Where, IUnknown* RelatedObject);
