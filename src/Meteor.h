#pragma once

#include <DirectXMath.h>
#include <random>

#include "Camera.h"
#include "GpuContext.h"

// Meteorite event: a flaming rock falls at ~45 degrees into the ocean and
// spawns Cauchy-Poisson ring waves (see ImpactWaves in Common.hlsli; the
// CPU mirror HeightAt below feeds the buoy physics). Up to kMaxImpacts wave
// systems can be live at once; each decays back to a calm sea.
class Meteor
{
public:
    static constexpr uint32_t kMaxImpacts = 4;
    static constexpr uint32_t kMaxParticles = 768;

    void Create(GpuContext& ctx);
    void Launch(const Camera& camera, float power); // random spot ahead of the camera
    // Fall exactly onto a chosen water point, approaching horizontally along
    // approachHoriz (the direction of travel; its reverse is where it comes from).
    void LaunchAt(const DirectX::XMFLOAT3& target, const DirectX::XMFLOAT3& approachHoriz, float power);
    void Update(float simDt);

    void UploadParticles(GpuContext& ctx);
    void DrawRock(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB, const DirectX::XMFLOAT3& camPos);
    void DrawTrail(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB);

    // Impact array for FrameCB: xy = world XZ, z = age seconds, w = amplitude.
    void FillImpacts(DirectX::XMFLOAT4 out[kMaxImpacts]) const;

    // CPU water-height contribution of all live impacts (buoy physics).
    // Must match ImpactWaves in Common.hlsli.
    float HeightAt(float x, float z) const;
    bool AnyImpactActive() const;
    bool Flying() const { return flying; }

private:
    struct Impact
    {
        bool active = false;
        float x = 0, z = 0;
        float t = 0;
        float a0 = 0;
    };

    struct Particle
    {
        bool active = false;
        DirectX::XMFLOAT3 pos{};
        DirectX::XMFLOAT3 vel{};
        float gravity = 0; // m/s^2 downward (negative = buoyant smoke)
        float drag = 0;
        float life = 0, maxLife = 1;
        float size = 1, growth = 0;
        DirectX::XMFLOAT3 colStart{}, colEnd{};
        float intensity = 1;
    };

    // GPU-side particle layout (matches Meteor.hlsl).
    struct GpuParticle
    {
        DirectX::XMFLOAT4 posSize;
        DirectX::XMFLOAT4 color;
    };

    void BeginFlight(const DirectX::XMFLOAT3& target, const DirectX::XMFLOAT2& uHoriz,
                     float elevRad, float power);
    void EmitTrail(float dt);
    void SpawnImpact();
    Particle* AllocParticle();
    float Rand01() { return dist01(rng); }

    ComPtr<ID3D12PipelineState> psoRock, psoParticle;
    ComPtr<ID3D12Resource> vb, ib;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    uint32_t indexCount = 0;

    ComPtr<ID3D12Resource> particleBuf[GpuContext::kFramesInFlight];
    GpuParticle* particleMapped[GpuContext::kFramesInFlight] = {};
    uint32_t liveParticleCount = 0;

    bool flying = false;
    DirectX::XMFLOAT3 pos{}, vel{};
    DirectX::XMFLOAT3 impactTarget{}; // exact water point to strike (wave center)
    float tumble = 0;
    float emitAccum = 0;
    float flightTime = 0;
    float power = 3.0f;

    Impact impacts[kMaxImpacts];
    Particle particles[kMaxParticles];
    std::mt19937 rng{ 20260719u };
    std::uniform_real_distribution<float> dist01{ 0.0f, 1.0f };
};
