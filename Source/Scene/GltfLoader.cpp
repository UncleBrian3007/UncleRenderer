#include "GltfLoader.h"
#include "Mesh.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
    enum class EJsonType
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    struct FJsonValue
    {
        EJsonType Type = EJsonType::Null;
        bool BoolValue = false;
        double NumberValue = 0.0;
        std::string StringValue;
        std::vector<FJsonValue> ArrayValue;
        std::map<std::string, FJsonValue> ObjectValue;

        bool IsNull() const { return Type == EJsonType::Null; }
        bool IsArray() const { return Type == EJsonType::Array; }
        bool IsObject() const { return Type == EJsonType::Object; }
        bool IsString() const { return Type == EJsonType::String; }
        bool IsNumber() const { return Type == EJsonType::Number; }

        const FJsonValue* Find(const std::string& Key) const
        {
            auto It = ObjectValue.find(Key);
            return It != ObjectValue.end() ? &It->second : nullptr;
        }
    };

    class FSimpleJsonParser
    {
    public:
        explicit FSimpleJsonParser(const std::string& InText)
            : Text(InText)
        {
        }

        FJsonValue Parse()
        {
            Position = 0;
            return ParseValue();
        }

    private:
        void SkipWhitespace()
        {
            while (Position < Text.size() && std::isspace(static_cast<unsigned char>(Text[Position])))
            {
                ++Position;
            }
        }

        bool Match(const char Expected)
        {
            SkipWhitespace();
            if (Position < Text.size() && Text[Position] == Expected)
            {
                ++Position;
                return true;
            }
            return false;
        }

        FJsonValue ParseValue()
        {
            SkipWhitespace();
            if (Position >= Text.size())
            {
                return {};
            }

            char Ch = Text[Position];
            if (Ch == '"')
            {
                return ParseString();
            }
            if (Ch == '{')
            {
                return ParseObject();
            }
            if (Ch == '[')
            {
                return ParseArray();
            }
            if (std::isdigit(static_cast<unsigned char>(Ch)) || Ch == '-' || Ch == '+')
            {
                return ParseNumber();
            }
            if (Text.compare(Position, 4, "true") == 0)
            {
                Position += 4;
                FJsonValue V; V.Type = EJsonType::Bool; V.BoolValue = true; return V;
            }
            if (Text.compare(Position, 5, "false") == 0)
            {
                Position += 5;
                FJsonValue V; V.Type = EJsonType::Bool; V.BoolValue = false; return V;
            }
            if (Text.compare(Position, 4, "null") == 0)
            {
                Position += 4;
                return {};
            }

            return {};
        }

        FJsonValue ParseString()
        {
            FJsonValue V;
            V.Type = EJsonType::String;

            if (!Match('"'))
            {
                return {};
            }

            std::string Result;
            while (Position < Text.size())
            {
                char Ch = Text[Position++];
                if (Ch == '"')
                {
                    break;
                }
                // Minimal string parsing; assumes no escape sequences in provided glTF.
                Result.push_back(Ch);
            }

            V.StringValue = Result;
            return V;
        }

        FJsonValue ParseNumber()
        {
            const size_t Start = Position;
            if (Text[Position] == '-' || Text[Position] == '+')
            {
                ++Position;
            }
            while (Position < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Position])))
            {
                ++Position;
            }
            if (Position < Text.size() && Text[Position] == '.')
            {
                ++Position;
                while (Position < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Position])))
                {
                    ++Position;
                }
            }

            const std::string NumberText = Text.substr(Start, Position - Start);
            FJsonValue V;
            V.Type = EJsonType::Number;
            V.NumberValue = std::strtod(NumberText.c_str(), nullptr);
            return V;
        }

        FJsonValue ParseArray()
        {
            FJsonValue V;
            V.Type = EJsonType::Array;

            if (!Match('['))
            {
                return {};
            }

            SkipWhitespace();
            if (Match(']'))
            {
                return V;
            }

            while (Position < Text.size())
            {
                V.ArrayValue.push_back(ParseValue());
                SkipWhitespace();
                if (Match(']'))
                {
                    break;
                }
                Match(',');
            }

            return V;
        }

        FJsonValue ParseObject()
        {
            FJsonValue V;
            V.Type = EJsonType::Object;

            if (!Match('{'))
            {
                return {};
            }

            SkipWhitespace();
            if (Match('}'))
            {
                return V;
            }

            while (Position < Text.size())
            {
                FJsonValue Key = ParseString();
                Match(':');
                FJsonValue Value = ParseValue();
                V.ObjectValue.emplace(Key.StringValue, std::move(Value));
                SkipWhitespace();
                if (Match('}'))
                {
                    break;
                }
                Match(',');
            }

            return V;
        }

        const std::string& Text;
        size_t Position = 0;
    };

    std::vector<uint8_t> DecodeBase64(const std::string& Input)
    {
        static const int8_t DecodingTable[256] =
        {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
        };

        std::vector<uint8_t> Output;
        Output.reserve(Input.size());

        uint32_t Buffer = 0;
        int32_t BitsCollected = 0;
        for (unsigned char Ch : Input)
        {
            if (Ch == '=')
            {
                break;
            }
            const int8_t Decoded = DecodingTable[Ch];
            if (Decoded < 0)
            {
                continue;
            }

            Buffer = (Buffer << 6) | static_cast<uint32_t>(Decoded);
            BitsCollected += 6;
            if (BitsCollected >= 8)
            {
                BitsCollected -= 8;
                Output.push_back(static_cast<uint8_t>((Buffer >> BitsCollected) & 0xFF));
            }
        }

        return Output;
    }

    const FJsonValue* GetObjectField(const FJsonValue* Object, const std::string& Key)
    {
        if (!Object || !Object->IsObject())
        {
            return nullptr;
        }
        return Object->Find(Key);
    }

    int64_t GetIntField(const FJsonValue* Object, const std::string& Key, int64_t Default = 0)
    {
        const FJsonValue* Field = GetObjectField(Object, Key);
        if (Field && Field->IsNumber())
        {
            return static_cast<int64_t>(Field->NumberValue);
        }
        return Default;
    }

    std::string GetStringField(const FJsonValue* Object, const std::string& Key)
    {
        const FJsonValue* Field = GetObjectField(Object, Key);
        if (Field && Field->IsString())
        {
            return Field->StringValue;
        }
        return {};
    }

    const FJsonValue* GetArrayElem(const FJsonValue* Array, size_t Index)
    {
        if (!Array || !Array->IsArray() || Index >= Array->ArrayValue.size())
        {
            return nullptr;
        }
        return &Array->ArrayValue[Index];
    }

}

