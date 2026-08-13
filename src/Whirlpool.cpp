#include "Whirlpool.h"

#include <algorithm>
#include <cmath>
#include <iterator>

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

    struct Preset
    {
        const char* name;
        WhirlpoolParams p;
    };
    // depth, sizeMul, grow, tau, gain, sink, reach. Depth carries the power
    // (peak swirl ~ sqrt(2 g depth)); width, duration and reach grow with it
    // so every step stays a plausible vortex rather than a deeper spike.
    const Preset kPresets[] = {
        { "Weak",      {  1.5f, 0.9f,  3.0f, 0.8f, 1.4f, 0.06f,  70.0f } },
        { "Medium",    {  4.5f, 1.0f,  5.0f, 1.0f, 2.2f, 0.10f, 140.0f } },
        { "Strong",    {  8.0f, 1.1f,  6.5f, 1.3f, 2.8f, 0.14f, 200.0f } },
        { "Super",     { 13.0f, 1.2f,  8.0f, 1.6f, 3.4f, 0.18f, 280.0f } },
        { "Huge",      { 18.0f, 1.4f, 10.0f, 2.0f, 4.0f, 0.22f, 360.0f } },
        { "Gigantic",  { 24.0f, 1.6f, 13.0f, 2.6f, 4.8f, 0.28f, 460.0f } },
        { "Monstrous", { 30.0f, 1.8f, 16.0f, 3.2f, 5.6f, 0.34f, 600.0f } },
    };
    constexpr int kNumPresets = int(std::size(kPresets));
}

int Whirlpool::PresetCount()
{
    return kNumPresets;
}

const char* Whirlpool::PresetName(int index)
{
    return kPresets[std::clamp(index, 0, kNumPresets - 1)].name;
}

WhirlpoolParams Whirlpool::Preset(int index)
{
    return kPresets[std::clamp(index, 0, kNumPresets - 1)].p;
}

bool Whirlpool::MatchesPreset(const WhirlpoolParams& p, int index)
{
    const WhirlpoolParams& q = kPresets[std::clamp(index, 0, kNumPresets - 1)].p;
    auto eq = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    return eq(p.depth, q.depth) && eq(p.sizeMul, q.sizeMul) && eq(p.grow, q.grow)
        && eq(p.tau, q.tau) && eq(p.gain, q.gain) && eq(p.sink, q.sink)
        && eq(p.reach, q.reach);
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
    // Take a free slot; if all are busy, recycle whichever is nearest the end
    // of its life so the pop lands on the least visible one.
    Vortex* slot = nullptr;
    for (Vortex& w : v)
        if (!w.active)
        {
            slot = &w;
            break;
        }
    if (!slot)
    {
        slot = &v[0];
        for (Vortex& w : v)
            if (w.t / std::max(w.maxAge, 1e-3f) > slot->t / std::max(slot->maxAge, 1e-3f))
                slot = &w;
    }

    slot->cx = target.x;
    slot->cz = target.z;
    slot->t = 0.001f;
    slot->p = params;
    slot->maxAge = Lifetime(params);
    slot->active = true;
}

void Whirlpool::Update(float simDt)
{
    for (Vortex& w : v)
    {
        if (!w.active)
            continue;
        w.t += simDt;
        if (w.t > w.maxAge)
            w.active = false; // sea fully restored
    }
}

bool Whirlpool::AnyActive() const
{
    for (const Vortex& w : v)
        if (w.active)
            return true;
    return false;
}

uint32_t Whirlpool::ActiveCount() const
{
    uint32_t n = 0;
    for (const Vortex& w : v)
        n += w.active ? 1 : 0;
    return n;
}

void Whirlpool::FillCB(XMFLOAT4 whirl[kMaxActive], XMFLOAT4 whirl2[kMaxActive],
                       XMFLOAT4 whirl3[kMaxActive]) const
{
    for (uint32_t i = 0; i < kMaxActive; ++i)
    {
        const Vortex& w = v[i];
        const WhirlpoolParams& p = w.p;
        whirl[i] = w.active ? XMFLOAT4(w.cx, w.cz, w.t, p.depth)
                            : XMFLOAT4(0, 0, -1.0f, 0);
        whirl2[i] = XMFLOAT4(p.grow, p.tau, CoreRadius(p), p.clockwise ? -p.gain : p.gain);
        whirl3[i] = XMFLOAT4(p.sink, p.reach, kCullMul * p.reach, kDissolveMul * p.tau);
    }
}

float Whirlpool::HeightAt(float x, float z) const
{
    // Funnel + collapse-rebound rings, summed over every live vortex; mirrors
    // WhirlWaves in Common.hlsli (the small spiral arms are ignored here).
    float h = 0.0f;
    for (const Vortex& w : v)
    {
        if (!w.active)
            continue;
        const WhirlpoolParams& p = w.p;
        float dx = x - w.cx, dz = z - w.cz;
        float r = std::max(std::sqrt(dx * dx + dz * dz), 1e-3f);
        if (r > kCullMul * p.reach)
            continue;

        float rc, D;
        Envelope(w.t, p, rc, D);
        float eta = r * r / (rc * rc);
        h += -D / (1.0f + eta);

        float dAge = w.t - p.grow;
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
    }
    return h;
}
