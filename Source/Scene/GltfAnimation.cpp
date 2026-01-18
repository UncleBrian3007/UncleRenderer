#include "GltfAnimation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

namespace
{
    using FMatrix4 = std::array<float, 16>;

    FMatrix4 MakeMirrorZMatrix()
    {
        return { 1.0f, 0.0f,  0.0f, 0.0f,
                 0.0f, 1.0f,  0.0f, 0.0f,
                 0.0f, 0.0f, -1.0f, 0.0f,
                 0.0f, 0.0f,  0.0f, 1.0f };
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

    DirectX::XMFLOAT4X4 BuildLocalMatrixLH(const FFloat3& Translation, const FFloat4& Rotation, const FFloat3& Scale)
    {
        const FMatrix4 Local = MatrixFromTRS(Translation, Rotation, Scale);
        return ToFloat4x4(ToLeftHandedMatrix(Local));
    }

    float WrapAnimationTime(const std::vector<float>& Times, float TimeSeconds)
    {
        if (Times.empty())
        {
            return 0.0f;
        }

        const float Start = Times.front();
        const float End = Times.back();
        const float Duration = End - Start;
        if (Duration <= 0.0f)
        {
            return Start;
        }

        const float Wrapped = std::fmod(TimeSeconds - Start, Duration);
        return Wrapped < 0.0f ? (Wrapped + Duration + Start) : (Wrapped + Start);
    }

    size_t FindKeyframeIndex(const std::vector<float>& Times, float TimeSeconds)
    {
        if (Times.size() <= 1)
        {
            return 0;
        }

        const auto Upper = std::upper_bound(Times.begin(), Times.end(), TimeSeconds);
        if (Upper == Times.begin())
        {
            return 0;
        }

        const size_t Index = static_cast<size_t>(std::distance(Times.begin(), Upper) - 1);
        return std::min(Index, Times.size() - 2);
    }

    float ComputeLerpAlpha(const std::vector<float>& Times, size_t Index, float TimeSeconds)
    {
        if (Times.size() <= 1)
        {
            return 0.0f;
        }

        const float t0 = Times[Index];
        const float t1 = Times[Index + 1];
        if (t1 <= t0)
        {
            return 0.0f;
        }

        return (TimeSeconds - t0) / (t1 - t0);
    }

    FFloat3 LerpVec3(const FFloat3& A, const FFloat3& B, float Alpha)
    {
        return FFloat3(
            A.x + (B.x - A.x) * Alpha,
            A.y + (B.y - A.y) * Alpha,
            A.z + (B.z - A.z) * Alpha);
    }

    FFloat4 SlerpQuat(const FFloat4& A, const FFloat4& B, float Alpha)
    {
        using namespace DirectX;
        const XMVECTOR Qa = XMLoadFloat4(&A);
        const XMVECTOR Qb = XMLoadFloat4(&B);
        const XMVECTOR Qn = XMQuaternionNormalize(XMQuaternionSlerp(Qa, Qb, Alpha));
        FFloat4 Out{};
        XMStoreFloat4(&Out, Qn);
        return Out;
    }

    struct FNodePose
    {
        FFloat3 Translation{ 0.0f, 0.0f, 0.0f };
        FFloat4 Rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
        FFloat3 Scale{ 1.0f, 1.0f, 1.0f };
        bool bAnimated = false;
    };

    void ComputeWorldMatrices(
        const std::vector<FGltfNodeTransform>& NodeTransforms,
        const std::vector<DirectX::XMFLOAT4X4>& LocalMatrices,
        std::vector<DirectX::XMFLOAT4X4>& OutWorldMatrices)
    {
        const size_t NodeCount = NodeTransforms.size();
        OutWorldMatrices.resize(NodeCount);
        std::vector<uint8_t> Visited(NodeCount, 0);

        std::function<void(size_t)> Visit = [&](size_t Index)
        {
            if (Visited[Index])
            {
                return;
            }

            const int ParentIndex = NodeTransforms[Index].ParentIndex;
            if (ParentIndex >= 0)
            {
                Visit(static_cast<size_t>(ParentIndex));
            }

            using namespace DirectX;
            const XMMATRIX Local = XMLoadFloat4x4(&LocalMatrices[Index]);
            if (ParentIndex >= 0)
            {
                const XMMATRIX Parent = XMLoadFloat4x4(&OutWorldMatrices[static_cast<size_t>(ParentIndex)]);
                const XMMATRIX World = XMMatrixMultiply(Local, Parent);
                XMStoreFloat4x4(&OutWorldMatrices[Index], World);
            }
            else
            {
                XMStoreFloat4x4(&OutWorldMatrices[Index], Local);
            }

            Visited[Index] = 1;
        };

        for (size_t Index = 0; Index < NodeCount; ++Index)
        {
            Visit(Index);
        }
    }
}

void InitializeGltfAnimationPose(const FGltfScene& Scene, FGltfAnimationPose& OutPose)
{
    const size_t NodeCount = Scene.NodeTransforms.size();
    OutPose.LocalMatrices.resize(NodeCount);
    OutPose.WorldMatrices.resize(NodeCount);

    for (size_t NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
    {
        OutPose.LocalMatrices[NodeIndex] = Scene.NodeTransforms[NodeIndex].LocalMatrix;
    }

    ComputeWorldMatrices(Scene.NodeTransforms, OutPose.LocalMatrices, OutPose.WorldMatrices);

    OutPose.SkinMatrices.resize(Scene.Skins.size());
    for (size_t SkinIndex = 0; SkinIndex < Scene.Skins.size(); ++SkinIndex)
    {
        OutPose.SkinMatrices[SkinIndex].resize(Scene.Skins[SkinIndex].Joints.size());
    }
}

void UpdateGltfAnimationPose(const FGltfScene& Scene, float TimeSeconds, FGltfAnimationPose& InOutPose)
{
    const size_t NodeCount = Scene.NodeTransforms.size();
    if (NodeCount == 0)
    {
        return;
    }

    if (InOutPose.LocalMatrices.size() != NodeCount)
    {
        InitializeGltfAnimationPose(Scene, InOutPose);
    }

    std::vector<FNodePose> NodePoses(NodeCount);
    for (size_t NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
    {
        const FGltfNodeTransform& Transform = Scene.NodeTransforms[NodeIndex];
        NodePoses[NodeIndex].Translation = Transform.Translation;
        NodePoses[NodeIndex].Rotation = Transform.Rotation;
        NodePoses[NodeIndex].Scale = Transform.Scale;
    }

	// Sampler는 “시간에 따른 값”이라는 순수 데이터고,
	// Channel은 “그 값을 어디에 연결할지”라는 배선 정보다.
    if (!Scene.Animations.empty())
    {
        const FGltfAnimation& Animation = Scene.Animations.front();
        for (const FGltfAnimationChannel& Channel : Animation.Channels)
        {
            if (Channel.NodeIndex < 0 || static_cast<size_t>(Channel.NodeIndex) >= NodeCount)
            {
                continue;
            }

            if (Channel.SamplerIndex < 0 || static_cast<size_t>(Channel.SamplerIndex) >= Animation.Samplers.size())
            {
                continue;
            }

            const FGltfAnimationSampler& Sampler = Animation.Samplers[static_cast<size_t>(Channel.SamplerIndex)];
            if (Sampler.InputTimes.empty())
            {
                continue;
            }

            const float SampleTime = WrapAnimationTime(Sampler.InputTimes, TimeSeconds);
            const size_t KeyIndex = FindKeyframeIndex(Sampler.InputTimes, SampleTime);
            const float Alpha = ComputeLerpAlpha(Sampler.InputTimes, KeyIndex, SampleTime);

            FNodePose& Pose = NodePoses[static_cast<size_t>(Channel.NodeIndex)];
            Pose.bAnimated = true;

            if (Channel.Path == EGltfAnimationPath::Translation && Sampler.OutputVec3.size() > KeyIndex)
            {
                const FFloat3& A = Sampler.OutputVec3[KeyIndex];
                const FFloat3& B = Sampler.OutputVec3[std::min(KeyIndex + 1, Sampler.OutputVec3.size() - 1)];
                Pose.Translation = (Sampler.Interpolation == EGltfAnimationInterpolation::Step)
                    ? A
                    : LerpVec3(A, B, Alpha);
            }
            else if (Channel.Path == EGltfAnimationPath::Scale && Sampler.OutputVec3.size() > KeyIndex)
            {
                const FFloat3& A = Sampler.OutputVec3[KeyIndex];
                const FFloat3& B = Sampler.OutputVec3[std::min(KeyIndex + 1, Sampler.OutputVec3.size() - 1)];
                Pose.Scale = (Sampler.Interpolation == EGltfAnimationInterpolation::Step)
                    ? A
                    : LerpVec3(A, B, Alpha);
            }
            else if (Channel.Path == EGltfAnimationPath::Rotation && Sampler.OutputVec4.size() > KeyIndex)
            {
                const FFloat4& A = Sampler.OutputVec4[KeyIndex];
                const FFloat4& B = Sampler.OutputVec4[std::min(KeyIndex + 1, Sampler.OutputVec4.size() - 1)];
                Pose.Rotation = (Sampler.Interpolation == EGltfAnimationInterpolation::Step)
                    ? A
                    : SlerpQuat(A, B, Alpha);
            }
        }
    }

    InOutPose.LocalMatrices.resize(NodeCount);
    for (size_t NodeIndex = 0; NodeIndex < NodeCount; ++NodeIndex)
    {
        const FGltfNodeTransform& BaseTransform = Scene.NodeTransforms[NodeIndex];
        const FNodePose& Pose = NodePoses[NodeIndex];
        if (!Pose.bAnimated && BaseTransform.bHasMatrix)
        {
            InOutPose.LocalMatrices[NodeIndex] = BaseTransform.LocalMatrix;
        }
        else
        {
            InOutPose.LocalMatrices[NodeIndex] = BuildLocalMatrixLH(Pose.Translation, Pose.Rotation, Pose.Scale);
        }
    }

    ComputeWorldMatrices(Scene.NodeTransforms, InOutPose.LocalMatrices, InOutPose.WorldMatrices);

    InOutPose.SkinMatrices.resize(Scene.Skins.size());
    for (size_t SkinIndex = 0; SkinIndex < Scene.Skins.size(); ++SkinIndex)
    {
        const FGltfSkin& Skin = Scene.Skins[SkinIndex];
        std::vector<DirectX::XMFLOAT4X4>& SkinMatrices = InOutPose.SkinMatrices[SkinIndex];
        SkinMatrices.resize(Skin.Joints.size());

        for (size_t JointIndex = 0; JointIndex < Skin.Joints.size(); ++JointIndex)
        {
            const int NodeIndex = Skin.Joints[JointIndex];
            if (NodeIndex < 0 || static_cast<size_t>(NodeIndex) >= InOutPose.WorldMatrices.size())
            {
                SkinMatrices[JointIndex] = DirectX::XMFLOAT4X4();
                continue;
            }

            using namespace DirectX;
            const XMMATRIX JointWorld = XMLoadFloat4x4(&InOutPose.WorldMatrices[static_cast<size_t>(NodeIndex)]);
            const XMMATRIX InvBind = XMLoadFloat4x4(&Skin.InverseBindMatrices[JointIndex]); // Bone 기준 좌표계(Joint Space)로 되돌리기
			const XMMATRIX SkinMatrix = XMMatrixMultiply(InvBind, JointWorld); // 애니메이션된 좌표계로 변환
            XMStoreFloat4x4(&SkinMatrices[JointIndex], SkinMatrix);
        }
    }
}
