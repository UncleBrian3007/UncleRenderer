#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <string>
#include <unordered_map>

class FDX12Device;

class FTextureLoader
{
public:
    explicit FTextureLoader(FDX12Device* InDevice);

    bool LoadOrDefault(const std::wstring& TexturePath, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);
    void ClearCache();

private:
    bool TryGetCachedTexture(const std::wstring& TexturePath, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture) const;
    bool LoadTextureInternal(const std::wstring& TexturePath, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);
    bool CreateDefaultGridTexture(Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);

private:
    FDX12Device* Device = nullptr;
    static std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID3D12Resource>> GlobalTextureCache;
};
