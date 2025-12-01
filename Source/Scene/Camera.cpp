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
    return XMMatrixPerspectiveFovLH(FovY, AspectRatio, NearClip, FarClip);
}