bool FGltfLoader::LoadMeshFromFile(const std::wstring& FilePath, FMesh& OutMesh, std::wstring* OutBaseColorTexturePath)
{
    std::ifstream File(std::filesystem::path(FilePath), std::ios::binary);
    if (!File.is_open())
    {
        return false;
    }

    std::string JsonText((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());
    File.close();

    FSimpleJsonParser Parser(JsonText);
    const FJsonValue Root = Parser.Parse();

    const FJsonValue* Buffers = GetObjectField(&Root, "buffers");
    if (!Buffers || !Buffers->IsArray() || Buffers->ArrayValue.empty())
    {
        return false;
    }

    const std::string Uri = GetStringField(&Buffers->ArrayValue[0], "uri");
    const std::string Prefix = "data:application/octet-stream;base64,";

    std::vector<uint8_t> BufferData;
    if (Uri.compare(0, Prefix.size(), Prefix) == 0)
    {
        BufferData = DecodeBase64(Uri.substr(Prefix.size()));
    }
    else
    {
        const std::filesystem::path BasePath = std::filesystem::path(FilePath).parent_path();
        const std::filesystem::path BufferPath = BasePath / std::filesystem::path(Uri);

        std::ifstream BinFile(BufferPath, std::ios::binary | std::ios::ate);
        if (!BinFile.is_open())
        {
            return false;
        }

        const std::streamsize FileSize = BinFile.tellg();
        if (FileSize <= 0)
        {
            return false;
        }

        BufferData.resize(static_cast<size_t>(FileSize));
        BinFile.seekg(0, std::ios::beg);
        BinFile.read(reinterpret_cast<char*>(BufferData.data()), FileSize);
    }

    if (BufferData.empty())
    {
        return false;
    }

    const FJsonValue* BufferViews = GetObjectField(&Root, "bufferViews");
    const FJsonValue* Accessors = GetObjectField(&Root, "accessors");
    const FJsonValue* Meshes = GetObjectField(&Root, "meshes");
    if (!BufferViews || !BufferViews->IsArray() || !Accessors || !Accessors->IsArray() || !Meshes || !Meshes->IsArray())
    {
        return false;
    }

    const FJsonValue* Mesh = GetArrayElem(Meshes, 0);
    const FJsonValue* Primitives = GetObjectField(Mesh, "primitives");
    const FJsonValue* Primitive = GetArrayElem(Primitives, 0);
    const FJsonValue* Attributes = GetObjectField(Primitive, "attributes");

    if (!Primitive || !Attributes || !Primitive->IsObject())
    {
        return false;
    }

    const int64_t PositionAccessorIndex = GetIntField(Attributes, "POSITION", -1);
    const int64_t NormalAccessorIndex = GetIntField(Attributes, "NORMAL", -1);
    const int64_t TexcoordAccessorIndex = GetIntField(Attributes, "TEXCOORD_0", -1);
    const int64_t IndicesAccessorIndex = GetIntField(Primitive, "indices", -1);

    if (PositionAccessorIndex < 0 || IndicesAccessorIndex < 0)
    {
        return false;
    }

    const FJsonValue* PositionAccessor = GetArrayElem(Accessors, static_cast<size_t>(PositionAccessorIndex));
    const FJsonValue* NormalAccessor = NormalAccessorIndex >= 0 ? GetArrayElem(Accessors, static_cast<size_t>(NormalAccessorIndex)) : nullptr;
    const FJsonValue* TexcoordAccessor = TexcoordAccessorIndex >= 0 ? GetArrayElem(Accessors, static_cast<size_t>(TexcoordAccessorIndex)) : nullptr;
    const FJsonValue* IndexAccessor = GetArrayElem(Accessors, static_cast<size_t>(IndicesAccessorIndex));

    if (!PositionAccessor || !IndexAccessor)
    {
        return false;
    }

    const int64_t PositionBufferViewIndex = GetIntField(PositionAccessor, "bufferView", -1);
    const int64_t NormalBufferViewIndex = NormalAccessor ? GetIntField(NormalAccessor, "bufferView", -1) : -1;
    const int64_t TexcoordBufferViewIndex = TexcoordAccessor ? GetIntField(TexcoordAccessor, "bufferView", -1) : -1;
    const int64_t IndexBufferViewIndex = GetIntField(IndexAccessor, "bufferView", -1);

    const FJsonValue* PositionBufferView = GetArrayElem(BufferViews, static_cast<size_t>(PositionBufferViewIndex));
    const FJsonValue* NormalBufferView = NormalBufferViewIndex >= 0 ? GetArrayElem(BufferViews, static_cast<size_t>(NormalBufferViewIndex)) : nullptr;
    const FJsonValue* TexcoordBufferView = TexcoordBufferViewIndex >= 0 ? GetArrayElem(BufferViews, static_cast<size_t>(TexcoordBufferViewIndex)) : nullptr;
    const FJsonValue* IndexBufferView = GetArrayElem(BufferViews, static_cast<size_t>(IndexBufferViewIndex));

    if (!PositionBufferView || !IndexBufferView)
    {
        return false;
    }

    const int64_t PositionCount = GetIntField(PositionAccessor, "count", 0);
    if (PositionCount <= 0)
    {
        return false;
    }

    const int64_t IndexCount = GetIntField(IndexAccessor, "count", 0);
    if (IndexCount <= 0)
    {
        return false;
    }

    const int64_t PositionByteOffset = GetIntField(PositionAccessor, "byteOffset", 0) + GetIntField(PositionBufferView, "byteOffset", 0);
    const int64_t NormalByteOffset = (NormalAccessor ? GetIntField(NormalAccessor, "byteOffset", 0) + GetIntField(NormalBufferView, "byteOffset", 0) : 0);
    const int64_t TexcoordByteOffset = (TexcoordAccessor ? GetIntField(TexcoordAccessor, "byteOffset", 0) + GetIntField(TexcoordBufferView, "byteOffset", 0) : 0);
    const int64_t IndexByteOffset = GetIntField(IndexAccessor, "byteOffset", 0) + GetIntField(IndexBufferView, "byteOffset", 0);

    const int64_t PositionStride = GetIntField(PositionBufferView, "byteStride", static_cast<int64_t>(sizeof(float) * 3));
    const int64_t NormalStride = NormalBufferView ? GetIntField(NormalBufferView, "byteStride", static_cast<int64_t>(sizeof(float) * 3)) : static_cast<int64_t>(sizeof(float) * 3);
    const int64_t TexcoordStride = TexcoordBufferView ? GetIntField(TexcoordBufferView, "byteStride", static_cast<int64_t>(sizeof(float) * 2)) : static_cast<int64_t>(sizeof(float) * 2);

    if (PositionStride <= 0 || NormalStride <= 0 || TexcoordStride <= 0)
    {
        return false;
    }

    std::vector<FMesh::FVertex> Vertices;
    Vertices.reserve(static_cast<size_t>(PositionCount));

    for (int64_t i = 0; i < PositionCount; ++i)
    {
        const size_t PositionOffset = static_cast<size_t>(PositionByteOffset + i * PositionStride);
        if (PositionOffset + sizeof(float) * 3 > BufferData.size())
        {
            return false;
        }

        FMesh::FVertex Vertex{};
        float Position[3] = {};
        std::memcpy(Position, &BufferData[PositionOffset], sizeof(Position));
        Vertex.Position = { Position[0], Position[1], Position[2] };

        if (NormalAccessor && NormalBufferView)
        {
            const size_t Offset = static_cast<size_t>(NormalByteOffset + i * NormalStride);
            if (Offset + sizeof(float) * 3 > BufferData.size())
            {
                return false;
            }
            float Normal[3] = {};
            std::memcpy(Normal, &BufferData[Offset], sizeof(Normal));
            Vertex.Normal = { Normal[0], Normal[1], Normal[2] };
        }
        else
        {
            Vertex.Normal = { 0.0f, 0.0f, 1.0f };
        }

        if (TexcoordAccessor && TexcoordBufferView)
        {
            const size_t Offset = static_cast<size_t>(TexcoordByteOffset + i * TexcoordStride);
            if (Offset + sizeof(float) * 2 > BufferData.size())
            {
                return false;
            }
            float UV[2] = {};
            std::memcpy(UV, &BufferData[Offset], sizeof(UV));
            Vertex.UV = { UV[0], UV[1] };
        }
        else
        {
            Vertex.UV = { 0.0f, 0.0f };
        }

        Vertices.push_back(Vertex);
    }

    std::vector<uint32_t> Indices;
    Indices.reserve(static_cast<size_t>(IndexCount));

    const int64_t ComponentType = GetIntField(IndexAccessor, "componentType", 5125);
    const size_t ComponentSize = (ComponentType == 5121) ? 1 : (ComponentType == 5123 ? 2 : 4);

    for (int64_t i = 0; i < IndexCount; ++i)
    {
        const size_t Offset = static_cast<size_t>(IndexByteOffset + i * ComponentSize);
        if (Offset + ComponentSize > BufferData.size())
        {
            return false;
        }

        uint32_t Index = 0;
        switch (ComponentType)
        {
        case 5121: // UNSIGNED_BYTE
            Index = BufferData[Offset];
            break;
        case 5123: // UNSIGNED_SHORT
        {
            uint16_t Value = 0;
            std::memcpy(&Value, &BufferData[Offset], sizeof(uint16_t));
            Index = Value;
            break;
        }
        default: // 5125 UNSIGNED_INT
        {
            uint32_t Value = 0;
            std::memcpy(&Value, &BufferData[Offset], sizeof(uint32_t));
            Index = Value;
            break;
        }
        }

        Indices.push_back(Index);
    }

    OutMesh.SetVertices(Vertices);
    OutMesh.SetIndices(Indices);

    if (OutBaseColorTexturePath)
    {
        *OutBaseColorTexturePath = L"";

        const FJsonValue* Materials = GetObjectField(&Root, "materials");
        const FJsonValue* Textures = GetObjectField(&Root, "textures");
        const FJsonValue* Images = GetObjectField(&Root, "images");

        if (Materials && Materials->IsArray() && !Materials->ArrayValue.empty()
            && Textures && Textures->IsArray() && !Textures->ArrayValue.empty()
            && Images && Images->IsArray() && !Images->ArrayValue.empty())
        {
            const FJsonValue* Material = &Materials->ArrayValue[0];
            const FJsonValue* Pbr = GetObjectField(Material, "pbrMetallicRoughness");
            const FJsonValue* BaseColorTexture = GetObjectField(Pbr, "baseColorTexture");
            const int64_t TextureIndex = GetIntField(BaseColorTexture, "index", -1);

            if (TextureIndex >= 0)
            {
                const FJsonValue* Texture = GetArrayElem(Textures, static_cast<size_t>(TextureIndex));
                const int64_t ImageIndex = GetIntField(Texture, "source", -1);
                if (ImageIndex >= 0)
                {
                    const FJsonValue* Image = GetArrayElem(Images, static_cast<size_t>(ImageIndex));
                    const std::string ImageUri = GetStringField(Image, "uri");
                    if (!ImageUri.empty())
                    {
                        const std::filesystem::path BasePath = std::filesystem::path(FilePath).parent_path();
                        const std::filesystem::path FullPath = BasePath / std::filesystem::path(ImageUri);
                        *OutBaseColorTexturePath = FullPath.wstring();
                    }
                }
            }
        }
    }

    return true;
}
