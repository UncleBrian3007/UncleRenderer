#include "StringUtils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cwchar>

namespace StringUtils
{
    std::string WideToUtf8(const std::wstring& Text)
    {
        if (Text.empty())
        {
            return {};
        }

        const int Utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
        if (Utf8Length <= 0)
        {
            return {};
        }

        std::string Result(static_cast<size_t>(Utf8Length), '\0');
        const int Converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), Result.data(), Utf8Length, nullptr, nullptr);
        if (Converted <= 0)
        {
            return {};
        }

        return Result;
    }

    std::string WideToUtf8(const wchar_t* Text)
    {
        if (!Text || Text[0] == L'\0')
        {
            return {};
        }

        const int WideLength = static_cast<int>(wcslen(Text));
        const int Utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text, WideLength, nullptr, 0, nullptr, nullptr);
        if (Utf8Length <= 0)
        {
            return {};
        }

        std::string Result(static_cast<size_t>(Utf8Length), '\0');
        const int Converted = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, Text, WideLength, Result.data(), Utf8Length, nullptr, nullptr);
        if (Converted <= 0)
        {
            return {};
        }

        return Result;
    }

    std::wstring Utf8ToWide(const std::string& Text)
    {
        if (Text.empty())
        {
            return {};
        }

        const int WideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
        if (WideLength <= 0)
        {
            return {};
        }

        std::wstring Result(static_cast<size_t>(WideLength), L'\0');
        const int Converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.data(), static_cast<int>(Text.size()), Result.data(), WideLength);
        if (Converted <= 0)
        {
            return {};
        }

        return Result;
    }

    std::string PathToUtf8(const std::filesystem::path& Path)
    {
        const auto Utf8 = Path.u8string();
        return std::string(Utf8.begin(), Utf8.end());
    }
}
