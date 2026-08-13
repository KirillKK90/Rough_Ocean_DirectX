#pragma once

#include <DirectXMath.h>

// Whirlpool event (left-click mode): a bathtub / maelstrom vortex. A
// Lamb-Oseen vortex over a softened sink spins up for 5 seconds - the funnel
// deepens and widens while the surrounding sea is wound into a spiral - then
// the forcing stops and it swiftly relaxes away, radiating a small rebound
// ring packet. All surface math lives in Common.hlsli (WhirlEnvelope /
// WhirlWarp / WhirlWaves); this class owns the event state and the CPU
// height mirror that feeds the buoy physics. One vortex can be live at a
// time (like the meteorite, a new one cannot start until it is over).
class Whirlpool
{
public:
    void Spawn(const DirectX::XMFLOAT3& target, float depthPeak);
    void Update(float simDt);
    bool Active() const { return active; }

    // FrameCB value: xy = world XZ, z = age seconds (<0 off), w = peak depth.
    DirectX::XMFLOAT4 CBValue() const;

    // CPU water-height contribution (funnel + collapse rings) for the buoy.
    // Must match the height terms of WhirlWaves in Common.hlsli.
    float HeightAt(float x, float z) const;

private:
    bool active = false;
    float cx = 0, cz = 0;
    float t = 0;
    float depthPeak = 4.5f;
};
