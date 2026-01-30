#include "GltfLoader.h"
#include "Mesh.h"

#ifdef _MSC_VER
#pragma warning (disable : 4996)
#endif

#define CGLTF_IMPLEMENTATION
#include "../../ThirdParty/cgltf/cgltf.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <Windows.h>
#include <string>
#include <vector>

namespace
{
    using FMatrix4 = std::array<float, 16>;

    std::string ToUtf8String(const std::wstring& Wide)
    {
        if (Wide.empty())
        {
            return {};
        }

        const int RequiredSize = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            Wide.c_str(),
            static_cast<int>(Wide.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (RequiredSize <= 0)
        {
            return {};
        }

        std::string Result(static_cast<size_t>(RequiredSize), '\0');
        ::WideCharToMultiByte(
            CP_UTF8,
            0,
            Wide.c_str(),
            static_cast<int>(Wide.size()),
            Result.data(),
            RequiredSize,
            nullptr,
            nullptr);

        return Result;
    }

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

    FMatrix4 MatrixFromTRS(const FFloat3& Translation, const FFloat4& Rotation, const FFloat3& Scale)
    {
        const FMatrix4 T = { 1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f,
                             Translation.x, Translation.y, Translation.z, 1.0f };

        const FMatrix4 S = { Scale.x, 0.0f, 0.0f, 0.0f,
                             0.0f, Scale.y, 0.0f, 0.0f,
                             0.0f, 0.0f, Scale.z, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f };

        const FMatrix4 R = MatrixFromQuaternion(Rotation.x, Rotation.y, Rotation.z, Rotation.w);
        return MultiplyMatrix(MultiplyMatrix(T, R), S);
    }

    FMatrix4 ToLeftHandedMatrix(const FMatrix4& M)
    {
        const FMatrix4 MirrorZ = MakeMirrorZMatrix();
        return MultiplyMatrix(MirrorZ, MultiplyMatrix(M, MirrorZ));
    }

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

    struct FMeshData
    {
        struct FPrimitiveData
        {
            FMesh::FPrimitive Primitive;
            int64_t MaterialIndex = -1;
        };
        std::vector<FPrimitiveData> Primitives;
    };

    const cgltf_accessor* FindAccessor(const cgltf_primitive* Primitive, cgltf_attribute_type Type, cgltf_int Index)
    {
        if (!Primitive)
        {
            return nullptr;
        }

        for (cgltf_size AttributeIndex = 0; AttributeIndex < Primitive->attributes_count; ++AttributeIndex)
        {
            const cgltf_attribute& Attribute = Primitive->attributes[AttributeIndex];
            if (Attribute.type == Type && Attribute.index == Index)
            {
                return Attribute.data;
            }
        }

        return nullptr;
    }

    EGltfAnimationInterpolation ToInterpolation(cgltf_interpolation_type Type)
    {
        switch (Type)
        {
        case cgltf_interpolation_type_step:
            return EGltfAnimationInterpolation::Step;
        case cgltf_interpolation_type_cubic_spline:
            return EGltfAnimationInterpolation::CubicSpline;
        case cgltf_interpolation_type_linear:
        default:
            return EGltfAnimationInterpolation::Linear;
        }
    }

    bool ReadAccessorVec3(const cgltf_accessor* Accessor, std::vector<FFloat3>& OutValues)
    {
        if (!Accessor)
        {
            return false;
        }

        const cgltf_size Count = Accessor->count;
        OutValues.resize(static_cast<size_t>(Count));
        for (cgltf_size Index = 0; Index < Count; ++Index)
        {
            float Value[3] = {};
            cgltf_accessor_read_float(Accessor, Index, Value, 3);
            OutValues[static_cast<size_t>(Index)] = FFloat3(Value[0], Value[1], Value[2]);
        }

        return true;
    }

    bool ReadAccessorVec4(const cgltf_accessor* Accessor, std::vector<FFloat4>& OutValues)
    {
        if (!Accessor)
        {
            return false;
        }

        const cgltf_size Count = Accessor->count;
        OutValues.resize(static_cast<size_t>(Count));
        for (cgltf_size Index = 0; Index < Count; ++Index)
        {
            float Value[4] = {};
            cgltf_accessor_read_float(Accessor, Index, Value, 4);
            OutValues[static_cast<size_t>(Index)] = FFloat4(Value[0], Value[1], Value[2], Value[3]);
        }

        return true;
    }

    bool ReadAccessorScalar(const cgltf_accessor* Accessor, std::vector<float>& OutValues)
    {
        if (!Accessor)
        {
            return false;
        }

        const cgltf_size Count = Accessor->count;
        OutValues.resize(static_cast<size_t>(Count));
        for (cgltf_size Index = 0; Index < Count; ++Index)
        {
            float Value = 0.0f;
            cgltf_accessor_read_float(Accessor, Index, &Value, 1);
            OutValues[static_cast<size_t>(Index)] = Value;
        }

        return true;
    }

    DirectX::XMFLOAT4X4 ReadAccessorMat4LH(const cgltf_accessor* Accessor, cgltf_size Index)
    {
        float Value[16] = {};
        cgltf_accessor_read_float(Accessor, Index, Value, 16);
        FMatrix4 Matrix{};
        for (int i = 0; i < 16; ++i)
        {
            Matrix[static_cast<size_t>(i)] = Value[i];
        }

        return ToFloat4x4(ToLeftHandedMatrix(Matrix));
    }

    std::wstring ResolveTexturePath(const std::filesystem::path& BasePath, const cgltf_texture* Texture)
    {
        if (!Texture || !Texture->image || !Texture->image->uri)
        {
            return L"";
        }

        const std::filesystem::path FullPath = BasePath / std::filesystem::path(Texture->image->uri);
        return FullPath.wstring();
    }

    FGltfTextureTransform ResolveTextureTransform(const cgltf_texture_view& View)
    {
        FGltfTextureTransform Transform;

        if (!View.has_transform)
        {
            return Transform;
        }

        Transform.Offset.x = View.transform.offset[0];
        Transform.Offset.y = View.transform.offset[1];
        Transform.Scale.x = View.transform.scale[0];
        Transform.Scale.y = View.transform.scale[1];
        Transform.Rotation = View.transform.rotation;

        return Transform;
    }

    FGltfMaterialTextureSet ResolveMaterialTextures(const std::filesystem::path& BasePath, const cgltf_material* Material)
    {
        FGltfMaterialTextureSet TextureSet;
        if (!Material)
        {
            return TextureSet;
        }

        const cgltf_pbr_metallic_roughness& Pbr = Material->pbr_metallic_roughness;
        TextureSet.BaseColor = ResolveTexturePath(BasePath, Pbr.base_color_texture.texture);
        TextureSet.BaseColorTransform = ResolveTextureTransform(Pbr.base_color_texture);
        TextureSet.BaseColorFactor.x = Pbr.base_color_factor[0];
        TextureSet.BaseColorFactor.y = Pbr.base_color_factor[1];
        TextureSet.BaseColorFactor.z = Pbr.base_color_factor[2];
        TextureSet.BaseColorAlpha = Pbr.base_color_factor[3];
        TextureSet.MetallicFactor = Pbr.metallic_factor;
        TextureSet.RoughnessFactor = Pbr.roughness_factor;
        TextureSet.MetallicRoughness = ResolveTexturePath(BasePath, Pbr.metallic_roughness_texture.texture);
        TextureSet.MetallicRoughnessTransform = ResolveTextureTransform(Pbr.metallic_roughness_texture);

        TextureSet.Normal = ResolveTexturePath(BasePath, Material->normal_texture.texture);
        TextureSet.NormalTransform = ResolveTextureTransform(Material->normal_texture);
        TextureSet.Emissive = ResolveTexturePath(BasePath, Material->emissive_texture.texture);
        TextureSet.EmissiveTransform = ResolveTextureTransform(Material->emissive_texture);
        TextureSet.EmissiveFactor.x = Material->emissive_factor[0];
        TextureSet.EmissiveFactor.y = Material->emissive_factor[1];
        TextureSet.EmissiveFactor.z = Material->emissive_factor[2];

        if (Material->alpha_mode == cgltf_alpha_mode_mask)
        {
            TextureSet.bAlphaMask = true;
            TextureSet.AlphaCutoff = Material->alpha_cutoff;
        }
        else if (Material->alpha_mode == cgltf_alpha_mode_blend)
        {
            TextureSet.bAlphaBlend = true;
        }

        TextureSet.bDoubleSided = Material->double_sided;

        return TextureSet;
    }

    bool AppendPrimitiveToMesh(const cgltf_primitive* Primitive, FMeshData::FPrimitiveData& OutPrimitive)
    {
        if (!Primitive)
        {
            return false;
        }

        const cgltf_accessor* PositionAccessor = FindAccessor(Primitive, cgltf_attribute_type_position, 0);
        if (!PositionAccessor)
        {
            return false;
        }

        const cgltf_accessor* NormalAccessor = FindAccessor(Primitive, cgltf_attribute_type_normal, 0);
        const cgltf_accessor* TexcoordAccessor = FindAccessor(Primitive, cgltf_attribute_type_texcoord, 0);
        const cgltf_accessor* TangentAccessor = FindAccessor(Primitive, cgltf_attribute_type_tangent, 0);
        const cgltf_accessor* ColorAccessor = FindAccessor(Primitive, cgltf_attribute_type_color, 0);
        const cgltf_accessor* JointsAccessor = FindAccessor(Primitive, cgltf_attribute_type_joints, 0);
        const cgltf_accessor* WeightsAccessor = FindAccessor(Primitive, cgltf_attribute_type_weights, 0);

        const cgltf_size PositionCount = PositionAccessor->count;
        if (PositionCount == 0)
        {
            return false;
        }

        FMesh::FVertexStreams& Streams = OutPrimitive.Primitive.VertexStreams;
        Streams.Positions.resize(static_cast<size_t>(PositionCount));
        Streams.Normals.resize(static_cast<size_t>(PositionCount), FFloat3(0.0f, 0.0f, 1.0f));
        Streams.UVs.resize(static_cast<size_t>(PositionCount), FFloat2(0.0f, 0.0f));
        Streams.Tangents.resize(static_cast<size_t>(PositionCount), FFloat4(0.0f, 0.0f, 0.0f, 1.0f));
        Streams.Colors.resize(static_cast<size_t>(PositionCount), FFloat4(1.0f, 1.0f, 1.0f, 1.0f));
        Streams.Joints.resize(static_cast<size_t>(PositionCount), FUInt4{});
        Streams.Weights.resize(static_cast<size_t>(PositionCount), FFloat4(0.0f, 0.0f, 0.0f, 0.0f));

        for (cgltf_size VertexIndex = 0; VertexIndex < PositionCount; ++VertexIndex)
        {
            float Position[3] = {};
            cgltf_accessor_read_float(PositionAccessor, VertexIndex, Position, 3);
            FFloat3 Pos = { Position[0], Position[1], Position[2] };
            Pos.z = -Pos.z;
            Streams.Positions[static_cast<size_t>(VertexIndex)] = Pos;

            if (NormalAccessor)
            {
                float Normal[3] = {};
                cgltf_accessor_read_float(NormalAccessor, VertexIndex, Normal, 3);
                FFloat3 NormalValue{ Normal[0], Normal[1], Normal[2] };
                NormalValue.z = -NormalValue.z;
                Streams.Normals[static_cast<size_t>(VertexIndex)] = NormalValue;
            }
            else
            {
                FFloat3 NormalValue{ 0.0f, 0.0f, 1.0f };
                NormalValue.z = -NormalValue.z;
                Streams.Normals[static_cast<size_t>(VertexIndex)] = NormalValue;
            }

            if (TangentAccessor)
            {
                float Tangent[4] = {};
                cgltf_accessor_read_float(TangentAccessor, VertexIndex, Tangent, 4);
                FFloat4 TangentValue{ Tangent[0], Tangent[1], Tangent[2], Tangent[3] };
                TangentValue.z = -TangentValue.z;
                TangentValue.w = -TangentValue.w;
                Streams.Tangents[static_cast<size_t>(VertexIndex)] = TangentValue;
            }
            else
            {
                FFloat4 TangentValue{ 0.0f, 0.0f, 0.0f, 1.0f };
                TangentValue.z = -TangentValue.z;
                TangentValue.w = -TangentValue.w;
                Streams.Tangents[static_cast<size_t>(VertexIndex)] = TangentValue;
            }

            if (TexcoordAccessor)
            {
                float UV[2] = {};
                cgltf_accessor_read_float(TexcoordAccessor, VertexIndex, UV, 2);
                Streams.UVs[static_cast<size_t>(VertexIndex)] = FFloat2(UV[0], UV[1]);
            }

            if (ColorAccessor)
            {
                const cgltf_size ComponentCount = cgltf_num_components(ColorAccessor->type);
                float Color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                cgltf_accessor_read_float(ColorAccessor, VertexIndex, Color, ComponentCount);
                const float Alpha = (ComponentCount >= 4) ? Color[3] : 1.0f;
                Streams.Colors[static_cast<size_t>(VertexIndex)] = FFloat4(Color[0], Color[1], Color[2], Alpha);
            }

            if (JointsAccessor)
            {
                cgltf_uint Joints[4] = {};
                cgltf_accessor_read_uint(JointsAccessor, VertexIndex, Joints, 4);
                Streams.Joints[static_cast<size_t>(VertexIndex)] = FUInt4{
                    static_cast<uint32_t>(Joints[0]),
                    static_cast<uint32_t>(Joints[1]),
                    static_cast<uint32_t>(Joints[2]),
                    static_cast<uint32_t>(Joints[3])
                };
            }

            if (WeightsAccessor)
            {
                float Weights[4] = {};
                cgltf_accessor_read_float(WeightsAccessor, VertexIndex, Weights, 4);
                Streams.Weights[static_cast<size_t>(VertexIndex)] = FFloat4(Weights[0], Weights[1], Weights[2], Weights[3]);
            }
        }

        std::vector<uint32_t> RawIndices;
        if (Primitive->indices)
        {
            const cgltf_size IndexCount = Primitive->indices->count;
            RawIndices.reserve(static_cast<size_t>(IndexCount));
            for (cgltf_size Index = 0; Index < IndexCount; ++Index)
            {
                const cgltf_size Value = cgltf_accessor_read_index(Primitive->indices, Index);
                RawIndices.push_back(static_cast<uint32_t>(Value));
            }
        }
        else
        {
            RawIndices.reserve(static_cast<size_t>(PositionCount));
            for (cgltf_size Index = 0; Index < PositionCount; ++Index)
            {
                RawIndices.push_back(static_cast<uint32_t>(Index));
            }
        }

        switch (Primitive->type)
        {
        case cgltf_primitive_type_triangles:
        {
            if (RawIndices.size() % 3 != 0)
            {
                return false;
            }
            OutPrimitive.Primitive.Indices.insert(OutPrimitive.Primitive.Indices.end(), RawIndices.begin(), RawIndices.end());
            break;
        }
        case cgltf_primitive_type_triangle_strip:
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
                    OutPrimitive.Primitive.Indices.push_back(i0);
                    OutPrimitive.Primitive.Indices.push_back(i1);
                    OutPrimitive.Primitive.Indices.push_back(i2);
                }
                else
                {
                    OutPrimitive.Primitive.Indices.push_back(i1);
                    OutPrimitive.Primitive.Indices.push_back(i0);
                    OutPrimitive.Primitive.Indices.push_back(i2);
                }
            }
            break;
        }
        case cgltf_primitive_type_triangle_fan:
        {
            if (RawIndices.size() < 3)
            {
                return false;
            }
            for (size_t i = 2; i < RawIndices.size(); ++i)
            {
                OutPrimitive.Primitive.Indices.push_back(RawIndices[0]);
                OutPrimitive.Primitive.Indices.push_back(RawIndices[i - 1]);
                OutPrimitive.Primitive.Indices.push_back(RawIndices[i]);
            }
            break;
        }
        default:
            return false;
        }

        if (OutPrimitive.Primitive.Indices.empty())
        {
            return false;
        }

        return true;
    }

    void ProcessNodeRecursive(const cgltf_data* Data, const cgltf_node* Node, std::vector<FGltfNode>& OutNodes)
    {
        if (!Data || !Node)
        {
            return;
        }

        const ptrdiff_t NodeIndex = Node - Data->nodes;

        if (Node->mesh)
        {
            const ptrdiff_t MeshIndex = Node->mesh - Data->meshes;
            if (MeshIndex >= 0 && MeshIndex < static_cast<ptrdiff_t>(Data->meshes_count))
            {
                cgltf_float Matrix[16] = {};
                cgltf_node_transform_world(Node, Matrix);
                FMatrix4 World{};
                for (int i = 0; i < 16; ++i)
                {
                    World[static_cast<size_t>(i)] = Matrix[i];
                }

                const FMatrix4 WorldLH = ToLeftHandedMatrix(World);

                FGltfNode LoadedNode;
                LoadedNode.MeshIndex = static_cast<int>(MeshIndex);
                if (Node->skin)
                {
                    LoadedNode.SkinIndex = static_cast<int>(Node->skin - Data->skins);
                }
                LoadedNode.NodeIndex = static_cast<int>(NodeIndex);
                LoadedNode.WorldMatrix = ToFloat4x4(WorldLH);
                LoadedNode.Name = Node->mesh->name ? Node->mesh->name : (Node->name ? Node->name : "");
                OutNodes.push_back(LoadedNode);
            }
        }
        else if (NodeIndex >= 0 && NodeIndex < static_cast<ptrdiff_t>(Data->nodes_count))
        {
            cgltf_float Matrix[16] = {};
            cgltf_node_transform_world(Node, Matrix);
            FMatrix4 World{};
            for (int i = 0; i < 16; ++i)
            {
                World[static_cast<size_t>(i)] = Matrix[i];
            }

            const FMatrix4 WorldLH = ToLeftHandedMatrix(World);

            FGltfNode LoadedNode;
            LoadedNode.MeshIndex = -1;
            if (Node->skin)
            {
                LoadedNode.SkinIndex = static_cast<int>(Node->skin - Data->skins);
            }
            LoadedNode.NodeIndex = static_cast<int>(NodeIndex);
            LoadedNode.WorldMatrix = ToFloat4x4(WorldLH);
            LoadedNode.Name = Node->name ? Node->name : "";
            OutNodes.push_back(LoadedNode);
        }

        for (cgltf_size ChildIndex = 0; ChildIndex < Node->children_count; ++ChildIndex)
        {
            ProcessNodeRecursive(Data, Node->children[ChildIndex], OutNodes);
        }
    }
}

