#include "GltfLoader.h"
#include "Mesh.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
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

            if (Position < Text.size() && (Text[Position] == 'e' || Text[Position] == 'E'))
            {
                ++Position;
                if (Position < Text.size() && (Text[Position] == '+' || Text[Position] == '-'))
                {
                    ++Position;
                }
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

    double GetNumberField(const FJsonValue* Object, const std::string& Key, double Default = 0.0)
    {
        const FJsonValue* Field = GetObjectField(Object, Key);
        if (Field && Field->IsNumber())
        {
            return Field->NumberValue;
        }
        return Default;
    }

	const FJsonValue* GetArrayElem(const FJsonValue* Array, size_t Index)
	{
		if (!Array || !Array->IsArray() || Index >= Array->ArrayValue.size())
		{
			return nullptr;
		}
		return &Array->ArrayValue[Index];
	}

    double GetNumberField(const FJsonValue* Array, size_t Index, double Default = 0.0)
    {
        const FJsonValue* Field = GetArrayElem(Array, Index);
        if (Field && Field->IsNumber())
        {
            return Field->NumberValue;
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

    using FMatrix4 = std::array<float, 16>;

    FMatrix4 MakeMirrorZMatrix()
    {
        return { 1.0f, 0.0f,  0.0f, 0.0f,
                 0.0f, 1.0f,  0.0f, 0.0f,
                 0.0f, 0.0f, -1.0f, 0.0f,
                 0.0f, 0.0f,  0.0f, 1.0f };
    }

    FMatrix4 MakeIdentityMatrix()
    {
        return { 1.0f, 0.0f, 0.0f, 0.0f,
                 0.0f, 1.0f, 0.0f, 0.0f,
                 0.0f, 0.0f, 1.0f, 0.0f,
                 0.0f, 0.0f, 0.0f, 1.0f };
    }

    FMatrix4 MultiplyMatrix(const FMatrix4& A, const FMatrix4& B)
    {
        // Column-major multiplication (A * B)
        FMatrix4 Result{};
        for (int Col = 0; Col < 4; ++Col)
        {
            for (int Row = 0; Row < 4; ++Row)
            {
                float Sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    Sum += A[k * 4 + Row] * B[Col * 4 + k];
                }
                Result[Col * 4 + Row] = Sum;
            }
        }
        return Result;
    }

    FMatrix4 MatrixFromQuaternion(float x, float y, float z, float w)
    {
        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float wx = w * x;
        const float wy = w * y;
        const float wz = w * z;

        return {
            1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz),       2.0f * (xz - wy),       0.0f,
            2.0f * (xy - wz),       1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx),       0.0f,
            2.0f * (xz + wy),       2.0f * (yz - wx),       1.0f - 2.0f * (xx + yy), 0.0f,
            0.0f,                   0.0f,                   0.0f,                   1.0f
        };
    }

    FMatrix4 MatrixFromTRS(const FJsonValue* Node)
    {
        if (!Node || !Node->IsObject())
        {
            return MakeIdentityMatrix();
        }

        const FJsonValue* MatrixValue = Node->Find("matrix");
        if (MatrixValue && MatrixValue->IsArray() && MatrixValue->ArrayValue.size() == 16)
        {
            FMatrix4 M{};
            for (size_t i = 0; i < 16; ++i)
            {
                M[i] = static_cast<float>(MatrixValue->ArrayValue[i].NumberValue);
            }
            return M;
        }

        const FJsonValue* Translation = Node->Find("translation");
        const FJsonValue* Rotation = Node->Find("rotation");
        const FJsonValue* Scale = Node->Find("scale");

        const float tx = (Translation && Translation->IsArray() && Translation->ArrayValue.size() == 3)
            ? static_cast<float>(Translation->ArrayValue[0].NumberValue)
            : 0.0f;
        const float ty = (Translation && Translation->IsArray() && Translation->ArrayValue.size() == 3)
            ? static_cast<float>(Translation->ArrayValue[1].NumberValue)
            : 0.0f;
        const float tz = (Translation && Translation->IsArray() && Translation->ArrayValue.size() == 3)
            ? static_cast<float>(Translation->ArrayValue[2].NumberValue)
            : 0.0f;

        const float sx = (Scale && Scale->IsArray() && Scale->ArrayValue.size() == 3)
            ? static_cast<float>(Scale->ArrayValue[0].NumberValue)
            : 1.0f;
        const float sy = (Scale && Scale->IsArray() && Scale->ArrayValue.size() == 3)
            ? static_cast<float>(Scale->ArrayValue[1].NumberValue)
            : 1.0f;
        const float sz = (Scale && Scale->IsArray() && Scale->ArrayValue.size() == 3)
            ? static_cast<float>(Scale->ArrayValue[2].NumberValue)
            : 1.0f;

        const float rx = (Rotation && Rotation->IsArray() && Rotation->ArrayValue.size() == 4)
            ? static_cast<float>(Rotation->ArrayValue[0].NumberValue)
            : 0.0f;
        const float ry = (Rotation && Rotation->IsArray() && Rotation->ArrayValue.size() == 4)
            ? static_cast<float>(Rotation->ArrayValue[1].NumberValue)
            : 0.0f;
        const float rz = (Rotation && Rotation->IsArray() && Rotation->ArrayValue.size() == 4)
            ? static_cast<float>(Rotation->ArrayValue[2].NumberValue)
            : 0.0f;
        const float rw = (Rotation && Rotation->IsArray() && Rotation->ArrayValue.size() == 4)
            ? static_cast<float>(Rotation->ArrayValue[3].NumberValue)
            : 1.0f;

        const FMatrix4 T = { 1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f,
                             tx,   ty,   tz,   1.0f };

        const FMatrix4 S = { sx,   0.0f, 0.0f, 0.0f,
                             0.0f, sy,   0.0f, 0.0f,
                             0.0f, 0.0f, sz,   0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f };

        const FMatrix4 R = MatrixFromQuaternion(rx, ry, rz, rw);

        // glTF uses column-major matrices with column vectors; compose as T * R * S
        return MultiplyMatrix(MultiplyMatrix(T, R), S);
    }

    FMatrix4 ToLeftHandedMatrix(const FMatrix4& M)
    {
        const FMatrix4 MirrorZ = MakeMirrorZMatrix();
        return MultiplyMatrix(MirrorZ, MultiplyMatrix(M, MirrorZ));
    }

    FFloat3 TransformPosition(const FMatrix4& M, const FFloat3& P)
    {
        FFloat3 Out;
        Out.x = M[0] * P.x + M[4] * P.y + M[8]  * P.z + M[12];
        Out.y = M[1] * P.x + M[5] * P.y + M[9]  * P.z + M[13];
        Out.z = M[2] * P.x + M[6] * P.y + M[10] * P.z + M[14];
        return Out;
    }

    FFloat3 TransformDirection(const FMatrix4& M, const FFloat3& D)
    {
        FFloat3 Out;
        Out.x = M[0] * D.x + M[4] * D.y + M[8]  * D.z;
        Out.y = M[1] * D.x + M[5] * D.y + M[9]  * D.z;
        Out.z = M[2] * D.x + M[6] * D.y + M[10] * D.z;

        const float Length = std::sqrt(Out.x * Out.x + Out.y * Out.y + Out.z * Out.z);
        if (Length > 0.0f)
        {
            Out.x /= Length;
            Out.y /= Length;
            Out.z /= Length;
        }
        return Out;
    }

    struct FMeshData
    {
        std::vector<FMesh::FVertex> Vertices;
        std::vector<uint32_t> Indices;
    };

    DirectX::XMFLOAT4X4 ToFloat4x4(const FMatrix4& M)
    {
        DirectX::XMFLOAT4X4 Result{};
        for (int Row = 0; Row < 4; ++Row)
        {
            for (int Col = 0; Col < 4; ++Col)
            {
                Result.m[Row][Col] = M[Row * 4 + Col];
            }
        }

        return Result;
    }

    void ProcessNodeRecursive(
        const FJsonValue* Nodes,
        int64_t NodeIndex,
        const FMatrix4& ParentTransform,
        const std::vector<FMeshData>& MeshDatas,
        std::vector<FGltfNode>& OutNodes)
    {
        const FJsonValue* Node = GetArrayElem(Nodes, static_cast<size_t>(NodeIndex));
        if (!Node || !Node->IsObject())
        {
            return;
        }

        const FMatrix4 Local = MatrixFromTRS(Node);
        const FMatrix4 LocalLH = ToLeftHandedMatrix(Local);
        const FMatrix4 World = MultiplyMatrix(ParentTransform, LocalLH);

        const int64_t MeshIndex = GetIntField(Node, "mesh", -1);
        if (MeshIndex >= 0 && MeshIndex < static_cast<int64_t>(MeshDatas.size()))
        {
            FGltfNode LoadedNode;
            LoadedNode.MeshIndex = static_cast<int>(MeshIndex);
            LoadedNode.WorldMatrix = ToFloat4x4(World);
            LoadedNode.Name = GetStringField(Node, "name");
            OutNodes.push_back(LoadedNode);
        }

        const FJsonValue* Children = GetObjectField(Node, "children");
        if (Children && Children->IsArray())
        {
            for (size_t i = 0; i < Children->ArrayValue.size(); ++i)
            {
                const int64_t ChildIndex = static_cast<int64_t>(Children->ArrayValue[i].NumberValue);
                ProcessNodeRecursive(Nodes, ChildIndex, World, MeshDatas, OutNodes);
            }
        }
    }
}

