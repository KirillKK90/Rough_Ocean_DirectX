#include "Whirlpool.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    // Must match the WHIRL_* constants in Common.hlsli.
    constexpr float kPi = 3.14159265359f;
    constexpr float kG = 9.81f;
    constexpr float kBeta = 1.2564312f;   // Lamb-Oseen peak-velocity constant
    constexpr float kWindKnee = 12.566f;  // 2 turns
    constexpr float kWindMax = 37.699f;   // 6 turns
    constexpr float kDissolveMul = 2.2f;  // pattern dissolve time, in decay times
    constexpr float kCullMul = 2.15f;     // cull radius, in reach lengths

    // CPU mirror of WhirlEnvelope (rc and D only; the wound-angle integral S
    // drives the visual warp, which has no CPU consumer).
    void Envelope(float age, const WhirlpoolParams& p, float& rc, float& D)
    {
        float rcPeak = Whirlpool::CoreRadius(p);
        float gmax = 2.0f * kPi * rcPeak * std::sqrt(2.0f * kG * p.depth);
        float gam;
        if (age <= p.grow)
        {
            float u = age / p.grow;
            gam = gmax * u;
            rc = rcPeak * (0.35f + 0.65f * u);
        }
        else
        {
            float d = age - p.grow;
            gam = gmax * std::exp(-d / p.tau);
            rc = rcPeak * (1.0f + 0.5f * (1.0f - std::exp(-d / (1.6f * p.tau))));
        }
        D = gam * gam / (8.0f * kPi * kPi * kG * rc * rc);
    }

    // CPU mirror of WhirlSoftLimit (magnitude only).
    float SoftLimit(float x, float knee, float lim)
    {
        float ax = std::abs(x);
        if (ax <= knee)
            return ax;
        float s = std::max(lim - knee, 1e-3f);
        return knee + s * std::tanh((ax - knee) / s);
    }
}

float Whirlpool::Circulation(const WhirlpoolParams& p)
{
    return 2.0f * kPi * CoreRadius(p) * std::sqrt(2.0f * kG * p.depth);
}

float Whirlpool::PeakSwirlSpeed(const WhirlpoolParams& p)
{
    // v_theta peaks at r = rc: Gamma/(2 pi rc) * (1 - exp(-beta)).
    return std::sqrt(2.0f * kG * p.depth) * (1.0f - std::exp(-kBeta));
}

float Whirlpool::WindTurns(const WhirlpoolParams& p)
{
    // Winding angle at the core at full spin-up, after the saturating limit.
    float raw = std::abs(p.gain) * kBeta * std::sqrt(2.0f * kG * p.depth)
              * (0.5f * p.grow) / std::max(CoreRadius(p), 1e-3f);
    return SoftLimit(raw, kWindKnee, kWindMax) / (2.0f * kPi);
}

float Whirlpool::Lifetime(const WhirlpoolParams& p)
{
    // Long enough for the funnel, the dissolving spiral and the rebound ring
    // to all fall below a centimetre, without stalling the next click forever.
    return p.grow + std::clamp(9.0f * p.tau, 12.0f, 26.0f);
}

void Whirlpool::Spawn(const XMFLOAT3& target, const WhirlpoolParams& params)
{
    if (active)
        return;
    cx = target.x;
    cz = target.z;
    t = 0.001f;
    p = params;
    maxAge = Lifetime(p);
    active = true;
}

void Whirlpool::Update(float simDt)
{
    if (!active)
        return;
    t += simDt;
    if (t > maxAge)
        active = false; // sea fully restored
}

XMFLOAT4 Whirlpool::CBValue() const
{
    return active ? XMFLOAT4(cx, cz, t, p.depth) : XMFLOAT4(0, 0, -1.0f, 0);
}

XMFLOAT4 Whirlpool::CBParams0() const
{
    return XMFLOAT4(p.grow, p.tau, CoreRadius(p), p.clockwise ? -p.gain : p.gain);
}

XMFLOAT4 Whirlpool::CBParams1() const
{
    return XMFLOAT4(p.sink, p.reach, kCullMul * p.reach, kDissolveMul * p.tau);
}

float Whirlpool::HeightAt(float x, float z) const
{
    // Funnel + collapse-rebound rings; mirrors WhirlWaves in Common.hlsli
    // (the small spiral ripple arms are ignored for physics).
    if (!active)
        return 0.0f;
    float dx = x - cx, dz = z - cz;
    float r = std::max(std::sqrt(dx * dx + dz * dz), 1e-3f);
    if (r > kCullMul * p.reach)
        return 0.0f;

    float rc, D;
    Envelope(t, p, rc, D);
    float eta = r * r / (rc * rc);
    float h = -D / (1.0f + eta);

    float dAge = t - p.grow;
    if (dAge > 0.05f)
    {
        float rr = std::max(r, 0.6f * rc);
        float kloc = kG * dAge * dAge / (4.0f * rr * rr);
        float phase = -kG * dAge * dAge / (4.0f * rr);
        float k0 = 2.0f * kPi / (2.5f * rc);
        float lx = std::log(std::max(kloc / k0, 1e-6f));
        float envr = std::exp(-lx * lx * 1.4f);
        float Ar = 0.32f * p.depth * std::pow(rc / (rc + r), 0.8f)
                 * std::exp(-dAge / 4.5f) * envr;
        h += Ar * std::cos(phase);
    }
    return h;
}
