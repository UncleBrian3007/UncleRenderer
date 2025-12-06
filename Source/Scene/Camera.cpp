#include "Camera.h"
#include <DirectXMath.h>

FCamera::FCamera()
    : Position(0.0f, 0.0f, -5.0f)
    , Forward(0.0f, 0.0f, 1.0f)
    , Up(0.0f, 1.0f, 0.0f)
    , FovY(DirectX::XM_PIDIV4)
    , AspectRatio(16.0f / 9.0f)
    , NearClip(0.1f)
    , FarClip(1000.0f)
{
}

void FCamera::SetPerspective(float InFovYRadians, float InAspectRatio, float InNearClip, float InFarClip)
{
    FovY = InFovYRadians;
    AspectRatio = InAspectRatio;
    NearClip = InNearClip;
    FarClip = InFarClip;
}

FMatrix FCamera::GetViewMatrix() const
{
    using namespace DirectX;

    const XMVECTOR Eye = XMLoadFloat3(&Position);
    const XMVECTOR Dir = XMLoadFloat3(&Forward);
    const XMVECTOR UpVector = XMLoadFloat3(&Up);
    return XMMatrixLookToLH(Eye, Dir, UpVector);
}

FMatrix FCamera::GetProjectionMatrix() const
{
    using namespace DirectX;

    const float YScale = 1.0f / tanf(FovY * 0.5f);
    const float XScale = YScale / AspectRatio;

    // Reverse-Z infinite perspective projection (DirectX style, mul(vector, matrix))
    return XMMatrixSet(
        XScale, 0.0f, 0.0f, 0.0f,
        0.0f, YScale, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, NearClip, 0.0f
    );
}
