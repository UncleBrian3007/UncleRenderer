#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

using FFloat2 = DirectX::XMFLOAT2;
using FFloat3 = DirectX::XMFLOAT3;
using FFloat4 = DirectX::XMFLOAT4;
using FMatrix4 = std::array<float, 16>;
using FMatrix = DirectX::XMMATRIX;
using FQuaternion = DirectX::XMVECTOR;

struct FUInt4
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    uint32_t w = 0;
};

inline uint32_t AlignDispatch(uint32_t Value, uint32_t GroupSize)
{
    return (Value + GroupSize - 1u) / GroupSize;
}

namespace VectorMath
{
    template<typename TVector3>
    TVector3 Add3(const TVector3& A, const TVector3& B)
    {
        TVector3 Result{};
        Result.x = A.x + B.x;
        Result.y = A.y + B.y;
        Result.z = A.z + B.z;
        return Result;
    }

    template<typename TVector3>
    TVector3 Sub3(const TVector3& A, const TVector3& B)
    {
        TVector3 Result{};
        Result.x = A.x - B.x;
        Result.y = A.y - B.y;
        Result.z = A.z - B.z;
        return Result;
    }

    template<typename TVector3>
    TVector3 Scale3(const TVector3& Value, float Scale)
    {
        TVector3 Result{};
        Result.x = Value.x * Scale;
        Result.y = Value.y * Scale;
        Result.z = Value.z * Scale;
        return Result;
    }

    template<typename TVector3>
    TVector3 Lerp3(const TVector3& A, const TVector3& B, float Alpha)
    {
        TVector3 Result{};
        Result.x = A.x + (B.x - A.x) * Alpha;
        Result.y = A.y + (B.y - A.y) * Alpha;
        Result.z = A.z + (B.z - A.z) * Alpha;
        return Result;
    }

    template<typename TVector3>
    float Dot3(const TVector3& A, const TVector3& B)
    {
        return A.x * B.x + A.y * B.y + A.z * B.z;
    }

    template<typename TVector3>
    TVector3 Cross3(const TVector3& A, const TVector3& B)
    {
        TVector3 Result{};
        Result.x = A.y * B.z - A.z * B.y;
        Result.y = A.z * B.x - A.x * B.z;
        Result.z = A.x * B.y - A.y * B.x;
        return Result;
    }

    template<typename TVector3>
    float LengthSquared3(const TVector3& Value)
    {
        return Dot3(Value, Value);
    }

    template<typename TVector3>
    float Length3(const TVector3& Value)
    {
        return std::sqrt(LengthSquared3(Value));
    }

    template<typename TVector3>
    float DistanceSquared3(const TVector3& A, const TVector3& B)
    {
        return LengthSquared3(Sub3(A, B));
    }

    template<typename TVector3>
    float Distance3(const TVector3& A, const TVector3& B)
    {
        return std::sqrt(DistanceSquared3(A, B));
    }

    template<typename TVector3>
    TVector3 Normalize3(const TVector3& Value, const TVector3& Fallback)
    {
        const float Length = Length3(Value);
        if (Length <= 1e-20f)
        {
            return Fallback;
        }

        return Scale3(Value, 1.0f / Length);
    }
}

namespace MatrixMath
{
    inline FMatrix4 Identity()
    {
        return { 1.0f, 0.0f, 0.0f, 0.0f,
                 0.0f, 1.0f, 0.0f, 0.0f,
                 0.0f, 0.0f, 1.0f, 0.0f,
                 0.0f, 0.0f, 0.0f, 1.0f };
    }

    inline FMatrix4 MakeMirrorZ()
    {
        return { 1.0f, 0.0f,  0.0f, 0.0f,
                 0.0f, 1.0f,  0.0f, 0.0f,
                 0.0f, 0.0f, -1.0f, 0.0f,
                 0.0f, 0.0f,  0.0f, 1.0f };
    }

    inline FMatrix4 Multiply(const FMatrix4& A, const FMatrix4& B)
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

    template<typename TScalar>
    FMatrix4 FromArray16(const TScalar* Values)
    {
        FMatrix4 Result{};
        for (int i = 0; i < 16; ++i)
        {
            Result[static_cast<std::size_t>(i)] = static_cast<float>(Values[i]);
        }
        return Result;
    }

    inline FMatrix4 ToLeftHanded(const FMatrix4& Matrix)
    {
        const FMatrix4 MirrorZ = MakeMirrorZ();
        return Multiply(MirrorZ, Multiply(Matrix, MirrorZ));
    }

    inline float ComputeMaxScale(const DirectX::XMFLOAT4X4& Matrix)
    {
        const float ScaleX = std::sqrt(Matrix._11 * Matrix._11 + Matrix._21 * Matrix._21 + Matrix._31 * Matrix._31);
        const float ScaleY = std::sqrt(Matrix._12 * Matrix._12 + Matrix._22 * Matrix._22 + Matrix._32 * Matrix._32);
        const float ScaleZ = std::sqrt(Matrix._13 * Matrix._13 + Matrix._23 * Matrix._23 + Matrix._33 * Matrix._33);
        return (std::max)(ScaleX, (std::max)(ScaleY, ScaleZ));
    }

    inline DirectX::XMFLOAT4X4 ToFloat4x4(const FMatrix4& Matrix)
    {
        DirectX::XMFLOAT4X4 Result{};
        for (int Row = 0; Row < 4; ++Row)
        {
            for (int Col = 0; Col < 4; ++Col)
            {
                Result.m[Row][Col] = Matrix[Row * 4 + Col];
            }
        }

        return Result;
    }

    inline FMatrix4 FromQuaternion(float x, float y, float z, float w)
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

    template<typename TVector4>
    FMatrix4 FromQuaternion(const TVector4& Rotation)
    {
        return FromQuaternion(Rotation.x, Rotation.y, Rotation.z, Rotation.w);
    }

    template<typename TVector3, typename TVector4>
    FMatrix4 FromTRS(const TVector3& Translation, const TVector4& Rotation, const TVector3& Scale)
    {
        const FMatrix4 T = { 1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f,
                             Translation.x, Translation.y, Translation.z, 1.0f };

        const FMatrix4 S = { Scale.x, 0.0f, 0.0f, 0.0f,
                             0.0f, Scale.y, 0.0f, 0.0f,
                             0.0f, 0.0f, Scale.z, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f };

        const FMatrix4 R = FromQuaternion(Rotation);
        return Multiply(Multiply(T, R), S);
    }
}
