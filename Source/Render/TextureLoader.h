#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>

class FDX12Device;

struct FTextureLoadRequest
{
    std::wstring Path;
    uint32_t SolidColor = 0;
    bool bUseSolidColor = false;
    Microsoft::WRL::ComPtr<ID3D12Resource>* OutTexture = nullptr;
    bool bSuccess = false;
};

class FTextureLoader
{
public:
    explicit FTextureLoader(FDX12Device* InDevice);

    bool LoadOrDefault(const std::wstring& TexturePath, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);
    bool LoadOrSolidColor(const std::wstring& TexturePath, uint32_t Color, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);
    void ClearCache();

    /**
     * Load multiple textures in parallel using the task system.
     * @param Requests Vector of texture load requests to process
     * @return True if all textures loaded successfully
     */
    bool LoadTexturesParallel(std::vector<FTextureLoadRequest>& Requests);

private:
    bool TryGetCachedTexture(const std::wstring& TexturePath, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture) const;
    bool LoadTextureInternal(const std::wstring& TexturePath, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);
    bool CreateDefaultGridTexture(Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);
    bool CreateSolidColorTexture(uint32_t Color, Microsoft::WRL::ComPtr<ID3D12Resource>& OutTexture);

private:
    FDX12Device* Device = nullptr;
    static std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID3D12Resource>> GlobalTextureCache;
};