namespace
{
    std::wstring ResolveTexturePath(const FJsonValue* Textures, const FJsonValue* Images, const std::filesystem::path& BasePath, int64_t TextureIndex)
    {
        if (TextureIndex < 0)
        {
            return L"";
        }

        const FJsonValue* Texture = GetArrayElem(Textures, static_cast<size_t>(TextureIndex));
        const int64_t ImageIndex = GetIntField(Texture, "source", -1);
        if (ImageIndex < 0)
        {
            return L"";
        }

        const FJsonValue* Image = GetArrayElem(Images, static_cast<size_t>(ImageIndex));
        const std::string ImageUri = GetStringField(Image, "uri");
        if (ImageUri.empty())
        {
            return L"";
        }

        const std::filesystem::path FullPath = BasePath / std::filesystem::path(ImageUri);
        return FullPath.wstring();
    }

    FGltfTextureTransform ResolveTextureTransform(const FJsonValue* TextureInfo)
    {
        FGltfTextureTransform Transform;

        if (!TextureInfo || !TextureInfo->IsObject())
        {
            return Transform;
        }

        const FJsonValue* Extensions = GetObjectField(TextureInfo, "extensions");
        const FJsonValue* TransformExt = Extensions ? GetObjectField(Extensions, "KHR_texture_transform") : nullptr;
        const FJsonValue* Source = TransformExt ? TransformExt : TextureInfo;

        const FJsonValue* Offset = GetObjectField(Source, "offset");
        if (Offset && Offset->IsArray())
        {
            Transform.Offset.x = static_cast<float>(GetNumberField(Offset, 0, Transform.Offset.x));
            Transform.Offset.y = static_cast<float>(GetNumberField(Offset, 1, Transform.Offset.y));
        }

        const FJsonValue* Scale = GetObjectField(Source, "scale");
        if (Scale && Scale->IsArray())
        {
            Transform.Scale.x = static_cast<float>(GetNumberField(Scale, 0, Transform.Scale.x));
            Transform.Scale.y = static_cast<float>(GetNumberField(Scale, 1, Transform.Scale.y));
        }

        Transform.Rotation = static_cast<float>(GetNumberField(Source, "rotation", Transform.Rotation));

        return Transform;
    }
}

