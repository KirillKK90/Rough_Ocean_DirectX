#pragma once

#include <DirectXMath.h>

// Tunable shape of a whirlpool event. The defaults are the calm "bathtub
// drain" look; the UI exposes every field. Values are captured when the
// vortex spawns, so editing a slider never snaps a vortex that is already
// spinning (the wound-angle integral S(t) would jump).
struct WhirlpoolParams
{
    float depth = 4.5f;    // peak funnel depth at full spin-up, meters
    float sizeMul = 1.0f;  // core-radius multiplier on the depth-derived width
    float grow = 5.0f;     // spin-up: how long the "plug" keeps pulling, s
    float tau = 1.0f;      // circulation decay time once the forcing stops, s
    float gain = 2.2f;     // how far the surrounding sea is wound in
    float sink = 0.10f;    // how strongly the surroundings converge on the drain
    float reach = 140.0f;  // distance scale of the disturbance, meters
    bool clockwise = false; // spin direction seen from above
};

// Strength presets: one named step drives the whole parameter pack, from a
// bathtub drain up to a ship-swallowing maelstrom. Bigger steps are also
// wider and longer-lived, which is both what real vortices do and what keeps
// them in the well-resolved part of the parameter space. kWhirlDefaultPreset
// ("Medium") matches the WhirlpoolParams defaults exactly.
constexpr int kWhirlDefaultPreset = 1;

// Whirlpool events (left-click mode): bathtub / maelstrom vortices. A
// Lamb-Oseen vortex over a softened sink spins up for `grow` seconds - the
// funnel deepens and widens while the surrounding sea is wound into a spiral
// - then the forcing stops and it relaxes away over ~`tau` seconds, radiating
// a rebound ring packet. All surface math lives in Common.hlsli
// (WhirlEnvelope / WhirlWarp / WhirlWaves); this class owns the event state
// and the CPU height mirror that feeds the buoy physics.
//
// Up to kMaxActive vortices live at once and interact: their particle maps
// compose and their surfaces superpose (see Common.hlsli). Spawning past that
// recycles whichever is nearest the end of its life.
class Whirlpool
{
public:
    static constexpr uint32_t kMaxActive = 4; // must match WHIRL_MAX in Common.hlsli

    void Spawn(const DirectX::XMFLOAT3& target, const WhirlpoolParams& params);
    void Update(float simDt);
    bool AnyActive() const;
    uint32_t ActiveCount() const;
    bool SlotActive(uint32_t i) const { return v[i].active; }
    float SlotAge(uint32_t i) const { return v[i].t; }
    float SlotLifetime(uint32_t i) const { return v[i].maxAge; }

    // FrameCB values; see the gWhirl* packing comment in Common.hlsli.
    void FillCB(DirectX::XMFLOAT4 whirl[kMaxActive], DirectX::XMFLOAT4 whirl2[kMaxActive],
                DirectX::XMFLOAT4 whirl3[kMaxActive]) const;

    // CPU water-height contribution (funnel + collapse rings) for the buoy.
    // Must match the height terms of WhirlWaves in Common.hlsli.
    float HeightAt(float x, float z) const;

    // Derived physical quantities, for the UI readout and for the shader
    // constants. A deeper whirl is also broader unless sizeMul says otherwise.
    static float CoreRadius(const WhirlpoolParams& p) { return (3.0f + 0.6f * p.depth) * p.sizeMul; }
    static float Circulation(const WhirlpoolParams& p); // peak Gamma, m^2/s
    static float PeakSwirlSpeed(const WhirlpoolParams& p); // max tangential speed, m/s
    static float WindTurns(const WhirlpoolParams& p);   // full turns the core pattern winds
    static float Lifetime(const WhirlpoolParams& p);    // total event duration, s

    // Strength presets (Weak .. Monstrous).
    static int PresetCount();
    static const char* PresetName(int index);
    static WhirlpoolParams Preset(int index);
    // True if p carries a preset's magnitudes (spin direction is independent).
    static bool MatchesPreset(const WhirlpoolParams& p, int index);

private:
    struct Vortex
    {
        bool active = false;
        float cx = 0, cz = 0;
        float t = 0;
        float maxAge = 17.0f;
        WhirlpoolParams p;
    };

    Vortex v[kMaxActive];
};
