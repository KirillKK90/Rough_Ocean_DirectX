#pragma once

#include <DirectXMath.h>

#include "GpuContext.h"
#include "Ocean.h"

class Meteor;

// Navigation buoy: procedural mesh, buoyancy physics driven by CPU water
// samples, and an alternating red/green flashing lamp.
class Buoy
{
public:
    void Create(GpuContext& ctx);

    // Advance physics; ocean provides water height/normal at the anchor, and
    // meteor (optional) adds impact ring waves.
    void Update(GpuContext& ctx, Ocean& ocean, const Meteor* meteor,
                float dt, float simTime, float lambda);

    void Draw(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB, const DirectX::XMFLOAT3& camPos);
    void DrawGlow(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB, const DirectX::XMFLOAT3& camPos);

    // Lamp state for lighting the scene.
    bool LightOn() const { return lightOn; }
    DirectX::XMFLOAT3 LightColor() const { return lightColor; }
    DirectX::XMFLOAT3 LampWorldPos() const { return lampWorldPos; }

    DirectX::XMFLOAT2 anchor{ 0.0f, 130.0f }; // world XZ
    float flashPeriod = 2.5f;                 // full red+green cycle, seconds
    float flashDuration = 0.5f;
    float lampIntensity = 30.0f;              // emissive HDR scale

private:
    struct Vertex
    {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 nrm;
        DirectX::XMFLOAT3 col;
    };

    void BuildMesh(GpuContext& ctx);
    DirectX::XMMATRIX WorldMatrix() const;

    ComPtr<ID3D12PipelineState> psoBody, psoGlow;
    ComPtr<ID3D12Resource> vb, ib;
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    uint32_t bodyIndexCount = 0;
    uint32_t lampIndexStart = 0, lampIndexCount = 0;

    // Physics state.
    float heaveY = 0.0f, heaveV = 0.0f;
    DirectX::XMFLOAT3 up{ 0, 1, 0 };
    DirectX::XMFLOAT2 sway{ 0, 0 };
    float rollPhase = 0.0f;

    bool lightOn = false;
    DirectX::XMFLOAT3 lightColor{ 1, 0, 0 };
    DirectX::XMFLOAT3 lampWorldPos{ 0, 3, 130 };
    static constexpr float kLampHeight = 3.35f;
};