bool FGltfLoader::LoadSceneFromFile(const std::wstring& FilePath, FGltfScene& OutScene)
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

    const auto AppendPrimitiveToMesh = [&](const FJsonValue* Primitive, FMeshData& MeshData) -> bool
    {
        if (!Primitive || !Primitive->IsObject())
        {
            return false;
        }

        const FJsonValue* Attributes = GetObjectField(Primitive, "attributes");
        if (!Attributes || !Attributes->IsObject())
        {
            return false;
        }

        const int64_t PositionAccessorIndex = GetIntField(Attributes, "POSITION", -1);
        const int64_t NormalAccessorIndex = GetIntField(Attributes, "NORMAL", -1);
        const int64_t TexcoordAccessorIndex = GetIntField(Attributes, "TEXCOORD_0", -1);
        const int64_t TangentAccessorIndex = GetIntField(Attributes, "TANGENT", -1);
        const int64_t ColorAccessorIndex = GetIntField(Attributes, "COLOR_0", -1);
        const int64_t PrimitiveMode = GetIntField(Primitive, "mode", 4);
        const int64_t IndicesAccessorIndex = GetIntField(Primitive, "indices", -1);

        if (PositionAccessorIndex < 0 || IndicesAccessorIndex < 0)
        {
            return false;
        }

        const FJsonValue* PositionAccessor = GetArrayElem(Accessors, static_cast<size_t>(PositionAccessorIndex));
        const FJsonValue* NormalAccessor = NormalAccessorIndex >= 0 ? GetArrayElem(Accessors, static_cast<size_t>(NormalAccessorIndex)) : nullptr;
        const FJsonValue* TexcoordAccessor = TexcoordAccessorIndex >= 0 ? GetArrayElem(Accessors, static_cast<size_t>(TexcoordAccessorIndex)) : nullptr;
        const FJsonValue* TangentAccessor = TangentAccessorIndex >= 0 ? GetArrayElem(Accessors, static_cast<size_t>(TangentAccessorIndex)) : nullptr;
        const FJsonValue* ColorAccessor = ColorAccessorIndex >= 0 ? GetArrayElem(Accessors, static_cast<size_t>(ColorAccessorIndex)) : nullptr;
        const FJsonValue* IndexAccessor = GetArrayElem(Accessors, static_cast<size_t>(IndicesAccessorIndex));

        if (!PositionAccessor || !IndexAccessor)
        {
            return false;
        }

        const int64_t PositionBufferViewIndex = GetIntField(PositionAccessor, "bufferView", -1);
        const int64_t NormalBufferViewIndex = NormalAccessor ? GetIntField(NormalAccessor, "bufferView", -1) : -1;
        const int64_t TexcoordBufferViewIndex = TexcoordAccessor ? GetIntField(TexcoordAccessor, "bufferView", -1) : -1;
        const int64_t TangentBufferViewIndex = TangentAccessor ? GetIntField(TangentAccessor, "bufferView", -1) : -1;
        const int64_t ColorBufferViewIndex = ColorAccessor ? GetIntField(ColorAccessor, "bufferView", -1) : -1;
        const int64_t IndexBufferViewIndex = GetIntField(IndexAccessor, "bufferView", -1);

        const FJsonValue* PositionBufferView = GetArrayElem(BufferViews, static_cast<size_t>(PositionBufferViewIndex));
        const FJsonValue* NormalBufferView = NormalBufferViewIndex >= 0 ? GetArrayElem(BufferViews, static_cast<size_t>(NormalBufferViewIndex)) : nullptr;
        const FJsonValue* TexcoordBufferView = TexcoordBufferViewIndex >= 0 ? GetArrayElem(BufferViews, static_cast<size_t>(TexcoordBufferViewIndex)) : nullptr;
        const FJsonValue* TangentBufferView = TangentBufferViewIndex >= 0 ? GetArrayElem(BufferViews, static_cast<size_t>(TangentBufferViewIndex)) : nullptr;
        const FJsonValue* ColorBufferView = ColorBufferViewIndex >= 0 ? GetArrayElem(BufferViews, static_cast<size_t>(ColorBufferViewIndex)) : nullptr;
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
        const int64_t TangentByteOffset = (TangentAccessor ? GetIntField(TangentAccessor, "byteOffset", 0) + GetIntField(TangentBufferView, "byteOffset", 0) : 0);
        const int64_t ColorByteOffset = (ColorAccessor ? GetIntField(ColorAccessor, "byteOffset", 0) + GetIntField(ColorBufferView, "byteOffset", 0) : 0);
        const int64_t IndexByteOffset = GetIntField(IndexAccessor, "byteOffset", 0) + GetIntField(IndexBufferView, "byteOffset", 0);

        const int64_t PositionStride = GetIntField(PositionBufferView, "byteStride", static_cast<int64_t>(sizeof(float) * 3));
        const int64_t NormalStride = NormalBufferView ? GetIntField(NormalBufferView, "byteStride", static_cast<int64_t>(sizeof(float) * 3)) : static_cast<int64_t>(sizeof(float) * 3);
        const int64_t TexcoordStride = TexcoordBufferView ? GetIntField(TexcoordBufferView, "byteStride", static_cast<int64_t>(sizeof(float) * 2)) : static_cast<int64_t>(sizeof(float) * 2);
        const int64_t TangentStride = TangentBufferView ? GetIntField(TangentBufferView, "byteStride", static_cast<int64_t>(sizeof(float) * 4)) : static_cast<int64_t>(sizeof(float) * 4);
        const std::string ColorType = ColorAccessor ? GetStringField(ColorAccessor, "type") : std::string();
        const int64_t DefaultColorStride = ColorType == "VEC4" ? static_cast<int64_t>(sizeof(float) * 4) : static_cast<int64_t>(sizeof(float) * 3);
        const int64_t ColorStride = ColorBufferView ? GetIntField(ColorBufferView, "byteStride", DefaultColorStride) : DefaultColorStride;

        if (PositionStride <= 0 || NormalStride <= 0 || TexcoordStride <= 0 || TangentStride <= 0 || ColorStride <= 0)
        {
            return false;
        }

        const uint32_t VertexOffset = static_cast<uint32_t>(MeshData.Vertices.size());
        MeshData.Vertices.reserve(MeshData.Vertices.size() + static_cast<size_t>(PositionCount));

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
            Vertex.Position.z = -Vertex.Position.z;

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
            Vertex.Normal.z = -Vertex.Normal.z;

            if (TangentAccessor && TangentBufferView)
            {
                const size_t Offset = static_cast<size_t>(TangentByteOffset + i * TangentStride);
                if (Offset + sizeof(float) * 4 > BufferData.size())
                {
                    return false;
                }
                float Tangent[4] = {};
                std::memcpy(Tangent, &BufferData[Offset], sizeof(Tangent));
                Vertex.Tangent = { Tangent[0], Tangent[1], Tangent[2], Tangent[3] };
            }
            else
            {
                Vertex.Tangent = { 0.0f, 0.0f, 0.0f, 1.0f };
            }
            Vertex.Tangent.z = -Vertex.Tangent.z;
            Vertex.Tangent.w = -Vertex.Tangent.w;

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

            if (ColorAccessor && ColorBufferView)
            {
                const size_t Offset = static_cast<size_t>(ColorByteOffset + i * ColorStride);
                const size_t ExpectedBytes = ColorType == "VEC4" ? sizeof(float) * 4 : sizeof(float) * 3;
                if (Offset + ExpectedBytes > BufferData.size())
                {
                    return false;
                }

                float Color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                std::memcpy(Color, &BufferData[Offset], ExpectedBytes);
                const float Alpha = ColorType == "VEC4" ? Color[3] : 1.0f;
                Vertex.Color = { Color[0], Color[1], Color[2], Alpha };
            }

            MeshData.Vertices.push_back(Vertex);
        }

        std::vector<uint32_t> RawIndices;
        RawIndices.reserve(static_cast<size_t>(IndexCount));

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

            RawIndices.push_back(Index + VertexOffset);
        }

        const size_t IndexStart = MeshData.Indices.size();

        switch (PrimitiveMode)
        {
        case 4: // TRIANGLES
        {
            if (RawIndices.size() % 3 != 0)
            {
                return false;
            }
            MeshData.Indices.insert(MeshData.Indices.end(), RawIndices.begin(), RawIndices.end());
            break;
        }
        case 5: // TRIANGLE_STRIP
        {
            if (RawIndices.size() < 3)
            {
                return false;
            }
            for (size_t i = 2; i < RawIndices.size(); ++i)
            {
                const bool bEven = (i % 2) == 0;
                const uint32_t i0 = RawIndices[i - 2];
                const uint32_t i1 = RawIndices[i - 1];
                const uint32_t i2 = RawIndices[i];
                if (bEven)
                {
                    MeshData.Indices.push_back(i0);
                    MeshData.Indices.push_back(i1);
                    MeshData.Indices.push_back(i2);
                }
                else
                {
                    MeshData.Indices.push_back(i1);
                    MeshData.Indices.push_back(i0);
                    MeshData.Indices.push_back(i2);
                }
            }
            break;
        }
        case 6: // TRIANGLE_FAN
        {
            if (RawIndices.size() < 3)
            {
                return false;
            }
            for (size_t i = 2; i < RawIndices.size(); ++i)
            {
                MeshData.Indices.push_back(RawIndices[0]);
                MeshData.Indices.push_back(RawIndices[i - 1]);
                MeshData.Indices.push_back(RawIndices[i]);
            }
            break;
        }
        default:
            return false;
        }

        return true;
    };

    std::vector<FMeshData> MeshDatas;
    MeshDatas.resize(Meshes->ArrayValue.size());

    std::vector<int64_t> MeshMaterialIndices(Meshes->ArrayValue.size(), -1);

    for (size_t MeshIndex = 0; MeshIndex < Meshes->ArrayValue.size(); ++MeshIndex)
    {
        const FJsonValue* Mesh = GetArrayElem(Meshes, MeshIndex);
        const FJsonValue* Primitives = GetObjectField(Mesh, "primitives");
        if (!Primitives || !Primitives->IsArray())
        {
            return false;
        }

        for (const FJsonValue& PrimitiveValue : Primitives->ArrayValue)
        {
            const int64_t MaterialIndex = GetIntField(&PrimitiveValue, "material", -1);
            if (MeshMaterialIndices[MeshIndex] < 0 && MaterialIndex >= 0)
            {
                MeshMaterialIndices[MeshIndex] = MaterialIndex;
            }

            if (!AppendPrimitiveToMesh(&PrimitiveValue, MeshDatas[MeshIndex]))
            {
                return false;
            }
        }
    }

    const FJsonValue* Materials = GetObjectField(&Root, "materials");
    const FJsonValue* Textures = GetObjectField(&Root, "textures");
    const FJsonValue* Images = GetObjectField(&Root, "images");

    const bool bHasMaterialData = Materials && Materials->IsArray() && !Materials->ArrayValue.empty()
        && Textures && Textures->IsArray() && !Textures->ArrayValue.empty()
        && Images && Images->IsArray() && !Images->ArrayValue.empty();

    std::vector<FGltfMaterialTextureSet> MeshTextureSets(MeshMaterialIndices.size());

    if (bHasMaterialData)
    {
        const std::filesystem::path BasePath = std::filesystem::path(FilePath).parent_path();

        const auto ResolveMaterialTextures = [&](const FJsonValue* Material) -> FGltfMaterialTextureSet
        {
            FGltfMaterialTextureSet TextureSet;

            const FJsonValue* Pbr = GetObjectField(Material, "pbrMetallicRoughness");
            if (Pbr)
            {
                const FJsonValue* BaseColorTexture = GetObjectField(Pbr, "baseColorTexture");
                TextureSet.BaseColor = ResolveTexturePath(Textures, Images, BasePath, GetIntField(BaseColorTexture, "index", -1));
                TextureSet.BaseColorTransform = ResolveTextureTransform(BaseColorTexture);

                const FJsonValue* BaseColorFactor = GetObjectField(Pbr, "baseColorFactor");
                if (BaseColorFactor && BaseColorFactor->IsArray())
                {
                    TextureSet.BaseColorFactor.x = static_cast<float>(GetNumberField(BaseColorFactor, 0, TextureSet.BaseColorFactor.x));
                    TextureSet.BaseColorFactor.y = static_cast<float>(GetNumberField(BaseColorFactor, 1, TextureSet.BaseColorFactor.y));
                    TextureSet.BaseColorFactor.z = static_cast<float>(GetNumberField(BaseColorFactor, 2, TextureSet.BaseColorFactor.z));
                }

                TextureSet.MetallicFactor = static_cast<float>(GetNumberField(Pbr, "metallicFactor", 1.0));
                TextureSet.RoughnessFactor = static_cast<float>(GetNumberField(Pbr, "roughnessFactor", 1.0));

                const FJsonValue* MetallicRoughnessTexture = GetObjectField(Pbr, "metallicRoughnessTexture");
                TextureSet.MetallicRoughness = ResolveTexturePath(Textures, Images, BasePath, GetIntField(MetallicRoughnessTexture, "index", -1));
                TextureSet.MetallicRoughnessTransform = ResolveTextureTransform(MetallicRoughnessTexture);
            }

            const FJsonValue* NormalTexture = GetObjectField(Material, "normalTexture");
            TextureSet.Normal = ResolveTexturePath(Textures, Images, BasePath, GetIntField(NormalTexture, "index", -1));
            TextureSet.NormalTransform = ResolveTextureTransform(NormalTexture);

            const FJsonValue* EmissiveTexture = GetObjectField(Material, "emissiveTexture");
            TextureSet.Emissive = ResolveTexturePath(Textures, Images, BasePath, GetIntField(EmissiveTexture, "index", -1));
            TextureSet.EmissiveTransform = ResolveTextureTransform(EmissiveTexture);

            const FJsonValue* EmissiveFactor = GetObjectField(Material, "emissiveFactor");
            if (EmissiveFactor && EmissiveFactor->IsArray())
            {
                TextureSet.EmissiveFactor.x = static_cast<float>(GetNumberField(EmissiveFactor, 0, TextureSet.EmissiveFactor.x));
                TextureSet.EmissiveFactor.y = static_cast<float>(GetNumberField(EmissiveFactor, 1, TextureSet.EmissiveFactor.y));
                TextureSet.EmissiveFactor.z = static_cast<float>(GetNumberField(EmissiveFactor, 2, TextureSet.EmissiveFactor.z));
            }

            return TextureSet;
        };

        std::vector<FGltfMaterialTextureSet> MaterialTextureSets(Materials->ArrayValue.size());
        for (size_t MaterialIndex = 0; MaterialIndex < Materials->ArrayValue.size(); ++MaterialIndex)
        {
            MaterialTextureSets[MaterialIndex] = ResolveMaterialTextures(GetArrayElem(Materials, MaterialIndex));
        }

        for (size_t MeshIndex = 0; MeshIndex < MeshMaterialIndices.size(); ++MeshIndex)
        {
            const int64_t MaterialIndex = MeshMaterialIndices[MeshIndex];
            if (MaterialIndex >= 0 && MaterialIndex < static_cast<int64_t>(MaterialTextureSets.size()))
            {
                MeshTextureSets[MeshIndex] = MaterialTextureSets[static_cast<size_t>(MaterialIndex)];
            }
        }
    }

    OutScene = {};
    OutScene.MeshMaterials = MeshTextureSets;

    const FJsonValue* Nodes = GetObjectField(&Root, "nodes");
    const FJsonValue* Scenes = GetObjectField(&Root, "scenes");
    const int64_t SceneIndex = GetIntField(&Root, "scene", 0);

    if (Nodes && Nodes->IsArray() && Scenes && Scenes->IsArray())
    {
        const FJsonValue* Scene = GetArrayElem(Scenes, static_cast<size_t>(SceneIndex));
        const FJsonValue* SceneNodes = GetObjectField(Scene, "nodes");
        if (SceneNodes && SceneNodes->IsArray())
        {
            for (const FJsonValue& NodeValue : SceneNodes->ArrayValue)
            {
                const int64_t NodeIndex = static_cast<int64_t>(NodeValue.NumberValue);
                ProcessNodeRecursive(Nodes, NodeIndex, MakeIdentityMatrix(), MeshDatas, OutScene.Nodes);
            }
        }
    }

    if (OutScene.Nodes.empty())
    {
        for (size_t MeshIndex = 0; MeshIndex < MeshDatas.size(); ++MeshIndex)
        {
            FGltfNode Node;
            Node.MeshIndex = static_cast<int>(MeshIndex);
            Node.WorldMatrix = ToFloat4x4(MakeIdentityMatrix());
            OutScene.Nodes.push_back(Node);
        }
    }

    for (const FMeshData& MeshData : MeshDatas)
    {
        FMesh Mesh;
        Mesh.SetVertices(MeshData.Vertices);
        Mesh.SetIndices(MeshData.Indices);
        Mesh.GenerateNormalsIfMissing();
        Mesh.GenerateTangentsIfMissing();
        OutScene.Meshes.push_back(std::move(Mesh));
    }

    return true;
}
