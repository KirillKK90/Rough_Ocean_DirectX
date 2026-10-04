#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

// Free-look camera. World units are meters, +Y up, left-handed, camera starts
// looking down +Z. Rendering is camera-relative: the view matrix has no
// translation; systems subtract the camera position on the CPU/VS side.
class Camera
{
public:
    DirectX::XMFLOAT3 pos{ 0.0f, 12.0f, 0.0f };
    float yaw = 0.0f;    // radians, 0 = +Z
    float pitch = 0.0f;  // radians, + looks up
    float fovY = 60.0f * DirectX::XM_PI / 180.0f;
    float zoom = 1.0f;   // optical zoom: focal length relative to the fovY lens
    float nearZ = 0.4f;
    float farZ = 80000.0f;
    float moveSpeed = 8.0f; // m/s

    DirectX::XMVECTOR Forward() const
    {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        float cy = std::cos(yaw), sy = std::sin(yaw);
        return DirectX::XMVectorSet(cp * sy, sp, cp * cy, 0.0f);
    }

    // Rotation-only view matrix (camera at origin).
    DirectX::XMMATRIX ViewRot() const
    {
        DirectX::XMVECTOR fwd = Forward();
        DirectX::XMVECTOR up = DirectX::XMVectorSet(0, 1, 0, 0);
        return DirectX::XMMatrixLookToLH(DirectX::XMVectorZero(), fwd, up);
    }

    // Field of view through the zoom lens: a longer focal length shrinks the
    // half-angle tangent by the zoom factor.
    float ZoomedFovY() const
    {
        return zoom == 1.0f ? fovY : 2.0f * std::atan(std::tan(0.5f * fovY) / zoom);
    }

    // Reversed-Z projection (near/far swapped) for good depth precision at
    // horizon distances.
    DirectX::XMMATRIX Proj(float aspect) const
    {
        return DirectX::XMMatrixPerspectiveFovLH(ZoomedFovY(), aspect, farZ, nearZ);
    }

    DirectX::XMMATRIX ViewProj(float aspect) const
    {
        return ViewRot() * Proj(aspect);
    }

    void AddLook(float dx, float dy)
    {
        // Wrapped, so the angle keeps full float precision: at 100x zoom one
        // pixel is ~1e-5 rad, which a yaw wound up over many turns would lose.
        yaw = std::remainder(yaw + dx, DirectX::XM_2PI);
        pitch = std::clamp(pitch - dy, -1.5f, 1.5f);
    }

    void Move(float fwd, float right, float up, float dt)
    {
        using namespace DirectX;
        XMVECTOR f = Forward();
        XMVECTOR r = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), f));
        XMVECTOR p = XMLoadFloat3(&pos);
        p = XMVectorAdd(p, XMVectorScale(f, fwd * moveSpeed * dt));
        p = XMVectorAdd(p, XMVectorScale(r, right * moveSpeed * dt));
        p = XMVectorAdd(p, XMVectorSet(0, up * moveSpeed * dt, 0, 0));
        XMStoreFloat3(&pos, p);
        pos.y = std::clamp(pos.y, 2.0f, 4000.0f); // keep the eye above the waves
    }
};
