#pragma once

#include <filesystem>
#include <string>

namespace StringUtils
{
    std::string WideToUtf8(const std::wstring& Text);
    std::string WideToUtf8(const wchar_t* Text);
    std::wstring Utf8ToWide(const std::string& Text);
    std::string PathToUtf8(const std::filesystem::path& Path);
}
