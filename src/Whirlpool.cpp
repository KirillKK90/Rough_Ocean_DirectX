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
    // Collapse height terms; must match WHIRL_BOIL_* / WHIRL_REB_* in Common.hlsli.
    constexpr float kBoilAmp = 0.12f, kBoilT = 1.0f, kBoilR = 1.5f;
    constexpr float kRebR = 1.2f, kRebR0 = 0.6f, kRebLam = 3.0f, kRebAmp = 0.15f,
                    kRebTau = 6.0f, kRebDelay = 1.0f;

    // CPU mirror of WhirlEnvelope (rc and D only; the wound-angle integral S
    // drives the visual warp, which has no CPU consumer). Smoothstep ramp,
    // sech release - C1 at the junction, so the buoy never takes a step.
    void Envelope(float age, const WhirlpoolParams& p, float& rc, float& D)
    {
        float rcPeak = Whirlpool::CoreRadius(p);
        float gmax = 2.0f * kPi * rcPeak * std::sqrt(2.0f * kG * p.depth);
        float gam;
        if (age <= p.grow)
        {
            float u = age / p.grow;
            float sm = u * u * (3.0f - 2.0f * u);
            gam = gmax * sm;
            rc = rcPeak * (0.6f + 0.4f * sm);
        }
        else
        {
            float x = (age - p.grow) / p.tau;
            float ex = std::exp(-x);
            float sech = 2.0f * ex / (1.0f + ex * ex);
            gam = gmax * sech;
            rc = rcPeak * (1.0f + 0.5f * (1.0f - sech));
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
    // tau sits at roughly the gravity fill time 0.9 sqrt(depth), so the funnel
    // drains over about one rotation instead of snapping shut; the gains wind
    // the sea into a wide open spiral rather than a tight corrugation.
    const Preset kPresets[] = {
        { "Weak",      {  1.5f, 0.9f,  3.0f, 1.2f, 1.0f, 0.06f,  70.0f } },
        { "Medium",    {  4.5f, 1.0f,  5.0f, 2.0f, 1.6f, 0.10f, 140.0f } },
        { "Strong",    {  8.0f, 1.1f,  6.5f, 2.6f, 2.0f, 0.14f, 200.0f } },
        { "Super",     { 13.0f, 1.2f,  8.0f, 3.3f, 2.5f, 0.18f, 280.0f } },
        { "Huge",      { 18.0f, 1.4f, 10.0f, 3.8f, 2.9f, 0.22f, 360.0f } },
        { "Gigantic",  { 24.0f, 1.6f, 13.0f, 4.4f, 3.5f, 0.28f, 460.0f } },
        { "Monstrous", { 30.0f, 1.8f, 16.0f, 5.0f, 4.1f, 0.34f, 600.0f } },
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
    // Funnel + collapse boil dome + rebound ring packet, summed over every
    // live vortex; mirrors the height terms of WhirlWavesOne in Common.hlsli
    // (the inward pull, the slopes and all foam are GPU-only).
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

        float d = w.t - p.grow;
        if (d > 0.0f)
        {
            // depthPeak == p.depth and rcPeak == CoreRadius(p) on the CPU.
            float rcR = kRebR * CoreRadius(p);
            float etaR = r * r / (rcR * rcR);
            float xb = d / (kBoilT * p.tau);
            float xe = xb * xb * std::exp(2.0f * (1.0f - xb));
            h += kBoilAmp * p.depth * xe * std::exp(-etaR / kBoilR);

            float dR = d - kRebDelay * p.tau;
            if (dR > 0.05f)
            {
                float r0 = kRebR0 * rcR;
                float rr = std::sqrt(r * r + r0 * r0);
                float kloc = kG * dR * dR / (4.0f * rr * rr);
                float k0 = 2.0f * kPi / (kRebLam * rcR);
                float phase = -kG * dR * dR / (4.0f * rr) + k0 * r0;
                float lx = std::log(std::max(kloc / k0, 1e-6f));
                float envr = std::exp(-lx * lx * 1.4f);
                float Ar = kRebAmp * p.depth * std::sqrt(rcR / (rcR + r))
                         * std::exp(-dR / kRebTau) * envr;
                h += Ar * std::cos(phase);
            }
        }
    }
    return h;
}
