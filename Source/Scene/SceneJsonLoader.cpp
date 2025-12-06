#include "SceneJsonLoader.h"

#include "../Core/Logger.h"

#include <algorithm>
#include <cctype>
#include <codecvt>
#include <fstream>
#include <locale>
#include <regex>
#include <sstream>
#include <string>
#include <limits>

namespace
{
    std::string ReadFileToString(const std::wstring& FilePath)
    {
        std::ifstream File(FilePath);
        if (!File.is_open())
        {
            return {};
        }

        std::string Contents((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
        return Contents;
    }

    std::wstring Utf8ToWide(const std::string& Text)
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> Converter;
        return Converter.from_bytes(Text);
    }

    std::string ExtractString(const std::string& Text, const std::string& Key)
    {
        const std::regex Pattern("\"" + Key + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch Match;
        if (std::regex_search(Text, Match, Pattern) && Match.size() > 1)
        {
            return Match[1].str();
        }
        return {};
    }

    bool ExtractBool(const std::string& Text, const std::string& Key, bool DefaultValue)
    {
        const std::regex Pattern("\"" + Key + "\"\\s*:\\s*(true|false|1|0)", std::regex_constants::icase);
        std::smatch Match;
        if (std::regex_search(Text, Match, Pattern) && Match.size() > 1)
        {
            const std::string Value = Match[1].str();
            if (Value == "1")
            {
                return true;
            }
            if (Value == "0")
            {
                return false;
            }

            std::string Lower = Value;
            std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char Char)
            {
                return static_cast<char>(std::tolower(Char));
            });

            if (Lower == "true")
            {
                return true;
            }
            if (Lower == "false")
            {
                return false;
            }
        }
        return DefaultValue;
    }

    FFloat3 ParseVectorAttribute(const std::string& Text, const std::string& Key, const FFloat3& DefaultValue)
    {
        const std::regex Pattern("\"" + Key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
        std::smatch Match;
        if (!std::regex_search(Text, Match, Pattern) || Match.size() < 2)
        {
            return DefaultValue;
        }

        FFloat3 Result = DefaultValue;
        std::stringstream Stream(Match[1].str());
        Stream >> Result.x;
        Stream.ignore(std::numeric_limits<std::streamsize>::max(), ',');
        Stream >> Result.y;
        Stream.ignore(std::numeric_limits<std::streamsize>::max(), ',');
        Stream >> Result.z;

        if (!Stream)
        {
            return DefaultValue;
        }

        return Result;
    }

    size_t FindMatchingBracket(const std::string& Text, size_t StartIndex)
    {
        int Depth = 0;
        for (size_t Index = StartIndex; Index < Text.size(); ++Index)
        {
            if (Text[Index] == '[')
            {
                ++Depth;
            }
            else if (Text[Index] == ']')
            {
                --Depth;
                if (Depth == 0)
                {
                    return Index;
                }
            }
        }
        return std::string::npos;
    }
}

bool FSceneJsonLoader::LoadScene(const std::wstring& FilePath, std::vector<FSceneModelDesc>& OutModels)
{
    OutModels.clear();

    const std::string Contents = ReadFileToString(FilePath);
    if (Contents.empty())
    {
        LogError("Failed to read scene JSON file: " + std::string(FilePath.begin(), FilePath.end()));
        return false;
    }

    const size_t ModelsKey = Contents.find("\"models\"");
    if (ModelsKey == std::string::npos)
    {
        LogError("Scene JSON is missing 'models' array: " + std::string(FilePath.begin(), FilePath.end()));
        return false;
    }

    const size_t ArrayStart = Contents.find('[', ModelsKey);
    if (ArrayStart == std::string::npos)
    {
        LogError("Scene JSON does not contain a models array block: " + std::string(FilePath.begin(), FilePath.end()));
        return false;
    }

    const size_t ArrayEnd = FindMatchingBracket(Contents, ArrayStart);
    if (ArrayEnd == std::string::npos || ArrayEnd <= ArrayStart)
    {
        LogError("Scene JSON models array is malformed: " + std::string(FilePath.begin(), FilePath.end()));
        return false;
    }

    const std::string ModelsBlock = Contents.substr(ArrayStart + 1, ArrayEnd - ArrayStart - 1);
    const std::regex ModelRegex(R"(\{[^\{\}]*\})");
    auto Begin = std::sregex_iterator(ModelsBlock.begin(), ModelsBlock.end(), ModelRegex);
    auto End = std::sregex_iterator();

    for (auto It = Begin; It != End; ++It)
    {
        const std::smatch& Match = *It;
        if (Match.size() < 1)
        {
            continue;
        }

        const std::string ModelText = Match[0].str();

        FSceneModelDesc ModelDesc;
        ModelDesc.MeshPath = Utf8ToWide(ExtractString(ModelText, "path"));
        ModelDesc.BaseColorTexturePath = Utf8ToWide(ExtractString(ModelText, "baseColor"));
        ModelDesc.bVisible = ExtractBool(ModelText, "visible", true);

        ModelDesc.Position = ParseVectorAttribute(ModelText, "translate", ModelDesc.Position);
        ModelDesc.RotationEuler = ParseVectorAttribute(ModelText, "rotate_euler", ModelDesc.RotationEuler);
        ModelDesc.Scale = ParseVectorAttribute(ModelText, "scale", ModelDesc.Scale);

        if (ModelDesc.MeshPath.empty())
        {
            LogError("Model entry is missing required 'path' field. Skipping entry.");
            continue;
        }

        if (ModelDesc.bVisible)
        {
            OutModels.push_back(ModelDesc);
        }
    }

    if (OutModels.empty())
    {
        LogError("No valid model entries found in scene: " + std::string(FilePath.begin(), FilePath.end()));
        return false;
    }

    return true;
}
