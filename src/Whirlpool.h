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

// Whirlpool event (left-click mode): a bathtub / maelstrom vortex. A
// Lamb-Oseen vortex over a softened sink spins up for `grow` seconds - the
// funnel deepens and widens while the surrounding sea is wound into a spiral
// - then the forcing stops and it relaxes away over ~`tau` seconds, radiating
// a rebound ring packet. All surface math lives in Common.hlsli
// (WhirlEnvelope / WhirlWarp / WhirlWaves); this class owns the event state
// and the CPU height mirror that feeds the buoy physics. One vortex can be
// live at a time (like the meteorite, a new one cannot start until it ends).
class Whirlpool
{
public:
    void Spawn(const DirectX::XMFLOAT3& target, const WhirlpoolParams& params);
    void Update(float simDt);
    bool Active() const { return active; }
    float Age() const { return t; }
    float Lifetime() const { return maxAge; }

    // FrameCB values; see the gWhirl* packing comment in Common.hlsli.
    DirectX::XMFLOAT4 CBValue() const;   // xy = center XZ, z = age (<0 off), w = peak depth
    DirectX::XMFLOAT4 CBParams0() const; // spin-up, decay, peak core radius, signed gain
    DirectX::XMFLOAT4 CBParams1() const; // draw-in, reach, cull radius, dissolve

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

private:
    bool active = false;
    float cx = 0, cz = 0;
    float t = 0;
    float maxAge = 17.0f;
    WhirlpoolParams p;
};
