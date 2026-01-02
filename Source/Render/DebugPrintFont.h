#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <cstdint>
#include <string>
#include <vector>

class FDX12Device;

struct FDebugPrintGlyph
{
    float UvMin[2];
    float UvMax[2];
    float Size[2];
    float Offset[2];
    float Advance = 0.0f;
    float Padding = 0.0f;
};

struct FDebugPrintFontResources
{
    Microsoft::WRL::ComPtr<ID3D12Resource> FontTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> GlyphBuffer;
    uint32_t AtlasWidth = 0;
    uint32_t AtlasHeight = 0;
    uint32_t FirstChar = 32;
    uint32_t CharCount = 96;
    float FontSize = 16.0f;
};

bool CreateDebugPrintFontResources(
    FDX12Device* Device,
    const std::wstring& FontPath,
    float FontSize,
    uint32_t AtlasWidth,
    uint32_t AtlasHeight,
    FDebugPrintFontResources& OutResources);