bool FGltfLoader::LoadSceneFromFile(const std::wstring& FilePath, FGltfScene& OutScene)
{
    const std::string FilePathUtf8 = ToUtf8String(FilePath);

    cgltf_options Options{};
    cgltf_data* Data = nullptr;

    if (cgltf_parse_file(&Options, FilePathUtf8.c_str(), &Data) != cgltf_result_success || !Data)
    {
        return false;
    }

    if (cgltf_load_buffers(&Options, Data, FilePathUtf8.c_str()) != cgltf_result_success)
    {
        cgltf_free(Data);
        return false;
    }

    if (cgltf_validate(Data) != cgltf_result_success)
    {
        cgltf_free(Data);
        return false;
    }

    std::vector<FMeshData> MeshDatas;
    MeshDatas.resize(Data->meshes_count);

    for (cgltf_size MeshIndex = 0; MeshIndex < Data->meshes_count; ++MeshIndex)
    {
        const cgltf_mesh& Mesh = Data->meshes[MeshIndex];
        for (cgltf_size PrimitiveIndex = 0; PrimitiveIndex < Mesh.primitives_count; ++PrimitiveIndex)
        {
            const cgltf_primitive& Primitive = Mesh.primitives[PrimitiveIndex];

            FMeshData::FPrimitiveData PrimitiveData;
            if (Primitive.material)
            {
                PrimitiveData.MaterialIndex = Primitive.material - Data->materials;
            }

            if (!AppendPrimitiveToMesh(&Primitive, PrimitiveData))
            {
                cgltf_free(Data);
                return false;
            }

            MeshDatas[MeshIndex].Primitives.push_back(std::move(PrimitiveData));
        }
    }

    const std::filesystem::path BasePath = std::filesystem::path(FilePath).parent_path();
    std::vector<FGltfMaterialTextureSet> MaterialTextureSets;
    MaterialTextureSets.resize(Data->materials_count);
    for (cgltf_size MaterialIndex = 0; MaterialIndex < Data->materials_count; ++MaterialIndex)
    {
        MaterialTextureSets[MaterialIndex] = ResolveMaterialTextures(BasePath, &Data->materials[MaterialIndex]);
    }

    std::vector<std::vector<FGltfPrimitiveSection>> MeshPrimitiveSections(MeshDatas.size());
    for (size_t MeshIndex = 0; MeshIndex < MeshDatas.size(); ++MeshIndex)
    {
        const std::vector<FMeshData::FPrimitiveData>& Primitives = MeshDatas[MeshIndex].Primitives;
        std::vector<FGltfPrimitiveSection>& Sections = MeshPrimitiveSections[MeshIndex];
        Sections.reserve(Primitives.size());
        uint32_t RunningIndexStart = 0;
        for (const FMeshData::FPrimitiveData& PrimitiveInfo : Primitives)
        {
            FGltfPrimitiveSection Section;
            Section.IndexStart = RunningIndexStart;
            Section.IndexCount = static_cast<uint32_t>(PrimitiveInfo.Primitive.Indices.size());
            RunningIndexStart += Section.IndexCount;
            if (PrimitiveInfo.MaterialIndex >= 0
                && PrimitiveInfo.MaterialIndex < static_cast<int64_t>(MaterialTextureSets.size()))
            {
                Section.Material = MaterialTextureSets[static_cast<size_t>(PrimitiveInfo.MaterialIndex)];
            }
            Sections.push_back(Section);
        }
    }

    std::vector<FGltfSkin> Skins;
    Skins.resize(Data->skins_count);
    for (cgltf_size SkinIndex = 0; SkinIndex < Data->skins_count; ++SkinIndex)
    {
        const cgltf_skin& Skin = Data->skins[SkinIndex];
        FGltfSkin SkinData;
        SkinData.Name = Skin.name ? Skin.name : "";
        if (Skin.skeleton)
        {
            SkinData.SkeletonNodeIndex = static_cast<int>(Skin.skeleton - Data->nodes);
        }

        SkinData.Joints.reserve(Skin.joints_count);
        for (cgltf_size JointIndex = 0; JointIndex < Skin.joints_count; ++JointIndex)
        {
            const cgltf_node* JointNode = Skin.joints[JointIndex];
            const int NodeIndex = JointNode ? static_cast<int>(JointNode - Data->nodes) : -1;
            SkinData.Joints.push_back(NodeIndex);
        }

        const cgltf_accessor* InverseBindAccessor = Skin.inverse_bind_matrices;
        const size_t MatrixCount = SkinData.Joints.size();
        SkinData.InverseBindMatrices.resize(MatrixCount);
        if (InverseBindAccessor)
        {
            const size_t AccessorCount = static_cast<size_t>(InverseBindAccessor->count);
            const size_t ReadCount = (std::min)(MatrixCount, AccessorCount);
            for (size_t Index = 0; Index < ReadCount; ++Index)
            {
                SkinData.InverseBindMatrices[Index] = ReadAccessorMat4LH(InverseBindAccessor, static_cast<cgltf_size>(Index));
            }
            for (size_t Index = ReadCount; Index < MatrixCount; ++Index)
            {
                SkinData.InverseBindMatrices[Index] = ToFloat4x4(MakeIdentityMatrix());
            }
        }
        else
        {
            for (size_t Index = 0; Index < MatrixCount; ++Index)
            {
                SkinData.InverseBindMatrices[Index] = ToFloat4x4(MakeIdentityMatrix());
            }
        }

        Skins[SkinIndex] = std::move(SkinData);
    }

    std::vector<FGltfAnimation> Animations;
    Animations.resize(Data->animations_count);
    for (cgltf_size AnimIndex = 0; AnimIndex < Data->animations_count; ++AnimIndex)
    {
        const cgltf_animation& Animation = Data->animations[AnimIndex];
        FGltfAnimation AnimData;
        AnimData.Name = Animation.name ? Animation.name : "";

        AnimData.Samplers.resize(Animation.samplers_count);
        for (cgltf_size SamplerIndex = 0; SamplerIndex < Animation.samplers_count; ++SamplerIndex)
        {
            const cgltf_animation_sampler& Sampler = Animation.samplers[SamplerIndex];
            FGltfAnimationSampler SamplerData;
            SamplerData.Interpolation = ToInterpolation(Sampler.interpolation);
            ReadAccessorScalar(Sampler.input, SamplerData.InputTimes);

            if (Sampler.output)
            {
                if (Sampler.output->type == cgltf_type_vec3)
                {
                    ReadAccessorVec3(Sampler.output, SamplerData.OutputVec3);
                }
                else if (Sampler.output->type == cgltf_type_vec4)
                {
                    ReadAccessorVec4(Sampler.output, SamplerData.OutputVec4);
                }
            }

            AnimData.Samplers[SamplerIndex] = std::move(SamplerData);
        }

        AnimData.Channels.reserve(Animation.channels_count);
        for (cgltf_size ChannelIndex = 0; ChannelIndex < Animation.channels_count; ++ChannelIndex)
        {
            const cgltf_animation_channel& Channel = Animation.channels[ChannelIndex];
            if (!Channel.target_node || !Channel.sampler)
            {
                continue;
            }

            FGltfAnimationChannel ChannelData;
            ChannelData.NodeIndex = static_cast<int>(Channel.target_node - Data->nodes);
            ChannelData.SamplerIndex = static_cast<int>(Channel.sampler - Animation.samplers);

            switch (Channel.target_path)
            {
            case cgltf_animation_path_type_rotation:
                ChannelData.Path = EGltfAnimationPath::Rotation;
                break;
            case cgltf_animation_path_type_scale:
                ChannelData.Path = EGltfAnimationPath::Scale;
                break;
            case cgltf_animation_path_type_translation:
            default:
                ChannelData.Path = EGltfAnimationPath::Translation;
                break;
            }

            AnimData.Channels.push_back(ChannelData);
        }

        Animations[AnimIndex] = std::move(AnimData);
    }

    std::vector<FGltfNodeTransform> NodeTransforms;
    NodeTransforms.resize(Data->nodes_count);
    for (cgltf_size NodeIndex = 0; NodeIndex < Data->nodes_count; ++NodeIndex)
    {
        const cgltf_node& Node = Data->nodes[NodeIndex];
        FGltfNodeTransform Transform;
        Transform.ParentIndex = Node.parent ? static_cast<int>(Node.parent - Data->nodes) : -1;
        Transform.bHasMatrix = Node.has_matrix != 0;

        if (Node.has_translation)
        {
            Transform.Translation = FFloat3(Node.translation[0], Node.translation[1], Node.translation[2]);
        }

        if (Node.has_rotation)
        {
            Transform.Rotation = FFloat4(Node.rotation[0], Node.rotation[1], Node.rotation[2], Node.rotation[3]);
        }

        if (Node.has_scale)
        {
            Transform.Scale = FFloat3(Node.scale[0], Node.scale[1], Node.scale[2]);
        }

        if (!Transform.bHasMatrix)
        {
            const FMatrix4 Local = MatrixFromTRS(Transform.Translation, Transform.Rotation, Transform.Scale);
            Transform.LocalMatrix = ToFloat4x4(ToLeftHandedMatrix(Local));
        }
        else
        {
            cgltf_float Matrix[16] = {};
            cgltf_node_transform_local(&Node, Matrix);
            FMatrix4 Local{};
            for (int i = 0; i < 16; ++i)
            {
                Local[static_cast<size_t>(i)] = Matrix[i];
            }
            Transform.LocalMatrix = ToFloat4x4(ToLeftHandedMatrix(Local));
        }

        NodeTransforms[NodeIndex] = Transform;
    }

    OutScene = {};
    OutScene.MeshPrimitiveSections = std::move(MeshPrimitiveSections);
    OutScene.Skins = std::move(Skins);
    OutScene.Animations = std::move(Animations);
    OutScene.NodeTransforms = std::move(NodeTransforms);

    const cgltf_scene* Scene = Data->scene ? Data->scene : (Data->scenes_count > 0 ? &Data->scenes[0] : nullptr);
    if (Scene)
    {
        for (cgltf_size NodeIndex = 0; NodeIndex < Scene->nodes_count; ++NodeIndex)
        {
            ProcessNodeRecursive(Data, Scene->nodes[NodeIndex], OutScene.Nodes);
        }
    }
    else
    {
        for (cgltf_size NodeIndex = 0; NodeIndex < Data->nodes_count; ++NodeIndex)
        {
            ProcessNodeRecursive(Data, &Data->nodes[NodeIndex], OutScene.Nodes);
        }
    }

    if (OutScene.Nodes.empty())
    {
        for (size_t MeshIndex = 0; MeshIndex < MeshDatas.size(); ++MeshIndex)
        {
            FGltfNode Node;
            Node.MeshIndex = static_cast<int>(MeshIndex);
            Node.NodeIndex = static_cast<int>(MeshIndex);
            Node.WorldMatrix = ToFloat4x4(MakeIdentityMatrix());
            OutScene.Nodes.push_back(Node);
        }
    }

    for (const FMeshData& MeshData : MeshDatas)
    {
        FMesh Mesh;
        std::vector<FMesh::FPrimitive> MeshPrimitives;
        MeshPrimitives.reserve(MeshData.Primitives.size());
        for (const FMeshData::FPrimitiveData& PrimitiveInfo : MeshData.Primitives)
        {
            MeshPrimitives.push_back(PrimitiveInfo.Primitive);
        }

        Mesh.SetPrimitives(MeshPrimitives);
        Mesh.GenerateNormalsIfMissing();
        Mesh.GenerateTangentsIfMissing();

        Mesh.SetMeshletIndexingAllowed(!MeshPrimitives.empty());
        Mesh.BuildMeshlets();
        OutScene.Meshes.push_back(std::move(Mesh));
    }

    cgltf_free(Data);
    return true;
}
