#include "Whirlpool.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    // Must match the WHIRL_* constants in Common.hlsli.
    constexpr float kPi = 3.14159265359f;
    constexpr float kG = 9.81f;
    constexpr float kGrow = 5.0f;
    constexpr float kTau = 1.0f;
    constexpr float kSkipR = 300.0f;
    constexpr float kMaxAge = 17.0f; // funnel, rings and dissolve all done

    // CPU mirror of WhirlEnvelope (rc and D only; S drives the visual warp
    // which has no CPU consumer).
    void Envelope(float age, float depthPeak, float& rc, float& D)
    {
        float rcPeak = 3.0f + 0.6f * depthPeak;
        float gmax = 2.0f * kPi * rcPeak * std::sqrt(2.0f * kG * depthPeak);
        float gam;
        if (age <= kGrow)
        {
            float u = age / kGrow;
            gam = gmax * u;
            rc = rcPeak * (0.35f + 0.65f * u);
        }
        else
        {
            float d = age - kGrow;
            gam = gmax * std::exp(-d / kTau);
            rc = rcPeak * (1.0f + 0.5f * (1.0f - std::exp(-d / 1.6f)));
        }
        D = gam * gam / (8.0f * kPi * kPi * kG * rc * rc);
    }
}

void Whirlpool::Spawn(const XMFLOAT3& target, float depth)
{
    if (active)
        return;
    cx = target.x;
    cz = target.z;
    t = 0.001f;
    depthPeak = depth;
    active = true;
}

void Whirlpool::Update(float simDt)
{
    if (!active)
        return;
    t += simDt;
    if (t > kMaxAge)
        active = false; // sea fully restored
}

XMFLOAT4 Whirlpool::CBValue() const
{
    return active ? XMFLOAT4(cx, cz, t, depthPeak) : XMFLOAT4(0, 0, -1.0f, 0);
}

float Whirlpool::HeightAt(float x, float z) const
{
    // Funnel + collapse-rebound rings; mirrors WhirlWaves in Common.hlsli
    // (the centimetre-scale spiral ripple arms are ignored for physics).
    if (!active)
        return 0.0f;
    float dx = x - cx, dz = z - cz;
    float r = std::max(std::sqrt(dx * dx + dz * dz), 1e-3f);
    if (r > kSkipR)
        return 0.0f;

    float rc, D;
    Envelope(t, depthPeak, rc, D);
    float eta = r * r / (rc * rc);
    float h = -D / (1.0f + eta);

    float dAge = t - kGrow;
    if (dAge > 0.05f)
    {
        float rr = std::max(r, 0.6f * rc);
        float kloc = kG * dAge * dAge / (4.0f * rr * rr);
        float phase = -kG * dAge * dAge / (4.0f * rr);
        float k0 = 2.0f * kPi / (2.5f * rc);
        float lx = std::log(std::max(kloc / k0, 1e-6f));
        float envr = std::exp(-lx * lx * 1.4f);
        float Ar = 0.32f * depthPeak * std::pow(rc / (rc + r), 0.8f)
                 * std::exp(-dAge / 4.5f) * envr;
        h += Ar * std::cos(phase);
    }
    return h;
}
