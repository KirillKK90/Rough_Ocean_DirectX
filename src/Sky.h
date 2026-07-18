#pragma once

#include <DirectXMath.h>

#include "GpuContext.h"

// Atmospheric sky baked into a cubemap with a full mip chain; regenerated only
// when the lighting/time-of-day parameters change. Also renders the skybox.
class Sky
{
public:
    struct Params
    {
        DirectX::XMFLOAT3 sunDir{ 0, 0.5f, 0.87f };
        float sunIntensity = 22.0f;
        DirectX::XMFLOAT3 moonDir{ 0, 0.7f, 0.7f };
        float moonIntensity = 0.0f;
        float cloudCover = 0.15f;
    };

    void Create(GpuContext& ctx, uint32_t resolution);
    void MarkDirty() { dirty = true; }
    bool IsDirty() const { return dirty; }
    uint32_t Resolution() const { return res; }

    // Regenerates the cubemap + mips if dirty. Call while a command list is open.
    void RecordGenerate(GpuContext& ctx, const Params& params);
    void Draw(GpuContext& ctx, D3D12_GPU_VIRTUAL_ADDRESS frameCB);

private:
    ComPtr<ID3D12Resource> cube;
    ComPtr<ID3D12PipelineState> psoGen, psoMip, psoDraw;
    uint32_t res = 128;
    uint32_t mips = 8;
    bool dirty = true;
    bool inSrvState = false;
};
